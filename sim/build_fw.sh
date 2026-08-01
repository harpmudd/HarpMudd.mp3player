#!/bin/bash
# Stage 0 firmware build. Usage: ./build_fw.sh {hello|helix|minimp3}
#
# -mno-relax is required: this bare-metal build has no real __global_pointer$,
# so GCC's gp-relative relaxation would emit stores through an uninitialised gp.
set -e

ROOT="C:/Projects/HarpMudd.mp3player"
TC="$ROOT/toolchain/xpack-riscv-none-elf-gcc-15.2.0-1/bin"
GCC="$TC/riscv-none-elf-gcc.exe"
OBJCOPY="$TC/riscv-none-elf-objcopy.exe"
SIZE="$TC/riscv-none-elf-size.exe"
COMMON="$ROOT/sim/common"
HELIX="$ROOT/third_party/libhelix-mp3"
MINIMP3="$ROOT/third_party/minimp3"

# EXTRA_CFLAGS lets callers add defines, e.g.
#   EXTRA_CFLAGS=-DUSE_VECTOR_128 ./build_fw.sh helix
# OUT_SUFFIX keeps variant builds from overwriting each other.
CFLAGS="-march=rv32im -mabi=ilp32 -mno-relax -O2 -ffreestanding -nostartfiles $EXTRA_CFLAGS"

case "$1" in
hello)
	OUT="$ROOT/sim/fw_hello"; NAME=fw_hello
	SRCS="$COMMON/start.S $OUT/main.c $COMMON/sysio.c"
	INC=""
	;;
helix)
	OUT="$ROOT/sim/fw_helix"; NAME=fw_helix$OUT_SUFFIX
	SRCS="$HELIX/mp3dec.c $HELIX/mp3tabs.c \
	  $HELIX/real/bitstream.c $HELIX/real/buffers.c $HELIX/real/dct32.c \
	  $HELIX/real/dequant.c $HELIX/real/dqchan.c $HELIX/real/huffman.c \
	  $HELIX/real/hufftabs.c $HELIX/real/imdct.c $HELIX/real/polyphase.c \
	  $HELIX/real/scalfact.c $HELIX/real/stproc.c $HELIX/real/subband.c \
	  $HELIX/real/trigtabs.c \
	  $COMMON/start.S $OUT/main.c $COMMON/sysio.c"
	INC="-I $HELIX/pub -I $HELIX/real"
	;;
minimp3)
	OUT="$ROOT/sim/fw_minimp3"; NAME=fw_minimp3
	SRCS="$COMMON/start.S $OUT/main.c $COMMON/sysio.c"
	INC="-I $MINIMP3"
	;;
*)
	echo "usage: $0 {hello|helix|minimp3}"; exit 1
	;;
esac

"$GCC" $CFLAGS $INC -T "$COMMON/link.ld" -o "$OUT/$NAME.elf" $SRCS -lm 2>&1 \
	| grep -v "LOAD segment with RWX" || true

"$OBJCOPY" -O binary "$OUT/$NAME.elf" "$OUT/$NAME.bin"
"$SIZE" "$OUT/$NAME.elf"

python -c "
data = open(r'$OUT/$NAME.bin','rb').read()
nwords = 1024*1024//4
with open(r'$OUT/$NAME.hex','w') as f:
    for i in range(nwords):
        if i*4 < len(data):
            w = data[i*4:i*4+4]; w = w + b'\x00'*(4-len(w))
            f.write('%02x%02x%02x%02x\n' % (w[3],w[2],w[1],w[0]))
        else:
            f.write('0\n')
"
echo "built $NAME"
