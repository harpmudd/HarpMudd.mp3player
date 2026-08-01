// =============================================================================
// tgt_cmd.v -- clock-domain bridge for APF target commands.
//
// The CPU runs on clk_sys; core_bridge_cmd is clocked by clk_74a. This module
// owns that crossing so neither side has to think about it.
//
// Protocol on the clk_74a side (see _mister_pocket_lib/core/core_bridge_cmd.v):
// pulse target_dataslot_read (-> APF cmd 0180) or target_dataslot_openfile
// (-> 0192) for one cycle with the parameter registers already stable, then
// wait for target_dataslot_done and latch target_dataslot_err.
//
// Crossing strategy:
//   * start  : toggle flipped on clk_sys, double-synced + edge-detected on
//              clk_74a. Toggles survive any clock ratio; a level pulse would not.
//   * done   : same trick in reverse.
//   * params : id/offset/bridgeaddr/length are written by firmware BEFORE `go`
//              and held for the whole command, so they are quasi-static and
//              cross without synchronisers (standard practice for this pattern).
//
// `busy` is set on the clk_sys side at request time and cleared only when the
// done toggle returns, so firmware polling busy/done can never observe the
// command as finished before it has actually started.
// =============================================================================

module tgt_cmd (
    input  wire        clk_sys,
    input  wire        rst_sys,
    input  wire        clk_74a,

    // ---- clk_sys side (CPU) ----
    input  wire        go,                 // 1-cycle pulse
    input  wire [1:0]  cmd_sel,            // 0 = 0180 read,  1 = 0192 openfile,
                                           // 2 = 0190 getfile, 3 = 0184 write
    output reg         busy,
    output reg         done,               // sticky until next go
    output reg  [7:0]  seq,                // ++ on each genuine completion
    output reg  [2:0]  err,

    // ---- clk_74a side (core_bridge_cmd) ----
    output reg         t_read,
    output reg         t_openfile,
    output reg         t_getfile,
    output reg         t_write,
    input  wire        t_ack,
    input  wire        t_done,
    input  wire [2:0]  t_err
);

    // ------------------------------------------------- clk_sys -> clk_74a ---
    reg go_tgl = 1'b0;

    reg [2:0] go_sync = 3'b0;
    always @(posedge clk_74a) go_sync <= {go_sync[1:0], go_tgl};
    wire start_74 = go_sync[2] ^ go_sync[1];

    // ------------------------------------------------- clk_74a state machine -
    // S_CLRDONE exists because `t_done` is NOT a pulse: core_bridge_cmd
    // documents it as "asserted upon command finish UNTIL NEXT COMMAND IS
    // ISSUED", and it only actually drops several clk_74a cycles later, when
    // that FSM reaches TARG_ST_DATASLOTOP. Sampling t_done immediately after
    // issuing a request therefore sees the PREVIOUS command's completion and
    // reports the new one as instantly finished, having transferred nothing.
    //
    // Only the very first command after reset escapes this, because t_done
    // starts at 0 -- which is precisely why the boot read worked and every
    // later one (reload, refill) silently returned early. Waiting for done to
    // go LOW first keys off core_bridge_cmd genuinely accepting the command.
    localparam S_IDLE = 2'd0, S_CLRDONE = 2'd1, S_WAIT = 2'd2;
    reg [1:0]  st74     = S_IDLE;
    reg        done_tgl = 1'b0;
    reg [2:0]  err_lat  = 3'b0;

    always @(posedge clk_74a) begin
        t_read     <= 1'b0;
        t_openfile <= 1'b0;
        t_getfile  <= 1'b0;
        t_write    <= 1'b0;
        case (st74)
            S_IDLE:
                if (start_74) begin
                    case (cmd_sel)
                        2'd1:    t_openfile <= 1'b1;   // 0192
                        2'd2:    t_getfile  <= 1'b1;   // 0190
                        2'd3:    t_write    <= 1'b1;   // 0184
                        default: t_read     <= 1'b1;   // 0180
                    endcase
                    st74 <= S_CLRDONE;
                end
            S_CLRDONE:
                if (!t_done) st74 <= S_WAIT;
            S_WAIT:
                if (t_done) begin
                    err_lat  <= t_err;
                    done_tgl <= ~done_tgl;
                    st74     <= S_IDLE;
                end
            default: st74 <= S_IDLE;
        endcase
    end

    // ------------------------------------------------- clk_74a -> clk_sys ---
    reg [2:0] done_sync = 3'b0;
    always @(posedge clk_sys) done_sync <= {done_sync[1:0], done_tgl};
    wire done_pulse = done_sync[2] ^ done_sync[1];

    // `done` is sticky until the next `go`, which leaves the SAME stale-
    // completion hazard on this side of the bridge: firmware writes GO and
    // polls, and if its first read lands before the write has propagated into
    // this block it sees the PREVIOUS command's done still set. `seq` removes
    // the race by construction -- it only ever advances on a real completion,
    // so firmware samples it before issuing and waits for it to change rather
    // than trying to interpret a level whose meaning depends on timing.
    always @(posedge clk_sys) begin
        if (rst_sys) begin
            busy <= 1'b0;
            done <= 1'b0;
            seq  <= 8'd0;
            err  <= 3'b0;
        end else begin
            if (go && !busy) begin
                busy <= 1'b1;
                done <= 1'b0;
            end
            if (done_pulse) begin
                busy <= 1'b0;
                done <= 1'b1;
                seq  <= seq + 8'd1;
                err  <= err_lat;      // stable long before the toggle arrives
            end
        end
    end

    always @(posedge clk_sys) begin
        if (rst_sys)            go_tgl <= 1'b0;
        else if (go && !busy)   go_tgl <= ~go_tgl;
    end

endmodule
