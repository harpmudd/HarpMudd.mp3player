"""Generate the preset EQ coefficient table, and prove it before any RTL exists.

    python tools/gen_eq_coeffs.py            # report + ASCII response curves
    python tools/gen_eq_coeffs.py --verilog  # emit the table to paste into RTL

docs/EQ_DESIGN.md fixes the plan this implements, and the reason it is fixed:
a hand-written sine table for the VU needle produced a 45-degree sweep instead
of 100 and cost a hardware round. Filter coefficients are far less forgiving
than that was, so nothing here is written by hand.

What this does that a coefficient calculator does not:

  * Everything is measured on the QUANTISED integers, never on the float
    design. Q2.16 rounding shifts a filter's response, and the whole point is
    to know the shipped curve rather than the intended one.
  * The Q2.16 range is checked per coefficient and the script FAILS rather
    than silently wrapping. a1 legitimately approaches -2, which is exactly
    why the format has two integer bits, so the margin is worth watching.
  * The preamp is LOUDNESS-MATCHED (pink-weighted RMS), not peak-matched.
    Peak-matching cannot clip, but it left every preset 3-6 dB quieter than
    FLAT, which reads as "someone turned it down" rather than "the EQ is on"
    and loses every A/B on level alone. Loudness-matching keeps presets at the
    same apparent volume; the cost is that a tone near full scale in a boosted
    band can exceed full scale, so the output saturation is now load-bearing
    and MUST clamp rather than wrap. Both numbers are reported.
"""

import argparse
import cmath
import os
import math
import sys

FS = 48000.0            # fixed output rate; see EQ_DESIGN "what rate it runs at"
QF = 16                 # Q2.16 -> 16 fractional bits
QSCALE = 1 << QF
QMIN, QMAX = -(1 << 17), (1 << 17) - 1        # 18-bit signed

# Five bands: low shelf, three peaks, high shelf. Classic preset shape, and
# what the named curves in EQ_DESIGN are actually describing.
BANDS = [
    ("lowshelf",   80.0,   0.70),
    ("peak",       250.0,  1.00),
    ("peak",       1000.0, 1.00),
    ("peak",       4000.0, 1.00),
    ("highshelf",  12000.0, 0.70),
]

# Gains in dB per band. FLAT is a true bypass in the RTL, not a unity biquad --
# a unity biquad still rounds, so "off" would not be bit-identical to today.
PRESETS = [
    ("FLAT",      [0, 0, 0, 0, 0]),
    ("BASS",      [+6, +3, 0, -1, 0]),
    ("ROCK",      [+5, +2, -2, +2, +4]),
    ("POP",       [-1, 0, +2, +3, +1]),
    ("JAZZ",      [+3, +1, 0, -1, +2]),
    ("CLASSICAL", [0, 0, 0, 0, +2]),
    ("VOCAL",     [-3, -1, +3, +2, 0]),
    ("TREBLE",    [0, 0, 0, +2, +6]),
]


def design(kind, f0, q, gain_db):
    """RBJ audio EQ cookbook. Returns (b0,b1,b2,a1,a2) normalised by a0."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / FS
    cw, sw = math.cos(w0), math.sin(w0)

    if kind == "peak":
        alpha = sw / (2.0 * q)
        b0, b1, b2 = 1 + alpha * A, -2 * cw, 1 - alpha * A
        a0, a1, a2 = 1 + alpha / A, -2 * cw, 1 - alpha / A
    else:
        # Shelf slope S expressed through Q, per the cookbook's note.
        alpha = sw / 2.0 * math.sqrt((A + 1.0 / A) * (1.0 / q - 1.0) + 2.0)
        tsa = 2.0 * math.sqrt(A) * alpha
        if kind == "lowshelf":
            b0 = A * ((A + 1) - (A - 1) * cw + tsa)
            b1 = 2 * A * ((A - 1) - (A + 1) * cw)
            b2 = A * ((A + 1) - (A - 1) * cw - tsa)
            a0 = (A + 1) + (A - 1) * cw + tsa
            a1 = -2 * ((A - 1) + (A + 1) * cw)
            a2 = (A + 1) + (A - 1) * cw - tsa
        else:
            b0 = A * ((A + 1) + (A - 1) * cw + tsa)
            b1 = -2 * A * ((A - 1) + (A + 1) * cw)
            b2 = A * ((A + 1) + (A - 1) * cw - tsa)
            a0 = (A + 1) - (A - 1) * cw + tsa
            a1 = 2 * ((A - 1) - (A + 1) * cw)
            a2 = (A + 1) - (A - 1) * cw - tsa
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def quantise(c, where, errors):
    q = int(round(c * QSCALE))
    if not (QMIN <= q <= QMAX):
        errors.append("%s: %.6f -> %d is outside Q2.16 [%d,%d]"
                      % (where, c, q, QMIN, QMAX))
        q = max(QMIN, min(QMAX, q))
    return q


def response_db(qcoefs, f):
    """Magnitude of the QUANTISED cascade at f, in dB."""
    z = cmath.exp(-2j * math.pi * f / FS)
    h = 1.0 + 0j
    for (b0, b1, b2, a1, a2) in qcoefs:
        b0, b1, b2 = b0 / QSCALE, b1 / QSCALE, b2 / QSCALE
        a1, a2 = a1 / QSCALE, a2 / QSCALE
        num = b0 + b1 * z + b2 * z * z
        den = 1.0 + a1 * z + a2 * z * z
        if abs(den) < 1e-12:
            return float("inf")
        h *= num / den
    return 20.0 * math.log10(abs(h)) if abs(h) > 0 else -999.0


def loudness_preamp_db(qcoefs):
    """Preamp that keeps APPARENT loudness equal to bypass.

    Pink-weighted: equal energy per octave, which is roughly how music is
    distributed and how hearing integrates it. A peak-weighted preamp is the
    safe alternative and is reported alongside, but it makes every preset
    audibly quieter than FLAT."""
    num = den = 0.0
    for i in range(1, len(FREQS)):
        w = math.log(FREQS[i] / FREQS[i - 1])        # equal weight per octave
        h = 10.0 ** (response_db(qcoefs, FREQS[i]) / 20.0)
        num += h * h * w
        den += w
    return -10.0 * math.log10(num / den) if num > 0 else 0.0


def stable(qcoefs):
    """Poles inside the unit circle, checked on the quantised values."""
    for (_b0, _b1, _b2, a1, a2) in qcoefs:
        a1f, a2f = a1 / QSCALE, a2 / QSCALE
        disc = a1f * a1f - 4.0 * a2f
        if disc >= 0:
            r = [(-a1f + math.sqrt(disc)) / 2, (-a1f - math.sqrt(disc)) / 2]
            mags = [abs(x) for x in r]
        else:
            mags = [math.sqrt(a2f)] * 2      # complex pair, |p| = sqrt(a2)
        if max(mags) >= 1.0:
            return False
    return True


FREQS = [20 * (10 ** (i / 24.0)) for i in range(int(24 * math.log10(20000 / 20)) + 1)]


# On-screen curve, at the real meter geometry from fw/player.c.
UI_WAVE_N = 36
UI_WAVE_H = 72
CURVE_DB = 8.0          # full bar deflection; presets peak at +6


def curve_bars(qcoefs):
    """Signed pixel offset from the meter's centre line, per bar.

    Deliberately the response WITHOUT the preamp: the preamp is a level
    adjustment, and what the user wants to see when they flip a preset is its
    tonal shape, not how it was normalised."""
    span = UI_WAVE_H // 2 - 1
    out = []
    for i in range(UI_WAVE_N):
        f = 20.0 * (20000.0 / 20.0) ** (i / (UI_WAVE_N - 1.0))
        db = max(-CURVE_DB, min(CURVE_DB, response_db(qcoefs, f)))
        out.append(int(round(db / CURVE_DB * span)))
    return out


def ascii_curve(name, qcoefs, preamp_db):
    """The shipped curve, drawn. Reading the shape is the whole point of this,
    so the grid stays out of the way: only the trace and the 0 dB line."""
    lo, hi, rows, width = -14.0, 4.0, 19, 61
    fmin, fmax = 20.0, 20000.0
    cols = []
    for x in range(width):
        f = fmin * (fmax / fmin) ** (x / (width - 1.0))
        cols.append(response_db(qcoefs, f) + preamp_db)

    print("  %s   preamp %+.2f dB" % (name, preamp_db))
    step = (hi - lo) / (rows - 1)
    for r in range(rows):
        db = hi - step * r
        line = "".join("*" if abs(v - db) < step / 2
                       else ("-" if abs(db) < step / 2 else " ")
                       for v in cols)
        label = "%+5.1f" % db if (r % 3 == 0) else "     "
        print("   %s |%s" % (label, line))
    axis = [" "] * width
    for f in (100, 1000, 10000):
        x = int(round(math.log10(f / fmin) / math.log10(fmax / fmin) * (width - 1)))
        tag = "%dk" % (f // 1000) if f >= 1000 else str(f)
        for i, ch in enumerate(tag):
            if x + i < width:
                axis[x + i] = ch
    print("         +" + "-" * width)
    print("          " + "".join(axis) + "   (20 Hz .. 20 kHz, log)")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verilog", action="store_true",
                    help="emit the coefficient table for the RTL")
    ap.add_argument("--curves", action="store_true",
                    help="emit the on-screen curve table for the firmware")
    args = ap.parse_args()

    errors, table = [], []
    print("Preset EQ coefficients -- Q2.16 (18-bit signed), Fs = %g Hz" % FS)
    print("Bands: " + ", ".join("%s %gHz Q%.2f" % (k, f, q) for k, f, q in BANDS))
    print()

    worst_margin = None
    for name, gains in PRESETS:
        qcoefs = []
        for (kind, f0, q), g in zip(BANDS, gains):
            c = design(kind, f0, q, g)
            qc = tuple(quantise(v, "%s/%gHz" % (name, f0), errors) for v in c)
            qcoefs.append(qc)
            for v in qc:
                m = min(QMAX - v, v - QMIN)
                if worst_margin is None or m < worst_margin[0]:
                    worst_margin = (m, "%s %gHz" % (name, f0))

        peak = max(response_db(qcoefs, f) for f in FREQS)
        peak_pre = -peak if peak > 0 else 0.0
        preamp_db = loudness_preamp_db(qcoefs)
        preamp_q = quantise(10 ** (preamp_db / 20.0), "%s/preamp" % name, errors)

        if not stable(qcoefs):
            errors.append("%s: a pole is on or outside the unit circle" % name)

        # How far a full-scale tone at the worst frequency can now exceed full
        # scale. This is the price of loudness-matching and the saturation
        # stage is what absorbs it -- so it is reported, not hidden.
        over = max(response_db(qcoefs, f) + preamp_db for f in FREQS)
        table.append((name, qcoefs, preamp_q, preamp_db))
        print("  %-10s preamp %+6.2f dB (peak-match would be %+6.2f) "
              "-> worst-case tone %+5.2f dB %s"
              % (name, preamp_db, peak_pre, over,
                 "" if over <= 0.01 else "-> saturates"))
        if over > 6.0:
            errors.append("%s can exceed full scale by %.1f dB -- too much to "
                          "leave to saturation" % (name, over))

    print()
    print("Tightest Q2.16 margin: %d counts (%s) -- 0 would mean the format is "
          "too narrow." % (worst_margin[0], worst_margin[1]))
    print()

    print("=== magnitude response of the QUANTISED cascade, preamp applied ===")
    print()
    for name, qcoefs, _pq, pdb in table:
        if name == "FLAT":
            print("  FLAT        true bypass in the RTL -- not evaluated here,"
                  " and not a unity biquad (which would still round).")
            print()
            continue
        ascii_curve(name, qcoefs, pdb)

    if args.verilog:
        out = []
        out.append("// GENERATED by tools/gen_eq_coeffs.py --verilog -- DO NOT EDIT.")
        out.append("//")
        out.append("// Q2.16 signed (18-bit), Fs = %g Hz. Five bands per preset," % FS)
        out.append("// five coefficients per band, in the order b0 b1 b2 a1 a2.")
        out.append("// Index: ((preset*%d) + band)*5 + k" % len(BANDS))
        out.append("//")
        out.append("// The difference equation SUBTRACTS a1 and a2:")
        out.append("//   y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2")
        out.append("")
        for i, (name, qcoefs, pq, pdb) in enumerate(table):
            out.append("// ---- %s (preset %d), preamp %+.2f dB" % (name, i, pdb))
            for j, ((kind, f0, _q), qc) in enumerate(zip(BANDS, qcoefs)):
                base = (i * len(BANDS) + j) * 5
                out.append("//   %-9s %6g Hz" % (kind, f0))
                for k, v in enumerate(qc):
                    out.append("crom[%3d] = 18'sd%-7d;  // %s"
                               % (base + k, v, "b0 b1 b2 a1 a2".split()[k])
                               if v >= 0 else
                               "crom[%3d] = -18'sd%-6d;  // %s"
                               % (base + k, -v, "b0 b1 b2 a1 a2".split()[k]))
        out.append("")
        for i, (name, _qc, pq, pdb) in enumerate(table):
            out.append("prom[%d] = 18'sd%-7d;  // %-10s %+.2f dB"
                       % (i, pq, name, pdb))
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "src", "fpga", "core", "eq_coefs.vh")
        path = os.path.normpath(path)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        print("wrote %s (%d lines)" % (path, len(out)))

    if args.curves:
        print("=== paste into fw/ ===")
        print("/* GENERATED by tools/gen_eq_coeffs.py --curves -- DO NOT EDIT.")
        print(" *")
        print(" * Each preset's response drawn on the meter, %d bars wide, as a"
              % UI_WAVE_N)
        print(" * SIGNED offset from the centre line in pixels: boost above,")
        print(" * cut below, clamped to +/-%g dB over +/-%d px. Generated from"
              % (CURVE_DB, UI_WAVE_H // 2 - 1))
        print(" * the same quantised coefficients the RTL uses, so what is on")
        print(" * screen is the filter's real response and cannot drift from")
        print(" * it. %d bytes total. */" % (len(PRESETS) * UI_WAVE_N))
        print("static const signed char eq_curve[%d][%d] = {"
              % (len(PRESETS), UI_WAVE_N))
        for name, qcoefs, _pq, _pdb in table:
            vals = curve_bars(qcoefs)
            print("    { " + ",".join("%3d" % v for v in vals)
                  + " },   /* %s */" % name)
        print("};")
        print()

    print()
    if errors:
        print("FAIL -- %d problem(s):" % len(errors))
        for e in errors:
            print("  - %s" % e)
        return 1
    print("all coefficient checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
