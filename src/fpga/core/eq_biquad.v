// =============================================================================
// eq_biquad.v -- preset equaliser: five cascaded biquads per channel.
//
// Sits between pcm_fifo and mp3_soc's audio_l/audio_r outputs. Entirely inside
// clk_sys, so it adds NO clock-domain crossing -- sound_i2s already crosses
// into clk_74a through its own sync_fifo, and this stays on the near side of
// that existing boundary.
//
// Zero CPU cost. Firmware writes one register to pick a preset; the decoder
// never knows this exists.
//
// -----------------------------------------------------------------------------
// EVERY WIDTH HERE WAS MEASURED, NOT CHOSEN. tools/eq_model.py is a bit-exact
// integer model of this arithmetic and sim/tb_eq_biquad.v checks this module
// against it SAMPLE FOR SAMPLE, so the numbers below are not adjustable
// opinions -- change one and the testbench fails.
//
//   coefficients  Q2.16   18-bit signed   a1 reaches magnitude 2 on the bands
//                                         a bass preset needs, so two integer
//                                         bits are required.
//   state         Q20.16  36-bit signed   16 fractional bits is a CLIFF, not a
//                                         preference: at 13 the cascade never
//                                         settles after a loud passage, at 14
//                                         it reaches exact zero in 23 ms. 16
//                                         leaves margin. Plus headroom, that
//                                         needs 33 bits -- so the 32 the design
//                                         doc first specified cannot hold both.
//   accumulator   58-bit signed           18x36 is a 54-bit product before five
//                                         are summed. Measured worst case 49.
//
// Rounding is round-to-nearest, never truncation: truncation inside an IIR
// feedback path biases toward a DC offset.
//
// The output CLAMPS. Presets are loudness-matched rather than peak-matched, so
// a full-scale tone in a boosted band can exceed full scale by up to ~3.6 dB.
// Clamping turns that into brief saturation; wrapping would turn it into
// full-scale noise on a bass hit.
// =============================================================================

module eq_biquad #(
    parameter integer CLK_HZ  = 60_000_000,
    parameter integer RATE_HZ = 48_000
) (
    input  wire               clk,
    input  wire               rst,

    input  wire signed [15:0] in_l,
    input  wire signed [15:0] in_r,
    input  wire        [2:0]  preset,        // 0 = FLAT = true bypass

    output reg  signed [15:0] out_l,
    output reg  signed [15:0] out_r
);

    localparam integer NBAND = 5;
    localparam integer NBQ   = NBAND * 2;          // both channels
    localparam integer DIV   = CLK_HZ / RATE_HZ;   // 1250 exactly at 60 MHz

    localparam integer CW = 18;                    // coefficient width
    localparam integer SW = 36;                    // sample / state width
    localparam integer AW = 58;                    // accumulator width
    localparam integer FS = 16;                    // sample fractional bits
    localparam integer FC = 16;                    // coefficient fractional bits

    // ------------------------------------------------------------- tables ---
    // romstyle/ramstyle "logic" is load-bearing, not decoration. M10K is at
    // 300/308 (97%) because the framebuffer owns it; a small array inferred
    // into M10K by accident is the one resource mistake this design cannot
    // absorb. Both of these are tiny and belong in LUTs/MLAB.
    (* romstyle = "logic" *) reg signed [CW-1:0] crom [0:NBAND*8*5-1];
    (* romstyle = "logic" *) reg signed [CW-1:0] prom [0:7];

    initial begin
        `include "eq_coefs.vh"
    end

    (* ramstyle = "logic" *) reg signed [SW-1:0] st [0:NBQ*4-1];

    integer i;

    // -------------------------------------------------------- sample tick ---
    reg [11:0] divctr;
    wire       tick = (divctr == DIV[11:0] - 12'd1);

    always @(posedge clk) begin
        if (rst)      divctr <= 12'd0;
        else          divctr <= tick ? 12'd0 : divctr + 12'd1;
    end

    // ---------------------------------------------------------- sequencer ---
    // One multiplier, time-multiplexed across all ten biquads. Per 48 kHz
    // sample there are 1250 clocks and this needs ~80, so there is no timing
    // pressure and no reason for a second multiplier.
    reg               busy;
    reg  [3:0]        bq;        // 0..4 = left, 5..9 = right
    reg  [2:0]        k;         // MAC index, then writeback / preamp / store
    reg signed [SW-1:0] smp;     // value flowing down the cascade
    reg signed [AW-1:0] acc;
    reg signed [15:0] r_hold;    // right input, latched at the tick

    localparam K_B0 = 3'd0, K_B1 = 3'd1, K_B2 = 3'd2,
               K_A1 = 3'd3, K_A2 = 3'd4, K_WB = 3'd5,
               K_PRE = 3'd6, K_OUT = 3'd7;

    wire [5:0] sbase = {bq, 2'b00};              // bq*4
    wire signed [SW-1:0] x1 = st[sbase + 0];
    wire signed [SW-1:0] x2 = st[sbase + 1];
    wire signed [SW-1:0] y1 = st[sbase + 2];
    wire signed [SW-1:0] y2 = st[sbase + 3];

    // Coefficient index: ((preset*NBAND) + band)*5 + k, where band is bq mod 5.
    wire [2:0] band = (bq >= NBAND) ? (bq - NBAND) : bq;
    wire [8:0] cidx = (({6'd0, preset} * NBAND) + {6'd0, band}) * 5 + {6'd0, k};
    wire signed [CW-1:0] cf = crom[cidx];

    // The one multiplier. Operand selected by the MAC index.
    reg signed [SW-1:0] opnd;
    always @(*) begin
        case (k)
            K_B0:    opnd = smp;
            K_B1:    opnd = x1;
            K_B2:    opnd = x2;
            K_A1:    opnd = y1;
            K_A2:    opnd = y2;
            default: opnd = smp;
        endcase
    end
    // The multiply is REGISTERED before the accumulate. Doing both in one
    // cycle closed at only +0.425 ns on clk_sys, and slack on this design has
    // been seen to move ~3 ns between fits, so that is a build away from
    // failing. Splitting them costs one cycle per MAC -- ~160 of the 1250
    // clocks per sample instead of ~80 -- which is free here and buys back the
    // whole path.
    wire signed [AW-1:0] mul_a = $signed(cf) * $signed(opnd);
    wire signed [AW-1:0] mul_p = $signed(prom[preset]) * $signed(smp);

    reg  signed [AW-1:0] p_reg;
    reg                  ph;        // 0 = multiply into p_reg, 1 = accumulate

    // Round-to-nearest arithmetic shift, matching _rnd_shift() in the model.
    function signed [SW-1:0] rnd16;
        input signed [AW-1:0] v;
        reg   signed [AW-1:0] t;
        begin
            t     = v + (58'sd1 <<< (FC - 1));
            rnd16 = t[FC + SW - 1 : FC];
        end
    endfunction

    // Q20.16 state -> whole sample, rounded then CLAMPED.
    function signed [15:0] clamp16;
        input signed [SW-1:0] v;
        reg   signed [SW-1:0] t;
        begin
            t = v + (36'sd1 <<< (FS - 1));
            if (t[SW-1] == 1'b0 && |t[SW-2 : FS + 15])      clamp16 = 16'sh7FFF;
            else if (t[SW-1] == 1'b1 && ~&t[SW-2 : FS + 15]) clamp16 = -16'sh8000;
            else                                             clamp16 = t[FS + 15 : FS];
        end
    endfunction

    reg signed [15:0] eq_l, eq_r;

    always @(posedge clk) begin
        if (rst) begin
            busy <= 1'b0; bq <= 4'd0; k <= K_B0;
            smp  <= {SW{1'b0}}; acc <= {AW{1'b0}};
            p_reg <= {AW{1'b0}}; ph <= 1'b0;
            eq_l <= 16'sd0; eq_r <= 16'sd0;
            out_l <= 16'sd0; out_r <= 16'sd0;
            r_hold <= 16'sd0;
            for (i = 0; i < NBQ*4; i = i + 1) st[i] <= {SW{1'b0}};
        end else if (!busy) begin
            if (tick) begin
                busy   <= 1'b1;
                bq     <= 4'd0;
                k      <= K_B0;
                ph     <= 1'b0;
                r_hold <= in_r;
                smp    <= {{(SW-16-FS){in_l[15]}}, in_l, {FS{1'b0}}};
            end
        end else if (k <= K_A2) begin
            // Two cycles per MAC: latch the product, then accumulate it.
            if (!ph) begin
                p_reg <= mul_a;
                ph    <= 1'b1;
            end else begin
                ph <= 1'b0;
                case (k)
                    K_B0: acc <= p_reg;             // b0*x
                    K_B1: acc <= acc + p_reg;       // + b1*x1
                    K_B2: acc <= acc + p_reg;       // + b2*x2
                    K_A1: acc <= acc - p_reg;       // - a1*y1
                    K_A2: acc <= acc - p_reg;       // - a2*y2
                endcase
                k <= k + 3'd1;
            end
        end else begin
            case (k)
                K_WB: begin
                    st[sbase + 1] <= x1;               // x2 <= x1
                    st[sbase + 0] <= smp;              // x1 <= this band's input
                    st[sbase + 3] <= y1;               // y2 <= y1
                    st[sbase + 2] <= rnd16(acc);       // y1 <= y
                    smp           <= rnd16(acc);       // feed the next band
                    if (band == NBAND - 1) begin
                        k <= K_PRE;
                    end else begin
                        bq <= bq + 4'd1;
                        k  <= K_B0;
                    end
                end
                K_PRE: begin
                    if (!ph) begin
                        p_reg <= mul_p;                // preamp, same 2 cycles
                        ph    <= 1'b1;
                    end else begin
                        ph  <= 1'b0;
                        smp <= rnd16(p_reg);
                        k   <= K_OUT;
                    end
                end
                K_OUT: begin
                    if (bq < NBAND) begin              // finished left
                        eq_l <= clamp16(smp);
                        bq   <= NBAND[3:0];
                        k    <= K_B0;
                        smp  <= {{(SW-16-FS){r_hold[15]}}, r_hold, {FS{1'b0}}};
                    end else begin                     // finished right
                        eq_r <= clamp16(smp);
                        busy <= 1'b0;
                        k    <= K_B0;
                    end
                end
                default: k <= K_B0;
            endcase
        end

        // FLAT is a TRUE BYPASS -- a mux, not a preset loaded with unity
        // coefficients. The engine keeps running underneath so its state stays
        // warm and switching away from FLAT does not start from silence.
        if (!rst) begin
            out_l <= (preset == 3'd0) ? in_l : eq_l;
            out_r <= (preset == 3'd0) ? in_r : eq_r;
        end
    end

endmodule
