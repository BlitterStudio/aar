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

# No command here is the producer of a pipeline: a pipeline reports the status
# of its last command, so `test | tail` passes when the test fails. A command
# whose output is shortened runs with its output in a file (run_tail), and a
# probe of the compiler reads its source from a file (cc_can), so that every
# status is the one of the program that ran, and set -e sees it.
run_tail() {
    lines=$1
    shift
    log="$OUT/run_tail.log"
    if "$@" > "$log" 2>&1; then
        tail -n "$lines" "$log"
    else
        status=$?
        cat "$log"
        echo "run_host.sh: $* failed with status $status" >&2
        exit "$status"
    fi
}

# Whether the compiler builds a trivial program with the flags given.
cc_can() {
    printf 'int main(void){return 0;}\n' > "$OUT/cc_can.c"
    "$CC" "$@" "$OUT/cc_can.c" -o "$OUT/cc_can" 2>/dev/null
}

# The generated headers are current, and the generator itself is right.
"$PYTHON" client/tools/fdgen.py --check
"$PYTHON" client/tests/fdgen_test.py

# The client API document names every public symbol.
"$PYTHON" client/tools/check_client_doc.py --self-test

# The Workbench icon of aar-run is what its generator makes.
"$PYTHON" client/run/make_icon.py --check

run() {
    name=$1
    shift
    "$CC" $CFLAGS "$@" -o "$OUT/$name" client/tests/header_test.c
    "$OUT/$name"
}

run header64
if [ "${1:-}" != nom32 ] && cc_can -m32; then
    run header32 -m32
else
    echo "run_host.sh: no -m32 support here, skipped the 32-bit run"
fi

# C++ sees the same headers.
if command -v "${CXX:-g++}" >/dev/null 2>&1; then
    for h in libraries/aar.h libraries/aar_zz9000.h libraries/aar_serve.h proto/aar.h; do
        printf '#include <%s>\n' "$h" > "$OUT/header.cc"
        "${CXX:-g++}" -std=c++11 -Wall -Wextra -Werror \
            -Iclient/tests/stubs -Iclient/include -Iinclude -fsyntax-only "$OUT/header.cc"
    done
    printf '#include "client/src/aar_plat.h"\n#include "client/src/aar_probe.h"\n#include "client/src/aar_core.h"\n' \
        > "$OUT/library.cc"
    "${CXX:-g++}" -std=c++11 -Wall -Wextra -Werror \
        -Iclient/tests/stubs -Iclient/include -Iinclude -I. -fsyntax-only "$OUT/library.cc"
else
    echo "run_host.sh: no C++ compiler here, skipped the C++ header check"
fi

# The image probe against tools/aar-check: the corpus of tests/aar_check_test.py
# and mutations of it. Plain build, then the sanitizer build.
PROBE_SRC="client/tests/probe_test.c client/src/aar_probe.c"
"$CC" $CFLAGS -O2 -o "$OUT/probe_test" $PROBE_SRC
"$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test"
if [ "${1:-}" != nom32 ] && cc_can -m32; then
    "$CC" $CFLAGS -m32 -O2 -o "$OUT/probe_test32" $PROBE_SRC
    "$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test32"
fi
if cc_can -fsanitize=address,undefined; then
    "$CC" $CFLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
        -o "$OUT/probe_test_san" $PROBE_SRC
    "$PYTHON" client/tests/probe_corpus.py "$OUT/probe_test_san"
else
    echo "run_host.sh: no sanitizers here, skipped the sanitizer run of the probe"
fi

# The library core (client/src/aar_core.c) against the reference platform, and
# the AmigaOS shell (client/src/aar_shell.c) on a fake system: plain, -m32 and
# with the sanitizers. Pointers into the reference arena travel through 32-bit
# APTR fields, so these programs are linked without PIE.
LIB_CFLAGS="$CFLAGS -Iclient/tests -no-pie -fno-pie"
CORE_SRC="client/tests/core_test.c client/src/aar_core.c client/src/refplat.c"
SHELL_SRC="client/tests/shell_test.c client/src/aar_core.c client/src/refplat.c client/src/aar_probe.c"
SERVE_CFLAGS="$CFLAGS -Iclient/serve -Iclient/run"
"$PYTHON" client/tests/make_images.py "$OUT/good.elf"
lib_tests() {
    tag=$1
    shift
    "$CC" $LIB_CFLAGS "$@" $CORE_SRC -o "$OUT/core_test_$tag"
    run_tail 1 "$OUT/core_test_$tag"
    "$CC" $LIB_CFLAGS "$@" $SHELL_SRC -o "$OUT/shell_test_$tag"
    run_tail 1 "$OUT/shell_test_$tag" "$OUT/good.elf"
    # The process handling of the leak probe, on a model of the calls it makes.
    "$CC" $LIB_CFLAGS -Wno-unused-function -pthread "$@" client/tests/aarleak_test.c -o "$OUT/aarleak_test_$tag"
    run_tail 1 "$OUT/aarleak_test_$tag"
    # The serving loop's OS-free core (client/serve) and the launcher's policy
    # (client/run): the request handlers against a fake
    # volume, console, display and audio, and the decisions of the loop and of
    # the launcher. No library, no reference platform.
    "$CC" $SERVE_CFLAGS "$@" client/tests/serve_core_test.c client/serve/aar_serve_core.c -o "$OUT/serve_core_test_$tag"
    run_tail 1 "$OUT/serve_core_test_$tag"
    "$CC" $SERVE_CFLAGS "$@" client/tests/serve_policy_test.c client/serve/aar_serve_core.c -o "$OUT/serve_policy_test_$tag"
    run_tail 1 "$OUT/serve_policy_test_$tag"
    "$CC" $SERVE_CFLAGS "$@" client/tests/run_policy_test.c -o "$OUT/run_policy_test_$tag"
    run_tail 1 "$OUT/run_policy_test_$tag"
    # The serving loop itself (client/serve/aar_serve.c with its backends) on the
    # fake system of serve_host.h, the library core and the reference platform.
    # The Amiga code passes addresses through 32-bit words (DOS packets, tags).
    "$CC" $LIB_CFLAGS -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Iclient/serve "$@" \
        client/tests/serve_loop_test.c client/serve/aar_serve_core.c client/src/aar_core.c client/src/refplat.c \
        -o "$OUT/serve_loop_test_$tag"
    run_tail 1 "$OUT/serve_loop_test_$tag"
}
lib_tests plain -O2
if [ "${1:-}" != nom32 ] && cc_can -m32; then
    lib_tests m32 -O2 -m32
fi
if cc_can -fsanitize=address,undefined; then
    lib_tests san -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
else
    echo "run_host.sh: no sanitizers here, skipped the sanitizer run of the library tests"
fi

# The tests fail on deliberately broken code (a check removed, two steps swapped).
if [ "${AAR_SKIP_MUTANTS:-0}" != 1 ]; then
    run_tail 3 "$PYTHON" client/tests/mutants.py
fi
