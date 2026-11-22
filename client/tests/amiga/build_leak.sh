#!/bin/sh
# Builds aar.library over the reference platform and the leak probe, for a real
# AmigaOS (no board needed). Run inside the pinned m68k image, from the aar
# checkout:
#
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/out:/out" \
#     amigadev/crosstools@sha256:9c81fcd973058fb37f1ca5b1c608c65473dc1dd657ab5c8533e7b43b80f01cf3 \
#     sh -c 'cp -r /src /w && cd /w && sh client/tests/amiga/build_leak.sh /out'
#
# Writes aar.library and aarleak to the directory given (default ./out).
#
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/../../.."
OUT="${1:-out}"
mkdir -p "$OUT"
CC="${CC:-m68k-amigaos-gcc}"
F="-std=gnu11 -noixemul -m68020 -Os -Wall -Wextra -Iclient/include -Iinclude -Iclient/tests/amiga"

# refplat's own aar_plat_get is renamed: leak_plat.c starts the platform clean.
$CC $F -Daar_plat_get=refplat_get -c client/src/refplat.c -o "$OUT/refplat.o"
$CC $F -c client/tests/amiga/leak_plat.c -o "$OUT/leak_plat.o"
$CC $F -c client/src/aar_shell.c -o "$OUT/aar_shell.o"
$CC $F -c client/src/aar_core.c -o "$OUT/aar_core.o"
$CC $F -c client/src/aar_probe.c -o "$OUT/aar_probe.o"
$CC -noixemul -nostartfiles -Os -m68020 "$OUT/aar_shell.o" "$OUT/aar_core.o" "$OUT/aar_probe.o" \
    "$OUT/refplat.o" "$OUT/leak_plat.o" -o "$OUT/aar.library"
$CC $F client/tests/amiga/aarleak.c -o "$OUT/aarleak"
ls -l "$OUT/aar.library" "$OUT/aarleak"
