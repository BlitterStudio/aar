#!/bin/sh
# Test of the test scripts: run_host.sh and m68k_check.sh must stop with a
# failing status when a program they run fails, wherever it sits in the script
# (a library test piped through tail, the mutation run, nm in a pipeline).
#
#   client/tests/scripts_test.sh
#
# It runs both scripts with fake tools that behave like the real ones, except
# for the one step a case makes fail. Needs no cross compiler. Writes to a
# temporary directory only.
#
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/../.."

OUT="${TMPDIR:-/tmp}/aar-scripts-test.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
failures=0

# A C compiler that builds everything for real, except that a program named
# core_test_* or shell_test_* (when FAKE_FAIL says so) becomes a stub that
# prints a failing summary line and exits 1.
cat > "$OUT/fakecc" <<'EOF'
#!/bin/sh
out=
prev=
for a in "$@"; do
    [ "$prev" = -o ] && out=$a
    prev=$a
done
case "$out" in
*/core_test_*)
    if [ "${FAKE_FAIL:-}" = core ]; then
        printf '#!/bin/sh\necho "core_test: 1 checks, 1 failures"\nexit 1\n' > "$out"
        chmod +x "$out"
        exit 0
    fi ;;
*/shell_test_*)
    if [ "${FAKE_FAIL:-}" = shell ]; then
        printf '#!/bin/sh\necho "shell_test: 1 checks, 1 failures"\nexit 1\n' > "$out"
        chmod +x "$out"
        exit 0
    fi ;;
esac
exec "${REAL_CC:-gcc}" "$@"
EOF
# python3, except that the mutation run reports a survivor and fails.
cat > "$OUT/fakepy" <<'EOF'
#!/bin/sh
case "$*" in
*mutants.py*)
    if [ "${FAKE_FAIL:-}" = mutants ]; then
        echo "mutants: 78 run, 1 not killed"
        exit 1
    fi ;;
esac
exec "${REAL_PYTHON:-python3}" "$@"
EOF
# The m68k tools: a compiler that writes an empty object and a stack-usage
# file, an objdump that shows a call through every library vector, an nm that
# lists no undefined symbol (or fails).
cat > "$OUT/fakecc68" <<'EOF'
#!/bin/sh
out=
prev=
for a in "$@"; do
    [ "$prev" = -o ] && out=$a
    prev=$a
done
: > "$out"
case " $* " in
*" -fstack-usage "*)
    printf 'x.c:1:1:f\t16\tstatic\n' > "${out%.o}.su" ;;
esac
EOF
cat > "$OUT/fakeobjdump" <<'EOF'
#!/bin/sh
lvo=30
while [ "$lvo" -le 174 ]; do
    # FAKE_FAIL=lvo: a header that lacks the last vector.
    if [ "${FAKE_FAIL:-}" = lvo ] && [ "$lvo" -eq 174 ]; then
        break
    fi
    echo "   0:	4eae ffe2 	jsr a6@(-$lvo)"
    lvo=$((lvo + 6))
done
EOF
cat > "$OUT/fakenm" <<'EOF'
#!/bin/sh
if [ "${FAKE_FAIL:-}" = nm ]; then
    echo "nm: cannot read the object" >&2
    exit 1
fi
if [ "${FAKE_FAIL:-}" = nm-undefined ]; then
    echo "         U _surprise"
fi
exit 0
EOF
chmod +x "$OUT/fakecc" "$OUT/fakepy" "$OUT/fakecc68" "$OUT/fakeobjdump" "$OUT/fakenm"

# expect_stop NAME MARKER COMMAND...: the command must exit non-zero and its
# output must contain MARKER (so that it stopped for the reason the case sets
# up, not for another).
expect_stop() {
    name=$1
    marker=$2
    shift 2
    "$@" > "$OUT/log" 2>&1
    status=$?
    if [ "$status" -ne 0 ] && grep -q "$marker" "$OUT/log"; then
        echo "ok   : $name (status $status)"
    else
        echo "FAIL : $name: status $status, marker '$marker' $(grep -c "$marker" "$OUT/log") times"
        tail -5 "$OUT/log"
        failures=$((failures + 1))
    fi
}

expect_pass() {
    name=$1
    shift
    "$@" > "$OUT/log" 2>&1
    status=$?
    if [ "$status" -eq 0 ]; then
        echo "ok   : $name"
    else
        echo "FAIL : $name: status $status"
        tail -5 "$OUT/log"
        failures=$((failures + 1))
    fi
}

host() {
    AAR_SKIP_MUTANTS="${SKIP_MUTANTS:-1}" CC="$OUT/fakecc" PYTHON="$OUT/fakepy" \
        sh client/tests/run_host.sh nom32
}
m68k() {
    CC="$OUT/fakecc68" OBJDUMP="$OUT/fakeobjdump" NM="$OUT/fakenm" \
        sh client/tests/m68k_check.sh
}

export FAKE_FAIL
FAKE_FAIL=none
expect_pass "m68k_check.sh with working tools" m68k
FAKE_FAIL=lvo
expect_stop "m68k_check.sh: the 25th vector is not called" "missing call through LVO -174" m68k
FAKE_FAIL=nm
expect_stop "m68k_check.sh: nm fails" "nm: cannot read" m68k
FAKE_FAIL=nm-undefined
expect_stop "m68k_check.sh: an undefined symbol" "needs:" m68k
FAKE_FAIL=core
expect_stop "run_host.sh: core_test fails" "core_test: 1 checks, 1 failures" host
FAKE_FAIL=shell
expect_stop "run_host.sh: shell_test fails" "shell_test: 1 checks, 1 failures" host
FAKE_FAIL=mutants
SKIP_MUTANTS=0
expect_stop "run_host.sh: a mutant survives" "1 not killed" host

if [ "$failures" -ne 0 ]; then
    echo "scripts_test: $failures failures"
    exit 1
fi
echo "scripts_test: all cases ok"
