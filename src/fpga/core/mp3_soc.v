// =============================================================================
// mp3_soc.v -- VexRiscv + BRAM + memory-mapped peripherals.
//
// Memory map (BYTE addresses as seen by firmware):
//   0x0000_0000 .. RAM_BYTES-1   RAM  (CACHED -- code, data, buffers)
//   0x8000_0000 +                MMIO (UNCACHED)
//
// The 0x8000_0000 split is MANDATORY, not stylistic: VexRiscv decides
// cacheability purely from physical address bit 31
//   assign ..._isIoAccess     = ..._physicalAddress[31];
//   assign stageB_bypassCache = (stageB_mmuRsp_isIoAccess || ...);
// so a peripheral placed below 0x8000_0000 would be read through the D-cache
// and firmware polling a status register would spin forever on a stale value.
// RAM deliberately stays cached -- that is what produced the measured 1.65 CPI.
//
// MMIO registers:
//   0x8000_0000  W  console character out (bits 7:0)
//   0x8000_0004  W  sim/exit + halt marker (unused on HW)
//   0x8000_0008  W  audio sample {right[31:16], left[15:0]}
//   0x8000_000C  R  free-running clk cycle counter
//   0x8000_0010  W  status word 0   (rendered on screen as 32 bit-blocks)
//   0x8000_0014  W  status word 1
//   0x8000_0018  W  status word 2
//   0x8000_001C  W  status word 3
//   0x8000_0020  W  target-cmd: id
//   0x8000_0024  W  target-cmd: slot offset
//   0x8000_0028  W  target-cmd: bridge address
//   0x8000_002C  W  target-cmd: length
//   0x8000_0030  W  target-cmd: go   (bit0 1=openfile 0=read -> starts command)
//                R  target-cmd: status {err[2:0], done, busy}
//
// The loader port (ld_*) writes bytes into RAM on port B. It carries BOTH the
// initial firmware image at boot AND, later, bytes streamed back by APF target
// reads -- both arrive over the same APF bridge write bus.
// =============================================================================

// RAM sizing: measured Helix needs are firmware image ~63 KB + decoder instance
// ~34 KB + MP3 read-ahead 32 KB + stack 16 KB = ~143 KB, so 128 KB is too small
// and the next power of two is 256 KB.
//
// RAM_WORDS **MUST BE A POWER OF TWO**. A 49152-entry array addressed by a
// 16-bit index cannot be mapped cleanly onto M10K, so Quartus built the memory
// out of logic instead and the fit exploded:
//   Error (170012): Fitter requires 3621 LABs ... device contains only 1848
// At 65536 it infers correctly: 256 KB costs ~256 M10K blocks (byte-wide arrays
// force x8 mode, ~80% bit efficiency), measured 271/308 total with the rest of
// the core. pcm_fifo adds ~8 more.
module mp3_soc #(
    parameter RAM_WORDS = 65536,           // 256 KB -- power of two, required
    parameter RAM_AW    = 16
) (
    input  wire        clk,
    input  wire        rst,                // active high, hold until firmware loaded
    input  wire        clk_74a,            // for the dataslot_update CDC only

    // Byte-write port for firmware load + streamed data (same clock domain)
    input  wire        ld_wr,
    input  wire [31:0] ld_addr,
    input  wire [7:0]  ld_data,

    // Controller + OS menu state (clk_74a domain; resynced internally)
    input  wire [31:0] cont_key,
    input  wire        in_menu,

    // Fires when the user reloads the MP3 slot from the Core menu ("Load MP3"
    // is User-Reloadable, parameters bit 0). Verified against Analogue's docs
    // rather than assumed (see the 0192 mistake earlier in this project):
    // "If the slot is marked User Reloadable, APF sends 008A Data slot update
    // whenever the user picks a new file." clk_74a domain, one-cycle pulse.
    input  wire        dataslot_update,
    input  wire [15:0] dataslot_update_id,
    input  wire [31:0] dataslot_update_size,

    // "Data slot access all complete" (host command 008F). The BOOT path
    // already depends on this -- core_game.vh holds the CPU in reset until it
    // fires -- but the reload path only ever saw 008A, which APF sends when
    // the user PICKS a file, not when the slot is ready to read. That is the
    // whole asymmetry behind "the tag shows on boot but not after Load MP3".
    input  wire        dataslot_allcomplete,

    // Audio out -- driven by pcm_fifo, drained in hardware at the programmed
    // sample rate. Firmware pushes bursts; it never has to meet DAC timing.
    output wire [15:0] audio_l,
    output wire [15:0] audio_r,

    // Debug/status words rendered by the video block
    output reg  [31:0] status0,
    output reg  [31:0] status1,
    output reg  [31:0] status2,
    output reg  [31:0] status3,

    // Console character stream (for a future text overlay / trace)
    output reg         con_wr,
    output reg  [7:0]  con_char,

    // APF target-command request (clk domain; CDC handled in tgt_cmd.v)
    output reg         tgt_go,             // 1-cycle pulse
    output reg  [1:0]  tgt_cmd_sel,        // 0=0180 read 1=0192 openfile 2=0190 getfile
    output reg  [15:0] tgt_id,
    output reg  [31:0] tgt_slotoffset,
    output reg  [31:0] tgt_bridgeaddr,
    output reg  [31:0] tgt_length,
    input  wire        tgt_busy,
    input  wire        tgt_done,
    input  wire [7:0]  tgt_seq,
    input  wire [2:0]  tgt_err,

    // Framebuffer draw command -> mp3_fb.sv (rev 7). One command is now a whole
    // primitive -- a run, a rect, or a complete anti-aliased glyph -- rather
    // than a single row, so the CPU issues ~30x fewer of them. The parameter
    // registers persist between pushes, so consecutive glyphs of the same
    // colour and size cost just an address write plus the GO write.
    // fb_cmd_full is checked before pushing; pushing while full is silently
    // dropped by mp3_fb's FIFO.
    output reg          fb_cmd_push,
    output reg  [1:0]   fb_cmd_op,
    output reg  [18:0]  fb_cmd_addr,
    output reg  [8:0]   fb_cmd_w,
    output reg  [8:0]   fb_cmd_h,
    output reg  [15:0]  fb_cmd_fg,
    output reg  [15:0]  fb_cmd_bg,
    output reg  [6:0]   fb_cmd_glyph,
    output reg  [1:0]   fb_cmd_sx,
    output reg  [1:0]   fb_cmd_sy,
    input  wire         fb_cmd_full,

    // core_bridge_cmd's datatable: a dual-port BRAM whose OTHER port APF can
    // read and write over the bridge at 0xF8xx2xxx. That is what makes the
    // 0190 getfile / 0192 openfile structs reachable at all -- the frozen
    // shell's bridge_rd_data mux serves core_bridge_cmd alone, so a core
    // cannot otherwise expose memory to a bridge READ. This BRAM already sits
    // on both sides, and core_top leaves the user-side port undriven.
    output reg  [9:0]   dt_addr,
    output reg          dt_wren,
    output reg  [31:0]  dt_wdata,
    input  wire [31:0]  dt_q
);

    // ---------------------------------------------------------------- CPU ---
    wire        iCYC, iSTB, iWE;
    wire [29:0] iADR;
    wire [31:0] iDAT_MOSI;
    wire [3:0]  iSEL;
    wire [2:0]  iCTI;
    wire [1:0]  iBTE;
    reg  [31:0] iDAT_MISO;
    reg         iACK;

    wire        dCYC, dSTB, dWE;
    wire [29:0] dADR;
    wire [31:0] dDAT_MOSI;
    wire [3:0]  dSEL;
    wire [2:0]  dCTI;
    wire [1:0]  dBTE;
    reg  [31:0] dDAT_MISO;
    reg         dACK;

    VexRiscv cpu (
        .externalResetVector   (32'h00000000),
        .timerInterrupt        (1'b0),
        .softwareInterrupt     (1'b0),
        .externalInterruptArray(32'b0),

        .iBusWishbone_CYC      (iCYC),
        .iBusWishbone_STB      (iSTB),
        .iBusWishbone_ACK      (iACK),
        .iBusWishbone_WE       (iWE),
        .iBusWishbone_ADR      (iADR),
        .iBusWishbone_DAT_MISO (iDAT_MISO),
        .iBusWishbone_DAT_MOSI (iDAT_MOSI),
        .iBusWishbone_SEL      (iSEL),
        .iBusWishbone_ERR      (1'b0),
        .iBusWishbone_CTI      (iCTI),
        .iBusWishbone_BTE      (iBTE),

        .dBusWishbone_CYC      (dCYC),
        .dBusWishbone_STB      (dSTB),
        .dBusWishbone_ACK      (dACK),
        .dBusWishbone_WE       (dWE),
        .dBusWishbone_ADR      (dADR),
        .dBusWishbone_DAT_MISO (dDAT_MISO),
        .dBusWishbone_DAT_MOSI (dDAT_MOSI),
        .dBusWishbone_SEL      (dSEL),
        .dBusWishbone_ERR      (1'b0),
        .dBusWishbone_CTI      (dCTI),
        .dBusWishbone_BTE      (dBTE),

        .clk                   (clk),
        .reset                 (rst)
    );

    // ------------------------------------------------------- RAM (4 banks) ---
    // Four byte-wide arrays so both ports get byte enables and Quartus infers
    // true dual-port M10K without a read-modify-write.
    reg [7:0] ram0 [0:RAM_WORDS-1];
    reg [7:0] ram1 [0:RAM_WORDS-1];
    reg [7:0] ram2 [0:RAM_WORDS-1];
    reg [7:0] ram3 [0:RAM_WORDS-1];

    // Port A: CPU (dBus has priority; iBus retries while stalled)
    //
    // Address decode (BYTE addresses; dADR is a WORD address so byte bit N maps
    // to dADR[N-2]):
    //   0x0000_0000  RAM, CACHED    -- code/data/stack, gives the good CPI
    //   0x8000_0000  MMIO           -- uncached (bit31), peripherals
    //   0xC000_0000  RAM, UNCACHED  -- same physical RAM, aliased
    //
    // The uncached alias exists because APF target reads DMA straight into RAM
    // over the bridge, behind the CPU's back. Reading that buffer through the
    // cached window could return stale lines with no way to tell. Firmware reads
    // streamed bytes via 0xC000_0000 and gets the real memory every time.
    wire        d_is_mmio = dADR[29] & ~dADR[28];     // byte 0x8xxx_xxxx
    wire        d_is_ram  = ~dADR[29] | dADR[28];     // byte 0x0xxx / 0xCxxx
    wire        d_req     = dCYC & dSTB & ~dACK;
    wire        i_req     = iCYC & iSTB & ~iACK;

    // Priority: loader > dBus > iBus. The loader must win because the APF
    // bridge cannot be back-pressured -- a dropped byte would silently corrupt
    // the firmware image or a streamed buffer. MMIO needs no RAM port, so a
    // dBus MMIO access proceeds even while the loader holds the RAM.
    wire        ld_req     = ld_wr;
    wire        d_mmio_req = d_req & d_is_mmio;
    wire        d_ram_req  = d_req & d_is_ram;
    wire        serve_d    = d_ram_req & ~ld_req;
    wire        serve_i    = i_req & ~d_ram_req & ~ld_req;

    // ONE always block, ONE address -> Quartus infers M10K cleanly.
    //
    // Originally the loader had its own write port in a second always block.
    // That is a true-dual-port pattern, and because it did not match Quartus's
    // inference template the whole 256 KB fell back to registers:
    //   Error (276003): Cannot convert all sets of registers into RAM megafunctions
    // Arbitrating the loader onto the single port avoids the issue entirely and
    // costs nothing real: during boot the CPU is held in reset, and while
    // streaming the bridge delivers only a few bytes per microsecond, so the
    // occasional stolen cycle is invisible next to a 50 MHz CPU.
    wire [RAM_AW-1:0] mem_addr = ld_req  ? ld_addr[RAM_AW+1:2]
                               : serve_d ? dADR[RAM_AW-1:0]
                                         : iADR[RAM_AW-1:0];
    wire              mem_we    = ld_req | (serve_d & dWE);
    wire [3:0]        mem_be    = ld_req ? (4'b0001 << ld_addr[1:0]) : dSEL;
    wire [31:0]       mem_wdata = ld_req ? {4{ld_data}} : dDAT_MOSI;
    reg  [31:0]       a_rdata;

    always @(posedge clk) begin
        if (mem_we & mem_be[0]) ram0[mem_addr] <= mem_wdata[7:0];
        if (mem_we & mem_be[1]) ram1[mem_addr] <= mem_wdata[15:8];
        if (mem_we & mem_be[2]) ram2[mem_addr] <= mem_wdata[23:16];
        if (mem_we & mem_be[3]) ram3[mem_addr] <= mem_wdata[31:24];
        a_rdata <= {ram3[mem_addr], ram2[mem_addr], ram1[mem_addr], ram0[mem_addr]};
    end

    // ------------------------------------------------------------- cycles ---
    reg [31:0] cycle_ctr;
    always @(posedge clk) cycle_ctr <= rst ? 32'd0 : cycle_ctr + 32'd1;

    // -------------------------------------------------------------- input ---
    // Two-stage resync from clk_74a. No bit-coherency requirement: these are
    // human button presses, so a one-cycle skew between bits is meaningless.
    reg [15:0] key_s0, key_s1;
    reg        menu_s0, menu_s1;
    always @(posedge clk) begin
        key_s0  <= cont_key[15:0];  key_s1  <= key_s0;
        menu_s0 <= in_menu;         menu_s1 <= menu_s0;
    end

    // ------------------------------------------------ dataslot_update CDC ---
    // Same toggle-crossing idiom as tgt_cmd.v: dataslot_update is a one-cycle
    // pulse on clk_74a, which a plain 2FF synchronizer would drop if it doesn't
    // line up with a clk_sys edge. Convert to a toggle (glitch-proof across any
    // clock ratio), latch the ID in the SAME clk_74a cycle so it is stable
    // by the time clk_sys detects the toggle edge, then compare against our
    // MP3 slot ID (2 -- must match data.json AND firmware's MP3_SLOT_ID).
    localparam [15:0] MP3_SLOT_ID = 16'd2;
    // Playlist slot ID (3 -- must match data.json AND firmware's PL_SLOT_ID).
    localparam [15:0] PL_SLOT_ID  = 16'd3;

    reg        upd_tgl_74 = 1'b0;
    reg [15:0] upd_id_74;
    reg [31:0] upd_size_74;
    always @(posedge clk_74a) begin
        if (dataslot_update) begin
            upd_tgl_74  <= ~upd_tgl_74;
            upd_id_74   <= dataslot_update_id;
            upd_size_74 <= dataslot_update_size;
        end
    end

    reg [2:0] upd_sync = 3'b0;
    always @(posedge clk) upd_sync <= {upd_sync[1:0], upd_tgl_74};
    wire mp3_reload_edge = (upd_sync[2] ^ upd_sync[1]) && (upd_id_74 == MP3_SLOT_ID);

    // The playlist slot raises 008A exactly as the MP3 slot does, but the edge
    // above is gated on the MP3 id -- so without this a "Load Playlist" pick is
    // invisible to firmware and silently does nothing. Polling slot 3 instead is
    // NOT an option: touching a different slot makes APF drop its fragment cache
    // for the MP3 slot, and every refill then re-walks the FAT cluster chain.
    wire pl_reload_edge = (upd_sync[2] ^ upd_sync[1]) && (upd_id_74 == PL_SLOT_ID);

    reg mp3_reloaded, pl_reloaded;
    // Acked PER BIT rather than by any write, so clearing one flag cannot drop
    // the other -- a track change and a playlist change can land together.
    wire wr_reload  = d_req & d_is_mmio & dWE & (mmio_reg == R_RELOAD);
    wire ack_reload = wr_reload & dDAT_MOSI[0];
    wire ack_pl     = wr_reload & dDAT_MOSI[4];
    always @(posedge clk) begin
        if (rst)                  mp3_reloaded <= 1'b0;
        else if (mp3_reload_edge) mp3_reloaded <= 1'b1;
        else if (ack_reload)      mp3_reloaded <= 1'b0;

        if (rst)                 pl_reloaded <= 1'b0;
        else if (pl_reload_edge) pl_reloaded <= 1'b1;
        else if (ack_pl)         pl_reloaded <= 1'b0;
    end

    // dataslot_allcomplete is a LEVEL in clk_74a (set by 008F, cleared by the
    // 0080/0082 request handlers), so a plain synchroniser is enough -- no
    // toggle needed, unlike the one-cycle dataslot_update pulse above.
    //
    // slot_ready deliberately keys off a RISING edge after the reload rather
    // than requiring a preceding fall: the exact 008A/0080/008F ordering APF
    // uses for a deferload reload is not documented anywhere we have, so this
    // works whichever way round they arrive. If APF never re-sequences
    // allcomplete at all, slot_ready simply stays low and firmware falls
    // through on its timeout -- i.e. no worse than the current behaviour.
    // slot_fell is pure observability, so one hardware test can tell us which
    // of those actually happens instead of another round of inference.
    reg [2:0] ac_sync = 3'b0;
    always @(posedge clk) ac_sync <= {ac_sync[1:0], dataslot_allcomplete};
    wire ac_rise = ac_sync[1] & ~ac_sync[2];
    wire ac_fall = ~ac_sync[1] & ac_sync[2];

    reg slot_ready, slot_fell;
    reg [31:0] slot_size;
    always @(posedge clk) begin
        if (rst) begin
            slot_ready <= 1'b0; slot_fell <= 1'b0; slot_size <= 32'd0;
        end else if (mp3_reload_edge) begin
            slot_ready <= 1'b0; slot_fell <= 1'b0;
            slot_size  <= upd_size_74;
        end else begin
            if (ac_fall) slot_fell  <= 1'b1;
            if (ac_rise) slot_ready <= 1'b1;
        end
    end

    // --------------------------------------------------------------- MMIO ---
    localparam [7:0] R_CONSOLE = 8'h00, R_EXIT    = 8'h04, R_AUDIO   = 8'h08,
                     R_CYCLES  = 8'h0C, R_STAT0   = 8'h10, R_STAT1   = 8'h14,
                     R_STAT2   = 8'h18, R_STAT3   = 8'h1C, R_TGT_ID  = 8'h20,
                     R_TGT_OFF = 8'h24, R_TGT_ADR = 8'h28, R_TGT_LEN = 8'h2C,
                     R_TGT_GO  = 8'h30, R_PCM_ST  = 8'h34, R_PCM_RATE= 8'h38,
                     R_INPUT   = 8'h3C, R_VERSION = 8'h40, R_RELOAD  = 8'h44,
                     R_FB_ADDR = 8'h48, R_FB_SIZE = 8'h4C, R_FB_COLOR= 8'h50,
                     R_FB_GO   = 8'h54, R_FB_STALL= 8'h58, R_SLOT_SZ = 8'h5C,
                     R_DT_ADDR = 8'h60, R_DT_DATA = 8'h64;

    // Bitstream/firmware interlock. Firmware compares this against its own
    // expected value and refuses to run on a mismatch.
    //
    // Rationale: the .rom loads from SD in seconds while the bitstream needs a
    // ~6 minute Quartus compile, so it is very easy to flash new firmware onto
    // stale RTL. That has already happened three times here, each time looking
    // like a logic bug (dead peripheral, no audio, unresponsive buttons) rather
    // than what it was. BUMP THIS whenever the MMIO map changes.
    localparam [31:0] CORE_VERSION = 32'h4D503310;   // "MP3" + rev 16 (param struct moved to word 64)

    wire [7:0] mmio_reg = {dADR[5:0], 2'b00};   // byte offset within MMIO page

    // Diagnostic: clk_sys cycles spent with the draw FIFO full. Firmware
    // busy-waits on exactly that condition, so this is the direct measurement
    // of "is drawing actually stalling the CPU" -- the question the rev 6 UI
    // could only answer by A/B-ing the whole feature on real hardware.
    reg [31:0] fb_stall_ctr;
    always @(posedge clk) begin
        if (rst)             fb_stall_ctr <= 32'd0;
        else if (fb_cmd_full) fb_stall_ctr <= fb_stall_ctr + 32'd1;
    end

    // ------------------------------------------------------------ PCM FIFO ---
    wire        pcm_push  = d_req & d_is_mmio & dWE & (mmio_reg == R_AUDIO);
    // Any write to R_PCM_ST (previously read-only) flushes the FIFO. Used on a
    // track change or a seek, where samples already queued from BEFORE the
    // jump must not keep draining out the DAC over the new position's audio.
    wire        pcm_flush = d_req & d_is_mmio & dWE & (mmio_reg == R_PCM_ST);
    reg  [31:0] pcm_rate;
    wire        pcm_full, pcm_empty, pcm_underrun;
    wire [11:0] pcm_level;

    pcm_fifo #(.AW(11)) u_pcm (
        .clk      (clk),
        .rst      (rst),
        .flush    (pcm_flush),
        .push     (pcm_push),
        .pdata    (dDAT_MOSI),
        .full     (pcm_full),
        .empty    (pcm_empty),
        .level    (pcm_level),
        .rate_inc (pcm_rate),
        .out_l    (audio_l),
        .out_r    (audio_r),
        .underrun (pcm_underrun)
    );

    always @(posedge clk) begin
        con_wr      <= 1'b0;
        tgt_go      <= 1'b0;
        fb_cmd_push <= 1'b0;
        dt_wren     <= 1'b0;

        if (rst) begin
            status0 <= 32'd0; status1 <= 32'd0;
            status2 <= 32'd0; status3 <= 32'd0;
            tgt_id  <= 16'd0; tgt_slotoffset <= 32'd0;
            tgt_bridgeaddr <= 32'd0; tgt_length <= 32'd0;
            tgt_cmd_sel <= 2'd0;
            dt_addr <= 10'd0; dt_wdata <= 32'd0;
            fb_cmd_op <= 2'd0; fb_cmd_addr <= 19'd0;
            fb_cmd_w  <= 9'd0; fb_cmd_h    <= 9'd0;
            fb_cmd_fg <= 16'd0; fb_cmd_bg  <= 16'd0;
            fb_cmd_glyph <= 7'd0; fb_cmd_sx <= 2'd0; fb_cmd_sy <= 2'd0;
            // Default to 48 kHz at 50 MHz so a plain sample write still makes
            // sound before firmware programs the real rate.
            pcm_rate <= 32'd3435974;   // 48 kHz at clk_sys = 60 MHz
        end else if (d_req & d_is_mmio & dWE) begin
            case (mmio_reg)
                R_CONSOLE: begin con_char <= dDAT_MOSI[7:0]; con_wr <= 1'b1; end
                R_AUDIO:   ;   /* handled by pcm_push -> pcm_fifo */
                R_PCM_RATE: pcm_rate <= dDAT_MOSI;
                R_PCM_ST:  ;   /* handled by pcm_flush -> pcm_fifo, above */
                R_STAT0:   status0 <= dDAT_MOSI;
                R_STAT1:   status1 <= dDAT_MOSI;
                R_STAT2:   status2 <= dDAT_MOSI;
                R_STAT3:   status3 <= dDAT_MOSI;
                R_TGT_ID:  tgt_id         <= dDAT_MOSI[15:0];
                R_TGT_OFF: tgt_slotoffset <= dDAT_MOSI;
                R_TGT_ADR: tgt_bridgeaddr <= dDAT_MOSI;
                R_TGT_LEN: tgt_length     <= dDAT_MOSI;
                R_TGT_GO:  begin tgt_cmd_sel <= dDAT_MOSI[1:0]; tgt_go <= 1'b1; end
                R_DT_ADDR: dt_addr  <= dDAT_MOSI[9:0];
                R_DT_DATA: begin dt_wdata <= dDAT_MOSI; dt_wren <= 1'b1; end
                R_FB_ADDR: fb_cmd_addr <= dDAT_MOSI[18:0];
                R_FB_SIZE: begin fb_cmd_w <= dDAT_MOSI[8:0];
                                 fb_cmd_h <= dDAT_MOSI[17:9]; end
                R_FB_COLOR: begin fb_cmd_fg <= dDAT_MOSI[15:0];
                                  fb_cmd_bg <= dDAT_MOSI[31:16]; end
                /* GO carries the per-glyph fields (op/char/scale) so drawing a
                 * string is two MMIO writes per character -- address, then this
                 * -- with colour and size left standing in their registers. */
                R_FB_GO:   begin fb_cmd_op    <= dDAT_MOSI[1:0];
                                 fb_cmd_glyph <= dDAT_MOSI[9:3];
                                 fb_cmd_sx    <= dDAT_MOSI[11:10];
                                 fb_cmd_sy    <= dDAT_MOSI[13:12];
                                 fb_cmd_push  <= 1'b1; end
                default: ;
            endcase
        end
    end

    reg [31:0] mmio_rdata;
    always @(*) begin
        case (mmio_reg)
            R_CYCLES:  mmio_rdata = cycle_ctr;
            // {seq[15:8], err[4:2], done[1], busy[0]}. Poll seq, not done:
            // done is a level that stays set from the PREVIOUS command until
            // the next one is picked up, so reading it right after issuing can
            // report a completion that hasn't happened. See tgt_cmd.v.
            R_TGT_GO:  mmio_rdata = {16'd0, tgt_seq, 3'd0, tgt_err, tgt_done, tgt_busy};
            R_PCM_ST:  mmio_rdata = {13'd0, pcm_underrun, pcm_full, pcm_empty,
                                     4'd0, pcm_level};
            // {in_menu, cont1_key[15:0]}
            //  key bits: 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 X, 7 Y,
            //            14 select, 15 start
            R_INPUT:   mmio_rdata = {15'd0, menu_s1, key_s1};
            R_VERSION: mmio_rdata = CORE_VERSION;
            //  bit0 reload pending, bit1 slot ready (allcomplete rose since),
            //  bit2 allcomplete level now, bit3 allcomplete fell since reload,
            //  bit4 PLAYLIST slot reloaded
            R_RELOAD:  mmio_rdata = {27'd0, pl_reloaded, slot_fell, ac_sync[2],
                                     slot_ready, mp3_reloaded};
            R_SLOT_SZ: mmio_rdata = slot_size;
            /* BRAM read is registered: firmware writes R_DT_ADDR, then reads
             * R_DT_DATA as a separate MMIO transaction, so q_a has settled. */
            R_DT_DATA: mmio_rdata = dt_q;
            R_FB_GO:   mmio_rdata = {31'd0, fb_cmd_full};
            R_FB_STALL: mmio_rdata = fb_stall_ctr;
            R_STAT0:   mmio_rdata = status0;
            R_STAT1:   mmio_rdata = status1;
            R_STAT2:   mmio_rdata = status2;
            R_STAT3:   mmio_rdata = status3;
            default:   mmio_rdata = 32'h0;
        endcase
    end

    // --------------------------------------------------------- bus returns ---
    reg d_was_mmio;
    always @(posedge clk) begin
        if (rst) begin
            dACK <= 1'b0; iACK <= 1'b0; d_was_mmio <= 1'b0;
        end else begin
            dACK       <= 1'b0;
            iACK       <= 1'b0;
            d_was_mmio <= d_is_mmio;
            // Ack only what actually got the port this cycle; anything blocked
            // by the loader simply retries (Wishbone masters hold their request).
            if (d_mmio_req | serve_d) dACK <= 1'b1;
            if (serve_i)              iACK <= 1'b1;
        end
    end

    always @(*) begin
        dDAT_MISO = d_was_mmio ? mmio_rdata : a_rdata;
        iDAT_MISO = a_rdata;
    end

endmodule
