# AAR conformance suite

This directory holds the tests that decide whether a platform implements the
AAR standard ([specification section 11](../spec/aar.md)). Version 0.2 of the suite tests API 0.6.

| Path | Content |
|---|---|
| [`TESTS.md`](TESTS.md) | The catalogue: one row per test id, with its spec section, requirement level, group, profile, judge and check. It also lists untested rules and spec questions |
| `payloads/` | The payloads: ARM apps (`conf_<area>.c`) that judge one area from the inside. `conf.h` is the protocol they print their results with |
| `runner/` | The runner `aar-conf`: an AmigaOS m68k program that uses only `aar.library` and the serving loop (`libaarserve.a`). `aar-conf.c` is the core (options, verdict, payload runs); each area has its file `r_<area>.c` for its runner-judged tests. It runs the payloads, judges the client-side tests, and writes the report. `conf_catalogue.h` is generated from `TESTS.md` at build time and is not in git |
| `tools/` | `conf_catalogue.py` reads `TESTS.md`, generates the runner's header and checks the ids against the sources; `check_results.py` checks one payload's output or a whole report; `conf_bundle.py` assembles the downloadable bundle (below) |
| `negative/` | The image corpus of C-ELF: malformed and accepted images (`<profile>/<name>.aarimg`) and the `MANIFEST` the runner reads, all made by `gen_negative.py` from the builder of `tests/aar_check_test.py`. Copy the directory next to the payload directories (`<DIR>/negative`) |
| `tests/qemu_run.sh` | Runs payloads under qemu user mode with the loader of `libaar/tests/loader.c` |

## What passing means

A platform conforms to a profile at an API major.minor when (spec 11.1):

1. it passes every MUST test of that profile and of every group it declares;
2. it does not declare a group it does not pass;
3. the suite ran with the platform's own `aar.library` and the unmodified
   payloads, and the results are in the standard format (below), with the
   suite version and the platform's `AARInfo` dump;
4. every SKIP has a reason, and a SKIP is only for a test of an undeclared
   group or a SHOULD test, or for a MUST row that the catalogue lets skip for an
   optional route. An example is C-AUDIO.1 to C-AUDIO.9 with the reason
   `no PLATFORM route`, when the platform has no mixer of its own and refuses the
   route PLATFORM with `AAR_E_NOSYS` (spec 8.4).

The host runner (`host-runner/`) passes the payloads that build for it and is the
reference for app semantics. It does not count as a platform. A test that needs a
client or a platform prints `SKIP host: ...` there; such a line is allowed in the
host run and in the qemu run and never in a platform's report.

Tests that need a person (reset the machine, hear a sound, see a picture, use the
keyboard and mouse) are `operator` tests. By default the runner skips them with the
reason `manual`. With `INTERACTIVE` the runner takes the operator through them while
the run goes on:

- A test that shows something prints what to look for or do, and waits for Return,
  so that you are looking when it starts. It runs, and the question (`y/n`) comes
  right after it. Such tests include C-DISPLAY.17 (a picture for 5 seconds), C-DISPLAY.30 (a
  window title and four mouse pointers, 12 seconds), C-DISPLAY.31 (resizing a window by hand
  for 25 seconds),
  C-INPUT.11 (a grey screen for 12 seconds in which to use the keyboard, mouse and joystick),
  C-INPUT.17 (a window for 20 seconds in which to press each mouse button, turn the wheel and
  type) and C-AUDIO.12 (a tone for 2 seconds).
- C-INPUT.11 also prints what the app saw. If no event is a key press, a mouse move, a
  button or a joystick movement, it says so and offers to repeat the 12 seconds once.
  A yes does not count when the app saw no such event: the test fails with "operator
  confirmed but the app saw no key or mouse event". The app's evidence decides.
- C-LIFE.24 is a procedure that you do before the run (start two apps, reset the machine,
  boot, start the suite). The runner prints the text and, at the start of the run, asks
  whether you did it and whether it behaved as described.

`ONLY` with one operator id runs only that test and its question, for example
`ONLY=C-INPUT.11 INTERACTIVE`. Without `INTERACTIVE` it is `SKIP manual`. A SHOULD
operator test that is skipped does not affect the verdict. A MUST operator test
(C-LIFE.24) that is skipped does, so a platform's report must include the operator's
run of it.

The native requester tests also need an operator, but the runner judges their
results from what the app returns. They run only with `INTERACTIVE` and an
exact `ONLY` id. See [C-FILE](TESTS.md#c-file-files-section-71) for the file
and message requester commands.

### CD32 pad

Run the row for the physical port with a CD32 pad connected:

```text
aar-conf ONLY=C-INPUT.20 INTERACTIVE
aar-conf ONLY=C-INPUT.21 INTERACTIVE
```

`C-INPUT.20` selects Amiga port 0 (the usual mouse port); `C-INPUT.21` selects
port 1. Run them separately, moving the pad between ports. Use the same `DIR`,
`PROFILE` and `TO` options as for a full run. Each row requires its exact
`ONLY` id. Full and area runs skip the rows, even with `INTERACTIVE`, so they
do not opt any port into `ReadJoyPort`, which drives the selected port's pins.

Start with all buttons released. When the grey screen opens, press and release
red, blue, green, yellow, play, forward and reverse one at a time within 30
seconds. Check the row's `PASS`, `FAIL` or `SKIP` line, since a failed SHOULD
row does not set a failure return code. `PASS` requires the app to see all
seven separate press and release pairs on the selected port. The report names
any missing presses and any observed presses with no matching release.

### Unimplemented tests

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

## The bundle

Every run of the CI workflow builds the suite for platform developers and uploads
it as the artifact `aar-conformance-bundle` (the `conf-bundle` job). It is the
directory below, ready to copy to the platform, with the m68k runner built from
the pinned cross compiler and the payloads of both profiles, each checked by
`aar-check`:

```
aar-conf
a32/conf_<area>.elf       (17 payloads)
a64/conf_<area>.elf       (17 payloads)
negative/MANIFEST
negative/a32/*.aarimg
negative/a64/*.aarimg
MANIFEST.txt
```

`MANIFEST.txt` holds the suite version, the commit it was built from, the run
commands (the full run, `INTERACTIVE`, `ONLY` for every area, `PROFILE=a64` for an
A64 platform) and the sha256 of every other file. Check them with
`grep -E '^[0-9a-f]{64}  ' MANIFEST.txt | sha256sum -c`.

To make the bundle yourself:

1. Run `make conf-bundle` in the SDK image. It stages the payloads and the corpus
   (set `BUNDLE=<dir>`).
2. Run `AAR_CONF_OUT=<dir>/aar-conf sh client/tests/m68k_check.sh` in the m68k image.
   It adds the runner.
3. Run `python3 conformance/tools/conf_bundle.py manifest <dir> --commit <sha>`. It
   writes the manifest.

`tests/bundle_test.py` tests the assembly. [docs/porting.md](../docs/porting.md) is
the guide for the platform that runs it.

## Running on a platform

Copy `aar-conf` and the payload directories to the Amiga. The payloads of a
profile are in a directory named after it:

```
aar-conf
a32/conf_time.elf
a32/conf_...elf
a64/...
negative/MANIFEST
negative/a32/ok-stack0.aarimg
negative/a32/...
negative/a64/...
```

The CI workflow builds this tree and uploads it as the artifact
`aar-conformance-bundle` (above). Then, from a Shell with at least 32 KiB of
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
AAR-CONF-SUITE 0.2 a32
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
  ends it. The verdict follows the rules under [What passing means](#what-passing-means),
  including catalogue-approved skips for an optional route.
  A filtered run (`ONLY`) says `PARTIAL`.

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
text, and relays every `AAR-CONF` line. It fails every id that the catalogue says
the payload prints and that did not arrive (judged by `payload`, impl `yes`, group
declared, profile matching). A payload that crashed, was refused or hung therefore
cannot pass by silence. A payload must print the end line last; the runner checks
that the counts in it match the lines, and that the exit code read through
`AARStatus` is 0 when nothing failed and 10 otherwise (C-LIFE.3).

A payload asks the runner for help by sending a 32-byte message with an ASCII
keyword. The core handles `CONF-STOP-NOW` (`payloads/conf_proto.h`); areas add
their own keywords and handlers in `payloads/conf_<area>.h` and
`runner/r_<area>.c`. When a test needs the runner and there is none
(`conf:peer=1` is missing in a run by hand, the host runner or the qemu loader), it
prints `SKIP host: <reason>`. See [CONTRIBUTING.md](CONTRIBUTING.md) for the peer
interface.

## Adding tests

[`CONTRIBUTING.md`](CONTRIBUTING.md) is the guide for the author of an area. It
covers which files the area owns, how to mark rows `impl yes`, how to add peer
keywords, payload variants and console modes to the runner, what to do about gaps
in the spec, and how to run the host, qemu and m68k checks. The rows are in
`TESTS.md`. A payload test is printed by `payloads/conf_<area>.c`, a runner test is
listed in `runner/r_<area>.c`, and `python3 conformance/tools/conf_catalogue.py
check` keeps the catalogue and the sources in step.
