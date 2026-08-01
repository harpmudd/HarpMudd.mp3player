"""
pack_rom.py — Build a flat ROM image for the HarpMudd Pac-Man Pocket core.

Supports three games on the SAME FPGA bitstream:

  pacman    -> pacman.rom    (Pac-Man,      Midway 1980)
  mspacman  -> mspacman.rom  (Ms. Pac-Man,  Midway 1981)
  pacplus   -> pacplus.rom   (Pac-Man Plus, Namco/Midway 1982)

Ms. Pac-Man is Pac-Man hardware plus the GCC auxiliary board. That board is
emulated by the descrambler's MSPACMAN overlay (mod_ms=1): rom0 holds the
unmodified Pac-Man program, rom1 holds the aux ROMs u5/u6/u7, and the overlay
applies the trap-latch + 40 patch regions at runtime.

Pac-Man Plus is plain Pac-Man hardware with an encrypted program. The
descrambler decrypts it on the fly when mod_plus=1; its ROM image has the
exact same layout as Pac-Man (program + mirror + gfx + PROMs).

Each game needs different ROM content AND a different mod_* — core_top reads
the variant tag byte (see below) to pick which.

Usage:
  python pack_rom.py [pacman|mspacman|pacplus]      (default: pacman)

ROM image layout (0xC320 bytes, byte offset = dn_addr in FPGA):
  0x0000-0x3FFF  Z80 program       -> descrambler rom0
  0x4000-0x7FFF  rom1 bank         -> Pac-Man: program mirror
                                      Ms.Pac:  aux ROMs u5/u5/u6/u7/u7
  0x8000-0x9FFF  tile + sprite graphics
  0xC000-0xC0FF  82s126.1m  audio waveform PROM
  0xC100-0xC1FF  82s126.4a  color palette PROM
  0xC200         variant tag byte  -> 0 = Pac-Man, 1 = Ms. Pac-Man,
                                       2 = Pac-Man Plus
                 (core_top latches this to drive mod_ms; it sits in the unused
                  gap between PROMs, so no chip-select decodes it)
  0xC300-0xC31F  82s123.7f  color lookup PROM
Image ends at 0xC320 — NO zero-padding. The PROM chip-selects in pacman_video.vhd
only partially decode dn_addr; padding writes (0xD300 etc.) would alias onto the
palette PROM and wipe it. Truncating to the real data extent avoids that.
"""

import sys
import zipfile
import zlib
import os

DEFAULT_ZIP_DIR = r"C:\Projects\Downloaded_Artifacts"
ASSETS_DIR      = r"C:\Projects\HarpMudd.pacman\dist\Assets\pacman\common"

ROM_IMAGE_SIZE = 0xC320

# (CRC32, expected_size, description, rom_image_offset, mirror_offset_or_None)

# ── Pac-Man (Midway) — standard MAME "pacman" set ─────────────────────────────
#   maincpu: pacman.6e/6f/6h/6j (unscrambled Z80 code), mirrored into rom1 bank
#   gfx1:    pacman.5e/5f       (tile / sprite graphics)
PACMAN_ROM_DEFS = [
    (0xc1e6ab10, 4096, "pacman.6e  (Z80 prog block 0)",  0x0000, 0x4000),
    (0x1a6fb2d4, 4096, "pacman.6f  (Z80 prog block 1)",  0x1000, 0x5000),
    (0xbcdd1beb, 4096, "pacman.6h  (Z80 prog block 2)",  0x2000, 0x6000),
    (0x817d94e3, 4096, "pacman.6j  (Z80 prog block 3)",  0x3000, 0x7000),
    (0x0c944964, 4096, "pacman.5e  (tile graphics)",     0x8000, None),
    (0x958fedf9, 4096, "pacman.5f  (sprite graphics)",   0x9000, None),
    (0xa9cc86bf,  256, "82s126.1m  (audio PROM)",        0xC000, None),
    (0x3eb3a8e4,  256, "82s126.4a  (color palette PROM)",0xC100, None),
    (0x2fc650bd,   32, "82s123.7f  (color lookup PROM)", 0xC300, None),
]

# ── Ms. Pac-Man — original auxiliary-board set ────────────────────────────────
#   The descrambler's MSPACMAN overlay (mod_ms=1) emulates the GCC aux board:
#   rom0 holds the UNMODIFIED Pac-Man program (pacman.6e/6f/6h/6j); rom1 holds
#   the aux-board ROMs u5/u6/u7 at the offsets the overlay expects.
#   rom1 "ROM hi" map (descrambler): u5 @ 0x000, u5 @ 0x800, u6 @ 0x1000,
#                                    u7 @ 0x2000, u7 @ 0x3000  (matches MiSTer .mra)
MSPACMAN_ROM_DEFS = [
    (0xc1e6ab10, 4096, "pacman.6e  (Z80 prog block 0)",  0x0000, None),
    (0x1a6fb2d4, 4096, "pacman.6f  (Z80 prog block 1)",  0x1000, None),
    (0xbcdd1beb, 4096, "pacman.6h  (Z80 prog block 2)",  0x2000, None),
    (0x817d94e3, 4096, "pacman.6j  (Z80 prog block 3)",  0x3000, None),
    (0xf45fbbcd, 2048, "u5  (Ms aux ROM)",               0x4000, 0x4800),
    (0xa90e7000, 4096, "u6  (Ms aux ROM)",               0x5000, None),
    (0xc82cd714, 4096, "u7  (Ms aux ROM)",               0x6000, 0x7000),
    (0x5c281d01, 4096, "5e  (Ms tile graphics)",         0x8000, None),
    (0x615af909, 4096, "5f  (Ms sprite graphics)",       0x9000, None),
    (0xa9cc86bf,  256, "82s126.1m  (audio PROM)",        0xC000, None),
    (0x3eb3a8e4,  256, "82s126.4a  (color palette PROM)",0xC100, None),
    (0x2fc650bd,   32, "82s123.7f  (color lookup PROM)", 0xC300, None),
]

# Puck Man (Namco) split graphics chips — fallback when Pac-Man graphics not
# found. MAME gfx1 layout: pm1_chg1 [0x800] pm1_chg2 [0x1000] pm1_chg3 [0x1800] pm1_chg4
PUCKMAN_GFX = [
    (0x8000, [0x2066a0b7, 0x3591b89d], "puckman tile graphics   (pm1_chg1+pm1_chg2)"),
    (0x9000, [0x9e39323a, 0x1b1d9096], "puckman sprite graphics (pm1_chg3+pm1_chg4)"),
]

# ── Pac-Man Plus — encrypted program, plain Pac-Man hardware ──────────────────
#   Same image layout as Pac-Man (program @ 0x0000-0x3FFF mirrored into rom1).
#   The program ROMs are encrypted; the descrambler decrypts them when mod_plus=1.
#   Its distinctive colors come from its own palette/lookup PROMs (4a / 7f).
PACPLUS_ROM_DEFS = [
    (0xd611ef68, 4096, "pacplus.6e  (Z80 prog block 0, encrypted)", 0x0000, 0x4000),
    (0xc7207556, 4096, "pacplus.6f  (Z80 prog block 1, encrypted)", 0x1000, 0x5000),
    (0xae379430, 4096, "pacplus.6h  (Z80 prog block 2, encrypted)", 0x2000, 0x6000),
    (0x5a6dff7b, 4096, "pacplus.6j  (Z80 prog block 3, encrypted)", 0x3000, 0x7000),
    (0x022c35da, 4096, "pacplus.5e  (tile graphics)",     0x8000, None),
    (0x4de65cdd, 4096, "pacplus.5f  (sprite graphics)",   0x9000, None),
    (0xa9cc86bf,  256, "82s126.1m  (audio PROM)",         0xC000, None),
    (0xe271a166,  256, "pacplus.4a  (color palette PROM)",0xC100, None),
    (0x063dd53a,   32, "pacplus.7f  (color lookup PROM)", 0xC300, None),
]

# Variant tag: written into the ROM image so core_top can pick the mod_*.
VARIANT_TAG_OFFSET = 0xC200

#          rom_defs            out_name        description                        puckman_gfx  variant_tag
GAMES = {
    "pacman":   (PACMAN_ROM_DEFS,   "pacman.rom",   "Pac-Man (Midway, 1980)",          True,  0),
    "mspacman": (MSPACMAN_ROM_DEFS, "mspacman.rom", "Ms. Pac-Man (Midway, 1981)",      False, 1),
    "pacplus":  (PACPLUS_ROM_DEFS,  "pacplus.rom",  "Pac-Man Plus (Namco/Midway, 1982)", False, 2),
}


def crc32_of(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def load_zip_by_crc(zip_path):
    """Return dict {crc32: bytes} for every file in the zip."""
    found = {}
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            data = zf.read(info.filename)
            found[crc32_of(data)] = data
    return found


def load_dir_by_crc(zip_dir):
    """Scan all .zip files in zip_dir and return a merged {crc32: bytes} dict."""
    found = {}
    zips = sorted(f for f in os.listdir(zip_dir) if f.lower().endswith('.zip'))
    if not zips:
        print(f"  (no zip files found in {zip_dir})")
        return found
    for zname in zips:
        print(f"  scanning {zname}")
        try:
            found.update(load_zip_by_crc(os.path.join(zip_dir, zname)))
        except Exception as e:
            print(f"  WARNING: could not read {zname}: {e}")
    return found


def main():
    game = sys.argv[1].lower() if len(sys.argv) > 1 else "pacman"
    if game not in GAMES:
        print(f"ERROR: unknown game '{game}'. Choose: {', '.join(GAMES)}")
        sys.exit(1)

    rom_defs, out_name, desc, allow_puckman_gfx, variant_tag = GAMES[game]
    out_path = os.path.join(ASSETS_DIR, out_name)

    print(f"ROM packer — {desc}")
    print(f"Output: {out_path}\n")
    print(f"Scanning all zips in: {DEFAULT_ZIP_DIR}")
    found = load_dir_by_crc(DEFAULT_ZIP_DIR)
    print()

    image = bytearray(ROM_IMAGE_SIZE)
    errors = []

    for (crc, size, d, offset, mirror) in rom_defs:
        if crc in found:
            data = found[crc]
            if len(data) != size:
                errors.append(f"  WRONG SIZE  {d}: expected {size}, got {len(data)}")
                continue
            image[offset:offset + size] = data
            if mirror is not None:
                image[mirror:mirror + size] = data
            print(f"  OK          {d}  @ 0x{offset:04X}")
        else:
            errors.append(f"  MISSING     {d}  (CRC {crc:08x})")

    # Pac-Man only: reconstruct graphics from Puck Man split chips if needed
    if allow_puckman_gfx and any("graphics" in e for e in errors):
        print("\nPac-Man graphics ROMs not found — trying Puck Man fallback...")
        for (offset, crcs, d) in PUCKMAN_GFX:
            if all(c in found and len(found[c]) == 2048 for c in crcs):
                image[offset:offset + 4096] = b"".join(found[c] for c in crcs)
                errors = [e for e in errors
                          if f"0x{offset:04X}" not in e and "graphics" not in e]
                print(f"  FALLBACK    {d}  @ 0x{offset:04X}")

    print()
    if errors:
        print("MISSING OR INVALID ROMs:")
        for e in errors:
            print(e)
        sys.exit(1)

    # Stamp the variant tag so core_top can pick Pac-Man vs Ms. Pac-Man (mod_ms).
    image[VARIANT_TAG_OFFSET] = variant_tag
    print(f"  variant tag 0x{variant_tag:02X}  @ 0x{VARIANT_TAG_OFFSET:04X}")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(image)

    print(f"\nSUCCESS: wrote {len(image)} bytes -> {out_path}")


if __name__ == "__main__":
    main()
