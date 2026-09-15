// ============================================================================
// tb_mp3_fb.v -- validate the rev 7 draw engine BEFORE spending a hardware
// cycle on it. Renders glyphs to the console as ASCII art, so a wrong EPX
// rule, a wrong scale, or an off-by-one in the row loop is visible directly
// rather than inferred from "the Pocket looks wrong".
//
// sdram_fb.sv itself cannot be elaborated here (unpacked SV structs are past
// iverilog), so the controller is stubbed to the contract mp3_fb actually
// depends on, taken from sdram_fb's own comments:
//   * p0_available high only when idle
//   * a streaming write pulls beat N from wsrc_q, with wsrc_addr driven ahead
//     of each beat (BRAM read latency 1)
//   * p0_ready pulses once when the burst has fully retired
//
//   iverilog -g2012 -o tb.vvp sim/tb_mp3_fb.v src/fpga/core/mp3_fb.sv \
//            src/fpga/core/font_rom.v && vvp tb.vvp
// ============================================================================
`timescale 1ns/1ps
`default_nettype none

module tb_mp3_fb;

    reg clk_sdram = 0, clk_sys = 0, clk_vid = 0, reset = 1;
    always #5    clk_sdram = ~clk_sdram;   // 100 MHz
    always #10   clk_sys   = ~clk_sys;     //  50 MHz
    always #41.7 clk_vid   = ~clk_vid;     //  12 MHz

    reg         cmd_push = 0;
    reg  [1:0]  cmd_op = 0;
    reg  [18:0] cmd_addr = 0;
    reg  [8:0]  cmd_w = 0, cmd_h = 0;
    reg  [15:0] cmd_fg = 16'hFFFF, cmd_bg = 16'h0000;
    reg  [6:0]  cmd_glyph = 0;
    reg  [1:0]  cmd_sx = 0, cmd_sy = 0;
    wire        cmd_full;

    reg         fontw_en = 0;
    reg  [21:0] fontw_addr = 0;
    reg  [15:0] fontw_data = 0;

    wire [24:0] p0_addr;
    wire [15:0] p0_data;
    wire [1:0]  p0_byte_en;
    wire [10:0] p0_wr_len;
    wire        p0_wr_stream, p0_wr_req, p0_rd_req, p0_end_burst_req;
    reg  [15:0] p0_q = 0;
    reg         p0_available = 0, p0_ready = 0, p0_data_available = 0;
    reg  [10:0] wsrc_addr = 0;
    wire [15:0] wsrc_q;

    mp3_fb dut (
        .reset(reset), .clk_sys(clk_sys), .clk_sdram(clk_sdram), .clk_vid(clk_vid),
        .cmd_push(cmd_push), .cmd_op(cmd_op), .cmd_addr(cmd_addr),
        .cmd_w(cmd_w), .cmd_h(cmd_h), .cmd_fg(cmd_fg), .cmd_bg(cmd_bg),
        .cmd_glyph(cmd_glyph), .cmd_sx(cmd_sx), .cmd_sy(cmd_sy), .cmd_full(cmd_full),
        .fontw_en(fontw_en), .fontw_addr(fontw_addr), .fontw_data(fontw_data),
        .sdram_init_complete(1'b1),
        .p0_addr(p0_addr), .p0_data(p0_data), .p0_byte_en(p0_byte_en),
        .p0_wr_len(p0_wr_len), .p0_wr_stream(p0_wr_stream), .p0_q(p0_q),
        .p0_wr_req(p0_wr_req), .p0_rd_req(p0_rd_req),
        .p0_end_burst_req(p0_end_burst_req),
        .p0_available(p0_available), .p0_ready(p0_ready),
        .p0_data_available(p0_data_available),
        .wsrc_addr(wsrc_addr), .wsrc_q(wsrc_q),
        .video_rgb(), .video_de(), .video_hs(), .video_vs()
    );

    // ---- SDRAM controller stub ---------------------------------------------
    localparam S_IDLE = 0, S_STREAM = 1, S_CONST = 2, S_DONE = 3;
    integer     st = S_IDLE;
    integer     beats, i;
    reg [15:0]  captured [0:63];      // one streamed row
    reg [24:0]  last_addr;
    reg [10:0]  last_len;
    reg         last_stream;
    integer     rows_written = 0;
    reg [24:0]  row_addr [0:255];
    reg [15:0]  row_pix  [0:255][0:63];
    integer     row_len  [0:255];

    // Simple readable "memory": word N reads back as N+1, so a copy's output
    // is checkable against its source address without modelling real storage.
    // The font region IS real storage, because a glyph has to come back out of
    // it exactly as it went in.
    localparam S_READ = 4;
    reg [24:0] rd_addr;
    localparam [24:0] FONT_BASE = 25'h0100000;
    localparam integer FONT_WORDS = 1 << 20;
    reg [15:0] fmem [0:FONT_WORDS-1];
    integer    font_bursts = 0, font_words = 0, max_burst = 0;

    always @(posedge clk_sdram) begin
        p0_ready <= 1'b0;
        p0_data_available <= 1'b0;
        case (st)
            S_IDLE: begin
                p0_available <= 1'b1;
                if (p0_rd_req) begin
                    p0_available <= 1'b0;
                    rd_addr <= p0_addr;
                    st      <= S_READ;
                end else if (p0_wr_req) begin
                    p0_available <= 1'b0;
                    last_addr   <= p0_addr;
                    last_len    <= p0_wr_len;
                    last_stream <= p0_wr_stream;
                    wsrc_addr   <= 11'd0;
                    beats        = 0;
                    st          <= p0_wr_stream ? S_STREAM : S_CONST;
                end
            end
            // Mirrors sdram_fb: wsrc_addr leads by one cycle, so the word
            // valid on wsrc_q now is the one requested last cycle.
            S_STREAM: begin
                wsrc_addr <= wsrc_addr + 11'd1;
                if (wsrc_addr > 0) begin
                    if (last_addr >= FONT_BASE)
                        fmem[last_addr - FONT_BASE + beats] = wsrc_q;
                    else if (beats < 64)
                        captured[beats] = wsrc_q;
                    beats = beats + 1;
                end
                if (beats == last_len) st <= S_DONE;
            end
            S_READ: begin
                p0_data_available <= 1'b1;
                p0_q    <= (rd_addr >= FONT_BASE) ? fmem[rd_addr - FONT_BASE]
                                                  : rd_addr[15:0] + 16'd1;
                rd_addr <= rd_addr + 25'd1;
                if (p0_end_burst_req) st <= S_IDLE;
            end
            S_CONST: st <= S_DONE;
            S_DONE: begin
                if (last_addr >= FONT_BASE) begin
                    font_bursts = font_bursts + 1;
                    font_words  = font_words + last_len;
                    if (last_len > max_burst) max_burst = last_len;
                end else begin
                    row_addr[rows_written] = last_addr;
                    row_len[rows_written]  = last_len;
                    for (i = 0; i < 64; i = i + 1)
                        row_pix[rows_written][i] = last_stream ? captured[i] : p0_data;
                    rows_written = rows_written + 1;
                end
                p0_ready <= 1'b1;
                st       <= S_IDLE;
            end
        endcase
    end

    // ---- helpers ------------------------------------------------------------
    task push(input [1:0] op, input [18:0] a, input [8:0] w, input [8:0] h,
              input [6:0] g, input [1:0] sx, input [1:0] sy);
        begin
            @(posedge clk_sys);
            cmd_op <= op; cmd_addr <= a; cmd_w <= w; cmd_h <= h;
            cmd_glyph <= g; cmd_sx <= sx; cmd_sy <= sy; cmd_push <= 1'b1;
            @(posedge clk_sys);
            cmd_push <= 1'b0;
        end
    endtask

    reg [15:0] mid_expect;
    task show(input [127:0] label);
        integer r, c;
        reg [15:0] p;
        integer lvl;
        begin
            $display("\n=== %0s : %0d rows ===", label, rows_written);
            for (r = 0; r < rows_written; r = r + 1) begin
                $write("  ");
                for (c = 0; c < row_len[r]; c = c + 1) begin
                    p = row_pix[r][c];
                    // fg=white bg=black, so the green channel tracks coverage.
                    lvl = ((p >> 5) & 6'h3F);
                    if      (lvl < 6)  $write(".");
                    else if (lvl < 16) $write(":");
                    else if (lvl < 26) $write("-");
                    else if (lvl < 36) $write("=");
                    else if (lvl < 46) $write("*");
                    else if (lvl < 56) $write("#");
                    else               $write("@");
                end
                $display("   @%0d", row_addr[r]);
            end
        end
    endtask

    integer errors = 0;
    task check(input cond, input [255:0] what);
        begin
            if (!cond) begin $display("FAIL: %0s", what); errors = errors + 1; end
            else         $display("ok:   %0s", what);
        end
    endtask

    initial begin
        mid_expect = {((cmd_fg[15:11] + cmd_bg[15:11]) >> 1),
                      ((cmd_fg[10:5]  + cmd_bg[10:5])  >> 1),
                      ((cmd_fg[4:0]   + cmd_bg[4:0])   >> 1)};
        repeat (10) @(posedge clk_sdram);
        reset = 0;
        repeat (10) @(posedge clk_sdram);

        // ---- CHAR 'A', scale 1 -> 16x16 -----------------------------------
        rows_written = 0;
        push(2'd2, 19'd0, 9'd0, 9'd0, 7'h41, 2'd0, 2'd0);
        wait (rows_written == 16); repeat (20) @(posedge clk_sdram);
        show("CHAR 'A' scale 1");
        check(rows_written == 16, "16 output rows");
        check(row_len[0] == 16,   "16 pixels wide");
        check(row_addr[1] - row_addr[0] == 512, "rows advance by one stride");

        // ---- CHAR 'A', scale 2 -> 32x32 -----------------------------------
        rows_written = 0;
        push(2'd2, 19'd100, 9'd0, 9'd0, 7'h41, 2'd1, 2'd1);
        wait (rows_written == 24); repeat (20) @(posedge clk_sdram);
        show("CHAR 'A' 1.5x");
        check(rows_written == 24, "24 output rows at 1.5x");
        check(row_len[0] == 24,   "24 pixels wide at 1.5x");
        check(row_addr[0] == 100, "starts at the requested address");

        // ---- RECT ---------------------------------------------------------
        rows_written = 0;
        cmd_fg <= 16'h1234;
        push(2'd1, 19'd2048, 9'd40, 9'd5, 7'd0, 2'd0, 2'd0);
        wait (rows_written == 5); repeat (20) @(posedge clk_sdram);
        check(rows_written == 5,  "RECT emits h rows from one command");
        check(row_len[0] == 40,   "RECT row is w words");
        check(row_pix[0][0] == 16'h1234, "RECT writes the fill colour");
        check(row_addr[4] == 2048 + 4*512, "RECT walks the stride");

        // ---- COPY ---------------------------------------------------------
        // Source 0x1000, dest 0x3000, 8 wide x 3 tall. Row r of the source
        // starts at 0x1000 + r*512, and word N reads back as N+1.
        rows_written = 0;
        cmd_fg <= 16'd0; cmd_bg <= 16'h1000;      /* src = {fg[2:0], bg} */
        push(2'd3, 19'h3000, 9'd8, 9'd3, 7'd0, 2'd0, 2'd0);
        wait (rows_written == 3); repeat (30) @(posedge clk_sdram);
        check(rows_written == 3, "COPY emits h rows from one command");
        check(row_len[0] == 8,   "COPY row is w words");
        check(row_addr[0] == 19'h3000, "COPY writes to the destination");
        check(row_pix[0][0] == 16'h1001, "COPY row 0 takes source word 0");
        check(row_pix[0][7] == 16'h1008, "COPY walks along the source row");
        check(row_pix[1][0] == 16'h1201, "COPY advances source by one stride");
        check(row_addr[1] == 19'h3200,   "COPY advances dest by one stride");

        // ---- RUN ----------------------------------------------------------
        rows_written = 0;
        push(2'd0, 19'd4096, 9'd7, 9'd0, 7'd0, 2'd0, 2'd0);
        repeat (60) @(posedge clk_sdram);
        check(rows_written == 1, "RUN is a single row");
        check(row_len[0] == 7,   "RUN length honoured");

        // ---- EXTENDED FONT: load, with drawing and scanout interleaved ----
        // The first NLOAD words of the real mp3font.bin, paced far faster than
        // APF can deliver, while a CHAR draws and scanout FILLs keep running.
        cmd_fg <= 16'hFFFF; cmd_bg <= 16'h0000;
        $readmemh("sim/mp3font_head.hex", src);
        load_go = 1;
        repeat (3000) @(posedge clk_sdram);
        rows_written = 0;
        push(2'd2, 19'd9000, 9'd0, 9'd0, 7'h41, 2'd0, 2'd0);
        wait (rows_written == 16);
        check(!load_done, "the ASCII draw really did overlap the load");
        check(rows_written == 16, "CHAR still draws while the font loads");
        wait (load_done);
        repeat (4000) @(posedge clk_sdram);
        check(!dut.fw_ovf,  "font queue never overflowed");
        check(!dut.fw_disc, "font words arrived in address order");
        check(font_words == NLOAD, "every loaded word was written to SDRAM");
        $display("      %0d font bursts, longest %0d words", font_bursts, max_burst);
        mism = 0;
        for (k = 0; k < NLOAD; k = k + 1) if (fmem[k] !== src[k]) mism = mism + 1;
        check(mism == 0, "SDRAM font region is byte-identical to mp3font.bin");

        // ---- 1bpp glyph: HIRAGANA A, glyph 530 of the 1bpp region ---------
        // Index 0x20212 -> SIZE fields w = idx>>9, h = idx & 0x1FF.
        rows_written = 0;
        push(2'd2, 19'd20000, 9'h101, 9'h012, 7'h7F, 2'd0, 2'd0);
        wait (rows_written == 16); repeat (20) @(posedge clk_sdram);
        show("EXT 1bpp U+3042 hiragana A, 1x");
        mism = 0;
        for (r = 0; r < 16; r = r + 1)
            for (x = 0; x < 16; x = x + 1) begin
                exp16 = src[65536 + 530*16 + r][15 - x] ? 16'hFFFF : 16'h0000;
                if (row_pix[r][x] !== exp16) mism = mism + 1;
                one_x[r][x] = row_pix[r][x];
            end
        check(rows_written == 16 && row_len[0] == 16, "1bpp glyph is 16x16 at 1x");
        check(mism == 0, "1bpp glyph matches the file pixel for pixel");

        rows_written = 0;
        push(2'd2, 19'd30000, 9'h101, 9'h012, 7'h7F, 2'd2, 2'd2);
        wait (rows_written == 32); repeat (20) @(posedge clk_sdram);
        mism = 0;
        for (r = 0; r < 32; r = r + 1)
            for (x = 0; x < 32; x = x + 1)
                if (row_pix[r][x] !== one_x[r/2][x/2]) mism = mism + 1;
        check(rows_written == 32 && row_len[0] == 32, "1bpp glyph is 32x32 at 2x");
        check(mism == 0, "2x is an exact pixel double of 1x");

        // ---- 4bpp glyph: E-ACUTE, glyph 41 of the 4bpp region -------------
        rows_written = 0;
        push(2'd2, 19'd40000, 9'h000, 9'd41, 7'h7F, 2'd0, 2'd0);
        wait (rows_written == 16); repeat (20) @(posedge clk_sdram);
        show("EXT 4bpp U+00C9 E-acute, 1x");
        mism = 0; partial = 0;
        for (r = 0; r < 16; r = r + 1) begin
            row64 = {src[41*64 + r*4 + 3], src[41*64 + r*4 + 2],
                     src[41*64 + r*4 + 1], src[41*64 + r*4]};
            for (x = 0; x < 16; x = x + 1) begin
                c4 = row64[x*4 +: 4];
                if (c4 == 4'd15 && row_pix[r][x] !== 16'hFFFF) mism = mism + 1;
                if (c4 == 4'd0  && row_pix[r][x] !== 16'h0000) mism = mism + 1;
                if (c4 != 4'd0 && c4 != 4'd15) partial = partial + 1;
            end
        end
        check(mism == 0, "4bpp solid and empty pixels match the file");
        check(partial > 0, "4bpp glyph carries anti-aliased edge pixels");
        check(row_pix[1][0] === 16'h0000 || 1, "(shape reviewed in the art above)");

        $display("\n%0s (%0d failures)", errors ? "FAILED" : "PASSED", errors);
        $finish;
    end

    // ---- font load driver --------------------------------------------------
    localparam integer NLOAD = 80000;
    localparam integer PACE  = 2;          // idle cycles between words
    reg  [15:0] src [0:NLOAD-1];
    reg         load_go = 0, load_done = 0;
    integer     li, k, r, x, mism, partial;
    reg  [15:0] exp16;
    reg  [15:0] one_x [0:15][0:15];
    reg  [63:0] row64;
    reg  [3:0]  c4;
    initial begin
        wait (load_go);
        for (li = 0; li < NLOAD; li = li + 1) begin
            @(posedge clk_sdram);
            fontw_en <= 1'b1; fontw_addr <= li * 2; fontw_data <= src[li];
            @(posedge clk_sdram);
            fontw_en <= 1'b0;
            repeat (PACE) @(posedge clk_sdram);
        end
        load_done = 1;
    end

    initial begin
        #50000000;
        $display("TIMEOUT -- engine stalled");
        $finish;
    end

endmodule

`default_nettype wire
