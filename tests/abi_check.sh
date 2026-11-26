#!/bin/sh
# Checks the AAR public headers (include/aar/*.h).
#
#   tests/abi_check.sh host      layout and transport model tests for 64-bit
#                                and -m32 (the -m32 run needs a multilib gcc),
#                                plus a C++ syntax check of every header
#   tests/abi_check.sh a32       headers and layout test for A32 with
#                                arm-none-eabi-gcc, and the AAR_NOTE bytes
#   tests/abi_check.sh a64       the same for A64 with aarch64-linux-gnu-gcc
#                                (or aarch64-none-elf-gcc)
#
# Run it from a container, for example:
#   docker run --rm -v "$PWD:/w" -w /w gcc:13 sh -c \
#       'apt-get update -qq && apt-get install -y -qq gcc-multilib g++-multilib && CC32=gcc-12 tests/abi_check.sh host'
#   docker run --rm -v "$PWD:/w" -w /w aar-sdk:local tests/abi_check.sh a32
#   docker run --rm -v "$PWD:/w" -w /w debian:bookworm sh -c \
#       'apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu && tests/abi_check.sh a64'
#
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -Iinclude"
OUT="${TMPDIR:-/tmp}/aar-abi-check.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

host() {
    # CC64 and CC32 name the compilers. The official gcc image has no 32-bit
    # libgcc for its own compiler, so there CC32=gcc-12 (Debian's, from the
    # gcc-multilib package) and CC64=gcc (13).
    for m in 32 64; do
        if [ "$m" = 32 ]; then cc="${CC32:-gcc} -m32"; else cc="${CC64:-gcc}"; fi
        $cc $CFLAGS tests/abi_layout_test.c -o "$OUT/layout$m"
        "$OUT/layout$m"
        $cc $CFLAGS -O2 tests/transport_model_test.c -o "$OUT/transport$m"
        "$OUT/transport$m"
    done
    # C++ must see the same headers.
    for h in app gl ext_zz9000 note transport; do
        echo "#include \"aar/$h.h\"" | "${CXX:-g++}" -std=c++11 -Wall -Wextra -Werror -pedantic -Iinclude -x c++ -fsyntax-only -
    done
}

# $1 = compiler prefix, $2.. = target flags; PROFILE_HEX is the profile word.
cross() {
    prefix=$1
    shift
    cc="${prefix}gcc"
    for h in app gl ext_zz9000 note transport; do
        echo "#include \"aar/$h.h\"" | $cc "$@" $CFLAGS -x c -fsyntax-only -
    done
    # The static asserts in the headers and the layout test, for the real data
    # model of the target.
    $cc "$@" $CFLAGS -c tests/abi_layout_test.c -o "$OUT/layout.o"
    $cc "$@" $CFLAGS -c tests/note_probe.c -o "$OUT/note.o"
    "${prefix}readelf" -n "$OUT/note.o"
    "${prefix}objcopy" -O binary -j .note.aar "$OUT/note.o" "$OUT/note.bin"
    size=$(wc -c < "$OUT/note.bin" | tr -d ' ')
    if [ "$size" != 56 ]; then
        echo "FAIL: note is $size bytes, expected 56 (12 header, 4 name, 40 descriptor)"
        exit 1
    fi
    got=$(od -An -tx1 -v "$OUT/note.bin" | tr -d ' \n')
    # namesz 4, descsz 40, type 1, "AAR\0", then ten words: magic, note_version
    # 1, profile, api_major 0, api_minor_required 6, stack 0x20000, min heap 8,
    # groups_required DISPLAY|GL = 9, groups_optional AUDIO = 4, reserved 0.
    want=$(printf '%s' \
        "04000000" "28000000" "01000000" "41415200" \
        "4141524e" "01000000" "$PROFILE_HEX" "00000000" "06000000" \
        "00000200" "08000000" "09000000" "04000000" "00000000")
    if [ "$got" != "$want" ]; then
        echo "FAIL: note bytes differ"
        echo "  got  $got"
        echo "  want $want"
        exit 1
    fi
    echo "note: 56 bytes, descriptor 40, bytes as expected"
}

case "${1:-}" in
host)
    host
    ;;
a32)
    PROFILE_HEX="01000000"
    cross arm-none-eabi- -mcpu=cortex-a9 -mfloat-abi=hard -mfpu=neon
    ;;
a64)
    PROFILE_HEX="02000000"
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        cross aarch64-linux-gnu-
    else
        cross aarch64-none-elf-
    fi
    ;;
*)
    echo "usage: $0 host|a32|a64" >&2
    exit 2
    ;;
esac
echo "abi_check $1: ok"
