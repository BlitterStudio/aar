# AAR conformance suite

This directory holds the tests that decide whether a platform implements the
AAR standard (specification section 11). Version 0.1 of the suite tests API 0.5.

| Path | Content |
|---|---|
| `TESTS.md` | The catalogue: one row for every test id, with the specification section, MUST or SHOULD, group, profile, who judges it and what it checks. It also lists what is not tested and why, and the questions this work raised about the specification |
| `payloads/` | The payloads: ARM apps (`conf_<area>.c`) that judge one area from the inside. `conf.h` is the protocol they print their results with |
| `runner/` | The runner `aar-conf`: an AmigaOS m68k program that uses only `aar.library` and the serving loop (`libaarserve.a`). `aar-conf.c` is the core (options, verdict, payload runs); each area has its file `r_<area>.c` for its runner-judged tests. It runs the payloads, judges the client-side tests, and writes the report. `conf_catalogue.h` is generated from `TESTS.md` at build time and is not in git |
| `tools/` | `conf_catalogue.py` reads `TESTS.md`, generates the runner's header and checks the ids against the sources; `check_results.py` checks one payload's output or a whole report |
| `tests/qemu_run.sh` | Runs payloads under qemu user mode with the loader of `libaar/tests/loader.c` |

## What passing means

A platform conforms to a profile at an API major.minor when (spec 11.1):

1. it passes every MUST test of that profile and of every group it declares;
2. it does not declare a group it does not pass;
3. the suite ran with the platform's own `aar.library` and the unmodified
   payloads, and the results are in the standard format (below), with the
   suite version and the platform's `AARInfo` dump;
4. every SKIP has a reason, and a SKIP is only for a test of an undeclared
   group or a SHOULD test.

The host runner (`host-runner/`) passes the payloads that build for it and is the
reference for app semantics. It does not count as a platform. A test that needs a
client or a platform prints `SKIP host: ...` there; such a line is allowed in the
host run and in the qemu run and never in a platform's report.

Tests that need a person (reset the machine, hear a sound, see a picture) are
`operator` tests. By default the runner skips them with the reason `manual`. With
`INTERACTIVE` it asks yes or no on the console and records the answer. A SHOULD
operator test that is skipped does not affect the verdict. A MUST operator test
that is skipped does, so a platform's report must include the operator's run of
those.

A test marked `todo` in `TESTS.md` is not written yet. The runner prints
`SKIP suite: not implemented` for it, which is not allowed for a MUST test, so a
full run of a suite that is not complete says `DOES-NOT-CONFORM` whatever the
platform does. Look at the PASS and FAIL lines in that case.

## Building

The payloads are built by `mk/aar.mk` like any app. In the SDK image:

```sh
make                  # build/a32/conf_*.elf, and build/a64/conf_*.elf with the A64 compiler
make check            # also runs aar-check on every payload and checks the catalogue
make check-runtime    # runs the payloads under qemu user mode (needs qemu and Linux cross gcc)
```

The runner needs the m68k cross compiler of the firmware SDK. It is built, with
no warning, by `client/tests/m68k_check.sh`:

```sh
docker run --rm -v "$PWD:/src:ro" \
  amigadev/crosstools@sha256:9c81fcd973058fb37f1ca5b1c608c65473dc1dd657ab5c8533e7b43b80f01cf3 \
  sh -c 'cp -r /src /w && cd /w && sh client/tests/m68k_check.sh'
```

To build it by hand against `libaarserve.a`, use the last compile line of that
script (`m68k-amigaos-gcc -noixemul -m68030 ... conformance/runner/aar-conf.c
-L<dir with libaarserve.a> -laarserve -lamiga`).

The host runner builds the payloads that make sense on a PC and runs them under
CTest with a checker (`cmake -S host-runner -B build-host && cmake --build
build-host && ctest --test-dir build-host -R conf`).

## Running on a platform

Copy `aar-conf` and the payload directories to the Amiga. The payloads of a
profile live in a directory named after it:

```
aar-conf
a32/conf_time.elf
a32/conf_...elf
a64/...
```

(`build/` has exactly this layout.) Then, from a Shell with at least 32 KiB of
stack:

```
Stack 32768
aar-conf DIR=<directory that holds a32 and a64> PROFILE=a32 TO=RAM:conf-a32.txt
```

| Option | Meaning |
|---|---|
| `DIR` | Directory with the `a32` and `a64` payload directories. Default `PROGDIR:` |
| `PROFILE` | `a32` or `a64`. Default: A32 if the platform lists it, else A64. Run the suite once per profile the platform declares |
| `TO` | Also write the report to this file |
| `ONLY` | Run only the ids that start with this text, for example `C-TIME` or `C-FILE.4`. The verdict is then `PARTIAL` |
| `ROOT` | The directory the payloads' relative file paths resolve under and where the runner prepares fixtures. Default `T:aar-conf` |
| `INTERACTIVE` | Ask the operator for the operator tests |

The runner returns 0 when the verdict is `CONFORMS` and 10 otherwise. The payloads
take a few minutes in all; the watchdog and timeout tests take most of it.

## Reading the result

The report is plain text, one fact per line. Lines that start with `#` are
comments: the console output of the payloads and diagnostics.

An excerpt of a report from a platform while the suite is half written (the numbers are an example):

```
AAR-CONF-SUITE 0.1 a32
AAR-CONF-INFO api_major=0
AAR-CONF-INFO groups=0x17
...
AAR-CONF C-TIME.1 PASS 200000 reads and a sleep, 20311 us in all
AAR-CONF C-TIME.2 PASS smallest step 1 us in 52 steps
AAR-CONF C-GL.1 SKIP group not declared
AAR-CONF-SUMMARY pass=41 fail=0 skip=359 verdict=DOES-NOT-CONFORM
```

- `AAR-CONF-SUITE <version> <profile>` is the first line.
- `AAR-CONF-INFO key=value` is the `AARInfo` of the platform (`AARQuery`), plus the
  library version.
- `AAR-CONF <id> PASS|FAIL|SKIP <detail>`: one line for every test of the
  profile, in the order of `TESTS.md`. The detail says what was measured or why
  the test failed or was skipped.
- `AAR-CONF-SUMMARY pass=N fail=N skip=N verdict=CONFORMS|DOES-NOT-CONFORM`
  ends it. The verdict follows spec 11.1: no MUST test failed, and no MUST test of
  a declared group was skipped. A filtered run (`ONLY`) says `PARTIAL`.

`python3 conformance/tools/check_results.py report <file>` validates a report: the
suite line, the `AARInfo` lines, one line per test, a summary whose counts match,
and a verdict that is the one the rules give. Add `--require-conforms` to make a
non-conforming report an error. The report goes into the pull request or the
platform's documentation.

## How a payload works

A payload is an AAR app with the usual `aar_main`. It prints one line per test id
through the console (`aar_console_write`, no libc needed) with the helpers of
`payloads/conf.h`:

```c
struct conf_test t;
conf_begin(&t, "C-AREA.1");
conf_check(&t, value == expected, "value differs from expected");
conf_end(&t);                      /* prints AAR-CONF C-AREA.1 PASS|FAIL ... */
...
return conf_finish("C-AREA");      /* prints AAR-CONF-END, returns 0 or 10 */
```

The runner starts the payload with `argv` = the file name, `alpha`, `beta gamma`,
`conf:peer=1` and some `info:<key>=<n>` words that carry the client's `AARInfo`
(groups, platform id, message capacity, stall limit, stop grace, argument limits).
It serves the payload with `aar_serve` and a console of its own that captures the
text, and relays every `AAR-CONF` line. It fails every id the catalogue says the
payload prints (judged by `payload`, impl `yes`, group declared, profile
matching) and that did not arrive, so a payload that crashed, was refused or hung
cannot pass by silence. A payload must print the end line last; the runner checks
that the counts in it match the lines, and that the exit code read through
`AARStatus` is 0 when nothing failed and 10 otherwise (C-LIFE.3).

A payload asks the runner for help by sending a 32-byte message that starts with a
keyword (`payloads/conf_proto.h`). Today there is one, `CONF-STOP-NOW`: the runner
stops the app as a client would. C-TIME.5 uses it. When a test needs the runner and
there is none (`conf:peer=1` missing: a run by hand, the host runner, the qemu
loader), it prints `SKIP host: <reason>`.

## Adding tests

`CONTRIBUTING.md` is the guide for the author of an area: which files the area
owns, how to mark rows `impl yes`, how to add peer keywords, payload variants and
console modes to the runner, what to do about gaps in the spec, and how to run the
host, qemu and m68k checks. In short: the rows are in `TESTS.md`; a payload test is
printed by `payloads/conf_<area>.c`; a runner test is listed in
`runner/r_<area>.c`; `python3 conformance/tools/conf_catalogue.py check` keeps the
catalogue and the sources in step.
