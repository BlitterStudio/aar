# AAR templates

Templates are MIT-licensed `aar_main` programs. Each one includes `aar/app.h`
and calls `AAR_NOTE()` from `aar/note.h`, so the image carries the `.note.aar`
note without a separate assembler file. The public API is `include/aar/`; the
transport headers are for platform implementers and apps never include them.
Every template builds for profile A32 and for profile A64. The few that touch
the instruction set (`fault.c`, `fpenv.c`, `membench.c`, `svcprobe.c`) have
both variants behind `__aarch64__`.

Apps link against the PIC newlib from `build_pic_newlib.sh`, except freestanding
apps (see "Freestanding" below). Stock newlib is not PIC and fails
`aar-check`, and neither is the toolchain's `libstdc++`: C++ apps link the PIC
C++ runtime that `build_pic_libstdcxx.sh` installs into the same sysroot. Set
`AAR_SDK` to the root of this repository (`/opt/aar-sdk` in the image) and
`AAR_SYSROOT_A32` and `AAR_SYSROOT_A64` to the sysroots that those scripts built
(the image sets all three).

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
`aar-check` on it. `make clean` removes the build. Optional `CFLAGS`,
`CXXFLAGS` and `LDLIBS` are appended to the SDK flags. A `.cpp`, `.cc` or `.cxx`
file in `SRCS` makes it a C++ app (see "C++" below). This repository builds
every template for every available profile through that include:

```text
make check
```

ELFs land in `build/<profile>/<name>.elf`. `make PROFILES=a32` limits the
profiles. The [root README](../README.md#building-apps) lists what `make check`
runs and the checks that need separate targets. These templates have fixed notes:

| Template | stack | min heap | groups required | groups optional |
| --- | --- | --- | --- | --- |
| `hello`, `freestanding` | 0 | 0 | none | none |
| `pixels` | 0 | 0 | display | input |
| `tone` | 0 | 0 | audio | none |
| `mgl-conformance` | 0 | 0 | display, GL | none |
| `console` | 0 | 0 | none | input |
| `membench` | 0 | 0 | none | display |
| `cxx_hello` | 0 | 0 | none | none |

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
`cmake/example` builds `hello`, `freestanding` and `cxx_hello`. A C++ target
needs `CXX` in `project()` (the example has `project(aar_hello C CXX ASM)`), and
`-DAAR_CXX_EXCEPTIONS=OFF` compiles C++ without exceptions.

## Docker

`docker/Dockerfile` is Debian slim plus Arm GNU Toolchain 15.3.rel2 for both
profiles (pinned URL and sha256), a PIC newlib and a PIC C++ runtime for each
(the C++ runtime is built from the toolchain release's source snapshot, pinned
by sha256), Python, make and CMake. The image sets `AAR_SDK`, `AAR_SYSROOT_A32`,
`AAR_SYSROOT_A64` and `PATH`.

```text
docker build -f docker/Dockerfile -t aar-sdk .
docker run --rm -v "$PWD:/src:ro" aar-sdk sh -c \
  'cp -r /src /w && cd /w && make AAR_SDK=/w check'
```

`ghcr.io/blitterstudio/aar-sdk` is pushed only on tag pushes and manual workflow
dispatch.

Without Docker, install Arm GNU Toolchain 15.3.rel2, run `build_pic_newlib.sh`
and `build_pic_libstdcxx.sh` once per profile, and export the three variables
above. `build_pic_libstdcxx.sh` needs a host C++ compiler, GMP, MPFR and MPC
development packages, flex, and about 4 GiB of disk. The Make and CMake entry
points are the same.

## Check

`tools/aar-check` checks build output against
[spec sections 2.2 and 2.3](../spec/aar.md), including the ELF profile,
segments, relocations and note. It also checks section headers; a runtime
loader may accept an image without them.

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
`-lc`) that must be rejected. The flavour has no C++ runtime: a C++ source in
a freestanding build is refused with an error.

## C++

`cxx_hello.cpp` is the C++ template. `make check` builds it for both profiles,
with exceptions and (as `cxx_hello_noexc`) with `AAR_CXX_EXCEPTIONS=0`, runs
`aar-check` on both, and `make check-runtime` runs both under qemu. C++ builds
use `g++` with the profile's flags, `-std=c++17` and exceptions on, link
`libstdc++` and `libgcc_eh` from the sysroot, and use `libaar/<profile>/aar-cxx.ld`
(which keeps the unwind tables) in place of `aar.ld`. The
[developer guide](../docs/aar-developer-guide.md#c-apps) lists what works and
what does not: exceptions and RTTI work, threads and the `<chrono>` clocks do
not.

## Host runner

The [host-runner manual](../host-runner/README.md) lists its native builds and
differences from an ARM target. Instruction-level templates need the target
instruction set.

## Templates

- `hello.c`: writes one diagnostic line through `aar_log`.
- `pixels.c`: opens a display surface and handles input. It ends on stop or a
  key; the key needs the input group, which the note lists as optional.
- `fault.c`: deliberately faults for lifecycle testing (UDF on both profiles).
- `isolate.c`: faults on forbidden stores, execution, or ungranted shared
  memory; its `svc` mode checks that a text pointer returns `AAR_E_FAULT`.
  See the ZZ9000 notes below before using its address probes elsewhere.
- `spin.c`: CPU-bound scheduler fairness probe; run it as `spin [name]`.
- `memhog.c`: allocates and touches 1 MiB blocks until `malloc` fails, reports
  each 16 MiB and low-memory events, holds the memory until Ctrl-C, then frees
  every block and exits normally.
- `svcbench.c`: times `should_stop`, `time_us` and `aar_get_info` in
  `aar_time_us` batches. Its cycle estimates assume the ZZ9000 clock; see below.
- `svcprobe.c`: times three table calls with the cycle counter on both
  profiles. User-mode counter access must be enabled by the platform.
  Its optional A32 SVC boundary probe is ZZ9000-specific; see below.
- `membench.c`: memory bandwidth benchmark. It times sequential and
  pointer-chase traffic with `aar_time_us`, without PMU or device-register
  access. The display region is optional.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  client's file calls (`aar_map_file` is optional, and the template fails
  with 10 where it is absent), computes SHA-256 with the services group, and
  writes the 32-byte digest to its output path. Invoke it as `filter input
  output`.
- `tone.c`: opens the normal stereo S16LE `aar_audio` ring and produces a
  440 Hz square wave. It is route-agnostic: it asks for 22050 Hz and derives
  the period from the rate it was granted.
- `echo.c`: echoes each fixed 32-byte client message. It parks in
  `aar_msg_wait` between messages and replies with `aar_msg_send`; use a client
  that drains messages (see [aar_serve](../docs/aar-serve-api.md)).
- `console.c`: prints its arguments to stdout and a line to stderr, echoes
  stdin lines in upper case, reports Ctrl-D/E/F break events, and exits at EOF.
  See the [aar-run console notes](../client/run/README.md#console-and-audio-routes)
  for redirecting input and stopping a keyboard read.
- `freestanding.c`: a program without newlib. It has a bump allocator over
  `aar_heap_range` and `aar_heap_grow`, a hand-written hash, and its own number
  formatting. Build it with `AAR_FREESTANDING=1`. The host runner builds it too and
  a test compares its hash with an independent implementation.
- `fpenv.c`: prints the floating-point control register as the app sees it on
  entry and checks the denormal behaviour of the developer guide's
  bit-identical maths section. A32: FPSCR is 0, a VFP add keeps a denormal
  result and a NEON add flushes it. A64: FPCR is 0 and a scalar and an ASIMD
  add both keep it. Exit status 0 means every check held; it is the card test
  for that section.
- `ringflood.c`: message queue conformance load. It starts quiet, parked in
  `aar_msg_wait`, and answers PING messages. On CTRL FLOOD it sends
  sequence-numbered, pattern-checked 32-byte messages as fast as the
  app-to-host queue takes them and bumps a counter word, big-endian, in a
  shared buffer on every loop. `aar_publish` makes each store visible before
  the message that carries the same value is sent. The INFO message carries the
  buffer's token (`aar_shared.haddr`). CTRL QUIET stops it. Its message layout
  is in the file header. `ringflood N` on the host runner leaves the flood after
  N data messages, and the host tests check its output against an independent
  implementation of the pattern.
- `mgl_conformance.c`: MiniGL texture-environment conformance probe through
  `aar_gl_bind`; see `mgl-conformance-README.md`.
- `sbrk_smoke.c`: a link fixture, not a template. It pulls newlib `malloc` so
  the relocation check sees more than the `hello` pointer.
- `cxx_hello.cpp`: the C++ template. A global object with a constructor and a
  destructor, a priority-101 constructor that throws and catches, `std::vector`
  and `std::string`, a function-level static, `new` and `delete`, a virtual
  call, RTTI, and three throw and catch cases. It writes
  each step with `aar_log`, logs `cxx_hello PASS` and exits 0 when every check
  held, and logs the exception mode it was built with.

## ZZ9000 notes

`isolate.c` hard-codes firmware and ungranted host-heap addresses from the
ZZ9000. Change those addresses for another isolated platform; they are not
part of the AAR contract.

`svcbench.c` uses a 667 MHz clock assumption for its timer-derived cycle
estimates. Use its time measurements on other platforms.

The A32 SVC boundary probes in `svcprobe.c` require the ZZ9000 firmware's
probe build option; they have no A64 form. The
[firmware SDK smoke guide](https://github.com/BlitterStudio/zz9000-firmware/blob/master/sdk/docs/zz9k-release-smoke.md)
describes the platform's message-ring test tools.
