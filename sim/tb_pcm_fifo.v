// ============================================================================
// tb_pcm_fifo.v -- the start-of-track priming, checked before a compile.
//
// Hardware measurement that prompted it: every track began with an underrun at
// second 0 and a click (U0, D~50), because the FIFO drained from the moment
// the first sample arrived and the decoder had not got ahead yet.
//
//   iverilog -g2012 -o tb_pcm.vvp sim/tb_pcm_fifo.v src/fpga/core/pcm_fifo.v
//   vvp tb_pcm.vvp
// ============================================================================
`timescale 1ns/1ps
`default_nettype none

module tb_pcm_fifo;
    localparam AW = 11;
    localparam DEPTH = 1 << AW;

    reg clk = 0, rst = 1, flush = 0, push = 0;
    reg [31:0] pdata = 0;
    wire full, empty;
    wire [AW:0] level;
    wire signed [15:0] out_l, out_r;
    wire underrun;

    // ~1 tick every 16 clocks, so a test does not take a million cycles
    localparam [31:0] RATE_INC = 32'h1000_0000;

    always #5 clk = ~clk;

    pcm_fifo #(.AW(AW)) dut (
        .clk(clk), .rst(rst), .flush(flush), .push(push), .pdata(pdata),
        .full(full), .empty(empty), .level(level), .rate_inc(RATE_INC),
        .out_l(out_l), .out_r(out_r), .underrun(underrun)
    );

    integer errors = 0;
    task check(input cond, input [255:0] what);
        begin
            if (!cond) begin $display("FAIL: %0s", what); errors = errors + 1; end
            else         $display("ok:   %0s", what);
        end
    endtask

    task push_n(input integer n);
        integer i;
        begin
            for (i = 0; i < n; i = i + 1) begin
                @(posedge clk);
                pdata <= {16'd1000, 16'd2000};
                push  <= 1'b1;
                @(posedge clk);
                push  <= 1'b0;
            end
        end
    endtask

    integer lvl_before;
    initial begin
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (4) @(posedge clk);

        // ---- a trickle must NOT drain before the cushion exists ----------
        push_n(64);
        repeat (4) @(posedge clk);             // let the last push land
        lvl_before = level;
        repeat (4000) @(posedge clk);          // many ticks pass
        $display("      level %0d -> %0d", lvl_before, level);
        check(level == lvl_before, "below the cushion nothing is consumed");
        check(underrun === 1'b0,   "priming does not report an underrun");
        check(out_l == 16'sd0 && out_r == 16'sd0,
              "output glides to silence while priming");

        // ---- crossing half full starts playback --------------------------
        push_n(DEPTH/2);
        repeat (200) @(posedge clk);
        check(level < DEPTH/2 + 64, "once primed, samples are consumed");
        check(out_l == 16'sd2000 && out_r == 16'sd1000, "output carries the data");

        // ---- a mid-track gap still reports an underrun -------------------
        while (level > 0) @(posedge clk);
        repeat (100) @(posedge clk);
        check(underrun === 1'b1, "a real underrun after priming still flags");

        // ---- flush re-arms priming ---------------------------------------
        @(posedge clk); flush <= 1'b1; @(posedge clk); flush <= 1'b0;
        push_n(64);
        repeat (4) @(posedge clk);
        lvl_before = level;
        repeat (4000) @(posedge clk);
        $display("      level %0d -> %0d", lvl_before, level);
        check(level == lvl_before, "after a flush it primes again");
        check(underrun === 1'b0,   "flush clears the underrun flag");

        $display("\n%0s (%0d failures)", errors ? "FAILED" : "PASSED", errors);
        $finish;
    end

    initial begin
        #5000000;
        $display("TIMEOUT");
        $finish;
    end
endmodule

`default_nettype wire
