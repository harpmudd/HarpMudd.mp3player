`timescale 1ns/1ps
// Stage 0 CPU comparison harness -- VexRiscv "Full" (RV32IM, 5-stage fully
// bypassed, 4kB I$ + 4kB D$, single-cycle multiply).
//
// Runs the EXACT SAME firmware binary as tb_picorv32.v so the comparison is
// apples-to-apples: instructions retired should match almost exactly, and any
// difference in cycles is purely the CPU microarchitecture.
//
// VexRiscv exposes classic Wishbone iBus/dBus (both cached, hence the burst
// CTI/BTE signals). Modelled here as a single 0-wait-state memory serving both
// -- representing the intended real design where firmware, tables and buffers
// all live in on-chip BRAM. Note tb_picorv32.v models 1 wait state, so also
// build that with -DZERO_WAIT for a like-for-like memory comparison.
//
//   0x10000000  console character out
//   0x20000000  simulation finish

module tb_vexriscv;
	reg clk = 0;
	reg reset = 1;
	always #5 clk = ~clk;

	initial begin
		repeat (10) @(posedge clk);
		reset <= 0;
	end

	localparam MEM_WORDS = 1024*1024/4;   // 1MB
	localparam AW = 18;                    // 2^18 words = 1MB
	reg [31:0] memory [0:MEM_WORDS-1];

	// MMIO at >= 0x8000_0000 so VexRiscv treats it as uncached (addr bit 31).
	localparam [29:0] ADR_CONSOLE = 30'h20000000; // byte 0x80000000 >> 2
	localparam [29:0] ADR_EXIT    = 30'h20000001; // byte 0x80000004 >> 2

	reg [1023:0] firmware_file;
	initial begin
		if (!$value$plusargs("firmware=%s", firmware_file))
			firmware_file = "fw_helix/fw_helix.hex";
		$readmemh(firmware_file, memory);
	end

	// ---------------- instruction bus (read only) ----------------
	wire        iCYC, iSTB, iWE;
	wire [29:0] iADR;
	wire [31:0] iDAT_MOSI;
	wire [3:0]  iSEL;
	wire [2:0]  iCTI;
	wire [1:0]  iBTE;

	wire [31:0] iDAT_MISO = memory[iADR[AW-1:0]];
	wire        iACK = iCYC & iSTB;   // 0 wait states

	// ---------------- data bus ----------------
	wire        dCYC, dSTB, dWE;
	wire [29:0] dADR;
	wire [31:0] dDAT_MOSI;
	wire [3:0]  dSEL;
	wire [2:0]  dCTI;
	wire [1:0]  dBTE;

	wire [31:0] dDAT_MISO = memory[dADR[AW-1:0]];
	wire        dACK = dCYC & dSTB;   // 0 wait states

	integer cycle_counter = 0;
	always @(posedge clk) if (!reset) cycle_counter <= cycle_counter + 1;

	always @(posedge clk) begin
		if (!reset && dCYC && dSTB && dWE) begin
			if (dADR == ADR_CONSOLE) begin
				$write("%c", dDAT_MOSI[7:0]);
				$fflush();
			end else if (dADR == ADR_EXIT) begin
				$display("\n[sim_finish after %0d cycles]", cycle_counter);
				$finish;
			end else begin
				if (dSEL[0]) memory[dADR[AW-1:0]][7:0]   <= dDAT_MOSI[7:0];
				if (dSEL[1]) memory[dADR[AW-1:0]][15:8]  <= dDAT_MOSI[15:8];
				if (dSEL[2]) memory[dADR[AW-1:0]][23:16] <= dDAT_MOSI[23:16];
				if (dSEL[3]) memory[dADR[AW-1:0]][31:24] <= dDAT_MOSI[31:24];
			end
		end
	end

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
		.reset                 (reset)
	);

	reg [31:0] timeout_cycles;
	initial begin
		if (!$value$plusargs("timeout=%d", timeout_cycles))
			timeout_cycles = 50_000_000;
		repeat (timeout_cycles) @(posedge clk);
		$display("\nTIMEOUT after %0d cycles", cycle_counter);
		$finish;
	end
endmodule
