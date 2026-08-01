// Copyright (c) 2023 Adam Gastineau
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

function integer rtoi(input integer x);
  return x;
endfunction

`define CEIL(x) ((rtoi(x) > x) ? rtoi(x) : rtoi(x) + 1)

// NOTE: Originally agg23's continuous-burst-READ controller (sdram_burst.sv).
// EXTENDED for HarpMudd.starwars (vector framebuffer): the single-word write
// path is replaced by a unified BURST-WRITE path so the per-frame framebuffer
// clear can zero ~358K words within one frame (single-word writes cannot).
//
// Because the SDRAM mode register has one write-burst bit, ALL writes are now
// full-page bursts terminated by manual precharge:
//   * a plot pixel  = a length-1 burst (byte-masked via p0_byte_en)
//   * a clear chunk = a length-N burst of constant p0_data (use 0, BE=11)
// One WRITE command, then N-1 data beats stream to auto-incrementing columns,
// then DQM-mask + tWR + PRECHARGE + tRP. Keep clear chunks <= 512 words so an
// auto-refresh can run between chunks (a full 1024-word burst outlasts tREF).
// Reads are unchanged. See DESIGN.md sec.3.
module sdram_fb #(
    parameter CLOCK_SPEED_MHZ = 0,
    parameter BURST_TYPE = 0,  // 1 for interleaved
    parameter CAS_LATENCY = 2,  // 1, 2, or 3 cycle delays
    parameter WRITE_BURST = 1  // MUST be 1 here: writes are full-page bursts
) (
    input wire clk,
    input wire reset,  // Used to trigger start of FSM
    output wire init_complete,  // SDRAM is done initializing

    // Port 0
    input  wire [24:0] p0_addr,
    input  wire [15:0] p0_data,
    input  wire [ 1:0] p0_byte_en,  // Byte enable for writes
    input  wire [10:0] p0_wr_len,   // # of words to write this burst (1..512)
    output wire [15:0] p0_q,

    // Streaming-write source: when p0_wr_stream is set on a write request, each
    // burst beat's data comes from wsrc_q (a caller BRAM) at address wsrc_addr,
    // instead of the constant p0_data. The controller drives wsrc_addr ~2 cycles
    // ahead of each beat (BRAM read latency 1) so the data is ready in time.
    // (Used for the phosphor copy-fade: write a faded line, word by word.)
    input  wire        p0_wr_stream,
    output wire [10:0] wsrc_addr,
    input  wire [15:0] wsrc_q,

    input wire p0_wr_req,
    input wire p0_rd_req,
    input wire p0_end_burst_req,

    output wire p0_available,  // The port is able to be used
    output reg p0_ready = 0,  // The port has finished its task. Will rise for a single cycle
    output wire p0_data_available,

    inout  wire [15:0] SDRAM_DQ,    // Bidirectional data bus
    output reg  [12:0] SDRAM_A,     // Address bus
    output reg  [ 1:0] SDRAM_DQM,   // High/low byte mask
    output reg  [ 1:0] SDRAM_BA,    // Bank select (single bits)
    output wire        SDRAM_nCS,   // Chip select, neg triggered
    output wire        SDRAM_nWE,   // Write enable, neg triggered
    output wire        SDRAM_nRAS,  // Select row address, neg triggered
    output wire        SDRAM_nCAS,  // Select column address, neg triggered
    output reg         SDRAM_CKE,   // Clock enable
    output wire        SDRAM_CLK    // Chip clock
);
  // Config values
  // NOTE: These are configured by default for the Pocket's SDRAM
  localparam SETTING_INHIBIT_DELAY_MICRO_SEC = 100;

  // tCK - Min clock cycle time
  localparam SETTING_T_CK_MIN_CLOCK_CYCLE_TIME_NANO_SEC = 6;

  // tRAS - Min row active time
  localparam SETTING_T_RAS_MIN_ROW_ACTIVE_TIME_NANO_SEC = 48;

  // tRC - Min row cycle time
  localparam SETTING_T_RC_MIN_ROW_CYCLE_TIME_NANO_SEC = 60;

  // tRP - Min precharge command period
  localparam SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC = 18;

  // tRFC - Min autorefresh period
  localparam SETTING_T_RFC_MIN_AUTOREFRESH_PERIOD_NANO_SEC = 80;

  // tRC - Min active to active command period for the same bank
  localparam SETTING_T_RC_MIN_ACTIVE_TO_ACTIVE_PERIOD_NANO_SEC = 60;

  // tRCD - Min read/write delay
  localparam SETTING_T_RCD_MIN_READ_WRITE_DELAY_NANO_SEC = 18;

  // tWR - Min write auto precharge recovery time
  localparam SETTING_T_WR_MIN_WRITE_AUTO_PRECHARGE_RECOVERY_NANO_SEC = 15;

  // tMRD - Min number of clock cycles between mode set and normal usage
  localparam SETTING_T_MRD_MIN_LOAD_MODE_CLOCK_CYCLES = 2;

  // MiSTer values
  // // tCK - Min clock cycle time
  // localparam SETTING_T_CK_MIN_CLOCK_CYCLE_TIME_NANO_SEC = 7;

  // // tRAS - Min row active time
  // localparam SETTING_T_RAS_MIN_ROW_ACTIVE_TIME_NANO_SEC = 48;

  // // tRC - Min row cycle time
  // localparam SETTING_T_RC_MIN_ROW_CYCLE_TIME_NANO_SEC = 63;

  // // tRP - Min precharge command period
  // localparam SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC = 21;

  // // tRFC - Min autorefresh period
  // localparam SETTING_T_RFC_MIN_AUTOREFRESH_PERIOD_NANO_SEC = 80;

  // // tRC - Min active to active command period for the same bank
  // localparam SETTING_T_RC_MIN_ACTIVE_TO_ACTIVE_PERIOD_NANO_SEC = 63;

  // // tRCD - Min read/write delay
  // localparam SETTING_T_RCD_MIN_READ_WRITE_DELAY_NANO_SEC = 21;

  // // tWR - Min write auto precharge recovery time
  // localparam SETTING_T_WR_MIN_WRITE_AUTO_PRECHARGE_RECOVERY_NANO_SEC = 15;

  // // tMRD - Min number of clock cycles between mode set and normal usage
  // localparam SETTING_T_MRD_MIN_LOAD_MODE_CLOCK_CYCLES = 14;

  // 8,192 refresh commands every 64ms = 7.8125us, which we round to 7500ns to make sure we hit them all
  localparam SETTING_REFRESH_TIMER_NANO_SEC = 7500;

  // Reads will be delayed by 1 cycle when enabled
  // Highly recommended that you use with SDRAM with FAST_INPUT_REGISTER enabled for timing and stability
  // This makes read timing incompatible with the test model
  localparam SETTING_USE_FAST_INPUT_REGISTER = 1;

  // The number of bits in a column in the SDRAM. It's 10 for the Pocket's 64MB (512Mb x16) chips
  localparam SETTING_COLUMN_BITS = 10;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Generated parameters

  localparam CLOCK_PERIOD_NANO_SEC = 1000.0 / CLOCK_SPEED_MHZ;

  // Number of cycles after reset until we start command inhibit
  localparam CYCLES_UNTIL_START_INHIBIT =
  `CEIL(SETTING_INHIBIT_DELAY_MICRO_SEC * 500 / CLOCK_PERIOD_NANO_SEC);
  // Number of cycles after reset until we clear command inhibit and start operation
  // We add 100 cycles for good measure
  localparam CYCLES_UNTIL_CLEAR_INHIBIT = 100 +
  `CEIL(SETTING_INHIBIT_DELAY_MICRO_SEC * 1000 / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles for precharge duration
  localparam CYCLES_FOR_PRECHARGE =
  `CEIL(SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles for autorefresh duration
  localparam CYCLES_FOR_AUTOREFRESH =
  `CEIL(SETTING_T_RFC_MIN_AUTOREFRESH_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles between two active commands to the same bank
  localparam CYCLES_BETWEEN_ACTIVE_COMMAND =
  `CEIL(SETTING_T_RC_MIN_ACTIVE_TO_ACTIVE_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles after active command before a read/write can be executed
  localparam CYCLES_FOR_ACTIVE_ROW =
  `CEIL(SETTING_T_RCD_MIN_READ_WRITE_DELAY_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles after write before next command
  localparam CYCLES_AFTER_WRITE_FOR_NEXT_COMMAND =
  `CEIL(
      (SETTING_T_WR_MIN_WRITE_AUTO_PRECHARGE_RECOVERY_NANO_SEC + SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC) / CLOCK_PERIOD_NANO_SEC);

  // tWR only: cycles to wait after the last burst-write data beat before we are
  // allowed to issue the manual PRECHARGE that terminates the write burst.
  localparam CYCLES_FOR_WRITE_RECOVERY =
  `CEIL(SETTING_T_WR_MIN_WRITE_AUTO_PRECHARGE_RECOVERY_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  // Number of cycles between each autorefresh command
  localparam CYCLES_PER_REFRESH =
  `CEIL(SETTING_REFRESH_TIMER_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  ////////////////////////////////////////////////////////////////////////////////////////
  // Init helpers
  // Number of cycles after reset until we are done with precharge
  // We add 10 cycles for good measure
  localparam CYCLES_UNTIL_INIT_PRECHARGE_END = 10 + CYCLES_UNTIL_CLEAR_INHIBIT +
  `CEIL(SETTING_T_RP_MIN_PRECHARGE_CMD_PERIOD_NANO_SEC / CLOCK_PERIOD_NANO_SEC);

  localparam CYCLES_UNTIL_REFRESH1_END = CYCLES_UNTIL_INIT_PRECHARGE_END + CYCLES_FOR_AUTOREFRESH;
  localparam CYCLES_UNTIL_REFRESH2_END = CYCLES_UNTIL_REFRESH1_END + CYCLES_FOR_AUTOREFRESH;

  localparam COLUMN_BITS_16 = {{(16 - SETTING_COLUMN_BITS) {1'b0}}, {SETTING_COLUMN_BITS{1'b1}}};

  // Measures when auto refresh needs to be triggered (declared early so strict
  // SV compilers/simulators don't trip on the forward use just below).
  reg [15:0] refresh_counter = 0;
  wire [15:0] burst_refresh_amount = refresh_counter + COLUMN_BITS_16 + 16'h10;

  wire [2:0] concrete_burst_length = 3'h7;
  // Reserved, write burst, operating mode, CAS latency, burst type, burst length
  wire [12:0] configured_mode = {
    3'b0, ~WRITE_BURST[0], 2'b0, CAS_LATENCY[2:0], BURST_TYPE[0], concrete_burst_length
  };

  typedef struct {
    reg [9:0]  port_addr;
    reg [15:0] port_data;
    reg [1:0]  port_byte_en;
  } port_selection;

  // nCS, nRAS, nCAS, nWE
  typedef enum bit [3:0] {
    COMMAND_NOP           = 4'b0111,
    COMMAND_ACTIVE        = 4'b0011,
    COMMAND_READ          = 4'b0101,
    COMMAND_WRITE         = 4'b0100,
    COMMAND_PRECHARGE     = 4'b0010,
    COMMAND_AUTO_REFRESH  = 4'b0001,
    COMMAND_LOAD_MODE_REG = 4'b0000
  } command;

  ////////////////////////////////////////////////////////////////////////////////////////
  // State machine

  typedef enum bit [2:0] {
    INIT,
    IDLE,
    DELAY,
    WRITE,
    WRITE_STREAM,   // stream remaining burst-write data beats
    WRITE_RECOVER,  // after tWR, issue the terminating PRECHARGE
    READ,
    READ_OUTPUT
  } state_fsm;

  state_fsm state;

  // TODO: Could use fewer bits
  reg [31:0] delay_counter = 0;
  // The number of words we're reading
  reg [3:0] read_counter = 0;

  reg [1:0] active_port = 0;

  state_fsm delay_state;
  // If true, refresh after delay
  reg delay_refresh = 0;

  typedef enum bit [1:0] {
    IO_NONE,
    IO_WRITE,
    IO_READ
  } io_operation;

  io_operation current_io_operation;

  command sdram_command;
  assign {SDRAM_nCS, SDRAM_nRAS, SDRAM_nCAS, SDRAM_nWE} = sdram_command;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Port specifics

  // Cache the signals we received, potentially while busy
  reg p0_wr_queue = 0;
  reg p0_rd_queue = 0;
  reg [1:0] p0_byte_en_queue = 0;
  reg [24:0] p0_addr_queue = 0;
  reg [15:0] p0_data_queue = 0;
  reg [10:0] p0_wr_len_queue = 0;
  reg        p0_wr_stream_queue = 0;

  // Remaining burst-write data beats after the first (issued in WRITE state)
  reg [10:0] wr_beats_remaining = 0;

  // Streaming-write source address pipeline. wsrc_cnt leads each beat so the
  // 1-cycle BRAM read (wsrc_q) is ready when sdram_data is latched.
  reg [10:0] wsrc_cnt = 0;
  reg        wsrc_run = 0;
  assign wsrc_addr = wsrc_cnt;

  wire p0_req = p0_wr_req || p0_rd_req;
  wire p0_req_queue = p0_wr_queue || p0_rd_queue;
  // The current p0 address that should be used for any operations on this first cycle only
  wire [24:0] p0_addr_current = p0_req_queue ? p0_addr_queue : p0_addr;

  // An active new request or cached request
  wire port_req = p0_req || p0_req_queue;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Burst handling

  reg [24:0] burst_addr = 0;
  reg [3:0] extra_burst_data_cycles = 0;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Helpers

  // Precharges all banks and close them from use
  task set_precharge_command();
    sdram_command <= COMMAND_PRECHARGE;

    // Mark all banks for refresh
    SDRAM_A[10]   <= 1;
  endtask

  // Activates a row
  task set_active_command(reg [1:0] port, reg [24:0] addr);
    sdram_command <= COMMAND_ACTIVE;

    // Upper two bits choose the bank
    SDRAM_BA <= addr[24:23];

    // Row address
    SDRAM_A <= addr[22:10];

    active_port <= port;
    // Current construction takes two cycles to write next data
    delay_counter <= CYCLES_FOR_ACTIVE_ROW > 32'h2 ? CYCLES_FOR_ACTIVE_ROW - 32'h2 : 32'h0;
  endtask

  task set_autorefresh_command();
    state <= DELAY;
    delay_state <= IDLE;
    delay_counter <= CYCLES_FOR_AUTOREFRESH - 32'h2;

    refresh_counter <= 0;

    sdram_command <= COMMAND_AUTO_REFRESH;
  endtask

  task read_port_0_start();
    state <= DELAY;
    delay_state <= READ;

    burst_addr <= p0_addr;

    current_io_operation <= IO_READ;

    set_active_command(0, p0_addr_current);
  endtask

  function port_selection get_active_port();
    port_selection selection;

    selection.port_addr = 10'h0;
    selection.port_data = 16'h0;
    selection.port_byte_en = 2'h0;

    case (active_port)
      0: begin
        selection.port_addr = p0_addr_queue[9:0];
        selection.port_data = p0_data_queue;
        selection.port_byte_en = p0_byte_en_queue;
      end
    endcase

    return selection;
  endfunction

  reg dq_output = 0;

  reg [15:0] sdram_data = 0;
  assign SDRAM_DQ = dq_output ? sdram_data : 16'hZZZZ;
  assign p0_q = SDRAM_DQ;

  assign init_complete = state != INIT;

  assign p0_available = state == IDLE && ~port_req;

  wire next_cycle_is_read = state == DELAY && delay_state == READ_OUTPUT && delay_counter == 0;
  assign p0_data_available = next_cycle_is_read || state == READ_OUTPUT || extra_burst_data_cycles > 0;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Process

  // Optimizations for better timing
  reg needs_refresh = 0;
  reg needs_burst_refresh = 0;

  always @(posedge clk) begin
    if (reset) begin
      // 2. Assert and hold CKE at logic low
      SDRAM_CKE <= 0;

      delay_counter <= 0;

      delay_state <= IDLE;
      current_io_operation <= IO_NONE;

      sdram_command <= COMMAND_NOP;

      p0_ready <= 0;

      p0_wr_queue <= 0;
      p0_rd_queue <= 0;
      wsrc_run    <= 0;
      wsrc_cnt    <= 0;

      dq_output <= 0;
    end else begin
      needs_refresh <= refresh_counter >= CYCLES_PER_REFRESH[15:0];
      needs_burst_refresh <= burst_refresh_amount >= CYCLES_PER_REFRESH[15:0];

      // streaming-write source counter: free-runs while a streaming burst is
      // active so wsrc_addr leads each beat (see WRITE / WRITE_STREAM).
      if (wsrc_run) wsrc_cnt <= wsrc_cnt + 11'd1;

      // Cache port 0 input values
      if (p0_wr_req) begin
        p0_wr_queue <= 1;

        p0_byte_en_queue <= p0_byte_en;
        p0_addr_queue <= p0_addr;
        p0_data_queue <= p0_data;
        p0_wr_len_queue <= (p0_wr_len == 11'd0) ? 11'd1 : p0_wr_len;
        p0_wr_stream_queue <= p0_wr_stream;
      end else if (p0_rd_req) begin
        p0_rd_queue   <= 1;

        p0_addr_queue <= p0_addr;
      end

      // Default to NOP at all times in between commands
      // NOP
      sdram_command <= COMMAND_NOP;

      if (state != INIT) begin
        refresh_counter <= refresh_counter + 16'h1;
      end

      if (extra_burst_data_cycles > 0) begin
        extra_burst_data_cycles <= extra_burst_data_cycles - 4'h1;
      end

      case (state)
        INIT: begin
          delay_counter <= delay_counter + 32'h1;

          if (delay_counter == CYCLES_UNTIL_START_INHIBIT) begin
            // Start setting inhibit
            // 5. Starting at some point during this 100us period, bring CKE high
            SDRAM_CKE <= 1;

            // We're already asserting NOP above
          end else if (delay_counter == CYCLES_UNTIL_CLEAR_INHIBIT) begin
            // Clear inhibit, start precharge
            set_precharge_command();
          end else if (delay_counter == CYCLES_UNTIL_INIT_PRECHARGE_END || delay_counter == CYCLES_UNTIL_REFRESH1_END) begin
            // Precharge done (or first auto refresh), auto refresh
            // CKE high specifies auto refresh
            SDRAM_CKE <= 1;

            sdram_command <= COMMAND_AUTO_REFRESH;
          end else if (delay_counter == CYCLES_UNTIL_REFRESH2_END) begin
            // Second auto refresh done, load mode register
            sdram_command <= COMMAND_LOAD_MODE_REG;

            SDRAM_BA <= 2'b0;

            SDRAM_A <= configured_mode;
          end else if (delay_counter == CYCLES_UNTIL_REFRESH2_END + SETTING_T_MRD_MIN_LOAD_MODE_CLOCK_CYCLES) begin
            // We can now execute commands
            state <= IDLE;
          end
        end
        IDLE: begin
          // Stop outputting on DQ and hold in high Z
          dq_output <= 0;

          p0_ready <= 0;

          current_io_operation <= IO_NONE;

          if (needs_refresh) begin
            // Trigger refresh
            set_autorefresh_command();
          end else if (p0_wr_req || p0_wr_queue) begin
            // Port 0 write
            state <= DELAY;
            delay_state <= WRITE;

            current_io_operation <= IO_WRITE;

            // Clear queued action
            p0_wr_queue <= 0;

            // Streaming write: start the source-address pipeline NOW (cycle A)
            // so wsrc_addr = 0 at A+1 and wsrc_q = src[0] is ready at the WRITE
            // beat (A+2) -- assumes the 1-cycle tRCD DELAY at this clock.
            if (p0_req_queue ? p0_wr_stream_queue : p0_wr_stream) begin
              wsrc_cnt <= 11'd0;
              wsrc_run <= 1'b1;
            end

            set_active_command(0, p0_addr_current);
          end else if (p0_rd_req || p0_rd_queue) begin
            // Port 0 read
            // reg [15:0] column_bits;
            // column_bits = {{(16 - SETTING_COLUMN_BITS) {1'b0}}, {SETTING_COLUMN_BITS{1'b1}}};

            if (p0_rd_req && needs_burst_refresh) begin
              // We're about to take a long time, so check if we have refreshes soon, and run them
              // We need to refresh, so we will queue the read (automatically, by p0_rd_queue)
              set_autorefresh_command();
            end else begin
              // Refresh is satisfied, start
              read_port_0_start();
            end
          end
        end
        DELAY: begin
          if (delay_counter > 0) begin
            delay_counter <= delay_counter - 32'h1;
          end else if (delay_refresh) begin
            delay_refresh <= 0;

            set_autorefresh_command();
          end else begin
            state <= delay_state;
            delay_state <= IDLE;

            if (delay_state == IDLE && current_io_operation != IO_NONE) begin
              case (active_port)
                0: p0_ready <= 1;
              endcase
            end
          end
        end
        WRITE: begin
          // First beat of a BURST write. Issue one WRITE command (NO auto
          // precharge, A[10]=0) at the start column; the chip then writes one
          // data word per clock to auto-incrementing columns until we stop the
          // burst with a manual PRECHARGE. Burst length is wr_beats+1.
          port_selection active_port_entries;

          active_port_entries = get_active_port();

          sdram_command <= COMMAND_WRITE;

          // A[10]=0 -> manual precharge (full-page burst forbids auto precharge)
          SDRAM_A <= {2'b0, 1'b0, active_port_entries.port_addr};

          // Drive the first data beat. Streaming -> word from wsrc_q (DQM=00,
          // full word); else the constant p0_data with its byte enable.
          dq_output <= 1;
          sdram_data <= p0_wr_stream_queue ? wsrc_q : active_port_entries.port_data;
          SDRAM_DQM  <= p0_wr_stream_queue ? 2'b00 : ~active_port_entries.port_byte_en;

          // Remaining beats after this first one
          wr_beats_remaining <= p0_wr_len_queue - 11'd1;

          state <= WRITE_STREAM;
        end
        WRITE_STREAM: begin
          // Constant mode: sdram_data holds (same word to every column -- used
          // for clears). Streaming mode: pull the next word from wsrc_q each beat
          // (wsrc_addr was driven 1 cycle ahead, so wsrc_q is the right word now).
          if (wr_beats_remaining != 11'd0) begin
            wr_beats_remaining <= wr_beats_remaining - 11'd1;
            if (p0_wr_stream_queue) sdram_data <= wsrc_q;
          end else begin
            // All data beats placed. Mask any further write, wait tWR, then we
            // will precharge in WRITE_RECOVER.
            SDRAM_DQM <= 2'b11;
            dq_output <= 0;
            wsrc_run  <= 1'b0;
            state <= DELAY;
            delay_state <= WRITE_RECOVER;
            delay_counter <= CYCLES_FOR_WRITE_RECOVERY;
          end
        end
        WRITE_RECOVER: begin
          // tWR satisfied -> terminate the burst with a precharge, then tRP.
          set_precharge_command();
          state <= DELAY;
          delay_state <= IDLE;
          delay_counter <= CYCLES_FOR_PRECHARGE;
        end
        READ: begin
          // Read to the selected row
          port_selection active_port_entries;

          if (CAS_LATENCY == 1 && ~SETTING_USE_FAST_INPUT_REGISTER) begin
            // Go directly to read
            state <= READ_OUTPUT;
          end else begin
            state <= DELAY;
            delay_state <= READ_OUTPUT;

            read_counter <= 0;

            // Takes one cycle to go to read data, and the user can read after that
            // Fast input register delays operation by a cycle
            delay_counter <= CAS_LATENCY - 32'h1 + SETTING_USE_FAST_INPUT_REGISTER;
          end

          active_port_entries = get_active_port();

          // Clear queued action
          p0_rd_queue <= 0;

          sdram_command <= COMMAND_READ;

          // NOTE: Bank is still set from ACTIVE command assertion
          // Don't enable auto precharge
          SDRAM_A <= {2'b0, 1'b0, active_port_entries.port_addr};

          // Fetch all bytes
          SDRAM_DQM <= 2'b0;
        end
        READ_OUTPUT: begin
          reg [127:0] temp;
          reg [3:0] expected_count;
          reg [SETTING_COLUMN_BITS-1:0] stop_burst_addr;

          stop_burst_addr = {SETTING_COLUMN_BITS{1'b1}} - (CAS_LATENCY - 1);

          burst_addr <= burst_addr + 25'h1;

          if (burst_addr[SETTING_COLUMN_BITS-1:0] + 'h1 == stop_burst_addr || p0_end_burst_req) begin
            // Stop burst 3 cycles before edge of page
            // Precharge
            set_precharge_command();

            extra_burst_data_cycles <= CAS_LATENCY - 1;

            state <= DELAY;
            delay_counter <= CYCLES_FOR_PRECHARGE;
          end

          if (needs_refresh && stop_burst_addr - burst_addr[SETTING_COLUMN_BITS-1:0] > 'h10) begin
            // We need to refresh
            // Only refresh if we're not close to the end of a page. If we're close to a page boundary,
            // we will stop the burst automatically (or the user will), so a refresh will happen anyway

            // Precharge to stop the burst
            set_precharge_command();

            extra_burst_data_cycles <= CAS_LATENCY - 1;

            state <= DELAY;
            delay_counter <= CYCLES_FOR_PRECHARGE;
            // Refresh after delay
            delay_refresh <= 1;
          end
        end
      endcase
    end
  end

  // This DDIO block doesn't double the clock, it just relocates the RAM clock to trigger
  // on the negative edge. Stubbed under `SIM` (the Cyclone V primitive isn't in
  // the sim libs and the SDRAM clock pin is irrelevant to logic validation).
`ifdef SIM
  assign SDRAM_CLK = ~clk;
`else
  altddio_out #(
      .extend_oe_disable("OFF"),
      .intended_device_family("Cyclone V"),
      .invert_output("OFF"),
      .lpm_hint("UNUSED"),
      .lpm_type("altddio_out"),
      .oe_reg("UNREGISTERED"),
      .power_up_high("OFF"),
      .width(1)
  ) sdramclk_ddr (
      .datain_h(1'b0),
      .datain_l(1'b1),
      .outclock(clk),
      .dataout(SDRAM_CLK),
      .oe(1'b1),
      .outclocken(1'b1)
      // .aclr(),
      // .aset(),
      // .sclr(),
      // .sset()
  );
`endif

  ////////////////////////////////////////////////////////////////////////////////////////
  // Parameter validation

  initial begin
    $info("Instantiated SDRAM with the following settings");
    $info("  Clock speed %f, period %f", CLOCK_SPEED_MHZ, CLOCK_PERIOD_NANO_SEC);

    if (CLOCK_SPEED_MHZ <= 0 || CLOCK_PERIOD_NANO_SEC <= SETTING_T_CK_MIN_CLOCK_CYCLE_TIME_NANO_SEC) begin
      $error("Invalid clock speed. Quitting");
    end

    $info("--------------------");
    $info("Configured values:");
    $info("  CAS Latency %h", CAS_LATENCY);

    if (CAS_LATENCY != 1 && CAS_LATENCY != 2 && CAS_LATENCY != 3) begin
      $error("Unknown CAS latency");
    end

    $info("  Burst type %s",
          BURST_TYPE == 0 ? "Sequential" : BURST_TYPE == 1 ? "Interleaved" : "Unknown");

    if (BURST_TYPE != 0 && BURST_TYPE != 1) begin
      $error("Unknown burst type");
    end

    $info("  Write burst %s",
          WRITE_BURST == 0 ? "Single word write" : WRITE_BURST == 1 ? "Write burst" : "Unknown");

    if (WRITE_BURST != 0 && WRITE_BURST != 1) begin
      $error("Unknown write burst");
    end

    $info("--------------------");
    $info("Delays:");
    $info("  Cycles until start inhibit %f, clear inhibit %f", CYCLES_UNTIL_START_INHIBIT,
          CYCLES_UNTIL_CLEAR_INHIBIT);

    $info("  Cycles between autorefresh instances %f", CYCLES_PER_REFRESH);

    $info("  CYCLES_FOR_AUTOREFRESH %f", CYCLES_FOR_AUTOREFRESH);
    $info("  CYCLES_AFTER_WRITE_FOR_NEXT_COMMAND %f", CYCLES_AFTER_WRITE_FOR_NEXT_COMMAND);

    $info("  Cycles until between active commands %f, command duration %f",
          CYCLES_BETWEEN_ACTIVE_COMMAND, CYCLES_FOR_ACTIVE_ROW);
  end

endmodule
