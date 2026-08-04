"""Bit-exact integer model of the EQ cascade -- the reference the RTL must match.

    python tools/eq_model.py

Everything is integer. This is not "a float model at roughly the right widths";
it is the arithmetic the Verilog will do, expressed in Python, so the iverilog
testbench can demand SAMPLE-FOR-SAMPLE EQUALITY rather than a tolerance. A
tolerance would hide exactly the bugs worth finding: a wrong shift, a truncation
where a round belongs, a state word one bit too narrow.

Fixed-point scheme (docs/EQ_DESIGN.md, with the accumulator width corrected --
see the report this prints):

    coefficients   Q2.16   18-bit signed          b0 b1 b2 a1 a2 per band
    sample/state   Q20.16  36-bit signed          input << 16
    product        coef * state                   32 fractional bits
    accumulator    sum of 5 products              58-bit signed
    round          nearest, (acc + 1<<15) >> 16   truncation biases a feedback
                                                  path toward a DC offset
    output         >> 16, then CLAMP to int16     clamp, never wrap

Direct Form I:  y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
"""

import importlib.util
import math
import os
import random
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "gen_eq_coeffs", os.path.join(_here, "gen_eq_coeffs.py"))
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)

# Both widths are MEASURED, not chosen. See the sweep in the commit message:
#
#   * Below 14 fractional bits the cascade NEVER settles -- silence after a
#     loud passage rings indefinitely. That is the limit cycle EQ_DESIGN warned
#     about, and it appears abruptly rather than gradually: 13 bits rings
#     forever, 14 bits settles in 23 ms. 16 leaves two bits of margin on a
#     threshold that has a cliff in it.
#   * At 16 fractional bits the worst observed |state| needs 33 bits, using
#     full-scale sines parked on each band centre -- close to worst case for a
#     resonant filter. 36 bits leaves 18 dB above that.
#
# EQ_DESIGN originally said a 32-bit state and a 40-bit accumulator. Neither
# survives contact with the numbers: 32 bits cannot hold 16 fractional bits
# plus the headroom, and 18x36 cannot fit in 40.
FRAC_S = 16                     # sample/state fractional bits
FRAC_C = gen.QF                 # coefficient fractional bits (16)
STATE_BITS = 36
ACC_BITS = 58
SAMP_MIN, SAMP_MAX = -32768, 32767

# Measured as the model runs, so the report states what was observed rather
# than what was hoped for.
peak_state = 0
peak_acc = 0


def _rnd_shift(v, n):
    """Round-to-nearest arithmetic right shift. Python's >> floors, which is
    what a Verilog arithmetic shift does too, so this matches the hardware."""
    return (v + (1 << (n - 1))) >> n


class Biquad:
    __slots__ = ("b0", "b1", "b2", "a1", "a2", "x1", "x2", "y1", "y2")

    def __init__(self, coefs):
        self.b0, self.b1, self.b2, self.a1, self.a2 = coefs
        self.reset()

    def reset(self):
        self.x1 = self.x2 = self.y1 = self.y2 = 0

    def step(self, x):
        global peak_state, peak_acc
        acc = (self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2
               - self.a1 * self.y1 - self.a2 * self.y2)
        y = _rnd_shift(acc, FRAC_C)
        self.x2, self.x1 = self.x1, x
        self.y2, self.y1 = self.y1, y
        peak_acc = max(peak_acc, abs(acc))
        peak_state = max(peak_state, abs(y), abs(x))
        return y


class Cascade:
    """Five bands plus the preset preamp, one channel."""

    def __init__(self, qcoefs, preamp_q):
        self.bands = [Biquad(c) for c in qcoefs]
        self.preamp = preamp_q

    def reset(self):
        for b in self.bands:
            b.reset()

    def step(self, sample16):
        v = sample16 << FRAC_S
        for b in self.bands:
            v = b.step(v)
        v = _rnd_shift(v * self.preamp, FRAC_C)      # preamp, still Q20.16
        out = _rnd_shift(v, FRAC_S)                  # back to whole samples
        # CLAMP. Wrapping here would turn a 4 dB overshoot into full-scale
        # noise, which is the one failure mode worse than the EQ being off.
        return max(SAMP_MIN, min(SAMP_MAX, out)), out


def build(name):
    for pname, gains in gen.PRESETS:
        if pname != name:
            continue
        errs = []
        qc = [tuple(gen.quantise(v, "", errs) for v in gen.design(k, f0, q, g))
              for (k, f0, q), g in zip(gen.BANDS, gains)]
        pre = gen.quantise(10 ** (gen.loudness_preamp_db(qc) / 20.0), "", errs)
        assert not errs, errs
        return Cascade(qc, pre)
    raise KeyError(name)


# ----------------------------------------------------------------- tests ---

def t_bypass_is_bit_exact():
    """FLAT must come out bit-identical to the input.

    EQ_DESIGN asserts a unity biquad "is not an identity function in fixed
    point: it still rounds". For THIS construction that turns out to be false,
    and the reason is worth knowing: at 0 dB the cookbook gives a numerator
    numerically equal to the denominator, so b1==a1 and b2==a2 exactly and b0
    quantises to exactly 1.0. The recursion then collapses to y = x with no
    rounding error at all.

    The bypass mux stays anyway -- it is free, it also skips the preamp
    multiply, and it keeps "off" provably identical even if a coefficient is
    ever retuned. But the claim it was justified by is wrong, and this test now
    asserts the truth rather than the assumption."""
    c = build("FLAT")
    random.seed(1)
    diffs = 0
    for _ in range(20000):
        s = random.randint(SAMP_MIN, SAMP_MAX)
        out, _ = c.step(s)
        if out != s:
            diffs += 1
    return ("FLAT is bit-exact over 20000 random samples (%d differ)" % diffs,
            diffs == 0)


def t_silence_settles():
    """Design plan step 5, the limit-cycle check: after a loud passage, feed
    zeros and the output must reach EXACTLY zero and stay there. This is the
    test that catches insufficient state width."""
    worst = []
    for name, _ in gen.PRESETS:
        if name == "FLAT":
            continue
        c = build(name)
        random.seed(2)
        for _ in range(4000):                       # loud passage
            c.step(random.randint(-32000, 32000))
        tail = 0
        for i in range(200000):                     # then silence
            out, _ = c.step(0)
            if out != 0:
                tail = i + 1
        worst.append((name, tail))
    # A filter does not stop dead -- it decays. What matters is that it reaches
    # EXACTLY zero and stays there, not that it is silent immediately. A limit
    # cycle shows up as a tail that never ends, which is what FRAC_S < 14 does.
    LIMIT = 48000                                   # one second
    bad = [w for w in worst if w[1] >= LIMIT]
    slowest = max(worst, key=lambda w: w[1])
    return ("every preset reaches exact zero; slowest %s at %d samples (%.0f ms)"
            % (slowest[0], slowest[1], slowest[1] / 48.0)
            if not bad else "LIMIT CYCLE, never settles: %s" % bad, not bad)


def t_saturation_clamps():
    """Loudness-matching means a boosted band CAN exceed full scale. The
    output must clamp at the rail, never wrap. A wrap would be full-scale
    noise on a bass hit."""
    worst = None
    for name, _ in gen.PRESETS:
        if name == "FLAT":
            continue
        c = build(name)
        # Worst case for a resonant filter: a full-scale sine parked on the
        # band it boosts most.
        for f in (60, 80, 100, 250, 1000, 4000, 8000, 12000, 16000):
            c.reset()
            over = 0
            for n in range(3000):
                s = int(32767 * math.sin(2 * math.pi * f * n / gen.FS))
                out, raw = c.step(s)
                if raw != out:
                    over = max(over, abs(raw))
                if not (SAMP_MIN <= out <= SAMP_MAX):
                    return ("CLAMP FAILED on %s @ %d Hz: %d" % (name, f, out),
                            False)
            if over:
                db = 20 * math.log10(over / 32767.0)
                if worst is None or db > worst[2]:
                    worst = (name, f, db)
    if worst:
        return ("clamped correctly; worst overshoot %+.2f dB (%s @ %d Hz)"
                % (worst[2], worst[0], worst[1]), True)
    return ("no preset ever exceeded full scale", True)


def t_widths():
    """Confirm the chosen widths cover what was actually observed."""
    need_acc = peak_acc.bit_length() + 1          # +1 for sign
    need_state = peak_state.bit_length() + 1
    ok = need_state <= STATE_BITS and need_acc <= ACC_BITS
    return ("state %d/%d bits (%.1f dB spare), acc %d/%d bits"
            % (need_state, STATE_BITS,
               6.02 * (STATE_BITS - need_state), need_acc, ACC_BITS), ok)


def main():
    print(__doc__.split("\n\n")[0])
    print()
    tests = [
        ("true bypass is required", t_bypass_is_bit_exact),
        ("silence settles to zero", t_silence_settles),
        ("output clamps, never wraps", t_saturation_clamps),
    ]
    fails = 0
    for label, fn in tests:
        msg, ok = fn()
        print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", label, msg))
        if not ok:
            fails += 1
    msg, ok = t_widths()                    # after the others, so peaks are real
    print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", "width check", msg))
    if not ok:
        fails += 1

    print()
    print("worst observed |state| = %d (%.1f%% of the %d-bit range)"
          % (peak_state, 100.0 * peak_state / (1 << (STATE_BITS - 1)),
             STATE_BITS))
    print("worst observed |acc|   = %d" % peak_acc)
    print()
    print("FAIL" if fails else "all model checks pass")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
