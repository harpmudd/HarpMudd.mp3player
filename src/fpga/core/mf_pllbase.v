// mf_pllbase.v - PLL wrapper for the MP3 player core.
//
// 74.25 MHz in -> 60 MHz (clk_sys) + 12 MHz (clk_vid) + 12 MHz 90deg
//              + 100 MHz (SDRAM framebuffer)
//
// The header and the port comments below both described a different core and a
// different set of clocks -- inherited from the Moon Patrol scaffold this was
// started from, then left behind when clk_sys went 50 -> 60 MHz. The megafunction
// parameters are the truth; timing analysis reports outclk_0 at 60.02 MHz.
`timescale 1 ps / 1 ps
module mf_pllbase (
    input  wire  refclk,
    input  wire  rst,
    output wire  outclk_0,  // 60 MHz  - CPU / system clock (clk_sys)
    output wire  outclk_1,  // 12 MHz  - pixel clock (clk_vid)
    output wire  outclk_2,  // 12 MHz 90 deg - APF DDR pixel clock
    output wire  outclk_3,  // 100 MHz - SDRAM framebuffer controller
    output wire  locked
);

mf_pllbase_0002 mf_pllbase_inst (
    .refclk   (refclk),
    .rst      (rst),
    .outclk_0 (outclk_0),
    .outclk_1 (outclk_1),
    .outclk_2 (outclk_2),
    .outclk_3 (outclk_3),
    .locked   (locked)
);

endmodule
