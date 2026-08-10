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
CFLAGS="-march=rv32im -mabi=ilp32 -mno-relax -O2 -ffreestanding -nostartfiles ${EXTRA_CFLAGS:-}"
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
      $FW/start.S $FW/player.c $FW/sysio.c $ROOT/third_party/picojpeg/picojpeg.c"
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
echo "built [$TARGET] -> $OUT/$ROM"
