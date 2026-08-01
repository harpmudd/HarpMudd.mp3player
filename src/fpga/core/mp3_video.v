// =============================================================================
// mp3_video.v -- 320x240 @ 60 Hz timing + a bit-block status display.
//
// Timing: 6.000 MHz pixel clock, 400 x 250 total -> exactly 60.000 Hz.
//   H: 320 active | 8 front | 32 sync | 40 back  = 400
//   V: 240 active | 2 front | 3 sync  | 5 back   = 250
// (clk_vid and its 90-degree partner come straight from the PLL, unchanged from
//  the mpatrol settings this project was scaffolded from.)
//
// Display: the four 32-bit status words from the SoC are drawn as four rows of
// 32 blocks -- lit = bit set. That is 128 bits of firmware-visible state with no
// font ROM and almost no logic, which is what a first bring-up actually needs:
// if the CPU is alive at all, you can see it. A real text console can replace
// this in Stage 4 once track titles need rendering.
//
// Row colours differ so a glance identifies which word you are reading.
// =============================================================================

module mp3_video (
    input  wire        clk_vid,

    input  wire [31:0] status0,
    input  wire [31:0] status1,
    input  wire [31:0] status2,
    input  wire [31:0] status3,
    input  wire        heartbeat,   // toggled by firmware; drawn as a corner box

    output reg  [7:0]  vid_r,
    output reg  [7:0]  vid_g,
    output reg  [7:0]  vid_b,
    output reg         vid_hs,
    output reg         vid_vs,
    output reg         vid_de
);

    localparam H_ACT = 400, H_VIS = 320, H_FP = 8,  H_SY = 32;
    localparam V_ACT = 250, V_VIS = 240, V_FP = 2,  V_SY = 3;

    reg [9:0] hc = 10'd0;
    reg [9:0] vc = 10'd0;

    always @(posedge clk_vid) begin
        if (hc == H_ACT-1) begin
            hc <= 10'd0;
            vc <= (vc == V_ACT-1) ? 10'd0 : vc + 10'd1;
        end else begin
            hc <= hc + 10'd1;
        end
    end

    wire visible = (hc < H_VIS) && (vc < V_VIS);
    wire hsync   = (hc >= H_VIS + H_FP) && (hc < H_VIS + H_FP + H_SY);
    wire vsync   = (vc >= V_VIS + V_FP) && (vc < V_VIS + V_FP + V_SY);

    // ---- block layout: 4 rows, 32 blocks each, 8 px wide, 24 px tall -------
    // rows start at y = 40, 80, 120, 160 ; blocks span x = 32..287
    wire [9:0] bx = hc - 10'd32;
    wire       in_x   = (hc >= 10'd32) && (hc < 10'd288);
    wire [4:0] bit_ix = bx[7:3];              // 0..31, 8 px per block
    wire       gap    = (bx[2:0] == 3'd0);    // 1 px separator

    wire row0 = (vc >= 10'd40)  && (vc < 10'd64);
    wire row1 = (vc >= 10'd80)  && (vc < 10'd104);
    wire row2 = (vc >= 10'd120) && (vc < 10'd144);
    wire row3 = (vc >= 10'd160) && (vc < 10'd184);

    // MSB on the left reads naturally as hex when comparing against firmware.
    wire b0 = status0[5'd31 - bit_ix];
    wire b1 = status1[5'd31 - bit_ix];
    wire b2 = status2[5'd31 - bit_ix];
    wire b3 = status3[5'd31 - bit_ix];

    // heartbeat box, top-left: proves the CPU is executing, not just powered
    wire hb_box = (hc < 10'd16) && (vc < 10'd16);

    always @(posedge clk_vid) begin
        vid_hs <= hsync;
        vid_vs <= vsync;
        vid_de <= visible;

        vid_r <= 8'h00; vid_g <= 8'h00; vid_b <= 8'h00;

        if (visible) begin
            if (hb_box) begin
                vid_r <= heartbeat ? 8'hFF : 8'h20;
                vid_g <= heartbeat ? 8'hFF : 8'h20;
                vid_b <= heartbeat ? 8'hFF : 8'h20;
            end else if (in_x && !gap && row0) begin
                vid_g <= b0 ? 8'hFF : 8'h18;               // green
            end else if (in_x && !gap && row1) begin
                vid_r <= b1 ? 8'hFF : 8'h18;               // red
                vid_g <= b1 ? 8'hA0 : 8'h08;
            end else if (in_x && !gap && row2) begin
                vid_b <= b2 ? 8'hFF : 8'h18;               // blue
                vid_g <= b2 ? 8'h80 : 8'h08;
            end else if (in_x && !gap && row3) begin
                vid_r <= b3 ? 8'hFF : 8'h18;               // yellow
                vid_g <= b3 ? 8'hFF : 8'h18;
            end else begin
                vid_r <= 8'h04; vid_g <= 8'h04; vid_b <= 8'h0C;  // dim backdrop
            end
        end
    end

endmodule
