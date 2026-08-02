// Regression test for pcm_fifo's decay-to-zero on underrun.
//
// The first attempt at this decay was shipped unsimulated and produced noise on
// every track: out_l/out_r were UNSIGNED regs, so `>>>` was a logical shift and
// negative samples diverged away from zero until they wrapped. This bench
// exists so that class of bug dies here, in seconds, instead of on hardware.
//
// Checks, for a positive and a negative held sample:
//   1. |out| never increases while the FIFO is empty (no divergence, no wrap)
//   2. out reaches exactly 0 and stays there
//   3. a pushed sample still passes through unmodified afterwards
`timescale 1ns/1ps

module tb_pcm_decay;

    reg         clk = 0, rst = 1, flush = 0, push = 0;
    reg  [31:0] pdata;
    wire        full, empty;
    wire [11:0] level;
    wire signed [15:0] out_l, out_r;
    wire        underrun;

    // rate_inc chosen huge so a "sample tick" lands every few clocks and the
    // test runs in microseconds of sim time rather than milliseconds.
    pcm_fifo #(.AW(11)) dut (
        .clk(clk), .rst(rst), .flush(flush),
        .push(push), .pdata(pdata), .full(full), .empty(empty), .level(level),
        .rate_inc(32'h4000_0000),        // tick every 4 clocks
        .out_l(out_l), .out_r(out_r), .underrun(underrun)
    );

    always #10 clk = ~clk;

    integer i, errors = 0;
    integer mag, prev_mag;

    task push_sample(input [15:0] l, input [15:0] r);
        begin
            @(negedge clk); push = 1; pdata = {r, l};
            @(negedge clk); push = 0;
        end
    endtask

    // Drain whatever is queued, then watch the decay from the held value.
    task check_decay(input [15:0] name_val);
        begin
            // wait for the FIFO to empty and the last sample to be presented
            while (!empty) @(negedge clk);
            repeat (8) @(negedge clk);

            prev_mag = (out_l < 0) ? -out_l : out_l;
            for (i = 0; i < 4000; i = i + 1) begin
                @(negedge clk);
                mag = (out_l < 0) ? -out_l : out_l;
                if (mag > prev_mag) begin
                    errors = errors + 1;
                    if (errors < 5)
                        $display("FAIL: |out_l| grew %0d -> %0d (start %0d) -- divergence/wrap",
                                 prev_mag, mag, $signed(name_val));
                end
                prev_mag = mag;
            end
            if (out_l !== 16'sd0) begin
                errors = errors + 1;
                $display("FAIL: out_l settled at %0d, not 0 (start %0d)",
                         out_l, $signed(name_val));
            end else
                $display("PASS: start %6d -> monotonic decay -> 0", $signed(name_val));
        end
    endtask

    initial begin
        repeat (4) @(negedge clk); rst = 0;

        // -- positive held sample ------------------------------------------
        push_sample(16'sd20000, 16'sd20000);
        check_decay(16'sd20000);

        // -- negative held sample (the case the unsigned version wrapped) --
        push_sample(-16'sd20000, -16'sd20000);
        check_decay(-16'sd20000);

        // -- small values must snap to zero, not stick ---------------------
        push_sample(16'sd100, -16'sd100);
        check_decay(16'sd100);

        // -- normal operation unaffected: pushed samples pass through -------
        // A burst, not one sample: a single sample is immediately followed by
        // emptiness, so the glide (correctly) resumes on it and the check
        // would fail against the DUT behaving as designed. Sample the output
        // while the FIFO is still non-empty, as it is in real playback.
        for (i = 0; i < 8; i = i + 1) push_sample(16'sd1234, -16'sd4321);
        while (empty) @(negedge clk);
        repeat (12) @(negedge clk);      // a few ticks in, still 4+ queued
        if (out_l !== 16'sd1234 || out_r !== -16'sd4321) begin
            errors = errors + 1;
            $display("FAIL: passthrough got L=%0d R=%0d, expected 1234/-4321",
                     out_l, out_r);
        end else
            $display("PASS: passthrough intact after decay logic");

        if (errors == 0) $display("ALL PASS");
        else             $display("%0d FAILURES", errors);
        $finish;
    end

endmodule
