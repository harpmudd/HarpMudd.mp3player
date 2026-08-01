// ============================================================================
// mp3_fb.sv -- SDRAM-backed framebuffer + 2D draw engine for the player UI.
//
// Scanout half adapted from HarpMudd.starwars' pocket_vector_fb.sv (hardware-
// validated via HarpMudd.sdramfbtest): parity-split double-buffered line
// buffer, fill-request CDC, and the HOFF/VOFF/guard-band lessons that were
// paid for on real hardware there. Everything vector-specific (AVG raster,
// phosphor copy-fade) is gone; this is a plain single-buffered RGB565
// framebuffer that a 2D engine draws into on the CPU's behalf.
//
// Geometry: 400x360 @ 12 MHz, 500x400 total = exactly 60.000 Hz. Chosen as an
// exact 4x integer scale of the Pocket's native 1600x1440 panel (confirmed via
// Analogue's own spec: the panel is BUILT around integer scaling), so nothing
// is filtered/blurred by the scaler.
//
// Memory: 1 pixel = 1 SDRAM word (RGB565), stride 512 words/line even though
// only 400 are used -- keeps every row inside a single SDRAM page. Total:
// 512*360*2 bytes =~360 KB of 32 MB SDRAM.
//
// ---------------------------------------------------------------------------
// REV 7: the CPU no longer draws row-by-row.
//
// The original engine had exactly one primitive: RUN (fill `len` words of one
// colour). Everything 2D was therefore N commands -- a rect cost one command
// per row, and TEXT cost one command per run of set bits per scaled row, so a
// five-character clock readout at 2x was ~180 MMIO round-trips. That is CPU
// work landing directly in the same per-frame budget that keeps the PCM FIFO
// fed, and a clean A/B on hardware (drawing fully disabled vs. enabled)
// confirmed it is what makes the audio jitter.
//
// So the engine now owns the loops instead of the CPU:
//   RUN   len words of one colour                        (1 command)
//   RECT  w x h block of one colour                      (1 command, was h)
//   CHAR  one anti-aliased scaled glyph, fg on bg        (1 command, was ~30)
// A 20-character title is now 20 pushes, not ~600, and each push is 2 MMIO
// writes because colour/size registers persist between glyphs.
//
// CHAR also fixes the OTHER complaint about the old UI -- jagged text. The
// 8x8 source font is upscaled by Scale2x/EPX (crisp diagonals, unlike bilinear
// which just blurs a bitmap font), and wherever EPX decides a corner should be
// broken the pixel is written as the 50% blend of fg and bg instead of a hard
// flip. That is genuine anti-aliasing for one adder per colour channel: the
// only coverages EPX can produce here are 0, 1/2 and 1.
//
//   clk_sys (CPU) --async FIFO--> clk_sdram ENGINE+ARBITER
//     FILL  : burst-read display line -> scanout line buffer (time-critical:
//             must finish inside one scanline, ~4167 clk_sdram cycles @100MHz)
//     RECT  : one constant-data burst write per row, h rows
//     CHAR  : compose one output row into glyphbuf, then one STREAMING burst
//             write (varying data via sdram_fb's wsrc port), 16*sy rows
//   clk_vid: read line buffer -> video_rgb / de / hs / vs
//
// Scanout FILL always wins arbitration. Everything else is decomposed into
// single-burst units that return to the dispatcher between rows, so the worst
// case a fill ever waits is one burst (<= ~500 cycles) against ~4167 of slack.
// ============================================================================

`default_nettype none

module mp3_fb (
    input  wire        reset,
    input  wire        clk_sys,     // CPU / FIFO write domain
    input  wire        clk_sdram,   // SDRAM controller + engine (~100 MHz)
    input  wire        clk_vid,     // pixel clock (12 MHz)

    // CPU draw command (clk_sys domain) -------------------------------------
    input  wire        cmd_push,
    input  wire [1:0]  cmd_op,      // 0=RUN 1=RECT 2=CHAR
    input  wire [18:0] cmd_addr,    // word address of top-left, y*512+x
    input  wire [8:0]  cmd_w,       // RUN: run length; RECT: width (words)
    input  wire [8:0]  cmd_h,       // RECT: height (rows)
    input  wire [15:0] cmd_fg,      // RGB565 fill / glyph foreground
    input  wire [15:0] cmd_bg,      // RGB565 glyph background
    input  wire [6:0]  cmd_glyph,   // CHAR: ASCII code
    input  wire [1:0]  cmd_sx,      // CHAR: h scale 0=1x 1=1.5x 2=2x 3=3x
    input  wire [1:0]  cmd_sy,      // CHAR: v scale, same encoding
    output wire        cmd_full,

    // SDRAM master port (clk_sdram) -> wired to sdram_fb in core_game.vh ----
    input  wire        sdram_init_complete,
    output reg  [24:0] p0_addr,
    output reg  [15:0] p0_data,
    output reg  [1:0]  p0_byte_en,
    output reg  [10:0] p0_wr_len,
    output reg         p0_wr_stream,
    input  wire [15:0] p0_q,
    output reg         p0_wr_req,
    output reg         p0_rd_req,
    output reg         p0_end_burst_req,
    input  wire        p0_available,
    input  wire        p0_ready,
    input  wire        p0_data_available,

    // Streaming-write source port: sdram_fb pulls one word per burst beat from
    // here when p0_wr_stream was set on the request. Used by CHAR, whose rows
    // are varying-colour (a constant-data burst cannot express a glyph).
    input  wire [10:0] wsrc_addr,
    output wire [15:0] wsrc_q,

    // Video output (clk_vid domain) ------------------------------------------
    output reg  [23:0] video_rgb,
    output reg         video_de,
    output reg         video_hs,
    output reg         video_vs
);

    // ---- Geometry ----------------------------------------------------------
    localparam H_ACT = 11'd400, V_ACT = 11'd360;
    localparam H_TOT = 11'd500, V_TOT = 11'd400;
    localparam [10:0] HOFF = 11'd8,  VOFF = 11'd4;
    localparam [10:0] HS_ST = HOFF + H_ACT + 11'd12, HS_EN = HS_ST + 11'd40;
    localparam [10:0] VS_ST = VOFF + V_ACT + 11'd3,  VS_EN = VS_ST + 11'd4;
    localparam [9:0]  STRIDE = 10'd512;             // words/line, page-aligned
    localparam [24:0] FB_BASE = 25'd0;

    localparam [1:0] OP_RUN = 2'd0, OP_RECT = 2'd1, OP_CHAR = 2'd2, OP_COPY = 2'd3;
    // COPY moves a w x h block SDRAM->SDRAM. It exists for the album-art panel:
    // sliding an image by re-sending its pixels from the CPU would be thousands
    // of commands per animation step and would starve the decoder, whereas the
    // engine can read a row and write it back with the CPU issuing ONE command
    // for the whole block. Source address rides in the fg/bg fields, which a
    // copy has no other use for.

    // scale sel -> (num, den) and the resulting painted extent of a 16px cell
    function [5:0] scale_nd(input [1:0] sel);
        case (sel)
            2'd0:    scale_nd = {3'd1, 3'd1};   // 1x
            2'd1:    scale_nd = {3'd2, 3'd3};   // 1.5x
            2'd2:    scale_nd = {3'd1, 3'd2};   // 2x
            default: scale_nd = {3'd1, 3'd3};   // 3x
        endcase
    endfunction
    wire [5:0] nd_x = scale_nd(q_sx);
    wire [5:0] nd_y = scale_nd(q_sy);
    wire [8:0] ext_x = scale_ext(q_sx);
    wire [8:0] ext_y = scale_ext(q_sy);
    function [8:0] scale_ext(input [1:0] sel);
        case (sel)
            2'd0: scale_ext = 9'd16;
            2'd1: scale_ext = 9'd24;
            2'd2: scale_ext = 9'd32;
            default: scale_ext = 9'd48;
        endcase
    endfunction

    // Top black guard-band: pocket_vector_fb.sv's hard-won finding -- the
    // scaler overshoots into a bright artifact at a black->content luminance
    // step on the FIRST active line specifically (not fixable via video.json).
    // Force the first GUARD_TOP active lines to black so that step happens a
    // few lines in, away from the DE edge, where the scaler handles it cleanly.
    localparam [10:0] GUARD_TOP = 11'd3;

    // ======================================================================
    // Async FIFO (clk_sys write -> clk_sdram read), Gray-coded pointers.
    // Same CDC idiom as pocket_vector_fb.sv's vector FIFO -- proven on
    // hardware there.
    //
    // 256 deep, was 512: a command is now a whole rect or a whole glyph
    // rather than a single row, so the queue holds vastly more DRAWING per
    // entry. A full-screen clear went from 360 entries to 1. Wider entries
    // (88b vs 44b) at half the depth costs the same three M10K blocks, which
    // matters -- this core is already at ~90% BRAM.
    // ======================================================================
    localparam FAW = 8;                              // 256 entries
    localparam CW  = 88;                             // command width
    (* ramstyle = "M10K" *) reg [CW-1:0] cmd_mem [0:255];
    reg [FAW:0] wr_ptr = 0, wr_ptr_g = 0;             // clk_sys
    reg [FAW:0] rd_ptr = 0, rd_ptr_g = 0;             // clk_sdram

    function [FAW:0] b2g(input [FAW:0] b); b2g = b ^ (b >> 1); endfunction
    function [FAW:0] g2b(input [FAW:0] g);
        integer i;
        begin
            g2b[FAW] = g[FAW];
            for (i = FAW-1; i >= 0; i = i - 1) g2b[i] = g2b[i+1] ^ g[i];
        end
    endfunction

    reg [FAW:0] rd_ptr_g_s1 = 0, rd_ptr_g_s2 = 0;
    always @(posedge clk_sys) begin
        rd_ptr_g_s1 <= rd_ptr_g; rd_ptr_g_s2 <= rd_ptr_g_s1;
    end
    wire [FAW:0] rd_ptr_bin = g2b(rd_ptr_g_s2);
    wire [FAW:0] fifo_fill  = wr_ptr - rd_ptr_bin;
    assign cmd_full = (fifo_fill >= {1'b0, {FAW{1'b1}}});

    always @(posedge clk_sys) begin
        if (reset) begin
            wr_ptr <= 0; wr_ptr_g <= 0;
        end else if (cmd_push && !cmd_full) begin
            cmd_mem[wr_ptr[FAW-1:0]] <= {cmd_op, cmd_addr, cmd_fg, cmd_bg,
                                         cmd_w, cmd_h, cmd_glyph,
                                         cmd_sx, cmd_sy, 6'd0};
            wr_ptr   <= wr_ptr + 1'b1;
            wr_ptr_g <= b2g(wr_ptr + 1'b1);
        end
    end

    reg [FAW:0] wr_ptr_g_s1 = 0, wr_ptr_g_s2 = 0;
    always @(posedge clk_sdram) begin
        wr_ptr_g_s1 <= wr_ptr_g; wr_ptr_g_s2 <= wr_ptr_g_s1;
    end
    wire fifo_empty = (rd_ptr_g == wr_ptr_g_s2);

    reg [CW-1:0] cmd_q;
    always @(posedge clk_sdram) cmd_q <= cmd_mem[rd_ptr[FAW-1:0]];
    wire [1:0]  q_op    = cmd_q[87:86];
    wire [18:0] q_addr  = cmd_q[85:67];
    wire [15:0] q_fg    = cmd_q[66:51];
    wire [15:0] q_bg    = cmd_q[50:35];
    wire [8:0]  q_w     = cmd_q[34:26];
    wire [8:0]  q_h     = cmd_q[25:17];
    wire [6:0]  q_glyph = cmd_q[16:10];
    wire [1:0]  q_sx    = cmd_q[9:8];
    wire [1:0]  q_sy    = cmd_q[7:6];

    // ======================================================================
    // Scanout line buffer: parity-split double buffer, exactly as
    // pocket_vector_fb.sv -- one half drains to clk_vid while the other half
    // fills from clk_sdram for the NEXT line, so fill and scanout of the same
    // line never race.
    // ======================================================================
    (* ramstyle = "M10K" *) reg [15:0] linebuf [0:1023];
    reg        lb_we;
    reg [9:0]  lb_waddr;
    reg [15:0] lb_wdata;
    always @(posedge clk_sdram) if (lb_we) linebuf[lb_waddr] <= lb_wdata;

    // ======================================================================
    // Glyph row buffer -- one composed output row of a CHAR, handed to
    // sdram_fb's streaming-write port one word per burst beat. 64 entries =
    // the widest glyph (8 source px * 2 EPX * 4x scale). Small enough that
    // Quartus will use MLAB rather than spending an M10K.
    // ======================================================================
    // 128 entries: shared by CHAR (max 48 px wide) and COPY, whose width is the
    // album-art panel rather than a glyph.
    reg [15:0] glyphbuf [0:127];
    reg [15:0] glyph_q;
    always @(posedge clk_sdram) glyph_q <= glyphbuf[wsrc_addr[6:0]];
    assign wsrc_q = glyph_q;

    // ---- Font ROM (generated; see tools/gen_font_rom.py) -------------------
    reg  [11:0] font_addr;
    wire [31:0] font_q;
    font_rom u_font (.clk(clk_sdram), .addr(font_addr), .q(font_q));

    // ======================================================================
    // Engine + arbiter (clk_sdram).
    //
    // A_IDLE is the single dispatch point, and every unit of work returns to
    // it, so scanout FILL -- the only deadline in the design -- can preempt
    // between any two bursts.
    // ======================================================================
    localparam A_IDLE=3'd0, A_FILL=3'd1, A_FILL_END=3'd2, A_WRWAIT=3'd3,
               A_ROWFETCH=3'd4, A_COMPOSE=3'd5, A_COPYRD=3'd6;
    reg [2:0]  astate = A_IDLE;
    reg [10:0] fill_cnt = 0;

    reg       fill_req_tgl = 0;
    reg [9:0] fill_line_req = 0;
    reg [2:0] fill_req_s = 0;
    wire      fill_req_edge = (fill_req_s[2] ^ fill_req_s[1]);
    reg       fill_pending = 0;
    reg [9:0] fill_line = 0;
    reg       fill_lb = 0;

    // RECT/RUN state (a RUN is just a RECT of height 1 -- one code path)
    reg        rect_active = 0;
    reg [18:0] rect_addr;
    reg [8:0]  rect_w, rect_rows;

    // CHAR state
    reg        char_rows_left_nz = 0;   // more rows still to COMPOSE
    reg [18:0] char_addr;
    reg [15:0] char_fg, char_bg;
    reg [1:0]  char_sx, char_sy;
    reg [8:0]  char_rows_left;
    reg        char_row_ready = 0;      // glyphbuf holds a row awaiting write
    reg [6:0]  char_w;                  // 16 * (sx+1)
    // Fractional scaling by Bresenham rather than integer replication: the
    // source position advances num/den per output pixel, so 2/3 gives 1.5x.
    // Integer-only scaling meant the smallest step above 16px was 32px --
    // double, with nothing usable in between for typographic hierarchy.
    reg [3:0]  ey;                      // source row 0..15
    reg [2:0]  acc_y;                   // vertical Bresenham accumulator
    reg [6:0]  ox;                      // output column being composed
    reg [3:0]  ex;                      // source column 0..15
    reg [2:0]  acc_x;                   // horizontal accumulator
    reg [2:0]  char_num, char_den;      // X: src pixels per output pixel
    reg [2:0]  char_numy, char_deny;    // Y: same, latched at command pop
    reg [1:0]  rowf_cnt;                // row-fetch sequencer
    reg [31:0] rowlo, rowhi;            // one source row, 16 px x 4bpp
    reg [11:0] char_base;               // (glyph - 0x20) * 32

    // Which unit of work the in-flight burst belongs to
    reg        wr_is_char;

    // COPY state. Reuses the CHAR row-write path (streaming burst out of
    // glyphbuf); only the way a row gets INTO glyphbuf differs.
    reg        copy_mode;
    reg [18:0] copy_src;
    reg [7:0]  copy_cnt;

    // ---- 4bpp coverage sampling (combinational) --------------------------
    // rowbits holds the CURRENT source row: 16 pixels x 4 bits, fetched as two
    // 32-bit words before the row is composed, so every pixel of the row is
    // available without another ROM access mid-compose.
    wire [63:0] rowbits = {rowhi, rowlo};
    wire [3:0]  cov     = rowbits[{ex, 2'b00} +: 4];

    // The coverage value IS the anti-aliasing -- blend fg->bg by it directly.
    // Nudge 15 to 16 so full coverage is exactly fg and the divide is a shift
    // rather than a division by 15.
    wire [4:0] cov16 = {1'b0, cov} + ((cov == 4'hF) ? 5'd1 : 5'd0);
    wire [4:0] inv16 = 5'd16 - cov16;

    wire [10:0] mix_r = char_fg[15:11] * cov16 + char_bg[15:11] * inv16;
    wire [11:0] mix_g = char_fg[10:5]  * cov16 + char_bg[10:5]  * inv16;
    wire [10:0] mix_b = char_fg[4:0]   * cov16 + char_bg[4:0]   * inv16;

    wire [15:0] px_color = {mix_r[8:4], mix_g[9:4], mix_b[8:4]};

    // Dispatch guards: a new command may only be popped once the previous one
    // has fully retired, and any SDRAM work needs the controller idle.
    wire engine_busy = rect_active || char_rows_left_nz || char_row_ready;
    wire can_sdram   = p0_available && sdram_init_complete;

    always @(posedge clk_sdram) begin
        p0_wr_req        <= 1'b0;
        p0_rd_req        <= 1'b0;
        p0_end_burst_req <= 1'b0;
        p0_wr_stream     <= 1'b0;
        lb_we            <= 1'b0;

        fill_req_s <= {fill_req_s[1:0], fill_req_tgl};
        if (fill_req_edge) begin
            fill_pending <= 1'b1;
            fill_line    <= fill_line_req;
            fill_lb      <= fill_line_req[0];
        end

        if (reset) begin
            astate <= A_IDLE;
            fill_pending <= 1'b0;
            rect_active <= 1'b0;
            char_rows_left_nz <= 1'b0;
            char_row_ready <= 1'b0;
            copy_mode <= 1'b0;
            rd_ptr <= 0; rd_ptr_g <= 0;
        end else begin
            case (astate)
                // ---------------------------------------------------- IDLE --
                A_IDLE: begin
                    if (fill_pending && can_sdram) begin
                        p0_addr   <= FB_BASE + {6'd0, fill_line, 9'd0};   // *512
                        p0_rd_req <= 1'b1;
                        fill_cnt  <= 0;
                        astate    <= A_FILL;

                    // A composed glyph row is written with a STREAMING burst:
                    // each beat's data comes from glyphbuf via wsrc_q.
                    end else if (char_row_ready && can_sdram) begin
                        p0_addr      <= FB_BASE + {6'd0, char_addr};
                        p0_byte_en   <= 2'b11;
                        p0_wr_len    <= {4'd0, char_w};
                        p0_wr_stream <= 1'b1;
                        p0_wr_req    <= 1'b1;
                        wr_is_char   <= 1'b1;
                        astate       <= A_WRWAIT;

                    // One rect row = one constant-data burst.
                    end else if (rect_active && can_sdram) begin
                        p0_addr      <= FB_BASE + {6'd0, rect_addr};
                        p0_data      <= char_fg;   // shared fill-colour reg
                        p0_byte_en   <= 2'b11;
                        p0_wr_len    <= {2'b00, rect_w};
                        p0_wr_stream <= 1'b0;
                        p0_wr_req    <= 1'b1;
                        wr_is_char   <= 1'b0;
                        astate       <= A_WRWAIT;

                    // Composing needs no SDRAM, so it runs only once nothing
                    // else wants the bus -- it can never delay a fill by more
                    // than the one row it is part-way through.
                    end else if (copy_mode && char_rows_left_nz
                                 && !char_row_ready && can_sdram) begin
                        p0_addr   <= FB_BASE + {6'd0, copy_src};
                        p0_rd_req <= 1'b1;
                        copy_cnt  <= 8'd0;
                        astate    <= A_COPYRD;
                    end else if (!copy_mode && char_rows_left_nz && !char_row_ready) begin
                        rowf_cnt <= 2'd0;
                        astate   <= A_ROWFETCH;

                    end else if (!fifo_empty && !engine_busy && can_sdram) begin
                        rd_ptr   <= rd_ptr + 1'b1;
                        rd_ptr_g <= b2g(rd_ptr + 1'b1);
                        char_fg  <= q_fg;
                        case (q_op)
                            OP_CHAR: begin
                                char_addr <= q_addr;
                                char_bg   <= q_bg;
                                copy_mode <= 1'b0;
                                char_sx   <= q_sx;
                                char_sy   <= q_sy;
                                // EPX doubles 8x8 -> 16x16, then each axis is
                                // replicated (scale+1) times: 16*(sx+1) wide,
                                // 16*(sy+1) rows. Max 64x64.
                                char_num  <= nd_x[5:3];
                                char_den  <= nd_x[2:0];
                                char_numy <= nd_y[5:3];
                                char_deny <= nd_y[2:0];
                                char_w            <= ext_x[6:0];
                                char_rows_left    <= ext_y;
                                char_rows_left_nz <= 1'b1;
                                ey <= 4'd0; acc_y <= 3'd0;
                                /* Glyphs below 0x20 or above 0x7E fall back to
                                 * space rather than reading past the atlas. */
                                char_base <= ((q_glyph >= 7'h20) && (q_glyph <= 7'h7E))
                                           ? {1'b0, (q_glyph - 7'h20), 5'd0}
                                           : 12'd0;
                                rowf_cnt  <= 2'd0;
                                astate    <= A_ROWFETCH;
                            end
                            OP_RECT: begin
                                rect_addr   <= q_addr;
                                rect_w      <= q_w;
                                rect_rows   <= q_h;
                                rect_active <= (q_h != 9'd0) && (q_w != 9'd0);
                            end
                            OP_COPY: begin
                                copy_src  <= {q_fg[2:0], q_bg};
                                copy_mode <= 1'b1;
                                char_addr <= q_addr;
                                char_w    <= q_w[6:0];
                                char_rows_left    <= q_h;
                                char_rows_left_nz <= (q_h != 9'd0) && (q_w != 9'd0);
                            end
                            default: begin   // OP_RUN -- a one-row rect
                                rect_addr   <= q_addr;
                                rect_w      <= q_w;
                                rect_rows   <= 9'd1;
                                rect_active <= (q_w != 9'd0);
                            end
                        endcase
                    end
                end

                // ------------------------------------------- scanout FILL --
                A_FILL: begin
                    if (p0_data_available) begin
                        lb_we    <= 1'b1;
                        lb_waddr <= {fill_lb, fill_cnt[8:0]};
                        lb_wdata <= p0_q;
                        if (fill_cnt == {1'b0, STRIDE} - 1'b1) begin
                            p0_end_burst_req <= 1'b1;
                            astate <= A_FILL_END;
                        end else fill_cnt <= fill_cnt + 1'b1;
                    end
                end
                A_FILL_END: begin
                    fill_pending <= 1'b0;
                    if (p0_available) astate <= A_IDLE;
                end

                // ------------------------------------- burst write retire --
                A_WRWAIT: if (p0_ready) begin
                    if (wr_is_char) begin
                        char_row_ready <= 1'b0;
                        char_addr      <= char_addr + 19'd512;   // next row
                        copy_src       <= copy_src  + 19'd512;
                        char_rows_left <= char_rows_left - 9'd1;
                        if (char_rows_left == 9'd1) begin
                            char_rows_left_nz <= 1'b0;
                            copy_mode         <= 1'b0;
                        end
                        // Bresenham step in Y: same accumulator idea as X, so
                        // vertical scaling can be fractional too.
                        if (acc_y + char_numy >= char_deny) begin
                            acc_y <= acc_y + char_numy - char_deny;
                            ey    <= ey + 4'd1;
                        end else begin
                            acc_y <= acc_y + char_numy;
                        end
                    end else begin
                        rect_addr <= rect_addr + 19'd512;        // next row
                        rect_rows <= rect_rows - 9'd1;
                        if (rect_rows == 9'd1) rect_active <= 1'b0;
                    end
                    astate <= A_IDLE;
                end

                // ------------------------------------------- copy row read --
                A_COPYRD: begin
                    if (p0_data_available) begin
                        glyphbuf[copy_cnt[6:0]] <= p0_q;
                        if (copy_cnt == char_w[6:0] - 7'd1) begin
                            p0_end_burst_req <= 1'b1;
                            char_row_ready   <= 1'b1;
                            astate <= A_IDLE;
                        end else copy_cnt <= copy_cnt + 8'd1;
                    end
                end

                // ----------------------------------------- glyph row fetch --
                // Two words = one 16-pixel source row. font_rom is registered,
                // so the word requested at count N arrives at N+2; fetching the
                // row up front means composing it needs no ROM port at all.
                A_ROWFETCH: begin
                    case (rowf_cnt)
                        2'd0: font_addr <= char_base + {7'd0, ey, 1'b0};
                        2'd1: font_addr <= char_base + {7'd0, ey, 1'b1};
                        2'd2: rowlo <= font_q;
                        2'd3: begin
                            rowhi  <= font_q;
                            ox <= 7'd0; ex <= 4'd0; acc_x <= 3'd0;
                            astate <= A_COMPOSE;
                        end
                    endcase
                    rowf_cnt <= rowf_cnt + 2'd1;
                end

                // ------------------------------------------ glyph compose --
                // One output pixel per cycle into glyphbuf. Worst case 64
                // cycles for a 4x glyph -- 1.5% of a scanline's slack.
                A_COMPOSE: begin
                    glyphbuf[ox[5:0]] <= px_color;
                    if (ox == char_w - 7'd1) begin
                        char_row_ready <= 1'b1;
                        astate <= A_IDLE;
                    end else begin
                        ox <= ox + 7'd1;
                        if (acc_x + char_num >= char_den) begin
                            acc_x <= acc_x + char_num - char_den;
                            ex    <= ex + 4'd1;
                        end else begin
                            acc_x <= acc_x + char_num;
                        end
                    end
                end

                default: astate <= A_IDLE;
            endcase
        end
    end

    // ======================================================================
    // Video timing (clk_vid domain) -- same structure as pocket_vector_fb.sv.
    // ======================================================================
    reg [10:0] hc = 0, vc = 0;
    always @(posedge clk_vid) begin
        if (reset) begin hc <= 0; vc <= 0; end
        else if (hc == H_TOT - 1'b1) begin
            hc <= 0;
            vc <= (vc == V_TOT - 1'b1) ? 11'd0 : vc + 1'b1;
        end else hc <= hc + 1'b1;
    end

    wire active   = (hc >= HOFF) && (hc < HOFF + H_ACT) &&
                    (vc >= VOFF) && (vc < VOFF + V_ACT);
    wire hs_pulse = (hc >= HS_ST) && (hc < HS_EN);
    wire vs_pulse = (vc >= VS_ST) && (vc < VS_EN);

    // Prefetch the row the NEXT scanline will display, so its burst-fill has
    // a full scanline period to complete before it's actually scanned out.
    wire [10:0] next_sl = (vc == V_TOT - 1'b1) ? 11'd0 : vc + 1'b1;
    wire        do_fill = (next_sl >= VOFF) && (next_sl < VOFF + V_ACT);
    wire [9:0]  pf_row  = next_sl[9:0] - VOFF[9:0];
    always @(posedge clk_vid) begin
        if (hc == 11'd0 && do_fill) begin
            fill_line_req <= pf_row;
            fill_req_tgl  <= ~fill_req_tgl;
        end
    end

    reg [15:0] lb_q;
    reg        active_p1, hs_p1, vs_p1, top_guard_p1;
    wire [10:0] hcsub = hc - HOFF;
    wire [10:0] vsub  = vc - VOFF;
    wire        top_guard = active && (vsub < GUARD_TOP);

    always @(posedge clk_vid) begin
        lb_q         <= linebuf[{vc[0], hcsub[8:0]}];   // vc-VOFF parity = vc[0] (VOFF even)
        active_p1    <= active;
        top_guard_p1 <= top_guard;
        hs_p1        <= hs_pulse;
        vs_p1        <= vs_pulse;
        video_rgb    <= (active_p1 && !top_guard_p1)
                       ? {lb_q[15:11], 3'b0, lb_q[10:5], 2'b0, lb_q[4:0], 3'b0}  // RGB565 -> 24-bit
                       : 24'h000000;
        video_de     <= active_p1;
        video_hs     <= hs_p1;
        video_vs     <= vs_p1;
    end

endmodule

`default_nettype wire
