`timescale 1ns/1ps
// Stage 0 benchmark harness: plain (non-AXI) PicoRV32 + a simple 1MB memory
// model + a memory-mapped print port at 0x10000000 (same convention PicoRV32's
// own reference firmware uses). Loads whichever firmware.hex is passed via
// +firmware=path, runs until the core traps (ebreak), then reports.

module tb_picorv32;
	reg clk = 0;
	reg resetn = 0;
	always #5 clk = ~clk;

	initial begin
		repeat (10) @(posedge clk);
		resetn <= 1;
	end

	wire        mem_valid;
	wire        mem_instr;
	reg         mem_ready;
	wire [31:0] mem_addr;
	wire [31:0] mem_wdata;
	wire [3:0]  mem_wstrb;
	reg  [31:0] mem_rdata;
	wire        trap;

	localparam MEM_WORDS = 1024*1024/4;
	reg [31:0] memory [0:MEM_WORDS-1];

	reg [1023:0] firmware_file;
	initial begin
		if (!$value$plusargs("firmware=%s", firmware_file))
			firmware_file = "fw_helix/fw_helix.hex";
		$readmemh(firmware_file, memory);
	end

`ifdef ZERO_WAIT
	// 0-wait-state memory: combinational read + combinational ready. Models
	// BRAM driven via picorv32's look-ahead interface, and matches the memory
	// model used in tb_vexriscv.v so CPU comparisons are like-for-like.
	// Explicit sensitivity list, NOT `always @*`: with @* Icarus derives a
	// sensitivity list covering all 262144 memory words and elaboration hangs.
	always @(mem_valid or mem_addr) begin
		mem_ready = mem_valid;
		mem_rdata = memory[mem_addr[21:2]];
	end
	always @(posedge clk) begin
		if (mem_valid) begin
			if (mem_addr == 32'h8000_0000) begin
				if (mem_wstrb != 0) begin $write("%c", mem_wdata[7:0]); $fflush(); end
			end else if (mem_addr == 32'h8000_0004) begin
				if (mem_wstrb != 0) begin
					$display("\n[sim_finish]");
					$finish;
				end
			end else begin
				if (mem_wstrb[0]) memory[mem_addr[21:2]][7:0]   <= mem_wdata[7:0];
				if (mem_wstrb[1]) memory[mem_addr[21:2]][15:8]  <= mem_wdata[15:8];
				if (mem_wstrb[2]) memory[mem_addr[21:2]][23:16] <= mem_wdata[23:16];
				if (mem_wstrb[3]) memory[mem_addr[21:2]][31:24] <= mem_wdata[31:24];
			end
		end
	end
`else
	// 1-wait-state memory (registered ready). Original model.
	always @(posedge clk) begin
		mem_ready <= 0;
		if (mem_valid && !mem_ready) begin
			mem_ready <= 1;
			if (mem_addr == 32'h8000_0000) begin
				if (mem_wstrb != 0) begin $write("%c", mem_wdata[7:0]); $fflush(); end
			end else if (mem_addr == 32'h8000_0004) begin
				if (mem_wstrb != 0) begin
					$display("\n[sim_finish]");
					$finish;
				end
			end else begin
				if (mem_wstrb[0]) memory[mem_addr[21:2]][7:0]   <= mem_wdata[7:0];
				if (mem_wstrb[1]) memory[mem_addr[21:2]][15:8]  <= mem_wdata[15:8];
				if (mem_wstrb[2]) memory[mem_addr[21:2]][23:16] <= mem_wdata[23:16];
				if (mem_wstrb[3]) memory[mem_addr[21:2]][31:24] <= mem_wdata[31:24];
				mem_rdata <= memory[mem_addr[21:2]];
			end
		end
	end
`endif

	// Helix is multiply-bound (MULSHIFT32 -> mulh everywhere in the filterbank/
	// IMDCT/dequant). ENABLE_MUL selects picorv32's *serial* multiplier (tens of
	// cycles per multiply); ENABLE_FAST_MUL selects the DSP-block one. Build with
	// -DFAST_MUL to measure the difference.
	picorv32 #(
		.ENABLE_COUNTERS(1),
		.ENABLE_COUNTERS64(1),
`ifdef FAST_MUL
		.ENABLE_MUL(0),
		.ENABLE_FAST_MUL(1),
		.BARREL_SHIFTER(1),
`else
		.ENABLE_MUL(1),
`endif
		.ENABLE_DIV(1),
		.ENABLE_IRQ(0)
	) cpu (
		.clk      (clk),
		.resetn   (resetn),
		.trap     (trap),
		.mem_valid(mem_valid),
		.mem_instr(mem_instr),
		.mem_ready(mem_ready),
		.mem_addr (mem_addr),
		.mem_wdata(mem_wdata),
		.mem_wstrb(mem_wstrb),
		.mem_rdata(mem_rdata),
		.mem_la_read(),
		.mem_la_write(),
		.mem_la_addr(),
		.mem_la_wdata(),
		.mem_la_wstrb(),
		.pcpi_valid(),
		.pcpi_insn(),
		.pcpi_rs1(),
		.pcpi_rs2(),
		.pcpi_wr(1'b0),
		.pcpi_rd(32'b0),
		.pcpi_wait(1'b0),
		.pcpi_ready(1'b0),
		.irq(32'b0),
		.eoi(),
		.trace_valid(),
		.trace_data()
	);

	reg [31:0] timeout_cycles;
	initial begin
		if (!$value$plusargs("timeout=%d", timeout_cycles))
			timeout_cycles = 50_000_000;
		repeat (timeout_cycles) @(posedge clk);
		$display("\nTIMEOUT");
		$finish;
	end

	always @(posedge clk) begin
		if (resetn && trap) begin
			$display("\n[trap - ebreak reached]");
			$finish;
		end
	end
endmodule
