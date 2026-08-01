"""
package.py — Package the compiled MP3 Player core for the Analogue Pocket.

Steps:
  1. Verify the bitstream exists in src/fpga/output_files/
  2. Convert .rbf -> .rbf_r (BIT-REVERSED bitstream for Pocket -- mandatory)
  3. Copy bitstream to dist/Cores/HarpMudd.Mp3Player/bitstream.rbf_r
  4. Run pack_rom.py to generate mp3player.rom (if not already present)
  5. Print copy instructions for the Pocket SD card

The .rbf_r suffix LITERALLY MEANS bit-reversed. Do NOT just rename .rbf to
.rbf_r -- the Pocket will configure the FPGA but the bridge handler will
never respond ("Error in framework RS: BRIDGE not responding").

Usage:
  python package.py [--skip-rom]
"""

import os
import sys
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
BITSTREAM_SRC = os.path.join(PROJECT_ROOT, "src", "fpga", "output_files", "ap_core.rbf")
DIST_CORE     = os.path.join(PROJECT_ROOT, "dist", "Cores", "HarpMudd.Mp3Player")
BITSTREAM_DST = os.path.join(DIST_CORE, "bitstream.rbf_r")
ROM_DST       = os.path.join(PROJECT_ROOT, "dist", "Assets", "mp3player", "common", "mp3player.rom")
PACK_ROM_PY   = os.path.join(PROJECT_ROOT, "pack_rom.py")
README_PATH   = os.path.join(PROJECT_ROOT, "README.md")


def rbf_to_rbf_r(src, dst):
    """Reverse the bit order of each byte in the .rbf.

    Pocket bootloader expects the bitstream in bit-reversed format (the _r
    suffix). Skipping this step produces a bitstream that the FPGA loads but
    whose bridge handler is silent -- "BRIDGE not responding" on game launch.
    """
    with open(src, "rb") as f:
        data = f.read()

    rev = bytearray(len(data))
    for i, b in enumerate(data):
        rev[i] = int(f"{b:08b}"[::-1], 2)

    with open(dst, "wb") as f:
        f.write(bytes(rev))

    print(f"  bitstream: {src}")
    print(f"  -> rbf_r : {dst}  ({len(rev)} bytes)")


def check_readme():
    """Warn (don't fail) if README.md still has unfilled scaffold markers.

    The scaffolded README ships with `<!-- TODO ... -->` placeholders for the
    game overview, hardware, credits, controls, etc. It's easy to ship a core
    with these never filled in, so flag it loudly at package time.
    """
    if not os.path.exists(README_PATH):
        print("\n!! README.md MISSING — every core should ship a filled-in README.")
        return
    text = open(README_PATH, encoding="utf-8", errors="ignore").read()
    todos = text.count("<!-- TODO")
    if todos:
        print(f"\n{'!' * 60}")
        print(f"!! README.md still has {todos} unfilled <!-- TODO --> marker(s).")
        print("!! Fill in the game/hardware/port/credits/controls sections")
        print("!! (pull author credits from the source headers) before shipping.")
        print(f"{'!' * 60}")


def main():
    skip_rom = "--skip-rom" in sys.argv

    print("=== MP3 Player Pocket Core Packager ===\n")

    # 1. Bitstream
    if not os.path.exists(BITSTREAM_SRC):
        print(f"ERROR: bitstream not found: {BITSTREAM_SRC}")
        print("Run Quartus compilation first:")
        print(f"  quartus_sh --flow compile {os.path.join(PROJECT_ROOT, 'src', 'fpga', 'ap_core.qpf')}")
        sys.exit(1)

    os.makedirs(DIST_CORE, exist_ok=True)
    print("Converting bitstream...")
    rbf_to_rbf_r(BITSTREAM_SRC, BITSTREAM_DST)

    # 2. ROM
    if not skip_rom:
        if os.path.exists(ROM_DST):
            print(f"\nROM already exists: {ROM_DST}")
        else:
            print("\nBuilding ROM image...")
            result = subprocess.run([sys.executable, PACK_ROM_PY], capture_output=False)
            if result.returncode != 0:
                print("\nROM build failed. Package incomplete.")
                print("Provide a complete MAME romset and re-run.")
                sys.exit(1)

    # 3. Summary
    print("\n=== Package contents ===")
    for root, dirs, files in os.walk(os.path.join(PROJECT_ROOT, "dist")):
        level = root.replace(os.path.join(PROJECT_ROOT, "dist"), "").count(os.sep)
        indent = "  " * level
        print(f"{indent}{os.path.basename(root)}/")
        for f in files:
            fpath = os.path.join(root, f)
            size = os.path.getsize(fpath)
            print(f"{indent}  {f}  ({size:,} bytes)")

    # 4. README completeness (non-fatal — packaging still succeeds)
    check_readme()

    print("\n=== Copy to Pocket SD card ===")
    print("Copy the contents of dist/ to the root of your Pocket SD card:")
    print(f"  xcopy /E /Y \"{os.path.join(PROJECT_ROOT, 'dist')}\\*\" X:\\")
    print("(replace X: with your Pocket SD card drive letter)")


if __name__ == "__main__":
    main()
