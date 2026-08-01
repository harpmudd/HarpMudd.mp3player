// MIT License
// Copyright (c) 2022 Adam Gastineau
//
// A very simple audio i2s bridge to APF, based on Analogue example code.
//
// Usage:
//   - CHANNEL_WIDTH: width of the per-channel audio bus, any value >= 1.
//                   Values < 15 are zero-padded LSB. Values > 15 keep top 15 bits.
//   - SIGNED_INPUT:  0 for unsigned positive-only audio (silence at 0),
//                   1 for signed two's-complement audio (silence at 0).
//   - clk_audio is the game core clock domain (clk_sys)
//   - clk_74a drives the serializer and MCLK generator

`default_nettype none

module sound_i2s #(
    parameter CHANNEL_WIDTH = 8,
    parameter SIGNED_INPUT  = 0
) (
    input wire clk_74a,
    input wire clk_audio,

    input wire [CHANNEL_WIDTH-1:0] audio_l,
    input wire [CHANNEL_WIDTH-1:0] audio_r,

    output reg audio_mclk,
    output reg audio_lrck,
    output reg audio_dac
);

  // ----------------------------------------------------------------
  // Generate MCLK ~12.288 MHz using fractional accumulator on clk_74a
  // 74.25 MHz * (245760/742500) ≈ 12.288 MHz
  // CYCLE_48KHZ = 21'd122880 * 2 = 245760
  // ----------------------------------------------------------------
  reg [21:0] audgen_accum = 0;
  parameter [20:0] CYCLE_48KHZ = 21'd122880 * 2;

  always @(posedge clk_74a) begin
    audgen_accum <= audgen_accum + CYCLE_48KHZ;
    if (audgen_accum >= 21'd742500) begin
      audio_mclk   <= ~audio_mclk;
      audgen_accum <= audgen_accum - 21'd742500 + CYCLE_48KHZ;
    end
  end

  // ----------------------------------------------------------------
  // Generate SCLK = MCLK / 4 = ~3.072 MHz
  // Serializer clocks on falling edge of SCLK
  // ----------------------------------------------------------------
  reg [1:0] aud_mclk_divider = 0;
  reg prev_audio_mclk = 0;
  wire audgen_sclk = aud_mclk_divider[1] /* synthesis keep */;

  always @(posedge clk_74a) begin
    if (audio_mclk && ~prev_audio_mclk)
      aud_mclk_divider <= aud_mclk_divider + 1'b1;
    prev_audio_mclk <= audio_mclk;
  end

  // ----------------------------------------------------------------
  // Pack audio channels into 32-bit sample word.
  // Each channel occupies a 16-bit signed slot ([15] sign, [14:0] magnitude).
  //   SIGNED_INPUT=1: audio_l/r is already signed -- pass through MSB-aligned.
  //   SIGNED_INPUT=0: audio_l/r is unsigned positive-only (silence at 0).
  //                   Map to a positive signed value: top bit forced 0,
  //                   audio bits placed in the 15-bit magnitude slot.
  //
  // NOTE: the earlier version of this module hardcoded an 8-bit slice
  // (`audgen_sampdata[14:7] = audio_l`). With CHANNEL_WIDTH > 8 Verilog
  // silently truncated to the low byte, dropping the actual signal and
  // leaving only LSB noise at full DAC range (crunchy / clipped sound).
  // ----------------------------------------------------------------
  // 15-bit magnitude, MSB-aligned from audio_l/r:
  wire [14:0] left_mag  = (CHANNEL_WIDTH >= 15)
        ? audio_l[CHANNEL_WIDTH-1 -: 15]
        : { audio_l, {(15 - CHANNEL_WIDTH){1'b0}} };
  wire [14:0] right_mag = (CHANNEL_WIDTH >= 15)
        ? audio_r[CHANNEL_WIDTH-1 -: 15]
        : { audio_r, {(15 - CHANNEL_WIDTH){1'b0}} };

  // For SIGNED_INPUT, the top bit of audio_l/r is the sign; keep it.
  // For unsigned (default), force sign bit to 0 (positive-only).
  wire left_sign  = (SIGNED_INPUT != 0) ? audio_l[CHANNEL_WIDTH-1] : 1'b0;
  wire right_sign = (SIGNED_INPUT != 0) ? audio_r[CHANNEL_WIDTH-1] : 1'b0;

  wire [31:0] audgen_sampdata;
  assign audgen_sampdata[15]    = left_sign;
  assign audgen_sampdata[14:0]  = left_mag;
  assign audgen_sampdata[31]    = right_sign;
  assign audgen_sampdata[30:16] = right_mag;

  // ----------------------------------------------------------------
  // Cross from clk_audio (game domain) to clk_74a (serializer domain)
  // via sync_fifo. Write whenever sample changes.
  // ----------------------------------------------------------------
  reg write_en = 0;
  reg [CHANNEL_WIDTH-1:0] prev_left  = 0;
  reg [CHANNEL_WIDTH-1:0] prev_right = 0;

  always @(posedge clk_audio) begin
    prev_left  <= audio_l;
    prev_right <= audio_r;
    write_en   <= 0;
    if (audio_l != prev_left || audio_r != prev_right)
      write_en <= 1;
  end

  wire [31:0] audgen_sampdata_s;

  sync_fifo #(
      .WIDTH(32)
  ) i_sync_fifo (
      .clk_write(clk_audio),
      .clk_read (clk_74a),
      .write_en (write_en),
      .data_in  (audgen_sampdata),
      .data_out (audgen_sampdata_s)
  );

  // ----------------------------------------------------------------
  // Serialize: shift out on falling edge of SCLK
  // 32 bits per channel (16 active + 16 padding), stereo = 64 SCLK cycles
  // LRCK toggles every 32 SCLK cycles → 3.072MHz / 64 = 48kHz
  // ----------------------------------------------------------------
  reg [31:0] audgen_sampshift = 0;
  reg [4:0]  audio_lrck_cnt  = 0;
  reg        prev_audgen_sclk = 0;

  always @(posedge clk_74a) begin
    if (prev_audgen_sclk && ~audgen_sclk) begin
      // Output next bit on falling SCLK edge
      audio_dac <= audgen_sampshift[31];

      audio_lrck_cnt <= audio_lrck_cnt + 1'b1;

      if (audio_lrck_cnt == 31) begin
        // Toggle LRCK, reload sample at start of left channel
        audio_lrck <= ~audio_lrck;
        if (~audio_lrck)
          audgen_sampshift <= audgen_sampdata_s;
      end else if (audio_lrck_cnt < 16) begin
        // Shift for first 16 clocks of each channel, pad rest with 0
        audgen_sampshift <= {audgen_sampshift[30:0], 1'b0};
      end
    end

    prev_audgen_sclk <= audgen_sclk;
  end

endmodule
