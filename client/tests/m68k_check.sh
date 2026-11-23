#!/bin/sh
# Compile-only check of the aar.library client headers for AmigaOS m68k with
# the pinned cross compiler (the image of the firmware SDK's
# build-m68k-amigaos.sh). Run it inside that image, from the aar checkout:
#
#   docker run --rm -v "$PWD:/src:ro" \
#     amigadev/crosstools@sha256:9c81fcd973058fb37f1ca5b1c608c65473dc1dd657ab5c8533e7b43b80f01cf3 \
#     sh -c 'cp -r /src /w && cd /w && client/tests/m68k_check.sh'
#
# It compiles client/tests/m68k_compile_test.c with warnings as errors, then
# checks the object: the 25 vector offsets of the .fd file all appear as calls
# through the library base, and no symbol is left undefined. It then compiles
# the library sources (core, probe, the image rules and loader of loader/,
# shell, reference platform), the serving loop
# (client/serve) and the launcher (client/run), and links aar-run, see below.
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
while [ "$lvo" -le 174 ]; do
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
# reference platform also the core's two extension functions, the shell
# utility.library's base and the core, probe and platform entry points. The
# shell is not built with -pedantic: the NDK's inline stubs convert function
# pointers. The largest stack frames are printed (-fstack-usage).
STR='memcpy|memset|memcmp|strlen'
stack=1024
check_source() {
    src=$1
    allowed=$2
    shift 2
    "$CC" -std=gnu11 -noixemul -m68020 -Os -Wall -Wextra -Werror "$@" \
        -Wstack-usage="$stack" -fstack-usage -Iclient/include -Iinclude -c "$src" -o "$OUT/src.o"
    # No pipelines: each program's status is checked (set -e), grep's only for
    # an error (1 means nothing was left over, which is the good case).
    "$NM" "$OUT/src.o" > "$OUT/src.nm"
    awk '$1 == "U" { print $2 }' "$OUT/src.nm" > "$OUT/src.undef"
    grep -Ev "$allowed" "$OUT/src.undef" > "$OUT/src.bad" || [ $? -eq 1 ]
    bad=$(cat "$OUT/src.bad")
    if [ -n "$bad" ]; then
        echo "$src needs: $bad"
        exit 1
    fi
    sort -t'	' -k2 -n -r "$OUT/src.su" > "$OUT/src.sorted"
    top=$(awk -F'	' 'NR == 1 { print $1 " " $2 " bytes" }' "$OUT/src.sorted")
    echo "$src: compiles for m68k, 0 warnings, largest stack frame: $top"
}
check_source client/src/aar_probe.c "^_?($STR|aar_image_check)\$" -pedantic
check_source loader/aar_image.c "^_?($STR)\$" -pedantic
check_source client/src/aar_core.c "^_?($STR)\$" -pedantic
check_source client/src/refplat.c "^_?($STR|aar_core_ext_(stats|lease_pause))\$" -pedantic
check_source client/src/aar_shell.c "^_?($STR|UtilityBase|aar_plat_get|aar_core_[a-z_]+|aar_probe_[a-z_]+)\$"

# The serving loop (client/serve) and the reference launcher (client/run). The
# public header compiles alone, in the C dialects the library header allows. The
# OS-free core compiles like the library core; its palette handler holds a 3 KiB
# table, so its stack limit is 4 KiB. aar_serve.c is the one translation unit of
# the loop (it includes the Amiga backends) and leaves undefined only the
# program's library bases, the core and the C library. aar-run is linked as the
# firmware SDK builds it (-lamiga -noixemul -m68030), so that a break is found
# here and not in the firmware build.
printf '#include <libraries/aar_serve.h>\nint aar_serve_header_alone;\n' > "$OUT/header_alone.c"
for std in gnu89 c99 "c11 -pedantic"; do
    "$CC" -std=$std -noixemul -m68020 -Wall -Wextra -Werror \
        -Iclient/include -Iinclude -c "$OUT/header_alone.c" -o "$OUT/header_alone.o"
done
echo "client/include/libraries/aar_serve.h: compiles alone as gnu89, c99 and c11 -pedantic, 0 warnings"
stack=4096
check_source client/serve/aar_serve_core.c "^_?($STR|strchr|strcmp)\$" -pedantic
check_source client/serve/aar_serve.c "^_?($STR|memmove|snprintf|AARBase|DOSBase|GfxBase|IntuitionBase|SysBase|__bcopz|aar_serve_[a-z0-9_]+)\$"
# aar-run is a program: it may leave the C library and the program's own library
# bases undefined; the link below is its undefined-symbol check.
check_source client/run/aar_run.c "."
# Every line aar-run prints goes through its own router, so that a Workbench
# start has one window: a reference to the C library's stdout writers is a bug.
grep -E '^_?(printf|puts|putchar|fprintf|vprintf|vfprintf)$' "$OUT/src.undef" > "$OUT/stdout.bad" || [ $? -eq 1 ]
if [ -s "$OUT/stdout.bad" ]; then
    echo "client/run/aar_run.c writes through the C library's stdout: $(cat "$OUT/stdout.bad")"
    exit 1
fi
echo "client/run/aar_run.c: no reference to the C library's stdout writers"
stack=1024
"$CC" -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror \
    -Iclient/include -Iinclude \
    client/run/aar_run.c client/serve/aar_serve.c client/serve/aar_serve_core.c \
    -lamiga -o "$OUT/aar-run"
test -e "$OUT/aar-run"
echo "aar-run: links for m68k with -lamiga -noixemul, 0 warnings, $(wc -c < "$OUT/aar-run") bytes"

# libaarserve.a, the loop as the firmware SDK package ships it (the source of the
# MIT code is the aar tag; the archive and the header go into the package's
# Developer directory), and the embedding example linked against it.
AR="${AR:-m68k-amigaos-ar}"
"$CC" -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror -Iclient/include -Iinclude \
    -c client/serve/aar_serve.c -o "$OUT/aar_serve.o"
"$CC" -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror -Iclient/include -Iinclude \
    -c client/serve/aar_serve_core.c -o "$OUT/aar_serve_core.o"
"$AR" rcs "$OUT/libaarserve.a" "$OUT/aar_serve.o" "$OUT/aar_serve_core.o"
check_source client/examples/aar-embed.c "."
"$CC" -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror -Iclient/include -Iinclude \
    client/examples/aar-embed.c -L"$OUT" -laarserve -lamiga -o "$OUT/aar-embed"
test -e "$OUT/aar-embed"
echo "libaarserve.a builds and client/examples/aar-embed.c links against it, 0 warnings"

# The conformance runner (conformance/runner): the program that runs the suite on
# a platform. It uses only aar.library and libaarserve.a and is built here like
# any other client program, no warning allowed. The core is aar-conf.c; each area
# has its r_<area>.c, and every .c file of the directory is part of the program
# (there is no list to edit). The catalogue header is generated from TESTS.md
# (python3 is in the image; it is not kept in git).
python3 conformance/tools/conf_catalogue.py gen
stack=4096
runner_srcs=$(ls conformance/runner/*.c)
for src in $runner_srcs; do
    check_source "$src" "."
done
# shellcheck disable=SC2086
"$CC" -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror -Iclient/include -Iinclude \
    $runner_srcs -L"$OUT" -laarserve -lamiga -o "$OUT/aar-conf"
test -e "$OUT/aar-conf"
echo "aar-conf: $(echo "$runner_srcs" | wc -l) files link against libaarserve.a for m68k, 0 warnings, $(wc -c < "$OUT/aar-conf") bytes"
stack=1024

# The test defines AARBase itself; every call goes through it, so nothing
# else may be undefined (no stub library, no helper function).
"$NM" "$OUT/t.o" > "$OUT/t.nm"
undefined=$(awk '$1 == "U" { printf "%s ", $2 }' "$OUT/t.nm")
if [ -n "$undefined" ]; then
    echo "undefined symbols: $undefined"
    exit 1
fi
echo "m68k header check: 25 vectors called, 0 warnings"
