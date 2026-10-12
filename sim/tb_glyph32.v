// Does the engine compose a FULL 32px glyph row, or lose its right-hand end?
//
// Hardware shows the last two ink columns of every native-cell glyph missing.
// Pixel forensics on a screenshot could not separate "never composed" from
// "composed then overwritten", so this drives one CHAR command with a real
// glyph and captures what the engine actually streams out.
//
// The testbench IS the SDRAM controller: it answers reads from the glyph
// image and captures the streaming write by driving wsrc_addr and sampling
// wsrc_q, exactly as sdram_fb does.
`timescale 1ns/1ps
`default_nettype none

module tb_glyph32;
    localparam [24:0] FONT_BASE  = 25'h0100000;
    localparam [24:0] FONT32_OFF = 25'd554864;
    localparam        GIDX       = 7'h6D - 7'h20;      // 'm'
    localparam [24:0] GBASE      = FONT_BASE + FONT32_OFF + GIDX * 256;

    reg clk_sdram = 0, clk_sys = 0, clk_vid = 0, reset = 1;
    always #5    clk_sdram = ~clk_sdram;   // 100 MHz
    always #7.5  clk_sys   = ~clk_sys;     // ~66 MHz
    always #41.6 clk_vid   = ~clk_vid;     // ~12 MHz

    reg  [15:0] gmem [0:255];
    initial $readmemh("sim/glyph_m.hex", gmem);

    reg         cmd_push = 0;
    reg  [1:0]  cmd_op = 0;
    reg  [18:0] cmd_addr = 0;
    reg  [8:0]  cmd_w = 0, cmd_h = 0;
    reg  [15:0] cmd_fg = 16'hFFFF, cmd_bg = 16'h0000;
    reg  [6:0]  cmd_glyph = 0;
    reg  [1:0]  cmd_sx = 0, cmd_sy = 0;
    wire        cmd_full;

    wire [24:0] p0_addr;
    wire [15:0] p0_data;
    wire [1:0]  p0_byte_en;
    wire [10:0] p0_wr_len;
    wire        p0_wr_stream, p0_wr_req, p0_rd_req, p0_end_burst_req;
    reg  [15:0] p0_q = 0;
    reg         p0_data_available = 0;
    wire        p0_available = !busy;
    reg  [10:0] wsrc_addr = 0;
    wire [15:0] wsrc_q;

    mp3_fb dut (
        .reset(reset), .clk_sys(clk_sys), .clk_sdram(clk_sdram), .clk_vid(clk_vid),
        .cmd_push(cmd_push), .cmd_op(cmd_op), .cmd_addr(cmd_addr),
        .cmd_w(cmd_w), .cmd_h(cmd_h), .cmd_fg(cmd_fg), .cmd_bg(cmd_bg),
        .cmd_glyph(cmd_glyph), .cmd_sx(cmd_sx), .cmd_sy(cmd_sy), .cmd_full(cmd_full),
        .fontw_en(1'b0), .fontw_addr(22'd0), .fontw_data(16'd0),
        .sdram_init_complete(1'b1),
        .p0_addr(p0_addr), .p0_data(p0_data), .p0_byte_en(p0_byte_en),
        .p0_wr_len(p0_wr_len), .p0_wr_stream(p0_wr_stream), .p0_q(p0_q),
        .p0_wr_req(p0_wr_req), .p0_rd_req(p0_rd_req),
        .p0_end_burst_req(p0_end_burst_req), .p0_available(p0_available),
        .p0_ready(1'b1), .p0_data_available(p0_data_available),
        .wsrc_addr(wsrc_addr), .wsrc_q(wsrc_q),
        .video_rgb(), .video_de(), .video_hs(), .video_vs()
    );

    // ---- SDRAM model -------------------------------------------------------
    reg        busy = 0;
    integer    rdly = 0;
    reg [24:0] raddr = 0;

    // Capture of the streaming write
    reg  [15:0] cap [0:63];
    integer     cap_n = 0, wbeats = 0;
    reg         wrun = 0;
    integer     rows_seen = 0;

    always @(posedge clk_sdram) begin
        p0_data_available <= 0;

        if (p0_rd_req && !busy) begin
            busy  <= 1; raddr <= p0_addr; rdly <= 4;   // tRCD + CAS
        end else if (busy && !wrun) begin
            if (rdly > 0) rdly <= rdly - 1;
            else begin
                p0_q <= (raddr >= GBASE && raddr < GBASE + 256)
                        ? gmem[raddr - GBASE] : 16'h0000;
                p0_data_available <= 1;
                busy <= 0;                              // one word per request
            end
        end

        // Streaming write: drive wsrc_addr a cycle ahead, sample wsrc_q.
        if (p0_wr_req && p0_wr_stream && !wrun) begin
            wrun <= 1; wbeats <= p0_wr_len; wsrc_addr <= 0; cap_n <= 0; busy <= 1;
        end else if (wrun) begin
            if (wsrc_addr > 0 || cap_n > 0) begin
                cap[cap_n] <= wsrc_q;
                cap_n <= cap_n + 1;
            end
            if (wsrc_addr + 1 < wbeats) wsrc_addr <= wsrc_addr + 1;
            else if (cap_n + 1 >= wbeats) begin
                wrun <= 0; busy <= 0;
                rows_seen = rows_seen + 1;
                if (rows_seen == 14) begin             // row index 13
                    $write("composed row 13 (32 words, bright=ink):\n  ");
                    for (cap_n = 0; cap_n < 32; cap_n = cap_n + 1)
                        $write("%s", (cap[cap_n] > 16'h8410) ? "#" :
                                     (cap[cap_n] != 16'h0000) ? "+" : ".");
                    $write("\n  expected:\n  ");
                    $write("..####+.....####+.....####+.....\n");
                    $finish;
                end
                cap_n <= 0;
            end
        end
    end

    initial begin
        repeat (20) @(posedge clk_sdram);
        reset = 0;
        repeat (20) @(posedge clk_sys);
        @(posedge clk_sys);
        cmd_op    <= 2'd2;              // CHAR
        cmd_glyph <= 7'h7F;             // extended: index rides in {w,h}
        // index = FEXT_32PX | GIDX = 0x8000 | 77 ; w = g[17:9], h = g[8:0]
        cmd_w     <= (({9'd0, 9'd0} | ((25'h8000 | GIDX) >> 9)) & 9'h1FF);
        cmd_h     <= ((25'h8000 | GIDX) & 9'h1FF);
        cmd_addr  <= 19'd0;
        cmd_sx    <= 2'd0; cmd_sy <= 2'd0;
        cmd_push  <= 1;
        @(posedge clk_sys);
        cmd_push  <= 0;
        repeat (200000) @(posedge clk_sdram);
        $display("TIMEOUT: only %0d rows streamed", rows_seen);
        $finish;
    end
endmodule
