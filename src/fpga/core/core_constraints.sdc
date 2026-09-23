#
# user core constraints — MP3 Player Pocket core
#
# The header used to read "Moon Patrol Pocket core" and list that port's
# clocks. It was copied in at scaffold time and never updated: none of those
# four frequencies were ever this core's. The CONSTRAINTS below were always
# right -- they name the PLL by instance path, not by frequency, which is why
# a wrong comment survived in a file about clocks without breaking anything.
#
# All clock domains are asynchronous to each other.
# ic = core_top instance in apf_top; mp1 = PLL instance in core_top.
# Four PLL outputs: [0] clk_sys 66.667M  [1] clk_vid 12M  [2] clk_vid_90 12M
#                   [3] clk_sdram 100M
#
# clk_sys is quantised: all four share one VCO, and the 12 MHz pixel clock
# (exactly 60.000 Hz) and the 100 MHz SDRAM clock pin it at 600 MHz. So
# clk_sys can only be 600/N -- 60, 66.667 or 75, and nothing between. Timing
# closes at 66.667 with ~1.5 ns; 75 is above the ~75.4 MHz real fmax.
#
# derive_pll_clocks in apf_constraints.sdc means these follow the PLL
# automatically -- changing the frequency needs no edit here.

set_clock_groups -asynchronous \
 -group { bridge_spiclk } \
 -group { clk_74a } \
 -group { clk_74b } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[1].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[2].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mp1|mf_pllbase_inst|altera_pll_i|general[3].gpll~PLL_OUTPUT_COUNTER|divclk } \
 -group { ic|mclk_r }
