#!/bin/sh
# Build the MacinTalk host as a native Linux shared library, plus its
# self-test.
#
# There is no emulator-of-an-emulator here and no architecture to prefer:
# the engines are 68000 machine code and Musashi interprets them in plain C,
# so this builds and runs the same on x86-64, i686 and aarch64.  That is the
# difference from the sibling Panthera build, whose i386 guest wants an x86
# host to run natively.
#
#   ./build_linux.sh              # build/linux/libosp_host.so + osp_selftest
#   CC=clang ./build_linux.sh     # another compiler
#
# The library exports the same C API the Windows DLL does, so the Python
# driver renders through it unchanged (osp.py picks the .so name on POSIX).
# `osp_selftest` carries its own inputs -- a few hand-assembled instructions
# -- and needs no engine data, which is what lets CI run it.
#
# Nothing of Apple's or Berkeley's is fetched, built or shipped by this script.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
MUS="$ROOT/third_party/musashi"
OUT="$ROOT/build/linux"
CC="${CC:-cc}"

[ -f "$MUS/m68kcpu.c" ] || { echo "third_party/musashi is missing"; exit 1; }
grep -q 'M68K_INSTRUCTION_HOOK       M68K_OPT_ON' "$MUS/m68kconf.h" \
    || { echo "third_party/musashi/m68kconf.h lost the instruction hook"; exit 1; }

mkdir -p "$OUT"

# -fno-strict-aliasing: Musashi's core and the host both read guest memory
# through more than one type, and both were written for compilers that let
# them.  -O2 is what the Windows build uses; keep the optimiser level equal so
# a floating-point difference in the SANE subset cannot hide behind it.
CFLAGS="${CFLAGS:--O2} -fno-strict-aliasing -fPIC -Wall -Wno-unused-function -Wno-unused-variable"
LDFLAGS="${LDFLAGS:-} -lm"

# --- generate Musashi's opcode tables --------------------------------------
# m68kmake reads m68k_in.c and emits m68kops.c / m68kops.h beside it.  Both
# are ignored by git; the generator is the only thing that may write them.
if [ ! -f "$MUS/m68kops.c" ]; then
    echo "=== generating m68kops.c ==="
    "$CC" -O1 -o "$OUT/m68kmake" "$MUS/m68kmake.c"
    ( cd "$MUS" && "$OUT/m68kmake" . m68k_in.c > "$OUT/m68kmake.log" )
fi

echo "=== libosp_host.so ==="
# One translation unit for the host, as on Windows: osp_host.c includes the
# rest.  OSP_API expands to nothing off Windows and every symbol is exported
# by default, which is what ctypes needs.
"$CC" $CFLAGS -shared -I"$MUS" -I"$ROOT/src" \
    "$MUS/m68kcpu.c" "$MUS/m68kops.c" "$MUS/m68kdasm.c" \
    "$MUS/softfloat/softfloat.c" "$ROOT/src/osp_host.c" "$ROOT/src/osp_plat_posix.c" \
    -o "$OUT/libosp_host.so" $LDFLAGS -lpthread
echo "  -> build/linux/libosp_host.so"

echo "=== osp_host ==="
# The same sources as a program: serve mode, a voice listing, a file renderer.
"$CC" $CFLAGS -I"$MUS" -I"$ROOT/src" \
    "$MUS/m68kcpu.c" "$MUS/m68kops.c" "$MUS/m68kdasm.c" \
    "$MUS/softfloat/softfloat.c" "$ROOT/src/osp_host.c" "$ROOT/src/osp_plat_posix.c" \
    "$ROOT/src/osp_main.c" -o "$OUT/osp_host" $LDFLAGS -lpthread
echo "  -> build/linux/osp_host"

echo "=== osp_selftest ==="
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/src/osp_selftest.c" \
    -L"$OUT" -losp_host -Wl,-rpath,'$ORIGIN' -o "$OUT/osp_selftest" $LDFLAGS
echo "  -> build/linux/osp_selftest"
echo "done."
