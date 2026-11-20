#!/bin/sh
# Compile-only check of the aar.library client headers for AmigaOS m68k with
# the pinned cross compiler (the image of the firmware SDK's
# build-m68k-amigaos.sh). Run it inside that image, from the aar checkout:
#
#   docker run --rm -v "$PWD:/src:ro" \
#     amigadev/crosstools@sha256:c0e453fde28fd9c52461f4f9a926c4481548dcfcaca94b0740d8cce4561f2e1e \
#     sh -c 'cp -r /src /w && cd /w && client/tests/m68k_check.sh'
#
# It compiles client/tests/m68k_compile_test.c with warnings as errors, then
# checks the object: the 24 vector offsets of the .fd file all appear as calls
# through the library base, and no symbol is left undefined.
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

# The test defines AARBase itself; every call goes through it, so nothing
# else may be undefined (no stub library, no helper function).
undefined=$("$NM" "$OUT/t.o" | awk '$1 == "U" { print $2 }' | tr '\n' ' ')
if [ -n "$undefined" ]; then
    echo "undefined symbols: $undefined"
    exit 1
fi
echo "m68k header check: 24 vectors called, 0 warnings"
