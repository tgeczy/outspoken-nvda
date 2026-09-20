#!/bin/sh
# Build the host as the Android app's native library, liboutspoken.so: the
# same sources and flags as build_linux.sh plus the JNI bridge, cross-built
# with the NDK's clang and dropped where Gradle packages prebuilt libraries.
#
#   sh build_android.sh                 arm64-v8a and armeabi-v7a
#   sh build_android.sh arm64-v8a       one ABI
#
# Needs the NDK: ANDROID_NDK_HOME, or the newest under $ANDROID_SDK_ROOT/ndk,
# or C:/Android/Sdk/ndk on this machine.  Needs m68kops.c, which build.sh or
# build_linux.sh generates -- run one of them first.  Nothing of Apple's or
# Berkeley's is anywhere near this.
set -e

# A Windows path on MSYS (pwd -W), because the NDK's clang is a Windows
# program and path conversion is switched off below, so a POSIX /c/... path
# would reach it unconverted; build.sh does the same for cl.exe.
ROOT="$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))"
MUS="$ROOT/third_party/musashi"
APP="$ROOT/src/platforms/android/app/src/main"
OUT="$ROOT/build/android"
API=26                                  # the app's minSdk

[ -f "$MUS/m68kcpu.c" ] || { echo "third_party/musashi is missing"; exit 1; }
[ -f "$MUS/m68kops.c" ] || { echo "third_party/musashi/m68kops.c is not generated: run sh build.sh or sh build_linux.sh first"; exit 1; }
grep -q 'M68K_INSTRUCTION_HOOK       M68K_OPT_ON' "$MUS/m68kconf.h" \
    || { echo "third_party/musashi/m68kconf.h lost the instruction hook"; exit 1; }

newest() { for p in "$@"; do [ -e "$p" ] && echo "$p"; done | sort -V | tail -1; }
NDK="${ANDROID_NDK_HOME:-}"
[ -n "$NDK" ] || NDK="$(newest "${ANDROID_SDK_ROOT:-/nonexistent}"/ndk/* "$LOCALAPPDATA"/Android/Sdk/ndk/* C:/Android/Sdk/ndk/* "$HOME"/Android/Sdk/ndk/* 2>/dev/null)"
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "no Android NDK found; set ANDROID_NDK_HOME"; exit 1; }
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe ;;
    Darwin) HOST=darwin-x86_64; EXE= ;;
    *) HOST=linux-x86_64; EXE= ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
[ -x "$BIN/clang$EXE" ] || { echo "no clang under $BIN"; exit 1; }
echo "NDK: $NDK"

# As build_linux.sh: -O2 to match the Windows build's optimiser level, so a
# floating-point difference in the SANE subset cannot hide behind it.
CFLAGS="-O2 -fno-strict-aliasing -fPIC -Wall -Wno-unused-function -Wno-unused-variable -DNDEBUG"
SRC="$MUS/m68kcpu.c $MUS/m68kops.c $MUS/m68kdasm.c $MUS/softfloat/softfloat.c \
     $ROOT/src/osp_host.c $ROOT/src/osp_plat_posix.c $APP/cpp/outspoken_jni.c"

build_abi() {
    ABI="$1"
    case "$ABI" in
        arm64-v8a)   TARGET="aarch64-linux-android$API" ;;
        armeabi-v7a) TARGET="armv7a-linux-androideabi$API" ;;
        x86_64)      TARGET="x86_64-linux-android$API" ;;
        *) echo "unknown ABI $ABI"; exit 1 ;;
    esac
    mkdir -p "$OUT/$ABI" "$APP/jniLibs/$ABI"
    echo "=== $ABI ==="
    # MSYS would rewrite --target's argument as a path; nothing here is one.
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 \
    "$BIN/clang$EXE" --target="$TARGET" $CFLAGS -shared -I"$MUS" -I"$ROOT/src" \
        $SRC -o "$OUT/$ABI/liboutspoken.so" -lm -llog
    "$BIN/llvm-strip$EXE" --strip-unneeded -o "$APP/jniLibs/$ABI/liboutspoken.so" "$OUT/$ABI/liboutspoken.so"
    ls -l "$APP/jniLibs/$ABI/liboutspoken.so"
    # The program too, for a shell check on a device: --list and --render
    # from /data/local/tmp say whether the data is readable before the app
    # is involved.
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 \
    "$BIN/clang$EXE" --target="$TARGET" $CFLAGS -I"$MUS" -I"$ROOT/src" \
        $MUS/m68kcpu.c $MUS/m68kops.c $MUS/m68kdasm.c $MUS/softfloat/softfloat.c \
        $ROOT/src/osp_host.c $ROOT/src/osp_plat_posix.c $ROOT/src/osp_main.c \
        -o "$OUT/$ABI/osp_host" -lm
    echo "  -> build/android/$ABI/osp_host"
}

if [ -n "$1" ]; then build_abi "$1"; else build_abi arm64-v8a; build_abi armeabi-v7a; fi
echo "done."
