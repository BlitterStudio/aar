#!/bin/sh
# Host tests of the aar.library client headers and the generator.
#
#   client/tests/run_host.sh            64-bit, plus -m32 when the compiler has it
#   client/tests/run_host.sh nom32      64-bit only
#
# Needs a C compiler and python3. Writes to a temporary directory only. The
# probe test builds a second time with the address and undefined behaviour
# sanitizers when the compiler has them.
#
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/../.."

CC="${CC:-gcc}"
PYTHON="${PYTHON:-python3}"
OUT="${TMPDIR:-/tmp}/aar-client-host.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -Iclient/tests/stubs -Iclient/include -Iinclude"

# The generated headers are current, and the generator itself is right.
"$PYTHON" client/tools/fdgen.py --check
"$PYTHON" client/tests/fdgen_test.py

run() {
    name=$1
    shift
    "$CC" $CFLAGS "$@" -o "$OUT/$name" client/tests/header_test.c
    "$OUT/$name"
}

run header64
if [ "${1:-}" != nom32 ] && echo 'int main(void){return 0;}' | "$CC" -m32 -x c - -o "$OUT/probe32" 2>/dev/null; then
    run header32 -m32
else
    echo "run_host.sh: no -m32 support here, skipped the 32-bit run"
fi

# The image probe against tools/aar-check: the corpus of tests/aar_check_test.py
# and mutations of it. Plain build, then the sanitizer build.
PROBE_SRC="client/tests/probe_test.c client/src/aar_probe.c"
"$CC" $CFLAGS -O2 -o "$OUT/probe_test" $PROBE_SRC
"$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test"
if [ "${1:-}" != nom32 ] && echo 'int main(void){return 0;}' | "$CC" -m32 -x c - -o "$OUT/probe32" 2>/dev/null; then
    "$CC" $CFLAGS -m32 -O2 -o "$OUT/probe_test32" $PROBE_SRC
    "$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test32"
fi
if echo 'int main(void){return 0;}' | "$CC" -fsanitize=address,undefined -x c - -o "$OUT/probe_san_check" 2>/dev/null; then
    "$CC" $CFLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
        -o "$OUT/probe_test_san" $PROBE_SRC
    "$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test_san"
else
    echo "run_host.sh: no sanitizers here, skipped the sanitizer run of the probe"
fi
