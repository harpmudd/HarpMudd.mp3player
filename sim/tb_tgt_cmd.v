// ============================================================================
// tb_tgt_cmd.v -- prove a SECOND target command actually waits for its own
// completion. The bug this exists for: core_bridge_cmd holds
// target_dataslot_done high from the previous command "until next command is
// issued", so the naive `S_WAIT: if (t_done)` reported command 2 as finished
// before it had started, and firmware read a buffer nothing had been written
// into yet.
//
// The clk_74a side here models core_bridge_cmd's real timing: the request is
// queued, TARG_ST_IDLE picks it up a cycle later, TARG_ST_DATASLOTOP clears
// done, and only then does the host eventually answer.
//
//   iverilog -g2012 -o tb.vvp sim/tb_tgt_cmd.v src/fpga/core/tgt_cmd.v
// ============================================================================
`timescale 1ns/1ps
`default_nettype none

module tb_tgt_cmd;

    reg clk_sys = 0, clk_74a = 0, rst = 1;
    always #10   clk_sys = ~clk_sys;   // 50 MHz
    always #6.73 clk_74a = ~clk_74a;   // 74.25 MHz

    reg  go = 0;
    wire busy, done;
    wire [7:0] seq;
    wire [2:0] err;
    wire t_read, t_openfile;
    reg  t_done = 0;
    reg  [2:0] t_err = 0;

    tgt_cmd dut (
        .clk_sys(clk_sys), .rst_sys(rst), .clk_74a(clk_74a),
        .go(go), .is_openfile(1'b0), .busy(busy), .done(done), .seq(seq), .err(err),
        .t_read(t_read), .t_openfile(t_openfile),
        .t_ack(1'b0), .t_done(t_done), .t_err(t_err)
    );

    // ---- core_bridge_cmd model ---------------------------------------------
    // Mirrors TARG_ST_IDLE -> TARG_ST_DATASLOTOP (done cleared) ->
    // TARG_ST_WAITRESULT_DSO -> done asserted and HELD until the next command.
    integer bstate = 0, delay = 0;
    integer transfers = 0;
    reg     queued = 0;

    always @(posedge clk_74a) begin
        if (t_read) queued <= 1'b1;
        case (bstate)
            0: if (queued) begin queued <= 1'b0; bstate <= 1; end
            1: begin t_done <= 1'b0; delay <= 0; bstate <= 2; end   // DATASLOTOP
            2: begin                                                // host works
                   delay <= delay + 1;
                   if (delay == 40) begin
                       transfers <= transfers + 1;   // the data lands HERE
                       t_done    <= 1'b1;            // ...and stays high
                       bstate    <= 0;
                   end
               end
        endcase
    end

    integer errors = 0;
    task chk(input cond, input [255:0] what);
        begin
            if (!cond) begin $display("FAIL: %0s", what); errors = errors + 1; end
            else         $display("ok:   %0s", what);
        end
    endtask

    integer seen_at;
    reg [7:0] seq0;
    task run_one(input [255:0] label);
        begin
            // Exactly what firmware does: sample seq BEFORE issuing, then wait
            // for it to change. Immune to the sticky-done level entirely.
            seq0 = seq;
            seen_at = transfers;
            @(posedge clk_sys); go <= 1'b1;
            @(posedge clk_sys); go <= 1'b0;
            wait (seq != seq0);
            // The whole point: completion must not be reported before the
            // transfer that belongs to THIS command.
            chk(transfers == seen_at + 1, label);
            @(posedge clk_sys);
        end
    endtask

    initial begin
        repeat (20) @(posedge clk_sys);
        rst = 0;
        repeat (20) @(posedge clk_sys);

        run_one("command 1 waits for its own transfer");
        run_one("command 2 waits for its own transfer");
        run_one("command 3 waits for its own transfer");

        $display("\n%0s (%0d failures)", errors ? "FAILED" : "PASSED", errors);
        $finish;
    end

    initial begin
        #200000;
        $display("TIMEOUT -- handshake stalled (%0d transfers)", transfers);
        $finish;
    end

endmodule

`default_nettype wire
