#!/bin/sh
# Host tests of the reference loader (loader/): 64-bit, -m32 and with the
# address and undefined behaviour sanitizers.
#
#   loader/tests/run_host.sh            64-bit, plus -m32 when the compiler has it
#   loader/tests/run_host.sh nom32      64-bit only
#
# Needs a C compiler and python3. Writes to a temporary directory only. The
# tests, in each build:
#   place_test     plan and place on images built in memory: relocations of A32
#                  REL and RELA and of A64 RELA, the bss, write base unlike the
#                  run address, the memory limit, damaged plans and images
#   manifest_test  the C-ELF corpus of the conformance suite (both profiles)
#                  against its MANIFEST
#   corpus_test    through loader_corpus.py: the corpus of tests/aar_check_test.py
#                  and its mutations against tools/aar-check
#   fuzz_test      a bounded mutation fuzz run over generated images and the
#                  accepted images of the corpus (AAR_FUZZ_ITER, AAR_FUZZ_SEED)
# The sources also compile as C99 with no warning.
#
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/../.."

CC="${CC:-gcc}"
PYTHON="${PYTHON:-python3}"
OUT="${TMPDIR:-/tmp}/aar-loader-host.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -Iloader"
SRC="loader/aar_load.c loader/aar_image.c"
MANIFEST=conformance/negative/MANIFEST

# Whether the compiler builds a trivial program with the flags given.
cc_can() {
    printf 'int main(void){return 0;}\n' > "$OUT/cc_can.c"
    "$CC" "$@" "$OUT/cc_can.c" -o "$OUT/cc_can" 2>/dev/null
}

# The accepted images of the corpus: the seeds of the fuzz run besides the
# generated ones.
SEEDS=
while read -r name prof ids load probe; do
    case "$name" in
        '#'*|'') continue ;;
    esac
    [ "$load" = OK ] || continue
    SEEDS="$SEEDS conformance/negative/$prof/$name.aarimg"
done < "$MANIFEST"

# Portable C99: the sources the platforms take. (Not -pedantic: the header
# include/aar/app.h uses C11's _Static_assert, which GCC and Clang accept in
# C99 mode.)
for src in $SRC; do
    "$CC" -std=c99 -Wall -Wextra -Werror -Iinclude -Iloader -c "$src" -o "$OUT/c99.o"
done
echo "loader: aar_image.c and aar_load.c compile as C99, 0 warnings"

suite() {
    tag=$1
    iter=$2
    shift 2
    "$CC" $CFLAGS "$@" loader/tests/place_test.c $SRC -o "$OUT/place_test_$tag"
    "$OUT/place_test_$tag"
    "$CC" $CFLAGS "$@" loader/tests/manifest_test.c $SRC -o "$OUT/manifest_test_$tag"
    "$OUT/manifest_test_$tag" "$MANIFEST"
    "$CC" $CFLAGS "$@" loader/tests/corpus_test.c $SRC -o "$OUT/corpus_test_$tag"
    "$PYTHON" loader/tests/loader_corpus.py "$OUT/corpus_test_$tag"
    "$CC" $CFLAGS "$@" loader/tests/fuzz_test.c $SRC -o "$OUT/fuzz_test_$tag"
    # shellcheck disable=SC2086
    AAR_FUZZ_ITER="$iter" "$OUT/fuzz_test_$tag" $SEEDS
}

echo "loader: 64-bit"
suite plain 20000 -O2
if [ "${1:-}" != nom32 ] && cc_can -m32; then
    echo "loader: -m32"
    suite m32 20000 -O2 -m32
else
    echo "loader/tests/run_host.sh: no -m32 support here, skipped the 32-bit run"
fi
if cc_can -fsanitize=address,undefined; then
    echo "loader: address and undefined behaviour sanitizers"
    suite san 8000 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
else
    echo "loader/tests/run_host.sh: no sanitizers here, skipped the sanitizer run"
fi
echo "loader host tests: ok"
