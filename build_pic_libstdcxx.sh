#!/bin/sh
# Build the PIE-safe C++ runtime for one AAR profile from the GCC source of Arm
# GNU Toolchain 15.3.rel2 (GCC 15.3.1, the source snapshot of that release):
# libstdc++.a and libsupc++.a, and libgcc_eh.a, the unwinder. The archives that
# ship with the toolchain are not usable in an app. Stock A32 libraries are
# Thumb code with absolute relocations, and both profiles carry data
# relocations in read-only sections, which the app image rules (spec 2.2) and
# `ld -z text` refuse. These are built -fPIC, in the profile's instruction set,
# with the same configuration as the toolchain's own libstdc++.
#
#   AAR_PROFILE=a32   arm-none-eabi, Cortex-A9, hard-float, NEON, ARM state
#   AAR_PROFILE=a64   aarch64-none-elf, ARMv8-A, FP and ASIMD
#
# The libraries land in the same directory as the PIC newlib of
# build_pic_newlib.sh, <prefix>/<target>/lib, where mk/aar.mk and
# cmake/aar-toolchain-*.cmake already look. The headers are the toolchain's:
# the script stops unless the generated bits/c++config.h matches the
# toolchain's for this profile. Run it once per profile. Suggested Docker cache
# volumes:
#   -v aar-sysroot-a32:/opt/aar-sysroot-a32
#   -v aar-gcc-cache:/opt/aar-gcc-cache
# The target compiler (arm-none-eabi-g++ or aarch64-none-elf-g++) must be on
# PATH, or set CROSS_COMPILE. The host needs a C++ compiler, make, tar, xz,
# curl or wget, and the GMP, MPFR and MPC development packages, because libgcc
# is configured from a GCC tree that has to build its generator programs (the
# target libraries themselves are compiled with the toolchain's compiler).
#
# SPDX-License-Identifier: MIT
set -e
set -u

ARM_TOOLCHAIN_VERSION=15.3.rel2
GCC_SRC_SHA256=59bf354b14755172eb238511542c599a6ec3540a19164b8dfe1accbe084c8089
GCC_SRC_URL=https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/${ARM_TOOLCHAIN_VERSION}/arm-gnu-toolchain-src-snapshot-${ARM_TOOLCHAIN_VERSION}.tar.xz

AAR_PROFILE=${AAR_PROFILE:-a32}
case "$AAR_PROFILE" in
a32)
    TARGET=arm-none-eabi
    PROFILE_FLAGS='-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard'
    DEFAULT_PREFIX=${AAR_SYSROOT_A32:-}
    ;;
a64)
    TARGET=aarch64-none-elf
    PROFILE_FLAGS='-march=armv8-a'
    DEFAULT_PREFIX=${AAR_SYSROOT_A64:-}
    ;;
*)
    echo "AAR_PROFILE must be a32 or a64, got '$AAR_PROFILE'" >&2
    exit 2
    ;;
esac
# Same flags as build_pic_newlib.sh.
PIC_FLAGS='-fPIC -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections'

PREFIX=${AAR_SYSROOT:-$DEFAULT_PREFIX}
if [ -z "$PREFIX" ]; then
    echo "set AAR_SYSROOT (or AAR_SYSROOT_A32 / AAR_SYSROOT_A64) to the PIC sysroot to build into" >&2
    exit 2
fi
CACHE=${AAR_GCC_CACHE:-"$(dirname "$PREFIX")/aar-gcc-cache"}
BUILD_ROOT=${AAR_GCC_BUILD:-"$(pwd)/.build-pic-libstdcxx-$AAR_PROFILE"}
JOBS=${JOBS:-2}

: "${CROSS_COMPILE:=$TARGET-}"
for tool in gcc g++ ar ranlib; do
    command -v "${CROSS_COMPILE}$tool" >/dev/null
done
command -v g++ >/dev/null
command -v make >/dev/null
command -v tar >/dev/null
command -v xz >/dev/null
command -v sha256sum >/dev/null
TARGET_CC="${CROSS_COMPILE}gcc"
TARGET_CXX="${CROSS_COMPILE}g++"
TOOLCHAIN_ROOT=$(cd "$(dirname "$(command -v "$TARGET_CC")")/.." && pwd)
TOOLCHAIN_HEADERS="$TOOLCHAIN_ROOT/$TARGET/include"
if [ ! -d "$TOOLCHAIN_HEADERS/c++" ]; then
    echo "no libstdc++ headers in $TOOLCHAIN_HEADERS: is $TARGET_CC the Arm GNU Toolchain?" >&2
    exit 1
fi
# Where the toolchain keeps the profile's bits/c++config.h.
GXX_VERSION=$("$TARGET_CXX" -dumpfullversion)
MULTI_DIR=$($TARGET_CXX $PROFILE_FLAGS -print-multi-directory)
TOOLCHAIN_CXXCONFIG="$TOOLCHAIN_HEADERS/c++/$GXX_VERSION/$TARGET/$MULTI_DIR/bits/c++config.h"
if [ ! -f "$TOOLCHAIN_CXXCONFIG" ]; then
    TOOLCHAIN_CXXCONFIG="$TOOLCHAIN_HEADERS/c++/$GXX_VERSION/$TARGET/bits/c++config.h"
fi
[ -f "$TOOLCHAIN_CXXCONFIG" ]

mkdir -p "$CACHE"
ARCHIVE="$CACHE/arm-gnu-toolchain-src-snapshot-${ARM_TOOLCHAIN_VERSION}.tar.xz"
if [ ! -f "$ARCHIVE" ]; then
    if command -v curl >/dev/null; then
        curl -fsSL "$GCC_SRC_URL" -o "$ARCHIVE"
    else
        wget -q "$GCC_SRC_URL" -O "$ARCHIVE"
    fi
fi
echo "$GCC_SRC_SHA256  $ARCHIVE" | sha256sum -c -

rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT/src" "$BUILD_ROOT/gcc" "$BUILD_ROOT/libstdc++"
# The snapshot holds the whole toolchain; the GCC tree is the one directory
# these libraries come from.
tar -xJf "$ARCHIVE" -C "$BUILD_ROOT/src" --strip-components=2 ./gcc
SRC="$BUILD_ROOT/src"

# ---- libgcc_eh. libgcc configures from a built GCC tree (tm.h, libgcc.mvars),
# so build the driver once, then libgcc with the toolchain's compiler.
cd "$BUILD_ROOT/gcc"
"$SRC/configure" \
    --target="$TARGET" \
    --enable-languages=c \
    --disable-bootstrap \
    --disable-nls \
    --disable-multilib \
    --disable-threads \
    --disable-shared \
    --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
    --with-newlib \
    --with-headers="$TOOLCHAIN_HEADERS" \
    --with-gnu-as --with-gnu-ld
make -j"$JOBS" all-gcc
make -j"$JOBS" all-target-libgcc \
    CC_FOR_TARGET="$TARGET_CC" \
    CFLAGS_FOR_TARGET="$PROFILE_FLAGS $PIC_FLAGS"

# A static libgcc.a holds the unwinder members (LIB2ADDEH). Collect them in
# libgcc_eh.a, which the link names before libgcc.a. libgcc.a itself stays the
# toolchain's.
LIBGCC_DIR="$BUILD_ROOT/gcc/$TARGET/libgcc"
cat > "$BUILD_ROOT/print-var.mk" <<'EOF'
print-%: ; @echo $($*)
EOF
EH_OBJS=$(make -s -C "$LIBGCC_DIR" -f Makefile -f "$BUILD_ROOT/print-var.mk" print-LIB2ADDEH \
    | tr ' ' '\n' | sed 's#.*/##; s#\.[cS]$#.o#')
[ -n "$EH_OBJS" ]
rm -f "$LIBGCC_DIR/libgcc_eh.a"
# shellcheck disable=SC2086
(cd "$LIBGCC_DIR" && "${CROSS_COMPILE}ar" rc libgcc_eh.a $EH_OBJS && "${CROSS_COMPILE}ranlib" libgcc_eh.a)

# ---- libstdc++ and libsupc++. The profile flags go in CXX, not CXXFLAGS: the
# configure tests replace CXXFLAGS and must still compile for the profile. The
# toolchain's configure ran without the C++ headers on the path and with its
# own newlib; the four settings below are what that run found.
# -U__STRICT_ANSI__ lets the <complex.h> probes see the C library header.
cd "$BUILD_ROOT/libstdc++"
CC="$TARGET_CC $PROFILE_FLAGS" \
CXX="$TARGET_CXX $PROFILE_FLAGS -U__STRICT_ANSI__" \
AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" \
CFLAGS="$PIC_FLAGS" CXXFLAGS="$PIC_FLAGS" \
LDFLAGS="--specs=nosys.specs" \
glibcxx_cv_getentropy=no glibcxx_cv_arc4random=no glibcxx_cv_truncate=yes \
    "$SRC/libstdc++-v3/configure" \
    --build="$("$SRC/config.guess")" \
    --host="$TARGET" \
    --target="$TARGET" \
    --prefix="$BUILD_ROOT/stage" \
    --disable-shared \
    --disable-nls \
    --disable-multilib \
    --disable-libstdcxx-pch \
    --with-newlib
if ! diff "$TOOLCHAIN_CXXCONFIG" "$BUILD_ROOT/libstdc++/include/$TARGET/bits/c++config.h"; then
    echo "libstdc++ configuration differs from the toolchain's ($TOOLCHAIN_CXXCONFIG)" >&2
    exit 1
fi
make -j"$JOBS"

LIBDIR="$PREFIX/$TARGET/lib"
mkdir -p "$LIBDIR"
cp "$BUILD_ROOT/libstdc++/src/.libs/libstdc++.a" "$LIBDIR/libstdc++.a"
cp "$BUILD_ROOT/libstdc++/libsupc++/.libs/libsupc++.a" "$LIBDIR/libsupc++.a"
cp "$LIBGCC_DIR/libgcc_eh.a" "$LIBDIR/libgcc_eh.a"
for lib in libstdc++.a libsupc++.a libgcc_eh.a; do
    if [ ! -f "$LIBDIR/$lib" ]; then
        echo "PIC C++ runtime install incomplete: no $lib in $LIBDIR" >&2
        exit 1
    fi
done
printf '%s\n' "$PROFILE_FLAGS $PIC_FLAGS" > "$PREFIX/.aar-pic-libstdcxx-cflags"
printf '%s\n' "$ARM_TOOLCHAIN_VERSION" > "$PREFIX/.aar-pic-libstdcxx-version"
printf '%s\n' "$AAR_PROFILE" > "$PREFIX/.aar-pic-libstdcxx-profile"
