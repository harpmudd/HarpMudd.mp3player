// Where does a glyph cell's data actually LAND, with the real controller?
//
// Hardware: the last one or two ink columns of every glyph are missing, the
// last glyph of a string is clean, and no ink ever appears early. The atlas
// is correct (audited, 1918 glyphs) and the engine composes the row correctly
// (tb_glyph32), so this runs the REAL sdram_fb against an SDRAM model that
// serves reads and records writes, draws two adjacent cells the way the
// firmware does, and prints what ended up in the framebuffer.
`timescale 1ns/1ps
`default_nettype none

module tb_write;
    localparam [24:0] FONT_BASE  = 25'h0100000;
    localparam [24:0] FONT32_OFF = 25'd554864;   // 32px region
    localparam        GM         = 7'h64 - 7'h20;          // 'd'
    localparam [24:0] GBASE      = FONT_BASE + FONT32_OFF + GM * 256;
    localparam [24:0] IDX        = 25'h8000 | GM;          // FEXT_32PX | glyph
    localparam [15:0] BG         = 16'h0842;

    reg clk = 0;     always #5    clk = ~clk;              // 100 MHz
    reg clk_sys = 0; always #7.5  clk_sys = ~clk_sys;
    reg clk_vid = 0; always #41.6 clk_vid = ~clk_vid;
    reg reset = 1;

    wire [24:0] p0_addr;  wire [15:0] p0_data; wire [1:0] p0_byte_en;
    wire [10:0] p0_wr_len; wire p0_wr_stream, p0_wr_req, p0_rd_req, p0_end_burst_req;
    wire [15:0] p0_q; wire p0_available, p0_ready, p0_data_available;
    wire [10:0] wsrc_addr; wire [15:0] wsrc_q; wire init_complete;

    reg  [1:0]  cmd_op = 0;   reg [18:0] cmd_addr = 0;
    reg  [8:0]  cmd_w = 0, cmd_h = 0;
    reg  [6:0]  cmd_glyph = 0; reg [1:0] cmd_sx = 0, cmd_sy = 0;
    reg         cmd_push = 0;

    mp3_fb dut (
        .reset(reset), .clk_sys(clk_sys), .clk_sdram(clk), .clk_vid(clk_vid),
        .cmd_push(cmd_push), .cmd_op(cmd_op), .cmd_addr(cmd_addr),
        .cmd_w(cmd_w), .cmd_h(cmd_h), .cmd_fg(16'hFFFF), .cmd_bg(BG),
        .cmd_glyph(cmd_glyph), .cmd_sx(cmd_sx), .cmd_sy(cmd_sy), .cmd_full(),
        .fontw_en(1'b0), .fontw_addr(22'd0), .fontw_data(16'd0),
        .sdram_init_complete(init_complete),
        .p0_addr(p0_addr), .p0_data(p0_data), .p0_byte_en(p0_byte_en),
        .p0_wr_len(p0_wr_len), .p0_wr_stream(p0_wr_stream), .p0_q(p0_q),
        .p0_wr_req(p0_wr_req), .p0_rd_req(p0_rd_req),
        .p0_end_burst_req(p0_end_burst_req), .p0_available(p0_available),
        .p0_ready(p0_ready), .p0_data_available(p0_data_available),
        .wsrc_addr(wsrc_addr), .wsrc_q(wsrc_q),
        .video_rgb(), .video_de(), .video_hs(), .video_vs()
    );

    wire [15:0] DQ; wire [12:0] A; wire [1:0] DQM, BA;
    wire nWE, nRAS, nCAS;

    sdram_fb #(.CLOCK_SPEED_MHZ(100), .BURST_TYPE(0), .CAS_LATENCY(2), .WRITE_BURST(1), .FAULT_INJECT(0))
    ctl (
        .clk(clk), .reset(reset), .init_complete(init_complete),
        .p0_addr(p0_addr), .p0_data(p0_data), .p0_byte_en(p0_byte_en),
        .p0_wr_len(p0_wr_len), .p0_q(p0_q),
        .p0_wr_stream(p0_wr_stream), .wsrc_addr(wsrc_addr), .wsrc_q(wsrc_q),
        .p0_wr_req(p0_wr_req), .p0_rd_req(p0_rd_req),
        .p0_end_burst_req(p0_end_burst_req),
        .p0_available(p0_available), .p0_ready(p0_ready),
        .p0_data_available(p0_data_available),
        .SDRAM_DQ(DQ), .SDRAM_A(A), .SDRAM_DQM(DQM), .SDRAM_BA(BA),
        .SDRAM_nCS(), .SDRAM_nWE(nWE), .SDRAM_nRAS(nRAS), .SDRAM_nCAS(nCAS),
        .SDRAM_CKE(), .SDRAM_CLK()
    );

    // ---- SDRAM model -------------------------------------------------------
    reg [15:0] gmem [0:255];                 // the 'd' 32px glyph image
    reg [15:0] fb   [0:16383];               // framebuffer words we watch
    reg [12:0] act_row [0:3];
    integer i;
    initial begin
        $readmemh("sim/glyph_d32.hex", gmem);
        for (i = 0; i < 16384; i = i + 1) fb[i] = 16'hDEAD;
        for (i = 0; i < 4; i = i + 1) act_row[i] = 0;
    end

    wire [2:0] cmd = {nRAS, nCAS, nWE};      // ACTIVE 011 WRITE 100 READ 101 PRE 010

    reg        wact = 0; reg [1:0] wbk = 0; reg [9:0] wcol = 0;
    reg        ract = 0; reg [1:0] rbk = 0; reg [9:0] rcol = 0;
    reg [15:0] rpipe0 = 0, rdata = 0;
    reg        rv0 = 0, rv2 = 0;

    function [15:0] fetch(input [24:0] a);
        if (a >= GBASE && a < GBASE + 256) fetch = gmem[a - GBASE];
        else if (a < 16384)                fetch = (fb[a] === 16'hDEAD) ? 16'h0 : fb[a];
        else                               fetch = 16'h0;
    endfunction

    // Drive ONLY while a read is actually outstanding -- otherwise the
    // model and the controller both drive DQ during writes and every
    // word resolves to X.
    assign DQ = (rv2 && ract && !wact) ? rdata : 16'hZZZZ;

    always @(posedge clk) if (!reset) begin
        if (cmd == 3'b011) act_row[BA] <= A;
        if (cmd == 3'b010) begin wact <= 0; ract <= 0; end

        // write: the WRITE command cycle carries beat 0, then one per cycle
        if (cmd == 3'b100) begin
            wact <= 1; wbk <= BA; ract <= 0;
            if (DQM != 2'b11 && {BA, act_row[BA], A[9:0]} < 16384)
                fb[{BA, act_row[BA], A[9:0]}] <= DQ;
            wcol <= A[9:0] + 10'd1;
        end else if (wact && cmd == 3'b111) begin
            if (DQM != 2'b11 && {wbk, act_row[wbk], wcol} < 16384)
                fb[{wbk, act_row[wbk], wcol}] <= DQ;
            wcol <= wcol + 10'd1;
        end

        // read: CAS latency 2
        rv0 <= 0;
        if (cmd == 3'b101) begin
            ract <= 1; rbk <= BA;
            if (ndbg < 4) begin
                $display("MODEL READ cmd: BA=%0d act_row=%0d col=%0d -> addr %0d (GBASE %0d) data %h",
                         BA, act_row[BA], A[9:0], {BA, act_row[BA], A[9:0]}, GBASE,
                         fetch({BA, act_row[BA], A[9:0]}));
                ndbg = ndbg + 1;
            end
            rpipe0 <= fetch({BA, act_row[BA], A[9:0]}); rv0 <= 1;
            rcol <= A[9:0] + 10'd1;
        end else if (ract) begin
            rpipe0 <= fetch({rbk, act_row[rbk], rcol}); rv0 <= 1;
            rcol <= rcol + 10'd1;
        end
        rdata <= rpipe0; rv2 <= rv0;
    end

    // ---- stimulus ----------------------------------------------------------
    task draw(input [18:0] at);
        begin
            @(posedge clk_sys);
            cmd_op <= 2'd2; cmd_glyph <= 7'h7F; cmd_addr <= at;
            cmd_w <= (IDX >> 9) & 9'h1FF; cmd_h <= IDX & 9'h1FF;
            cmd_sx <= 0; cmd_sy <= 0; cmd_push <= 1;
            @(posedge clk_sys); cmd_push <= 0;
        end
    endtask

    // probe: first read-data events and first glyph-row write beats
    integer nrd = 0, nwb = 0, ndbg = 0, nresume = 0;
    always @(posedge clk) if (!reset)
        if (dut.astate == 3'd7 && !p0_data_available && dut.bgot) begin
            nresume = nresume + 1;
            if (nresume < 6)
                $display("RESUME: burst cut short, row word %0d of %0d, re-requesting",
                         dut.ecnt, dut.ext_last_w);
        end
    always @(posedge clk) if (!reset) begin
        if (p0_data_available && nrd < 14) begin
            $display("RD %0d  p0_addr=%0d  p0_q=%h  DQ=%h  rv2=%b ract=%b wact=%b",
                     nrd, p0_addr, p0_q, DQ, rv2, ract, wact);
            nrd = nrd + 1;
        end
        if (p0_wr_req && p0_wr_stream && nwb < 3) begin
            $display("WR req addr=%0d len=%0d  rowlo=%h rowmid=%h rowhi=%h rowtop=%h",
                     p0_addr, p0_wr_len, dut.rowlo, dut.rowmid, dut.rowhi, dut.rowtop);
            nwb = nwb + 1;
        end
    end

    integer r13;
    initial begin
        repeat (200) @(posedge clk);
        reset = 0;
        wait (init_complete);
        repeat (100) @(posedge clk);
        draw(19'd106);                      // cell 1 at x=106
        repeat (150000) @(posedge clk);
        draw(19'd135);                      // cell 2 at x=135 (advance 29)
        repeat (150000) @(posedge clk);
        r13 = 13*512;
        $display("");
        $display("d 32px -- row 17 hits the STOP, row 18 is the row AFTER it");
        $display("row 18 expected: 12,15,15,15,7 at cols 1..5 then 5,15,15,15,15,2 at 12..17");
        $display("  x    word   what");
        for (i = 102; i <= 128; i = i + 1)
            $display("  row17 x=%0d  %h  %0s", i, fb[17*512+i],
                     (fb[17*512+i] === 16'hDEAD) ? "UNWRITTEN" :
                     (fb[17*512+i] === BG)       ? "background" : "INK");
        for (i = 102; i <= 128; i = i + 1)
            $display("  row18 x=%0d  %h  %0s", i, fb[18*512+i],
                     (fb[18*512+i] === 16'hDEAD) ? "UNWRITTEN" :
                     (fb[18*512+i] === BG)       ? "background" : "INK");
        $display("resume events: %0d", nresume);
        $finish;
    end
endmodule
