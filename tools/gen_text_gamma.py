#!/usr/bin/env python3
"""Derive the CHAR coverage weight table for mp3_fb.sv, and prove it numerically.

THE DEFECT THIS FIXES
---------------------
The glyph compose path blended coverage LINEARLY over RGB565 code values:

    mix_r = fg_r * cov16 + bg_r * (16 - cov16)      // then >> 4

RGB565 values are sRGB -- gamma-encoded, roughly a 2.2 power law. Averaging two
gamma-encoded numbers does not average the LIGHT they represent. A 50%-coverage
edge pixel got 0.5*fg in CODE space, which emits about 0.22x the light it owes.
Every partially covered pixel therefore came out too dark, so on this UI (light
type on a dark ramp) stems read thin and the edge fell off faster than the eye
expects. That is perceived as "soft" or "not crisp" -- not as "too dark", which
is why it survived so long: nothing on screen looks obviously wrong.

WHAT IS ACTUALLY CHANGED
------------------------
Only the WEIGHT applied to each of the 16 coverage levels. The hardware still
does one linear interpolation, so this costs no extra multiplier and no memory:
16 five-bit constants land in logic. A true fix would convert to linear light,
blend, and re-encode -- three transcendentals per channel per pixel, which is
absurd for a 2D blitter. Remapping the weight recovers most of the error for a
LUT, provided the weights are fitted to the colours actually used.

Hence: this table is fitted against the REAL palette (the 12 accents, white,
the dim grey) over the REAL backgrounds (the graphite->black gradient and the
card), weighted by luma so an error in green counts for more than one in blue.
Refit it if the palette changes materially.

Coverage 0 and 15 are pinned to weights 0 and 16. Those two are exact today --
a fully covered pixel IS the foreground and an uncovered one IS the background.
Fitting them would trade an exact answer for a marginally better average, and
make solid glyph interiors and untouched background subtly wrong.

    python tools/gen_text_gamma.py            # report + Verilog to stdout
    python tools/gen_text_gamma.py --check    # verify mp3_fb.sv matches, exit 1 if not
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FB = ROOT / "src" / "fpga" / "core" / "mp3_fb.sv"

# ---- the colours this core actually draws --------------------------------
# Foregrounds: white body text, the dim artist line, every accent.
FG = [
    0xFFFF,  # UI_WHITE
    0x94B2,  # UI_DIM
    0xFC65, 0xBF08, 0xAF13, 0x7E55, 0x5D5C, 0x7C5A,   # accents 0..5
    0xAC5E, 0xF534, 0xF32D, 0xB9E9, 0xEE07, 0xEF1A,   # accents 6..11
]
# Backgrounds: the vertical ramp text sits on, plus the card.
GRAD_TOP, GRAD_BOT = 0x2124, 0x0000
UI_PANEL = 0x2945

# Rec.709 luma. Text legibility is carried by green far more than by blue, so
# an unweighted fit spends accuracy where the eye cannot collect it.
LUMA = (0.2126, 0.7152, 0.0722)


def mix565(a, b, t, n):
    """The firmware's own per-channel band blend (ui_mix), so the fitted
    backgrounds are the ones that genuinely appear rather than an idealisation."""
    ar, ag, ab = a >> 11, (a >> 5) & 0x3F, a & 0x1F
    br, bg, bb = b >> 11, (b >> 5) & 0x3F, b & 0x1F
    return (((ar + (br - ar) * t // n) << 11)
            | ((ag + (bg - ag) * t // n) << 5)
            | (ab + (bb - ab) * t // n))


def backgrounds():
    bgs = [mix565(GRAD_TOP, GRAD_BOT, i, 40) for i in range(41)]
    bgs.append(UI_PANEL)
    return bgs


def channels(c):
    """(value, max) per channel for an RGB565 colour."""
    return ((c >> 11, 31), ((c >> 5) & 0x3F, 63), (c & 0x1F, 31))


def to_linear(s):
    return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4


def to_srgb(l):
    return l * 12.92 if l <= 0.0031308 else 1.055 * (l ** (1 / 2.4)) - 0.055


def ideal(fv, fmax, bv, bmax, a):
    """Gamma-correct blend, returned in the destination channel's code units."""
    lf = to_linear(fv / fmax)
    lb = to_linear(bv / bmax)
    return to_srgb(a * lf + (1 - a) * lb) * fmax


def hw(fv, bv, w, cmax):
    """Exactly what the RTL computes: multiply, add, >>4, truncate to width."""
    v = (fv * w + bv * (16 - w)) >> 4
    return min(v, cmax)


def fit():
    bgs = backgrounds()
    table, report = [], []
    for cov in range(16):
        if cov == 0:
            table.append(0)
            report.append((0, 0, 0.0, 0.0))
            continue
        if cov == 15:
            table.append(16)
            report.append((15, 16, 0.0, 0.0))
            continue
        a = cov / 15.0
        best, best_err = None, None
        old_err = 0.0
        for w in range(17):
            err = 0.0
            for fg in FG:
                for bg in bgs:
                    for (fv, fmax), (bv, _bmax), lw in zip(
                            channels(fg), channels(bg), LUMA):
                        want = ideal(fv, fmax, bv, fmax, a)
                        got = hw(fv, bv, w, fmax)
                        # Normalise by the channel's own range so 6-bit green
                        # does not automatically dominate 5-bit red and blue.
                        err += lw * ((got - want) / fmax) ** 2
            if best_err is None or err < best_err:
                best, best_err = w, err
            if w == cov + (1 if cov == 15 else 0):   # the old weight
                old_err = err
        n = len(FG) * len(bgs) * 3
        table.append(best)
        report.append((cov, best, (old_err / n) ** 0.5, (best_err / n) ** 0.5))
    return table, report


def verilog(table):
    body = ", ".join("5'd%d" % w for w in reversed(table))
    return (
        "    // Coverage -> blend weight. GENERATED by tools/gen_text_gamma.py.\n"
        "    //\n"
        "    // NOT the identity, and deliberately so. RGB565 is gamma-encoded,\n"
        "    // so interpolating code values linearly does not interpolate LIGHT:\n"
        "    // a half-covered pixel weighted 8/16 emits ~22% of the foreground,\n"
        "    // not 50%. Under-lit edge pixels are what made light-on-dark text\n"
        "    // read thin and hazy. These weights are fitted to the real palette\n"
        "    // over the real background ramp -- see the tool for the derivation.\n"
        "    // 0 and 15 stay exact: an untouched pixel is bg, a solid one is fg.\n"
        "    //\n"
        "    // 16 five-bit constants; synthesises to logic, no M10K, no DSP.\n"
        "    function [4:0] cov_weight(input [3:0] c);\n"
        "        case (c)\n"
        + "".join("            4'd%-2d: cov_weight = 5'd%d;\n" % (c, w)
                  for c, w in enumerate(table))
        + "        endcase\n"
        "    endfunction\n"
    ) if False else (
        "    function [4:0] cov_weight(input [3:0] c);\n"
        "        case (c)\n"
        + "".join("            4'd%-2d: cov_weight = 5'd%d;\n" % (c, w)
                  for c, w in enumerate(table))
        + "        endcase\n"
        "    endfunction\n"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify mp3_fb.sv carries this exact table")
    args = ap.parse_args()

    table, report = fit()

    print("coverage   old w   new w   RMS err before   after    improvement")
    print("-" * 66)
    for cov, w, old, new in report:
        if cov in (0, 15):
            print("   %2d       %2d      %2d      exact            exact"
                  % (cov, 0 if cov == 0 else 16, w))
            continue
        gain = (old / new) if new > 1e-9 else float("inf")
        print("   %2d       %2d      %2d      %7.4f        %7.4f  %5.2fx"
              % (cov, cov, w, old, new, gain))
    print()
    print("table: " + ", ".join(str(w) for w in table))

    if args.check:
        src = FB.read_text(encoding="utf-8")
        found = [int(m) for m in re.findall(
            r"4'd\d+\s*:\s*cov_weight\s*=\s*5'd(\d+);", src)]
        if found != table:
            print("\nMISMATCH: mp3_fb.sv has %s" % (found or "no table"),
                  file=sys.stderr)
            return 1
        print("\nmp3_fb.sv matches.")
        return 0

    print()
    print(verilog(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
