# AAR (Amiga ARM Runtime)

AAR is a standard way to run native ARM apps from AmigaOS. An app is a
position-independent ARM ELF that talks to its runtime through one function
table (`aar/app.h`). The runtime loads the ELF, checks it, and gives it a
display, input, audio, files, a console, a message queue, a heap and shared
buffers. An AmigaOS client library starts apps and watches them.

There are two profiles: **A32** (ARMv7-A, hard-float, VFPv3 and NEON, ILP32,
the ZZ9000's Cortex-A9) and **A64** (ARMv8-A AArch64, LP64, for Pi 4 and Pi 5
class cores). An image belongs to exactly one profile.

The ZZ9000 is the reference implementation: its firmware runs apps on the
card's ARM cores. The aim is to run the same app binaries on other Amiga
platforms, such as PiStorm with Emu68 or a CM5 card. This repository holds the
part that does not depend on any one of them: the headers, the runtime library,
the linker scripts, the build helpers, the image checker, the docs, the
transport contract, and a host runner that runs an app natively on a PC.

## Status

Version 0.x. The app API is **0.5**, the first AAR version, and may still
change before 1.0. It replaces the `zzapp` 0.4 SDK that this repository
started from: nothing of the old API shipped, so there is no compatibility
layer. [docs/zzapp-to-aar.md](docs/zzapp-to-aar.md) maps every old name.

The firmware in [BlitterStudio/zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware)
pins the commit of this repository it builds against in `sdk/AAR_REF`.

## Layout

| Path | Content |
| --- | --- |
| `include/aar/` | `app.h` (the table, types and wrappers), `note.h` (`AAR_NOTE()`), `gl.h`, `transport.h`, `ext_zz9000.h` |
| `client/` | `aar.library`, the Amiga client library: `include/` (the public headers and the generated call headers), `fd/aar_lib.fd`, `src/` (the platform interface and the image probe), `tools/fdgen.py`, `tests/` |
| `libaar/` | `src/` (the `aar_*` and `gl*` wrappers, the libc glue, the freestanding `mem*` routines), and per profile `a32/` and `a64/` with `crt0.S` and `aar.ld` |
| `mk/aar.mk` | Makefile include for building one app |
| `cmake/` | `aar.cmake` (`aar_add_executable`), `aar-toolchain-a32.cmake`, `aar-toolchain-a64.cmake`, and an example project |
| `build_pic_newlib.sh` | Builds the position-independent newlib that apps link against (`AAR_PROFILE=a32` or `a64`) |
| `docker/Dockerfile` | SDK image: Arm GNU Toolchain 15.3.rel2 for both profiles, PIC newlib for both, make, CMake, Python |
| `tools/aar-check` | Checks a finished ELF against the load contract |
| `templates/` | Example apps; `templates/README.md` describes each one |
| `host-runner/` | An SDL2 runner for apps on a PC; see `host-runner/README.md` |
| `conformance/` | The conformance suite: the test catalogue (`TESTS.md`), the payload apps, the `aar-conf` runner for the Amiga, and the scripts that check them; see `conformance/README.md` |
| `tests/` | Tests for the checker, the headers and the transport model, the link map check, FP code generation and mode switching. The client library's tests are in `client/tests/` |
| `docs/` | The API reference, the developer guide, the transport contract, the zzapp mapping, and the script that checks the reference covers every public symbol |
| `Makefile` | Builds every template and runs the checks (`make check`) |

## Building apps

The Docker image `ghcr.io/blitterstudio/aar-sdk` has everything the ARM build
needs. It installs this repository at `/opt/aar-sdk` and sets `AAR_SDK`,
`AAR_SYSROOT_A32` and `AAR_SYSROOT_A64`:

```sh
docker build -f docker/Dockerfile -t aar-sdk .
docker run --rm -v "$PWD":/src -w /src aar-sdk make check
```

`make check` builds every template, runs `tools/aar-check` on each ELF, and
runs the tests in `tests/`. ELFs land in `build/<profile>/<name>.elf` with a
`.map` beside each.

An app Makefile includes `mk/aar.mk`. `AAR_PROFILE` is `a32` (the default) or
`a64`:

```make
AAR_SDK ?= /opt/aar-sdk
APP := hello
SRCS := hello.c
include $(AAR_SDK)/mk/aar.mk
```

With CMake, pass a toolchain file and call `aar_add_executable` from
`cmake/aar.cmake`; `cmake/example` shows it:

```sh
cmake -S cmake/example -B build/hello \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/aar-toolchain-a32.cmake"
cmake --build build/hello
```

Without Docker, install Arm GNU Toolchain 15.3.rel2, run `build_pic_newlib.sh`
once per profile (`AAR_PROFILE=a32` and `AAR_PROFILE=a64`), and set `AAR_SDK`,
`AAR_SYSROOT_A32` and `AAR_SYSROOT_A64`. Freestanding apps
(`AAR_FREESTANDING=1`) link without newlib and need no sysroot.
`templates/README.md` and [docs/aar-developer-guide.md](docs/aar-developer-guide.md)
have the details.

## Host runner

`host-runner/` runs an app as a native process with SDL2 standing in for the
card:

```sh
cmake -S host-runner -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

`host-runner/README.md` lists the options and where the host differs from the
card.

## Documentation

- [docs/aar-api.md](docs/aar-api.md): the API reference, every call, type and
  constant.
- [docs/aar-client-api.md](docs/aar-client-api.md): the Amiga client library,
  `aar.library`: every call, structure, tag and error, for launchers and tools.
- [docs/aar-developer-guide.md](docs/aar-developer-guide.md): how an app is
  built, loaded and run, with the rules for shared buffers, messages and events.
- [docs/transport.md](docs/transport.md): the transport contract for platform
  implementers.
- [docs/zzapp-to-aar.md](docs/zzapp-to-aar.md): the old `zzapp` names and what
  replaced them.

## License

MIT. See [LICENSE](LICENSE).
