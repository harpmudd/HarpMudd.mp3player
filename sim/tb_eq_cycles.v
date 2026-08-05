`timescale 1ns/1ps
module tb_eq_cycles;
    reg clk=0, rst=1; reg signed [15:0] l=1000, r=-2000; reg [2:0] p=2;
    wire signed [15:0] ol, orr;
    integer n;
    always #8 clk = ~clk;
    eq_biquad dut(.clk(clk),.rst(rst),.in_l(l),.in_r(r),.preset(p),.out_l(ol),.out_r(orr));
    initial begin
        repeat(4) @(posedge clk); rst=0;
        while (dut.tick !== 1'b1) @(posedge clk);
        n = 0;
        @(posedge clk);                       // busy rises
        while (dut.busy !== 1'b0) begin @(posedge clk); n = n + 1; end
        $display("engine busy for %0d clocks of %0d available per sample (%0d.%0d%%)",
                 n, dut.DIV, (n*100)/dut.DIV, ((n*1000)/dut.DIV)%10);
        $finish;
    end
endmodule
