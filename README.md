# AAR (Amiga ARM Runtime)

AAR is a standard way to run native ARM apps from AmigaOS. An app is a
position-independent ARM ELF that talks to its runtime through one function
table (`aar/app.h`). The runtime loads the ELF, checks it, and gives it a
display, input, audio, files, a console, a message queue, a heap and shared
buffers. An AmigaOS client library starts apps and watches them.

There are two profiles: **A32** (ARMv7-A, hard-float, VFPv3 and NEON, ILP32)
and **A64** (ARMv8-A AArch64, LP64). An image belongs to exactly one profile.

AAR runs on a platform that implements the runtime, transport and Amiga client
library described in [spec/aar.md](spec/aar.md). This repository holds the
headers, runtime library, linker scripts, build helpers, image checker, reference
loader, docs, transport contract and a host runner for native PC builds.

The ZZ9000 is the reference platform (A32, Cortex-A9); see the
[porting guide](docs/porting.md) for the parts a platform supplies.

## Status

Version 0.x. The app API is **0.5**, the first AAR version, and may still
change before 1.0. The specification is [spec/aar.md](spec/aar.md) (review
draft 1). It is the normative text and states what the standard requires of an
app, a runtime, the Amiga library and a platform. Comments and questions are
welcome as issues in this repository.

The firmware in [BlitterStudio/zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware)
pins the commit of this repository it builds against in `sdk/AAR_REF`.

## Layout

| Path | Content |
| --- | --- |
| `include/aar/` | `app.h` (the table, types and wrappers), `note.h` (`AAR_NOTE()`), `gl.h`, `transport.h`, `ext_zz9000.h` |
| `client/` | `aar.library`, the Amiga client library: `include/` (the public headers and the generated call headers), `fd/aar_lib.fd`, `src/` (the platform interface and the image probe), `tools/fdgen.py`, `tests/` |
| `libaar/` | `src/` (the `aar_*` and `gl*` wrappers, the libc glue, the freestanding `mem*` routines, the C++ start-up support), and per profile `a32/` and `a64/` with `crt0.S`, `aar.ld` and `aar-cxx.ld` (the script for an app that has C++) |
| `loader/` | Portable reference ELF validator and loader; [`aar_load.h`](loader/aar_load.h) is its interface |
| `mk/aar.mk` | Makefile include for building one app |
| `cmake/` | `aar.cmake` (`aar_add_executable`), `aar-toolchain-a32.cmake`, `aar-toolchain-a64.cmake`, and an example project |
| `build_pic_newlib.sh` | Builds the position-independent newlib that apps link against (`AAR_PROFILE=a32` or `a64`) |
| `build_pic_libstdcxx.sh` | Builds the position-independent C++ runtime (`libstdc++`, `libsupc++`, `libgcc_eh`) into the same sysroot, from the GCC source of the toolchain release |
| `docker/Dockerfile` | SDK image: Arm GNU Toolchain 15.3.rel2 for both profiles, PIC newlib and PIC C++ runtime for both, make, CMake, Python |
| `tools/aar-check` | Checks a finished ELF against the load contract |
| `tools/aar-symbolize`, `tools/aar-profile` | Name the code in an `aar-run` crash report, and print a flat profile from its sample file; see the developer guide, Debugging |
| `templates/` | Example apps; `templates/README.md` describes each one |
| `host-runner/` | An SDL2 runner for apps on a PC; see `host-runner/README.md` |
| `spec/` | `aar.md`, the specification of the standard |
| `conformance/` | The conformance suite: the test catalogue (`TESTS.md`), the payload apps, the `aar-conf` runner for the Amiga, and the scripts that check them; see `conformance/README.md` |
| `tests/` | Tests for the checker, the debug tools, the headers and the transport model, the link map check, FP code generation, mode switching, the pair of linker scripts, and the C++ template under qemu. The client library's tests are in `client/tests/` |
| `docs/` | The API reference, the developer guide, the porting guide, the transport contract, and the script that checks the reference covers every public symbol |
| `Makefile` | Builds every template and runs the checks (`make check`) |

## Building apps

The Docker image `ghcr.io/blitterstudio/aar-sdk` has everything the ARM build
needs. It installs this repository at `/opt/aar-sdk` and sets `AAR_SDK`,
`AAR_SYSROOT_A32` and `AAR_SYSROOT_A64`:

```sh
docker build -f docker/Dockerfile -t aar-sdk .
docker run --rm -v "$PWD:/src:ro" aar-sdk sh -c \
  'cp -r /src /w && cd /w && make AAR_SDK=/w check'
```

`make check` runs the following for A32 and for A64 when its compiler is
available (`PROFILES=a32` limits the run):

- the image checker's synthetic-image tests (`check-tool`);
- the profile header layouts and note bytes (`check-abi`);
- floating-point code generation (`check-fp-codegen`);
- builds and `aar-check` for every template (the C++ template `cxx_hello` with
  and without exceptions), `sbrk_smoke` and every conformance payload;
- fixed template notes (`check-expect-note`), libc/freestanding mode switching
  (`check-mode-switch`), rejection of a freestanding libc link
  (`check-freestanding-guard`) and of freestanding C++ (`check-cxx-guard`), and
  that `aar-cxx.ld` matches `aar.ld` apart from the unwind tables (`check-cxx-ld`);
- C++ build behaviour (`check-cxx-build`): switching `AAR_CXX_EXCEPTIONS` relinks,
  `main.c` and `main.cpp` in one app are both linked, and a CMake reconfigure
  applies `AAR_CXX_EXCEPTIONS`;
- the conformance catalogue, tools and bundle tests, and generated negative-image
  corpus (`check-conf-catalogue`).

ELFs land in `build/<profile>/<name>.elf` with a `.map` beside each.
Other root Makefile targets are separate from `check`:

| Target | What it runs or builds | Extra requirements |
|---|---|---|
| `all` (default) | Templates and conformance payloads | Profile toolchains and PIC sysroots |
| `check-runtime` | libaar tests, the C++ template, marked conformance payloads and the negative-image corpus under qemu | qemu-user and Linux cross GCC per profile; missing tools skip that profile |
| `check-abi-host` | Host header layouts and transport model, including 32-bit and C++ | GCC, G++, multilib |
| `check-loader-host` | Reference loader placement, corpora and mutation tests, including 32-bit and sanitizers | Host GCC and Python 3; optional compiler features are probed |
| `check-conf-payloads` | Build and check only the conformance payloads | Profile toolchains and PIC sysroots |
| `conf-bundle` | Check and stage payloads and negative images | See [bundle assembly](conformance/README.md#the-bundle) for the runner and manifest |
| `clean` | Remove `build/` | None |

The named `check-*` prerequisites above can also run on their own. Client library
and serving-loop tests use `sh client/tests/run_host.sh`; m68k compile/link checks
use `sh client/tests/m68k_check.sh`. Host-runner checks are below.

An app Makefile includes `mk/aar.mk`. `AAR_PROFILE` is `a32` (the default) or
`a64`:

```make
AAR_SDK ?= /opt/aar-sdk
APP := hello
SRCS := hello.c
include $(AAR_SDK)/mk/aar.mk
```

A C++ app lists `.cpp`, `.cc` or `.cxx` files in `SRCS` the same way. Exceptions
and RTTI work; `AAR_CXX_EXCEPTIONS=0` compiles without exceptions. See
[C++ apps](docs/aar-developer-guide.md#c-apps).

With CMake, pass a toolchain file and call `aar_add_executable` from
`cmake/aar.cmake`; `cmake/example` shows it, with a C++ target:

```sh
cmake -S cmake/example -B build/hello \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/aar-toolchain-a32.cmake"
cmake --build build/hello
```

Without Docker, install Arm GNU Toolchain 15.3.rel2, run `build_pic_newlib.sh`
and then `build_pic_libstdcxx.sh` once per profile (`AAR_PROFILE=a32` and
`AAR_PROFILE=a64`), and set `AAR_SDK`, `AAR_SYSROOT_A32` and
`AAR_SYSROOT_A64`. The C++ runtime needs the GMP, MPFR and MPC development
packages and flex on the build machine. Freestanding apps
(`AAR_FREESTANDING=1`) link without newlib and need no sysroot.
`templates/README.md` and [docs/aar-developer-guide.md](docs/aar-developer-guide.md)
have the details.

## Host runner

`host-runner/` runs an app as a native process with SDL2 standing in for the
platform:

```sh
cmake -S host-runner -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

`host-runner/README.md` lists the options and where the host differs from the
target.

## Documentation

- [spec/aar.md](spec/aar.md): the specification, the normative text. The
  documents below are reference and guides.
- [docs/aar-api.md](docs/aar-api.md): the API reference, every call, type and
  constant.
- [docs/aar-client-api.md](docs/aar-client-api.md): the Amiga client library,
  `aar.library`. It lists every call, structure, tag and error, for launchers and tools.
- [docs/aar-developer-guide.md](docs/aar-developer-guide.md): how an app is
  built, loaded and run, with the rules for shared buffers, messages and events.
- [docs/transport.md](docs/transport.md): the transport contract for platform
  implementers.
- [docs/porting.md](docs/porting.md): the guide for a platform implementer. It
  covers what a platform provides, the image rules and the loader, the entry
  contract, and how to run the conformance suite (the CI bundle) and report.
- [docs/aar-serve-api.md](docs/aar-serve-api.md): `aar_serve` and
  `libaarserve.a`, for a client that serves app requests.
- [client/run/README.md](client/run/README.md): the aar-run manual, including
  Shell arguments, Workbench ToolTypes, exit codes and platform requirements.
- [templates/README.md](templates/README.md): example apps and how to build them.
- [host-runner/README.md](host-runner/README.md): native PC builds, options and
  differences from a target.
- [loader/aar_load.h](loader/aar_load.h) and [loader/aar_image.h](loader/aar_image.h):
  the reference loader and shared image-validation interfaces.
- [conformance/README.md](conformance/README.md): running and reporting the suite.
- [conformance/TESTS.md](conformance/TESTS.md): the test catalogue and spec questions.
- [conformance/CONTRIBUTING.md](conformance/CONTRIBUTING.md): writing conformance tests.

## License

MIT. See [LICENSE](LICENSE).
