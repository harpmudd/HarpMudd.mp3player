// =============================================================================
// core_game.vh -- MP3 player core logic, `included into the frozen core_top.v.
//
// This is NOT an arcade port: there is no MiSTer RTL and no game ROM. The core
// is a small SoC -- VexRiscv + BRAM + peripherals -- running C firmware that is
// itself loaded from the SD card like a ROM.
//
// Why firmware-from-SD rather than baked into the bitstream: a firmware change
// then costs a rebuild measured in seconds instead of a full Quartus compile.
// Stages 2-4 are almost entirely firmware work, so this is the single biggest
// lever on iteration speed (CLAUDE.md: "cut compile->test cycles").
//
// Everything here is inside module core_top, so the shell's nets -- including
// the target_dataslot_* registers -- are in scope and drivable from here. The
// shell itself stays untouched.
// =============================================================================

// -- 1. PLL / clocks ----------------------------------------------------------
// outclk_0 = 60 MHz  CPU/system.  Stage 0 measured 45.7 MHz as the worst-case
//            requirement (320 kbps) at 0 wait states. Bring-up ran at 50 MHz,
//            which left too little headroom once UI drawing and SD reads shared
//            the CPU with the decoder -- audible tics under load. 60 MHz is the
//            shipping speed and still closes timing comfortably.
// outclk_1 = 12 MHz  pixel clock: 500x400 total = exactly 60.000 Hz, driving
//            a 400x360 active image -- an exact 4x integer scale of the
//            Pocket's native 1600x1440 panel (confirmed via Analogue's own
//            spec: the panel is built around integer scaling).
// outclk_2 = 12 MHz shifted 90 degrees (APF requires both edges).
// outclk_3 = 100 MHz SDRAM framebuffer controller (mp3_fb.sv / sdram_fb.sv).
//            No phase shift needed -- confirmed against HarpMudd.starwars'
//            proven SDRAM PLL: the DAC-side DDR forwarding happens inside the
//            controller itself, not via a separately-phased clock input.
wire clk_sys;
wire clk_vid;
wire clk_vid_90;
wire clk_sdram;
wire pll_locked;
wire pll_locked_s;

mf_pllbase mp1 (
    .refclk   (clk_74a),
    .rst      (1'b0),
    .outclk_0 (clk_sys),
    .outclk_1 (clk_vid),
    .outclk_2 (clk_vid_90),
    .outclk_3 (clk_sdram),
    .locked   (pll_locked)
);

synch_3 s_pll (pll_locked, pll_locked_s, clk_74a);

// -- 2. Firmware load + streamed data via the APF bridge ----------------------
// One data_loader serves BOTH purposes: the boot-time firmware image AND the
// bytes APF pushes back during a target-slot read, because both arrive as
// bridge writes. Target reads therefore land directly in the SoC's own RAM at
// whatever bridge address the firmware asked for -- no separate receive buffer.
// ADDRESS_SIZE 18 covers the 256 KB RAM.
wire [17:0] dn_addr;
wire [7:0]  dn_data;
wire        dn_wr;
reg         fw_loaded_74 = 1'b0;
wire        fw_loaded;

// Shell contract: core_top.v drives status_setup_done from rom_loaded_s, and it
// must be in the clk_74a domain (APF needs a rising edge once loading is done).
wire        rom_loaded_s = fw_loaded_74;

synch_3 s_fw_to_sys (fw_loaded_74, fw_loaded, clk_sys);

data_loader #(
    .ADDRESS_MASK_UPPER_4 (4'h0),
    .ADDRESS_SIZE         (18),
    .OUTPUT_WORD_SIZE     (1)
) u_fw_loader (
    .clk_74a (clk_74a), .clk_memory (clk_sys),
    .bridge_wr (bridge_wr), .bridge_endian_little (bridge_endian_little),
    .bridge_addr (bridge_addr), .bridge_wr_data (bridge_wr_data),
    .write_en (dn_wr), .write_addr (dn_addr), .write_data (dn_data)
);

always @(posedge clk_74a) if (dataslot_allcomplete) fw_loaded_74 <= 1'b1;

// -- 3. Reset -----------------------------------------------------------------
// The CPU must stay in reset until the firmware image is fully in RAM,
// otherwise it executes whatever BRAM powers up holding.
wire reset_n_sys;
synch_3 s_resetn (reset_n, reset_n_sys, clk_sys);
reg  [7:0] reset_ctr = 8'hFF;
wire       cpu_reset_n = (reset_ctr == 8'h0) && fw_loaded && reset_n_sys;
wire       cpu_reset   = !cpu_reset_n;
always @(posedge clk_sys) begin
    if (!pll_locked)    reset_ctr <= 8'hFF;
    else if (reset_ctr) reset_ctr <= reset_ctr - 1'd1;
end

// -- 4. SoC -------------------------------------------------------------------
wire [15:0] soc_audio_l, soc_audio_r;
wire [31:0] soc_status0, soc_status1, soc_status2, soc_status3;
wire        soc_con_wr;
wire [7:0]  soc_con_char;

wire        soc_tgt_go;
wire [1:0]  soc_tgt_cmd_sel;
wire [3:0]  soc_set_idx;
wire        soc_set_wr;
wire [31:0] soc_set_wdata;
wire [31:0] soc_set_rdata;
wire [9:0]  soc_dt_addr;
wire        soc_dt_wren;
wire [31:0] soc_dt_wdata;
wire [15:0] soc_tgt_id;
wire [31:0] soc_tgt_slotoffset, soc_tgt_bridgeaddr, soc_tgt_length;
wire        soc_tgt_busy, soc_tgt_done;
wire [7:0]  soc_tgt_seq;
wire [2:0]  soc_tgt_err;

wire        soc_fb_cmd_push;
wire [1:0]  soc_fb_cmd_op;
wire [18:0] soc_fb_cmd_addr;
wire [8:0]  soc_fb_cmd_w, soc_fb_cmd_h;
wire [15:0] soc_fb_cmd_fg, soc_fb_cmd_bg;
wire [6:0]  soc_fb_cmd_glyph;
wire [1:0]  soc_fb_cmd_sx, soc_fb_cmd_sy;
wire        soc_fb_cmd_full;

mp3_soc u_soc (
    .clk     (clk_sys),
    .rst     (cpu_reset),
    .clk_74a (clk_74a),

    .ld_wr   (dn_wr),
    .ld_addr ({14'd0, dn_addr}),
    .ld_data (dn_data),

    // osnotify_inmenu lets firmware auto-pause when the Pocket's OS menu opens.
    // NOTE: a core cannot exit itself -- core_bridge_cmd implements only
    // 0180/0184/0190/0192, and leaving a core is an OS-level action.
    .cont_key (cont1_key),
    .in_menu  (osnotify_inmenu),

    // Fires when the user picks a new file via "Load MP3" (slot is User
    // Reloadable). Both wires already exist in core_top.v from
    // core_bridge_cmd -- no shell change needed, just routing.
    .dataslot_update      (dataslot_update),
    .dataslot_update_id   (dataslot_update_id),
    .dataslot_update_size (dataslot_update_size),

    // 008F "all slot access complete" -- the signal the BOOT path above
    // already gates on (fw_loaded_74). Routing it to the SoC too lets the
    // reload path wait for the same thing instead of reading the slot the
    // moment the user picks a file.
    .dataslot_allcomplete (dataslot_allcomplete),

    .audio_l (soc_audio_l),
    .audio_r (soc_audio_r),

    .status0 (soc_status0),
    .status1 (soc_status1),
    .status2 (soc_status2),
    .status3 (soc_status3),

    .con_wr   (soc_con_wr),
    .con_char (soc_con_char),

    .tgt_go          (soc_tgt_go),
    .tgt_cmd_sel     (soc_tgt_cmd_sel),
    .tgt_id          (soc_tgt_id),
    .tgt_slotoffset  (soc_tgt_slotoffset),
    .tgt_bridgeaddr  (soc_tgt_bridgeaddr),
    .tgt_length      (soc_tgt_length),
    .tgt_busy        (soc_tgt_busy),
    .tgt_done        (soc_tgt_done),
    .tgt_seq         (soc_tgt_seq),
    .tgt_err         (soc_tgt_err),

    .fb_cmd_push  (soc_fb_cmd_push),
    .fb_cmd_op    (soc_fb_cmd_op),
    .fb_cmd_addr  (soc_fb_cmd_addr),
    .fb_cmd_w     (soc_fb_cmd_w),
    .fb_cmd_h     (soc_fb_cmd_h),
    .fb_cmd_fg    (soc_fb_cmd_fg),
    .fb_cmd_bg    (soc_fb_cmd_bg),
    .fb_cmd_glyph (soc_fb_cmd_glyph),
    .fb_cmd_sx    (soc_fb_cmd_sx),
    .fb_cmd_sy    (soc_fb_cmd_sy),
    .fb_cmd_full  (soc_fb_cmd_full),

    .dt_addr  (soc_dt_addr),
    .dt_wren  (soc_dt_wren),
    .dt_wdata (soc_dt_wdata),
    .dt_q     (datatable_q),
    .set_idx  (soc_set_idx),
    .set_wr   (soc_set_wr),
    .set_wdata(soc_set_wdata),
    .set_rdata(soc_set_rdata)
);

// core_bridge_cmd's datatable, user-side port. core_top declares these wires
// and connects them but never drives them, so this is the intended extension
// point -- and the ONLY way to hand APF a memory region it can read, since the
// shell's bridge_rd_data mux serves core_bridge_cmd alone.
assign datatable_addr = soc_dt_addr;
assign datatable_wren = soc_dt_wren;
assign datatable_data = soc_dt_wdata;

// Where APF fetches/deposits the 0192 parameter and 0190 response structs.
// The datatable lives at bridge 0xF8xx2xxx and is word-addressed
// (b_datatable_addr = bridge_addr >> 2), so these are 256 words apart and
// cannot overlap.
// mf_datatable is 256 WORDS (widthad = 8), so word 256 does not exist -- the
// address wraps and the parameter struct landed on top of the response struct
// at word 0. They must not overlap: 0192's parameters are built FROM a 0190
// response, so one clobbering the other is a read-after-write on itself.
// Response occupies words 0..63; parameters start at word 64.
// MEASURED 2026-08-05, not assumed. A boot-time dump of this BRAM before the
// core touched it read:
//
//   w0=1 w1=0x1CB40(117568)  w2=2 w3=0        w4=3 w5=0x395(917)  w6=4 w7=0x20(32)
//
// which is APF's DATASLOT ID/SIZE TABLE -- {slot_id, size} pairs at stride 2 --
// matching mp3player.rom, the empty MP3 slot, playlist.m3u and settings.bin
// exactly. Analogue's docs say a slot's size "is determined by the Dataslot
// ID/Size Table BRAM in the core"; this is that table.
//
// The response struct USED TO SIT AT WORD 0. APF writes 64 words there on every
// 0190 getfile, and pl_open_name() issues one on every track change -- so the
// first skip destroyed APF's record of every slot's size. That is the root of
// the 0184 file corruption and the nonvolatile boot hang both.
//
// Everything now lives above word 63. data.json allows up to 32 slots, so the
// table could in principle reach word 63; starting at 64 is safe for a full
// complement. Words 224-226 also hold something APF writes (they decode as a
// date and a time), so nothing goes near them either.
//
//   64..127  0190 response      128..191  0192 parameters      192..199 settings
assign target_buffer_resp_struct  = 32'hF8002100;   // word  64
assign target_buffer_param_struct = 32'hF8002200;   // word 128

// -- 5. APF target-command bridge (clk_sys <-> clk_74a) -----------------------
// STAGE 2 GOAL: no core in this workspace has ever driven these. 0192 hands off
// to the Pocket's own file browser; 0180 reads from an arbitrary offset.
wire tgt_t_read, tgt_t_openfile, tgt_t_getfile, tgt_t_write;

tgt_cmd u_tgt (
    .clk_sys     (clk_sys),
    .rst_sys     (cpu_reset),
    .clk_74a     (clk_74a),

    .go          (soc_tgt_go),
    .cmd_sel     (soc_tgt_cmd_sel),
    .busy        (soc_tgt_busy),
    .done        (soc_tgt_done),
    .seq         (soc_tgt_seq),
    .err         (soc_tgt_err),

    .t_read      (tgt_t_read),
    .t_openfile  (tgt_t_openfile),
    .t_getfile   (tgt_t_getfile),
    .t_write     (tgt_t_write),
    .t_ack       (target_dataslot_ack),
    .t_done      (target_dataslot_done),
    .t_err       (target_dataslot_err)
);

// core_top declares these as `reg`, so they must be driven procedurally rather
// than connected as module outputs. Registering in clk_74a is also the correct
// domain for core_bridge_cmd. The parameter registers are quasi-static: firmware
// writes them before pulsing go and holds them for the whole command.
always @(posedge clk_74a) begin
    target_dataslot_read       <= tgt_t_read;
    target_dataslot_openfile   <= tgt_t_openfile;
    target_dataslot_write      <= tgt_t_write;
    target_dataslot_getfile    <= tgt_t_getfile;
    target_dataslot_id         <= soc_tgt_id;
    target_dataslot_slotoffset <= soc_tgt_slotoffset;
    target_dataslot_bridgeaddr <= soc_tgt_bridgeaddr;
    target_dataslot_length     <= soc_tgt_length;
end

// -- 6. Video -- SDRAM-backed framebuffer UI -----------------------------------
// Replaced the Stage 1-3 bring-up display (32-bit-block status rows) entirely;
// that module is gone. SDRAM is otherwise completely unused by this core -- Stage 0
// measured the decoder fits entirely in BRAM -- so the framebuffer costs
// essentially nothing (~360 KB of 32 MB) and needed no BRAM budget, which
// mattered: the core is already at 90% BRAM utilisation.
//
// reset(~pll_locked) directly, no synchroniser: matches HarpMudd.starwars'
// proven sdram_fb instantiation exactly (PLL lock status is a slow, stable
// flag in practice, not something that has bitten this pattern on hardware).
wire        sdram_init_complete;
wire [24:0] fb_p0_addr;
wire [15:0] fb_p0_data;
wire [1:0]  fb_p0_byte_en;
wire [10:0] fb_p0_wr_len;
wire        fb_p0_wr_stream;
wire [15:0] fb_p0_q;
wire        fb_p0_wr_req, fb_p0_rd_req, fb_p0_end_burst_req;
wire        fb_p0_available, fb_p0_ready, fb_p0_data_available;
wire [10:0] fb_wsrc_addr;
wire [15:0] fb_wsrc_q;

sdram_fb #(.CLOCK_SPEED_MHZ(100), .BURST_TYPE(0), .CAS_LATENCY(2), .WRITE_BURST(1)) u_sdram (
    .clk(clk_sdram), .reset(~pll_locked), .init_complete(sdram_init_complete),
    .p0_addr(fb_p0_addr), .p0_data(fb_p0_data), .p0_byte_en(fb_p0_byte_en),
    .p0_wr_len(fb_p0_wr_len), .p0_q(fb_p0_q),
    .p0_wr_stream(fb_p0_wr_stream), .wsrc_addr(fb_wsrc_addr), .wsrc_q(fb_wsrc_q),
    .p0_wr_req(fb_p0_wr_req), .p0_rd_req(fb_p0_rd_req), .p0_end_burst_req(fb_p0_end_burst_req),
    .p0_available(fb_p0_available), .p0_ready(fb_p0_ready), .p0_data_available(fb_p0_data_available),
    .SDRAM_DQ(dram_dq), .SDRAM_A(dram_a), .SDRAM_DQM(dram_dqm), .SDRAM_BA(dram_ba),
    .SDRAM_nCS(), .SDRAM_nWE(dram_we_n), .SDRAM_nRAS(dram_ras_n), .SDRAM_nCAS(dram_cas_n),
    .SDRAM_CKE(dram_cke), .SDRAM_CLK(dram_clk)
);

wire [23:0] vid_rgb_w;
wire        vid_hs_w, vid_vs_w, vid_de_w;

mp3_fb u_fb (
    .reset    (~pll_locked),
    .clk_sys  (clk_sys),
    .clk_sdram(clk_sdram),
    .clk_vid  (clk_vid),

    .cmd_push  (soc_fb_cmd_push),
    .cmd_op    (soc_fb_cmd_op),
    .cmd_addr  (soc_fb_cmd_addr),
    .cmd_w     (soc_fb_cmd_w),
    .cmd_h     (soc_fb_cmd_h),
    .cmd_fg    (soc_fb_cmd_fg),
    .cmd_bg    (soc_fb_cmd_bg),
    .cmd_glyph (soc_fb_cmd_glyph),
    .cmd_sx    (soc_fb_cmd_sx),
    .cmd_sy    (soc_fb_cmd_sy),
    .cmd_full  (soc_fb_cmd_full),

    .sdram_init_complete(sdram_init_complete),
    .p0_addr(fb_p0_addr), .p0_data(fb_p0_data), .p0_byte_en(fb_p0_byte_en),
    .p0_wr_len(fb_p0_wr_len), .p0_q(fb_p0_q),
    .p0_wr_stream(fb_p0_wr_stream), .wsrc_addr(fb_wsrc_addr), .wsrc_q(fb_wsrc_q),
    .p0_wr_req(fb_p0_wr_req), .p0_rd_req(fb_p0_rd_req), .p0_end_burst_req(fb_p0_end_burst_req),
    .p0_available(fb_p0_available), .p0_ready(fb_p0_ready), .p0_data_available(fb_p0_data_available),

    .video_rgb(vid_rgb_w), .video_de(vid_de_w), .video_hs(vid_hs_w), .video_vs(vid_vs_w)
);

assign video_rgb          = vid_rgb_w;
assign video_rgb_clock    = clk_vid;
assign video_rgb_clock_90 = clk_vid_90;
assign video_de           = vid_de_w;
assign video_skip         = 1'b0;
assign video_vs           = vid_vs_w;
assign video_hs           = vid_hs_w;

// -- 7. Audio -----------------------------------------------------------------
// Firmware writes signed 16-bit samples straight to the audio MMIO register, so
// no box-filter decimation is needed here (unlike an arcade core sampling a
// continuously-running sound chip). SIGNED_INPUT(1) -- PCM is two's complement.
sound_i2s #(.CHANNEL_WIDTH(16), .SIGNED_INPUT(1)) u_sound_i2s (
    .clk_74a (clk_74a), .clk_audio (clk_sys),
    .audio_l (soc_audio_l), .audio_r (soc_audio_r),
    .audio_mclk (audio_mclk), .audio_dac (audio_dac), .audio_lrck (audio_lrck)
);

// -- 8. Persistent settings, via interact.json -----------------------------------
// Eight words at 0x20000000. interact.json declares one `persist` variable per
// word; APF reads them back from here EVERY FRAME, lets the user adjust them in
// the Core Settings menu, writes them back, and saves them to its own
// /Settings/<core>/Interact/interact_persist.json when the core shuts down.
//
// APF does the storing, and it stores to its OWN file. No data slot is involved
// at any point, which is the whole reason this route was chosen: the two
// mechanisms tried before -- the 0184 target write and a nonvolatile data slot
// -- both reached the user's .mp3 files. This cannot.
//
// ONE array, not two. The interact protocol is a read-MODIFY-write at a single
// address: APF reads the word, applies the user's adjustment, writes it back,
// and reads it again next frame. Splitting it by direction -- bridge writes one
// array, bridge reads another -- meant APF never read back what it had just
// written, so every setting snapped straight back to the core's value and the
// menu items blinked without changing.
//
// One array means two writers in two clock domains, so the CPU's writes cross
// on a TOGGLE, the same idiom tgt_cmd.v uses for its command strobe: a level
// pulse would not survive the clk_sys -> clk_74a ratio.
//
// CPU WRITES WIN a same-cycle collision. APF rewrites every word every frame,
// so a dropped bridge write is re-sent 16 ms later and costs nothing; a dropped
// CPU write would silently lose the user's button press for good.
//
// CPU reads are asynchronous and quasi-static -- these words change on a button
// press, not continuously -- which is the same argument tgt_cmd.v makes for its
// parameter registers.
/* SIXTEEN words, was eight. The eighth was the last one free and a playlist
 * name needs three, so the index went from 3 bits to 4.
 *
 * Still ramstyle=logic: at 16x32 this is 512 flip-flops out of ~7000, and
 * block RAM is the binding resource on this device at 97%. Letting Quartus
 * infer an M10K here would spend one of the eight remaining blocks on
 * something that fits comfortably in fabric. */
(* ramstyle = "logic" *) reg [31:0] set_reg [0:15];

reg  [3:0]  cpu_set_idx;
reg  [31:0] cpu_set_dat;
reg         cpu_set_tgl = 1'b0;

always @(posedge clk_sys) begin
    if (soc_set_wr) begin
        cpu_set_idx <= soc_set_idx;
        cpu_set_dat <= soc_set_wdata;
        cpu_set_tgl <= ~cpu_set_tgl;
    end
end

reg [2:0] cpu_set_sync = 3'b0;
always @(posedge clk_74a) cpu_set_sync <= {cpu_set_sync[1:0], cpu_set_tgl};
wire cpu_set_wr_74 = cpu_set_sync[2] ^ cpu_set_sync[1];

/* One more address bit. With [4:2] the ninth variable at 0x20000020 wrapped
 * onto slot 0 and silently overwrote Volume -- no error, just a corrupted
 * setting, which is why this had to move in step with the depth above. */
wire [3:0] set_widx = bridge_addr[5:2];

always @(posedge clk_74a) begin
    if (cpu_set_wr_74)
        set_reg[cpu_set_idx] <= cpu_set_dat;
    else if (bridge_wr && bridge_addr[31:28] == 4'h2)
        set_reg[set_widx] <= bridge_wr_data;
end

assign set_bridge_rd_data = set_reg[set_widx];
assign soc_set_rdata      = set_reg[soc_set_idx];
