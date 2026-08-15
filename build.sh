#!/bin/sh
# Build the MacinTalk host DLL.
#
# Musashi generates most of its own source: m68kmake reads m68k_in.c and emits
# m68kops.c / m68kops.h.  That step has to happen before anything else compiles,
# which is the only reason this is a script rather than two cl invocations.
#
# Both architectures are built.  NVDA 2026.1 is 64-bit, but pctalker and
# flexvoice both taught us that the 32-bit copy is wanted eventually and that
# discovering its absence on someone else's machine is the expensive way.
#
#   sh build.sh          both
#   sh build.sh x64      one
set -e

# MSYS rewrites arguments that look like POSIX paths.  It sees `-FoC:/git/...`,
# treats the colon as a path-list separator, and hands cl.exe a path under the
# Git installation directory.  Turn the whole mechanism off; every path here is
# already a Windows path.
export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

# cl.exe needs Windows paths.  Handing it an MSYS `/c/git/...` makes it invent
# `C:\c\git\...` and fail on a missing directory rather than on anything real.
ROOT="$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || cygpath -m "$(pwd)")"
MUS="$ROOT/third_party/musashi"
OUT="$ROOT/build"

# Discover the toolchain rather than pinning it.  wintalker's toolchain file
# hardcoded an MSVC version that no longer exists on this machine, which is the
# failure this avoids -- take the newest of whatever is installed.
# Glob patterns must stay unquoted to expand; only the literal parts are
# quoted, so directory names containing spaces still survive intact.
newest() { for p in "$@"; do [ -e "$p" ] && echo "$p"; done | sort -V | tail -1; }

MSVC="$(newest "C:/Program Files (x86)/Microsoft Visual Studio"/*/*/VC/Tools/MSVC/* \
               "C:/Program Files/Microsoft Visual Studio"/*/*/VC/Tools/MSVC/*)"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDKV="$(newest "$SDK/Include"/* | sed 's#.*/##')"

[ -n "$MSVC" ] || { echo "no MSVC toolchain found"; exit 1; }
[ -n "$SDKV" ] || { echo "no Windows SDK found"; exit 1; }
echo "MSVC: $MSVC"
echo "SDK:  $SDKV"

mkdir -p "$OUT"

INC="-I\"$MSVC/include\" -I\"$SDK/Include/$SDKV/ucrt\" -I\"$SDK/Include/$SDKV/um\" -I\"$SDK/Include/$SDKV/shared\""

build_one() {
    ARCH="$1"                      # x86 | x64
    case "$ARCH" in
        x64) CL="$MSVC/bin/Hostx64/x64/cl.exe"; LIBA="x64";   SUF="";     ;;
        x86) CL="$MSVC/bin/Hostx64/x86/cl.exe"; LIBA="x86";   SUF="_x86"; ;;
        *) echo "unknown arch $ARCH"; exit 1 ;;
    esac
    LIB="-LIBPATH:\"$MSVC/lib/$LIBA\" -LIBPATH:\"$SDK/lib/$SDKV/ucrt/$LIBA\" -LIBPATH:\"$SDK/lib/$SDKV/um/$LIBA\""
    OBJ="$OUT/obj-$ARCH"
    mkdir -p "$OBJ"

    echo "=== $ARCH ==="
    # /MT: static CRT.  A /MD build needs the VC++ redistributable that NVDA
    # does not ship -- invisible on this machine, fatal on a clean one.  That
    # cost us a release once already.
    eval "\"$CL\" -nologo -c -O2 -MT -DNDEBUG $INC \
        -I\"$MUS\" -I\"$ROOT/src\" \
        \"$MUS/m68kcpu.c\" \"$MUS/m68kops.c\" \"$MUS/m68kdasm.c\" \
        \"$MUS/softfloat/softfloat.c\" \"$ROOT/src/osp_host.c\" \
        -Fo\"$OBJ/\"" > "$OBJ/compile.log" 2>&1 || {
            echo "compile failed; tail of log:"; tail -30 "$OBJ/compile.log"; exit 1; }

    eval "\"$CL\" -nologo -LD -MT \"$OBJ\"/*.obj \
        -Fe\"$OUT/osp_host$SUF.dll\" -link $LIB" > "$OBJ/link.log" 2>&1 || {
            echo "link failed; tail of log:"; tail -30 "$OBJ/link.log"; exit 1; }

    echo "  -> build/osp_host$SUF.dll"
}

# --- fetch Musashi -------------------------------------------------------
# Not vendored.  This repository contains only our own code; the emulator core
# is Karl Stenerud's and is fetched at build time.  See THIRD_PARTY_LICENSES.md.
MUSASHI_URL="https://github.com/kstenerud/Musashi"
if [ ! -f "$MUS/m68kcpu.c" ]; then
    echo "=== fetching Musashi ==="
    mkdir -p "$ROOT/third_party"
    git clone --depth 1 "$MUSASHI_URL" "$MUS"
    # The host needs a per-instruction hook to catch A-line traps at their
    # vector and to enforce a counted instruction budget.  Only change made.
    sed -i 's/^#define M68K_INSTRUCTION_HOOK       M68K_OPT_OFF/#define M68K_INSTRUCTION_HOOK       M68K_OPT_ON/' "$MUS/m68kconf.h"
    grep -q 'M68K_INSTRUCTION_HOOK       M68K_OPT_ON' "$MUS/m68kconf.h" \
        || { echo "failed to enable M68K_INSTRUCTION_HOOK"; exit 1; }
fi

# --- generate Musashi's opcode tables ------------------------------------
if [ ! -f "$MUS/m68kops.c" ]; then
    echo "=== generating m68kops.c ==="
    CLGEN="$MSVC/bin/Hostx64/x64/cl.exe"
    LIBGEN="-LIBPATH:\"$MSVC/lib/x64\" -LIBPATH:\"$SDK/lib/$SDKV/ucrt/x64\" -LIBPATH:\"$SDK/lib/$SDKV/um/x64\""
    eval "\"$CLGEN\" -nologo $INC \"$MUS/m68kmake.c\" -Fe\"$OUT/m68kmake.exe\" \
        -Fo\"$OUT/\" -link $LIBGEN" > "$OUT/m68kmake.log" 2>&1 || {
            echo "m68kmake build failed:"; tail -20 "$OUT/m68kmake.log"; exit 1; }
    ( cd "$MUS" && "$OUT/m68kmake.exe" . m68k_in.c )
    echo "  -> $(ls -1 "$MUS"/m68kops.* | tr '\n' ' ')"
fi

if [ -n "$1" ]; then build_one "$1"; else build_one x64; build_one x86; fi
echo "done."
