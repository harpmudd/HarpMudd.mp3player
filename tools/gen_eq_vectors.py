"""Generate the EQ testbench's stimulus and expected output from the model.

    python tools/gen_eq_vectors.py

Writes sim/eq_stim.hex and sim/eq_exp_<preset>.hex. The expected values come
from tools/eq_model.py, so the Verilog is checked against the same integer
arithmetic the design was verified with rather than against a second opinion.

The stimulus is chosen to hit what actually breaks filters:

  * an impulse, which exposes the entire response including its tail;
  * silence after it, where a limit cycle shows up as a signal that never
    reaches exactly zero;
  * full-scale random noise, the broadband worst case;
  * a full-scale 16 kHz sine, which drives TREBLE about 3.6 dB past full scale
    and so exercises the output clamp -- the one path loudness-matched presets
    made load-bearing;
  * silence again, to let it settle.
"""

import importlib.util
import math
import os
import random
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.normpath(os.path.join(_here, ".."))
_spec = importlib.util.spec_from_file_location(
    "eq_model", os.path.join(_here, "eq_model.py"))
m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m)


def stimulus():
    random.seed(4)                       # fixed: the vectors must be stable
    s = [32767] + [0] * 63
    s += [random.randint(-32768, 32767) for _ in range(256)]
    s += [int(32767 * math.sin(2 * math.pi * 16000 * n / 48000))
          for n in range(128)]
    s += [0] * 128
    return s


def main():
    simdir = os.path.join(_root, "sim")
    os.makedirs(simdir, exist_ok=True)
    stim = stimulus()

    with open(os.path.join(simdir, "eq_stim.hex"), "w") as f:
        for s in stim:
            f.write("%04x\n" % (s & 0xFFFF))

    for pi, (name, _gains) in enumerate(m.gen.PRESETS):
        c = m.build(name)
        with open(os.path.join(simdir, "eq_exp_%d.hex" % pi), "w") as f:
            for s in stim:
                # FLAT is a true bypass in the RTL -- a mux, not the cascade --
                # so the expected value is the input, unmodified.
                out = s if name == "FLAT" else c.step(s)[0]
                f.write("%04x\n" % (out & 0xFFFF))

    print("wrote sim/eq_stim.hex (%d samples) and %d expected vectors"
          % (len(stim), len(m.gen.PRESETS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
