// =============================================================================
// pcm_fifo.v -- decoupling buffer between the decoder and the DAC.
//
// WHY THIS EXISTS: decoding one MP3 frame costs ~1.1M cycles (~22 ms at 50 MHz)
// and produces 1152 samples, but the DAC needs a sample every ~21 us. A
// single-threaded CPU cannot both decode and feed the DAC on time, so firmware
// pushes a whole frame in a burst and hardware drains it at the exact sample
// rate. Without this, audio would gap on every frame boundary.
//
// The drain rate is PROGRAMMABLE via a fractional accumulator, because MP3 is
// commonly 44.1 kHz while sound_i2s runs at a fixed 48 kHz. Firmware sets
//     rate_inc = sample_rate * 2^32 / CLK_HZ
// so the FIFO empties at the file's true rate and playback is at correct pitch;
// sound_i2s then samples whatever is currently presented (zero-order hold).
// That is crude resampling -- fine to prove playback, worth replacing with a
// real interpolator later if it sounds rough.
//
// `underrun` is sticky: if the decoder ever falls behind, it latches so the
// condition is visible on screen instead of being an intermittent click.
// =============================================================================

module pcm_fifo #(
    parameter AW = 11                  // 2048 entries = ~43 ms at 48 kHz
) (
    input  wire        clk,
    input  wire        rst,

    // Drops all queued samples immediately (wptr = rptr = 0), independent of
    // the global reset. Needed for track changes/seeks: without this, samples
    // already queued from the PREVIOUS track keep draining out the DAC after
    // firmware starts feeding the new one -- ring buffer and decoder both reset
    // cleanly on a track change, but this FIFO does not unless told to.
    input  wire        flush,

    // ---- push side (CPU) ----
    input  wire        push,
    input  wire [31:0] pdata,          // {right[31:16], left[15:0]}
    output wire        full,
    output wire        empty,
    output wire [AW:0] level,

    // ---- drain side ----
    input  wire [31:0] rate_inc,       // sample_rate * 2^32 / CLK_HZ
    output reg  [15:0] out_l,
    output reg  [15:0] out_r,
    output reg         underrun
);

    localparam DEPTH = (1 << AW);

    reg [31:0] mem [0:DEPTH-1];
    reg [AW:0] wptr, rptr;

    wire [AW:0] used = wptr - rptr;
    assign level = used;
    assign full  = (used == DEPTH[AW:0]);
    assign empty = (used == 0);

    // Fractional-rate tick: carry-out of the accumulator is the sample strobe.
    reg [31:0] acc;
    reg        tick;
    always @(posedge clk) begin
        if (rst) begin
            acc  <= 32'd0;
            tick <= 1'b0;
        end else begin
            {tick, acc} <= {1'b0, acc} + {1'b0, rate_inc};
        end
    end

    // Memory block: ONE always block, UNCONDITIONAL registered read, write
    // guarded only by its enable. This exact shape is required for M10K
    // inference. Reading mem[] inside `if (tick)` and slicing the result
    // straight into out_l/out_r looks harmless but is NOT inferable -- Quartus
    // built a 2048x32 register file instead and the fit went ~2x over the
    // device (Error 170012, 3655 LABs vs 1848 available).
    reg [31:0] q;
    always @(posedge clk) begin
        if (push && !full) mem[wptr[AW-1:0]] <= pdata;
        q <= mem[rptr[AW-1:0]];
    end

    always @(posedge clk) begin
        if (rst) begin
            wptr     <= 0;
            rptr     <= 0;
            out_l    <= 16'd0;
            out_r    <= 16'd0;
            underrun <= 1'b0;
        end else if (flush) begin
            // Same effect as reset on the pointers, but does NOT touch
            // out_l/out_r: slamming those to 0 would produce an audible click,
            // whereas holding the last sample for the ~21us until the next
            // real one arrives is inaudible. underrun is cleared so a stale
            // flag from the old track doesn't paint the new one as broken.
            wptr     <= 0;
            rptr     <= 0;
            underrun <= 1'b0;
        end else begin
            if (push && !full) wptr <= wptr + 1'b1;

            if (tick) begin
                if (!empty) begin
                    // q trails rptr by one cycle, which is irrelevant here:
                    // ticks are ~1041 cycles apart at 48 kHz / 50 MHz, so the
                    // read has long since settled.
                    out_l <= q[15:0];
                    out_r <= q[31:16];
                    rptr  <= rptr + 1'b1;
                end else begin
                    // Hold the last sample rather than slamming to zero -- a
                    // held level is far less audible than a hard discontinuity.
                    underrun <= 1'b1;
                end
            end
        end
    end

endmodule
