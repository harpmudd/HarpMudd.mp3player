#!/bin/bash
# Build core firmware -> dist/Assets/mp3player/common/mp3player.rom
#
#   ./build.sh            # Stage 3 player (Helix decode + playback)  [default]
#   ./build.sh bringup    # Stage 1/2 bring-up (tone + 0180 test, no decoder)
#
# The .rom is loaded from SD into BRAM by data_loader at boot, exactly like an
# arcade core's ROM -- which is the point: firmware changes cost seconds here
# instead of a full Quartus compile.
set -e

TARGET="${1:-player}"

ROOT="C:/Projects/HarpMudd.mp3player"
TC="$ROOT/toolchain/xpack-riscv-none-elf-gcc-15.2.0-1/bin"
GCC="$TC/riscv-none-elf-gcc.exe"
OBJCOPY="$TC/riscv-none-elf-objcopy.exe"
SIZE="$TC/riscv-none-elf-size.exe"
FW="$ROOT/fw"
HELIX="$ROOT/third_party/libhelix-mp3"
OUT="$ROOT/dist/Assets/mp3player/common"

mkdir -p "$OUT"

# -mno-relax: no real __global_pointer$ in this bare-metal build, so gp-relative
# relaxation would emit stores through an uninitialised gp.
# EXTRA_CFLAGS lets a build turn on things that are off by default without
# editing source, e.g.:  EXTRA_CFLAGS=-DDEBUG_DIAG=1 bash fw/build.sh
CFLAGS="-march=rv32im -mabi=ilp32 -mno-relax -O2 -ffreestanding -nostartfiles -ffunction-sections -fdata-sections -Wl,--gc-sections ${EXTRA_CFLAGS:-}"
ROM="mp3player.rom"

case "$TARGET" in
bringup)
    SRCS="$FW/start.S $FW/main.c"
    INC=""
    ;;
player)
    SRCS="$HELIX/mp3dec.c $HELIX/mp3tabs.c \
      $HELIX/real/bitstream.c $HELIX/real/buffers.c $HELIX/real/dct32.c \
      $HELIX/real/dequant.c $HELIX/real/dqchan.c $HELIX/real/huffman.c \
      $HELIX/real/hufftabs.c $HELIX/real/imdct.c $HELIX/real/polyphase.c \
      $HELIX/real/scalfact.c $HELIX/real/stproc.c $HELIX/real/subband.c \
      $HELIX/real/trigtabs.c \
      $FW/start.S $FW/player.c $FW/sysio.c $FW/alloc.c $FW/picojpeg.o $FW/flac.o"
    INC="-I $HELIX/pub -I $HELIX/real -I $ROOT/third_party/picojpeg"
    ;;
*)
    echo "usage: $0 {player|bringup}"; exit 1 ;;
esac

# Compile status is checked EXPLICITLY. This used to be
#   "$GCC" ... 2>&1 | grep -v "LOAD segment with RWX" || true
# which reports the pipeline's status (grep's), and `|| true` then swallowed
# even that -- so a compile error printed, `set -e` did not fire, and objcopy
# happily re-packaged the PREVIOUS fw.elf. The script said "built" and shipped
# a stale .rom. That is a hardware-test cycle wasted on code that was never
# compiled, which is the exact opposite of what this script exists for.
# Deleting the elf first means a failure can never fall back to an old one.
rm -f "$FW/fw.elf"
# The FLAC decoder is compiled -Os and the rest -O2 on purpose. Its text is
# 10602 bytes at -O2 against 6182 at -Os, and those 4420 bytes are the
# difference between the image fitting below the DMA buffers and not. The hot
# MP3 path keeps -O2: it needs 45.7 MHz of 60 and cannot afford the loss.
# If FLAC turns out CPU-bound, this is the first thing to revisit.
rm -f "$FW/flac.o"
if ! "$GCC" -march=rv32im -mabi=ilp32 -mno-relax -Os -ffreestanding -c         -o "$FW/flac.o" "$FW/flac.c" > "$FW/build.log" 2>&1; then
    cat "$FW/build.log" >&2
    echo "*** flac.c FAILED TO COMPILE ***" >&2
    exit 1
fi

# picojpeg gets -Os for the same reason, and with less to lose than flac.c: it
# decodes album art ONCE per track load, inside the silent gap where the FIFO
# has already been flushed. Nothing it does is on the audio path, so trading
# its speed for size costs a few ms of a load that is already hundreds.
rm -f "$FW/picojpeg.o"
if ! "$GCC" -march=rv32im -mabi=ilp32 -mno-relax -Os -ffreestanding         -I "$ROOT/third_party/picojpeg" -c         -o "$FW/picojpeg.o" "$ROOT/third_party/picojpeg/picojpeg.c"         > "$FW/build.log" 2>&1; then
    cat "$FW/build.log" >&2
    echo "*** picojpeg.c FAILED TO COMPILE ***" >&2
    exit 1
fi

if ! "$GCC" $CFLAGS $INC -T "$FW/link.ld" -o "$FW/fw.elf" $SRCS -lm \
        > "$FW/build.log" 2>&1; then
    grep -v "LOAD segment with RWX" "$FW/build.log" >&2 || true
    echo "*** COMPILE FAILED -- no .rom written ***" >&2
    exit 1
fi
grep -v "LOAD segment with RWX" "$FW/build.log" >&2 || true

"$SIZE" "$FW/fw.elf"
"$OBJCOPY" -O binary "$FW/fw.elf" "$OUT/$ROM"

python -c "
import os
n = os.path.getsize(r'$OUT/$ROM')
lim = 192*1024 - 16*1024        # RAM minus stack reserve
print('$ROM: %d bytes (%.1f%% of usable RAM)' % (n, 100.0*n/lim))
assert n < lim, 'firmware image exceeds usable RAM'
"
# The splash version and the version the Pocket shows in its core list live in
# two different files, and they drifted: v1.3.0 was built, tested and pushed to
# a card for days still announcing 1.2.0 on both. Neither is checked by anything
# else, and neither is visible from the other, so this compares them on every
# build and FAILS rather than shipping a lie.
#
# date_release is not checked -- only a human knows the release date -- but it
# is printed here so it cannot be forgotten silently.
# cut on the quotes rather than a sed backreference: escaping is what broke the
# first version of this check, and one that silently compares two empty strings
# is worse than no check at all.
CORE_JSON="$ROOT/dist/Cores/HarpMudd.Mp3Player/core.json"
APP_VER=$(grep -m1 '#define APP_VER'  "$FW/player.c" | cut -d'"' -f2)
JSON_VER=$(grep -m1 '"version"'       "$CORE_JSON"   | cut -d'"' -f4)
JSON_DATE=$(grep -m1 '"date_release"' "$CORE_JSON"   | cut -d'"' -f4)
if [ -z "$APP_VER" ] || [ -z "$JSON_VER" ]; then
  echo "*** VERSION CHECK BROKE: could not read a version from either file ***" >&2
  exit 1
fi
if [ "$APP_VER" != "$JSON_VER" ]; then
  echo "*** VERSION MISMATCH: player.c says $APP_VER, core.json says $JSON_VER ***" >&2
  echo "    both must match before release; core.json also carries date_release" >&2
  exit 1
fi
echo "version $APP_VER (core.json date_release $JSON_DATE)"

echo "built [$TARGET] -> $OUT/$ROM"
