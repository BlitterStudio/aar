#!/bin/sh
# Compile-only check of the aar.library client headers for AmigaOS m68k with
# the pinned cross compiler (the image of the firmware SDK's
# build-m68k-amigaos.sh). Run it inside that image, from the aar checkout:
#
#   docker run --rm -v "$PWD:/src:ro" \
#     amigadev/crosstools@sha256:6d96ee62dfac1ca84d54b063b4cfbc82bc6bf0971d6f5200be848d8c2f5fceee \
#     sh -c 'cp -r /src /w && cd /w && client/tests/m68k_check.sh'
#
# It compiles client/tests/m68k_compile_test.c with warnings as errors, then
# checks the object: the 24 vector offsets of the .fd file all appear as calls
# through the library base, and no symbol is left undefined. It then compiles
# the library sources (core, probe, shell, reference platform), see below.
#
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/../.."

CC="${CC:-m68k-amigaos-gcc}"
OBJDUMP="${OBJDUMP:-m68k-amigaos-objdump}"
NM="${NM:-m68k-amigaos-nm}"
OUT="${TMPDIR:-/tmp}/aar-m68k-check.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

"$CC" -std=gnu11 -noixemul -m68020 -O2 -Wall -Wextra -Werror \
    -Iclient/include -Iinclude \
    -c client/tests/m68k_compile_test.c -o "$OUT/t.o"

"$OBJDUMP" -d "$OUT/t.o" > "$OUT/t.dis"
missing=0
lvo=30
while [ "$lvo" -le 168 ]; do
    # objdump prints the call as "jsr a6@(-30)".
    if ! grep -Eq "jsr[[:space:]]+a6@\(-$lvo\)" "$OUT/t.dis"; then
        echo "missing call through LVO -$lvo"
        missing=1
    fi
    lvo=$((lvo + 6))
done
[ "$missing" = 0 ]

# The library sources compile for the 68k with no warning, no function uses
# more than 1 KiB of stack (so none of the interrupt and dispatcher paths does),
# and each leaves undefined only what it should: the core and the probe the
# string functions (no 64-bit division or multiplication helpers), the
# reference platform also the core's three extension functions, the shell
# utility.library's base and the core, probe and platform entry points. The
# shell is not built with -pedantic: the NDK's inline stubs convert function
# pointers. The largest stack frames are printed (-fstack-usage).
STR='memcpy|memset|memcmp'
check_source() {
    src=$1
    allowed=$2
    shift 2
    "$CC" -std=gnu11 -noixemul -m68020 -Os -Wall -Wextra -Werror "$@" \
        -Wstack-usage=1024 -fstack-usage -Iclient/include -Iinclude -c "$src" -o "$OUT/src.o"
    bad=$("$NM" "$OUT/src.o" | awk '$1 == "U" { print $2 }' | \
        grep -Ev "$allowed" || true)
    if [ -n "$bad" ]; then
        echo "$src needs: $bad"
        exit 1
    fi
    top=$(sort -t'	' -k2 -n -r "$OUT/src.su" | head -1 | awk -F'	' '{ print $1 " " $2 " bytes" }')
    echo "$src: compiles for m68k, 0 warnings, largest stack frame: $top"
}
check_source client/src/aar_probe.c "^_?($STR)\$" -pedantic
check_source client/src/aar_core.c "^_?($STR)\$" -pedantic
check_source client/src/refplat.c "^_?($STR|aar_core_ext_(stats|lease_pause|set_log_sink))\$" -pedantic
check_source client/src/aar_shell.c "^_?($STR|UtilityBase|aar_plat_get|aar_core_[a-z_]+|aar_probe_[a-z_]+)\$"

# The test defines AARBase itself; every call goes through it, so nothing
# else may be undefined (no stub library, no helper function).
undefined=$("$NM" "$OUT/t.o" | awk '$1 == "U" { print $2 }' | tr '\n' ' ')
if [ -n "$undefined" ]; then
    echo "undefined symbols: $undefined"
    exit 1
fi
echo "m68k header check: 24 vectors called, 0 warnings"
