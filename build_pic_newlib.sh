#!/bin/sh
# Build the PIE-safe app libc/libm sysroot for one AAR profile from newlib
# 4.6.0.20260123, the release used by Arm GNU Toolchain 15.3.rel2. Stock
# archives are non-PIC.
#
#   AAR_PROFILE=a32   arm-none-eabi, Cortex-A9, hard-float, NEON (default)
#   AAR_PROFILE=a64   aarch64-none-elf, ARMv8-A, FP and ASIMD
#
# The output prefix is AAR_SYSROOT, or AAR_SYSROOT_A32 / AAR_SYSROOT_A64 for the
# chosen profile. The libraries land in <prefix>/<target>/lib. Run it once per
# profile. Suggested Docker cache volumes:
#   -v aar-sysroot-a32:/opt/aar-sysroot-a32
#   -v aar-newlib-cache:/opt/aar-newlib-cache
# The target compiler (arm-none-eabi-gcc or aarch64-none-elf-gcc) must be on
# PATH, or set CROSS_COMPILE.
#
# SPDX-License-Identifier: MIT
set -e
set -u

NEWLIB_VERSION=4.6.0.20260123
NEWLIB_SHA256=6ff27e3bf022666f43f7802255be680eeff722ac181b1725d21e2e8318604ee3
NEWLIB_URL=https://sourceware.org/pub/newlib/newlib-${NEWLIB_VERSION}.tar.gz

AAR_PROFILE=${AAR_PROFILE:-a32}
case "$AAR_PROFILE" in
a32)
    TARGET=arm-none-eabi
    TARGET_CFLAGS='-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -fPIC -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections'
    DEFAULT_PREFIX=${AAR_SYSROOT_A32:-}
    ;;
a64)
    TARGET=aarch64-none-elf
    TARGET_CFLAGS='-march=armv8-a -fPIC -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections'
    DEFAULT_PREFIX=${AAR_SYSROOT_A64:-}
    ;;
*)
    echo "AAR_PROFILE must be a32 or a64, got '$AAR_PROFILE'" >&2
    exit 2
    ;;
esac

PREFIX=${AAR_SYSROOT:-$DEFAULT_PREFIX}
if [ -z "$PREFIX" ]; then
    echo "set AAR_SYSROOT (or AAR_SYSROOT_A32 / AAR_SYSROOT_A64) to the PIC sysroot to build" >&2
    exit 2
fi
CACHE=${AAR_NEWLIB_CACHE:-"$(dirname "$PREFIX")/aar-newlib-cache"}
BUILD_ROOT=${AAR_NEWLIB_BUILD:-"$(pwd)/.build-pic-newlib-$AAR_PROFILE"}
JOBS=${JOBS:-2}

: "${CC:=gcc}"
: "${CROSS_COMPILE:=$TARGET-}"
command -v "${CROSS_COMPILE}gcc" >/dev/null
command -v "$CC" >/dev/null
command -v make >/dev/null
command -v tar >/dev/null
command -v sha256sum >/dev/null

mkdir -p "$CACHE"
ARCHIVE="$CACHE/newlib-${NEWLIB_VERSION}.tar.gz"
if [ ! -f "$ARCHIVE" ]; then
    if command -v curl >/dev/null; then
        curl -fsSL "$NEWLIB_URL" -o "$ARCHIVE"
    else
        wget -q "$NEWLIB_URL" -O "$ARCHIVE"
    fi
fi
echo "$NEWLIB_SHA256  $ARCHIVE" | sha256sum -c -

rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT/src" "$BUILD_ROOT/build"
tar -xzf "$ARCHIVE" -C "$BUILD_ROOT/src" --strip-components=1
cd "$BUILD_ROOT/build"

"$BUILD_ROOT/src/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-shared \
    --disable-multilib \
    --disable-nls \
    --disable-newlib-supplied-syscalls \
    --disable-newlib-multithread \
    --enable-newlib-io-long-long \
    --enable-newlib-io-c99-formats \
    --enable-newlib-retargetable-locking
make -j"$JOBS" \
    CC_FOR_TARGET="${CROSS_COMPILE}gcc" \
    AR_FOR_TARGET="${CROSS_COMPILE}ar" \
    RANLIB_FOR_TARGET="${CROSS_COMPILE}ranlib" \
    CFLAGS_FOR_TARGET="$TARGET_CFLAGS" \
    CXXFLAGS_FOR_TARGET="$TARGET_CFLAGS"
make install

LIBDIR="$PREFIX/$TARGET/lib"
if [ ! -f "$LIBDIR/libc.a" ] || [ ! -f "$LIBDIR/libm.a" ]; then
    echo "PIC newlib install incomplete: expected libc.a and libm.a in $LIBDIR" >&2
    exit 1
fi
printf '%s\n' "$TARGET_CFLAGS" > "$PREFIX/.aar-pic-newlib-cflags"
printf '%s\n' "$NEWLIB_VERSION" > "$PREFIX/.aar-pic-newlib-version"
printf '%s\n' "$AAR_PROFILE" > "$PREFIX/.aar-pic-newlib-profile"
