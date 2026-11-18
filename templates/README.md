# AAR templates

Templates are MIT-licensed `aar_main` programs. Each one includes `aar/app.h`
and calls `AAR_NOTE()` from `aar/note.h`, so the image carries the `.note.aar`
note without a separate assembler file. The public API is `include/aar/`; the
transport headers are for platform implementers and apps never include them.
Every template builds for profile A32 and for profile A64. The few that touch
the instruction set (`fault.c`, `fpenv.c`, `membench.c`, `svcprobe.c`) carry
both variants behind `__aarch64__`.

Apps link against the PIC newlib from `build_pic_newlib.sh`, except freestanding
apps (see "Freestanding" below). Stock newlib is not PIC and fails
`aar-check`. Set `AAR_SDK` to the root of this repository (`/opt/aar-sdk` in the
image) and `AAR_SYSROOT_A32` and `AAR_SYSROOT_A64` to the sysroots that
`build_pic_newlib.sh` built (the image sets all three).

## Make

A third-party Makefile is:

```make
AAR_SDK ?= /opt/aar-sdk
AAR_PROFILE ?= a32
APP := hello
SRCS := hello.c
include $(AAR_SDK)/mk/aar.mk
```

`make` writes `build/<profile>/hello.elf` and its `.map`. `make check` runs
`aar-check` on it. `make clean` removes the build. Optional `CFLAGS` and
`LDLIBS` are appended to the SDK flags. This repository builds every template
for every available profile through that include:

```text
make check
```

ELFs land in `build/<profile>/<name>.elf`. `make PROFILES=a32` limits the
profiles. `make check` also runs the checker's own tests, the header layout
and `AAR_NOTE` checks of both compilers, the floating-point code generation
test, the mode switch test, the freestanding guard, and `aar-check
--expect-note` on the templates whose note is fixed:

| Template | stack | min heap | groups required | groups optional |
| --- | --- | --- | --- | --- |
| `hello`, `freestanding` | 0 | 0 | none | none |
| `pixels` | 0 | 0 | display | input |
| `tone` | 0 | 0 | audio | none |
| `mgl-conformance` | 0 | 0 | display, GL | none |
| `console` | 0 | 0 | none | input |
| `membench` | 0 | 0 | none | display |

`AAR_FREESTANDING=1` links without newlib; see "Freestanding" below and
[the developer guide](../docs/aar-developer-guide.md#freestanding-apps).

## CMake

```text
cmake -S cmake/example -B build/cmake-a32 \
  -DCMAKE_TOOLCHAIN_FILE=$AAR_SDK/cmake/aar-toolchain-a32.cmake
cmake --build build/cmake-a32
```

`aar_add_executable(name [FREESTANDING] SOURCES ...)` in `cmake/aar.cmake`
links the same way and runs `aar-check` after the link. One build directory is
one profile: use `aar-toolchain-a64.cmake` in a second directory for A64.
`cmake/example` builds `hello` and `freestanding`.

## Docker

`docker/Dockerfile` is Debian slim plus Arm GNU Toolchain 15.3.rel2 for both
profiles (pinned URL and sha256), a PIC newlib for each, Python, make and
CMake. The image sets `AAR_SDK`, `AAR_SYSROOT_A32`, `AAR_SYSROOT_A64` and
`PATH`.

```text
docker build -f docker/Dockerfile -t aar-sdk .
docker run --rm -v "$PWD":/src -w /src aar-sdk make check
```

`ghcr.io/blitterstudio/aar-sdk` is pushed only on tag pushes and manual workflow
dispatch.

Without Docker, install Arm GNU Toolchain 15.3.rel2, run `build_pic_newlib.sh`
once per profile, and export the three variables above. The Make and CMake
entry points are the same.

## Check

`tools/aar-check` rejects an image the loader would reject: not a little-endian
ARM PIE of its profile, a relocation the profile does not allow, `PT_INTERP`,
`DT_NEEDED`, a writable and executable segment, a soft-float A32 image, or an
AAR note that is missing, doubled, has the wrong magic, an api major other than
0, an unknown group bit, or a stack other than 0 or 64 KiB to 1 MiB aligned to
16.

`aar-check --no-libc app.map` checks the link map of a freestanding build and
fails if any archive other than `libaar.a` and `libgcc.a` was linked.

## Freestanding

`AAR_FREESTANDING=1` (make), `FREESTANDING` on `aar_add_executable`, or
`-DAAR_FREESTANDING=ON` at CMake configure time links `crt0`, the app,
`libaar` and `libgcc` without newlib and without the PIC sysroot. `crt0` calls
`aar_exit` directly, `aar_alloc` and `aar_free` are not declared, and `libaar`
supplies weak `memcpy`, `memmove`, `memset` and `memcmp`. The developer guide
has the full list of differences. `make check` builds `freestanding.c` this
way, checks its link map, and links a fixture (`tests/freestanding_leak.c` with
`-lc`) that must be rejected.

## Host runner

`host-runner/` builds `hello`, `console`, `filter`, `freestanding`,
`ringflood`, `echo`, `pixels`, `tone` and `memhog` as native programs against
`libaar-host`, with SDL2 standing in for the platform, and tests most of them.
See `host-runner/README.md`. The runner has no GL group, no compression and no
decoders, so `mgl-conformance` only links there, and the instruction-level
templates are Arm only.

## Templates

- `hello.c`: writes one diagnostic line through `aar_log`.
- `pixels.c`: opens a display surface and handles input. It ends on stop or a
  key; the key needs the input group, which the note lists as optional.
- `fault.c`: deliberately faults for lifecycle testing (UDF on both profiles).
- `isolate.c`: faults on forbidden stores, execution, or host heap access; its
  `svc` mode verifies that a text pointer is rejected by a pointer-taking call
  with `AAR_E_FAULT`. The addresses are the ZZ9000's.
- `spin.c`: CPU-bound scheduler fairness probe; run it as `spin [name]`.
- `memhog.c`: allocates and touches 1 MiB blocks until `malloc` fails, reports
  each 16 MiB and low-memory events, holds the memory until Ctrl-C, then frees
  every block and exits normally.
- `svcbench.c`: times table calls in `aar_time_us` batches and reports 667 MHz
  timer-derived cycle estimates because PMU access is unavailable in user
  mode. It times `should_stop`, `time_us` and `aar_get_info`.
- `svcprobe.c`: on A32 with firmware built using `ZZAPP_SVC_PROBES=1`, times
  individual SVC path boundaries with PMCCNTR; then, on both profiles, times
  three table calls with the cycle counter. The probes are a ZZ9000 firmware
  option and have no A64 form.
- `membench.c`: memory bandwidth benchmark. It times sequential and
  pointer-chase traffic with `aar_time_us`. PMU, SCTLR/ACTLR, and PL310/DDRC
  registers are not available to apps. The display region is optional.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  client's file calls (`aar_map_file`, an optional call: the template fails
  with 10 where it is absent), computes SHA-256 with the services group, and
  writes the 32-byte digest to its output path. Invoke it as `filter input
  output`.
- `tone.c`: opens the normal stereo S16LE `aar_audio` ring and produces a
  440 Hz square wave. It is route-agnostic: it asks for 22050 Hz and derives
  the period from the rate it was granted.
- `echo.c`: echoes each fixed 32-byte client message. It parks in
  `aar_msg_wait` between messages and replies with `aar_msg_send`; use it with
  the `zz9k.library` app-client smoke tool.
- `console.c`: Shell console example. Prints its arguments to stdout and a
  line to stderr, echoes each stdin line in upper case, reports Ctrl-D/E/F
  break events, and exits at EOF (Ctrl-\ in a Shell window).
- `freestanding.c`: no newlib. A bump allocator over `aar_heap_range` and
  `aar_heap_grow`, a hand-written hash, and its own number formatting; build
  it with `AAR_FREESTANDING=1`. The host runner builds it too and a test
  compares its hash with an independent implementation.
- `fpenv.c`: prints the floating-point control register as the app sees it on
  entry and checks the denormal behaviour of the developer guide's
  bit-identical maths section. A32: FPSCR is 0, a VFP add keeps a denormal
  result and a NEON add flushes it. A64: FPCR is 0 and a scalar and an ASIMD
  add both keep it. Exit status 0 means every check held; it is the card test
  for that section.
- `ringflood.c`: message queue conformance load. Starts quiet, parked in
  `aar_msg_wait`, and answers PING messages. On CTRL FLOOD it sends
  sequence-numbered, pattern-checked 32-byte messages as fast as the
  app-to-host queue takes them and bumps a counter word, big-endian, in a
  shared buffer on every loop; `aar_publish` makes each store visible before
  the message that carries the same value is sent. The INFO message carries the
  buffer's token (`aar_shared.haddr`). CTRL QUIET stops it. Its message layout
  is in the file header. Use it with the `zz9k-app-ringtest` tool; `ringflood
  N` on the host runner leaves the flood after N data messages, and the host
  tests check its output against an independent implementation of the
  pattern.
- `mgl_conformance.c`: MiniGL texture-environment conformance probe through
  `aar_gl_bind`; see `mgl-conformance-README.md`.
- `sbrk_smoke.c`: a link fixture, not a template. It pulls newlib `malloc` so
  the relocation check sees more than the `hello` pointer.
