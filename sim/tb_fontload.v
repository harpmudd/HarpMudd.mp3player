// Does the font survive its own load into SDRAM?
//
// Chasing a glyph that renders wrong for a whole session and comes good
// after a core restart -- which is exactly the shape of font data corrupted
// in SDRAM, since a restart reloads it.
//
// sdram_fb's WRITE_STREAM has no page-edge check (unlike READ_OUTPUT, which
// precharges three columns early). A write burst that runs past a page edge
// therefore wraps the column counter back to the START of the same page
// instead of advancing into the next row. The font loader writes in bursts of
// whatever has queued, up to 128 words -- lengths that do not divide the page
// evenly -- so its bursts need not be page aligned.
//
// Feeds a known pattern (word N = N) through the font path and reads back
// what actually landed.
`timescale 1ns/1ps
`default_nettype none

module tb_fontload;
    localparam [24:0] FONT_BASE = 25'h0100000;
    localparam        NWORDS    = 4096;      // four pages: enough to cross three edges

    reg clk = 0;     always #5    clk = ~clk;
    reg clk_sys = 0; always #7.5  clk_sys = ~clk_sys;
    reg clk_vid = 0; always #41.6 clk_vid = ~clk_vid;
    reg reset = 1;

    wire [24:0] p0_addr; wire [15:0] p0_data; wire [1:0] p0_byte_en;
    wire [10:0] p0_wr_len; wire p0_wr_stream, p0_wr_req, p0_rd_req, p0_end_burst_req;
    wire [15:0] p0_q; wire p0_available, p0_ready, p0_data_available;
    wire [10:0] wsrc_addr; wire [15:0] wsrc_q; wire init_complete;

    reg         fontw_en = 0;
    reg  [21:0] fontw_addr = 0;
    reg  [15:0] fontw_data = 0;

    mp3_fb dut (
        .reset(reset), .clk_sys(clk_sys), .clk_sdram(clk), .clk_vid(clk_vid),
        .cmd_push(1'b0), .cmd_op(2'd0), .cmd_addr(19'd0),
        .cmd_w(9'd0), .cmd_h(9'd0), .cmd_fg(16'hFFFF), .cmd_bg(16'h0842),
        .cmd_glyph(7'd0), .cmd_sx(2'd0), .cmd_sy(2'd0), .cmd_full(),
        .fontw_en(fontw_en), .fontw_addr(fontw_addr), .fontw_data(fontw_data),
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

    sdram_fb #(.CLOCK_SPEED_MHZ(100), .BURST_TYPE(0), .CAS_LATENCY(2),
               .WRITE_BURST(1), .FAULT_INJECT(0))
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

    // ---- SDRAM model: record writes by (bank,row,column), as the chip does.
    reg [12:0] act_row [0:3];
    reg [15:0] mem [0:NWORDS-1];
    reg        wact = 0; reg [1:0] wbk = 0; reg [9:0] wcol = 0;
    integer i;
    initial begin
        for (i = 0; i < NWORDS; i = i + 1) mem[i] = 16'hDEAD;
        for (i = 0; i < 4; i = i + 1) act_row[i] = 0;
    end

    wire [2:0] cmd = {nRAS, nCAS, nWE};
    // Only the font window is watched; everything else is ignored.
    function integer idx(input [1:0] b, input [12:0] r, input [9:0] c);
        reg [24:0] a;
        begin
            a = {b, r, c};
            idx = (a >= FONT_BASE && a < FONT_BASE + NWORDS) ? (a - FONT_BASE) : -1;
        end
    endfunction

    always @(posedge clk) if (!reset) begin
        if (cmd == 3'b011) act_row[BA] <= A;
        if (cmd == 3'b010) wact <= 0;
        if (cmd == 3'b100) begin
            wact <= 1; wbk <= BA;
            if (DQM != 2'b11 && idx(BA, act_row[BA], A[9:0]) >= 0)
                mem[idx(BA, act_row[BA], A[9:0])] <= DQ;
            wcol <= A[9:0] + 10'd1;          // the column counter WRAPS at 1024
        end else if (wact && cmd == 3'b111) begin
            if (DQM != 2'b11 && idx(wbk, act_row[wbk], wcol) >= 0)
                mem[idx(wbk, act_row[wbk], wcol)] <= DQ;
            wcol <= wcol + 10'd1;
        end
    end

    // ---- feed the font loader, word N = N -----------------------------------
    integer sent;
    initial begin
        repeat (300) @(posedge clk);
        reset = 0;
        wait (init_complete);
        repeat (50) @(posedge clk);

        for (sent = 0; sent < NWORDS; sent = sent + 1) begin
            @(posedge clk);
            fontw_en   <= 1'b1;
            fontw_addr <= sent[21:0] << 1;    // byte offset, as data_loader gives
            fontw_data <= sent[15:0];
            @(posedge clk);
            fontw_en   <= 1'b0;
            // let the engine drain; the dispatcher batches at 64 words
            repeat (3) @(posedge clk);
        end

        repeat (20000) @(posedge clk);        // flush the tail

        begin : check
            integer bad, unwritten, first_bad;
            bad = 0; unwritten = 0; first_bad = -1;
            for (i = 0; i < NWORDS; i = i + 1) begin
                if (mem[i] === 16'hDEAD) unwritten = unwritten + 1;
                else if (mem[i] !== i[15:0]) begin
                    bad = bad + 1;
                    if (first_bad < 0) first_bad = i;
                end
            end
            $display("font load: %0d words fed", NWORDS);
            $display("  wrong    : %0d", bad);
            $display("  unwritten: %0d", unwritten);
            if (first_bad >= 0)
                $display("  first wrong at word %0d (page column %0d): got %h want %h",
                         first_bad, first_bad % 1024, mem[first_bad], first_bad[15:0]);
            $display(bad == 0 ? "FONT LOAD CLEAN" : "FONT LOAD CORRUPTS");
        end
        $finish;
    end
endmodule
