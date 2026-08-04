// =============================================================================
// tb_eq_biquad.v -- eq_biquad.v against tools/eq_model.py, sample for sample.
//
//   iverilog -g2005-sv -o sim/tb_eq.vvp -I src/fpga/core \
//            sim/tb_eq_biquad.v src/fpga/core/eq_biquad.v && vvp sim/tb_eq.vvp
//
// EXACT equality, not a tolerance. Both sides are integer, so any difference at
// all is a real defect -- a shift off by one, a truncation where a round
// belongs, a state word a bit too narrow. A tolerance would swallow exactly
// those and leave them to be found on hardware.
//
// The stimulus is chosen to hit the cases that break filters:
//   * an impulse, which exposes the whole response including the tail
//   * silence after it, where a limit cycle would show as a signal that never
//     reaches exactly zero
//   * random full-scale noise, the broadband worst case
//   * a full-scale 16 kHz sine, which drives TREBLE ~3.6 dB past full scale and
//     so proves the output CLAMPS rather than wraps
// =============================================================================

`timescale 1ns/1ps

module tb_eq_biquad;

    localparam integer NS  = 576;      // stimulus length
    localparam integer DIV = 1250;     // clk_sys / 48 kHz

    reg               clk = 1'b0;
    reg               rst = 1'b1;
    reg  signed [15:0] in_l = 16'sd0, in_r = 16'sd0;
    reg  [2:0]        preset = 3'd0;
    wire signed [15:0] out_l, out_r;

    always #8 clk = ~clk;              // ~60 MHz

    eq_biquad #(.CLK_HZ(60_000_000), .RATE_HZ(48_000)) dut (
        .clk(clk), .rst(rst),
        .in_l(in_l), .in_r(in_r), .preset(preset),
        .out_l(out_l), .out_r(out_r)
    );

    reg [15:0] stim [0:NS-1];
    reg [15:0] expv [0:NS-1];

    integer p, n, errors, total_errors, first_bad;
    reg signed [15:0] got, want;

    // Both channels are fed the same stimulus, so the right channel doubles as
    // a check that the two do not share state through the sequencer.
    task run_preset(input [2:0] ps, input [255:0] label);
        begin
            case (ps)
                3'd0: $readmemh("sim/eq_exp_0.hex", expv);
                3'd1: $readmemh("sim/eq_exp_1.hex", expv);
                3'd2: $readmemh("sim/eq_exp_2.hex", expv);
                3'd3: $readmemh("sim/eq_exp_3.hex", expv);
                3'd4: $readmemh("sim/eq_exp_4.hex", expv);
                3'd5: $readmemh("sim/eq_exp_5.hex", expv);
                3'd6: $readmemh("sim/eq_exp_6.hex", expv);
                3'd7: $readmemh("sim/eq_exp_7.hex", expv);
            endcase

            rst = 1'b1; preset = ps;
            repeat (4) @(posedge clk);
            rst = 1'b0;
            repeat (2) @(posedge clk);

            errors = 0; first_bad = -1;
            for (n = 0; n < NS; n = n + 1) begin
                in_l = stim[n];
                in_r = stim[n];
                // Synchronise to the ENGINE, not to a fixed number of clocks.
                // The rate divider free-runs, so a plain `repeat (DIV)` samples
                // at an arbitrary phase against it and reads the previous
                // sample's result about as often as not -- which looks exactly
                // like a broken filter.
                do @(posedge clk); while (dut.tick !== 1'b1);  // captures input
                @(posedge clk);                                // busy rises HERE
                while (dut.busy !== 1'b0) @(posedge clk);      // engine done
                repeat (2) @(posedge clk);                     // outputs registered

                want = expv[n];
                got  = out_l;
                if (got !== want) begin
                    errors = errors + 1;
                    if (first_bad < 0) begin
                        first_bad = n;
                        $display("    first mismatch at sample %0d: got %0d, want %0d",
                                 n, got, want);
                    end
                end
                if (out_r !== want) begin
                    errors = errors + 1;
                    if (first_bad < 0) begin
                        first_bad = n;
                        $display("    RIGHT channel differs at %0d: got %0d, want %0d",
                                 n, out_r, want);
                    end
                end
            end

            if (errors == 0)
                $display("  [PASS] %0s  %0d samples, both channels exact", label, NS);
            else
                $display("  [FAIL] %0s  %0d mismatches", label, errors);
            total_errors = total_errors + errors;
        end
    endtask

    initial begin
        $readmemh("sim/eq_stim.hex", stim);
        total_errors = 0;
        $display("eq_biquad vs tools/eq_model.py -- exact match required");
        run_preset(3'd0, "FLAT (true bypass)");
        run_preset(3'd1, "BASS");
        run_preset(3'd2, "ROCK");
        run_preset(3'd3, "POP");
        run_preset(3'd4, "JAZZ");
        run_preset(3'd5, "CLASSICAL");
        run_preset(3'd6, "VOCAL");
        run_preset(3'd7, "TREBLE");
        $display("");
        if (total_errors == 0) $display("all presets bit-exact");
        else                   $display("FAIL -- %0d total mismatches", total_errors);
        $finish;
    end

endmodule
