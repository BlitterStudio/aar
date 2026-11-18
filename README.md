# AAR (Amiga ARM Runtime)

AAR is a standard way to run native ARM apps from AmigaOS. An app is a
position-independent ARM ELF that talks to its host through one function
table (`zzapp.h`). The host loads the ELF, checks it, and gives it a display,
input, audio, files, a message ring, and a heap. Libraries are meant to use
the same contract later.

The ZZ9000 is the reference implementation: its firmware runs the apps on the
card's ARM cores. The aim is to run the same app binaries on other Amiga
platforms, such as PiStorm with Emu68 or a CM5 card. This repository holds
the app-side SDK that does not depend on any one of them: the header, the
runtime library, the linker script, the build helpers, the image checker, the
docs, and a host runner that runs an app natively on a PC.

## Status

Version 0.x. The app API is 0.4 and may still change. The names are still the
ones from the ZZ9000 SDK: `zzapp.h`, `ZZAPP_*`, `zz_*`, `libzzapp`, and the
`zz9000-app-sdk` image. They will be renamed to `aar_` names in a later step.

The firmware in [BlitterStudio/zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware)
pins the commit of this repository it builds against in `sdk/AAR_REF`.

## Layout

| Path | Content |
| --- | --- |
| `include/` | `zzapp.h` (the API) and `zzapp_note.h` (`ZZAPP_NOTE()`, the `.zzapp` ELF note) |
| `src/` | `libzzapp` sources: the `zz_*` wrappers, newlib syscalls, GL wrappers, and the freestanding `mem*` routines |
| `crt0.S`, `zzapp.ld` | Start-up code and the linker script |
| `mk/zzapp.mk` | Makefile include for building one app |
| `cmake/` | `zzapp-toolchain.cmake`, `zzapp.cmake` (`zzapp_add_executable`), and an example project |
| `build_pic_newlib.sh` | Builds the position-independent newlib that apps link against |
| `docker/Dockerfile` | SDK image: Arm GNU Toolchain 15.3.rel2, PIC newlib, make, CMake, Python |
| `tools/zzapp-check` | Checks a finished ELF against the loader contract |
| `templates/` | Example apps; `templates/README.md` describes each one |
| `host/` | `libzzapp-host`, an SDL2 runner for apps on a PC; see `host/README.md` |
| `tests/` | Tests for the checker, the link map check, FP code generation, and mode switching |
| `docs/` | The API reference, the developer guide, and the script that checks the reference covers every public symbol |
| `Makefile` | Builds every template and runs the checks (`make check`) |

## Building apps

The Docker image has everything the ARM build needs:

```sh
docker build -f docker/Dockerfile -t zz9000-app-sdk .
docker run --rm -v "$PWD":/src -w /src zz9000-app-sdk make check
```

`make check` builds every template, runs `tools/zzapp-check` on each ELF, and
runs the tests in `tests/`. ELFs land in `build/<name>.elf`. The image
sets `ZZAPP_SDK=/opt/zzapp-sdk` and `ZZAPP_PIC_SYSROOT=/opt/zzapp-pic-newlib`.

An app Makefile includes `mk/zzapp.mk`:

```make
ZZAPP_SDK ?= /opt/zzapp-sdk
APP := hello
SRCS := hello.c
include $(ZZAPP_SDK)/mk/zzapp.mk
```

With CMake, pass the toolchain file and call `zzapp_add_executable` from
`cmake/zzapp.cmake`; `cmake/example` shows it:

```sh
cmake -S cmake/example -B build/hello \
  -DCMAKE_TOOLCHAIN_FILE=cmake/zzapp-toolchain.cmake
cmake --build build/hello
```

Without Docker, install Arm GNU Toolchain 15.3.rel2, run
`build_pic_newlib.sh`, and set `ZZAPP_SDK` and `ZZAPP_PIC_SYSROOT`. Freestanding
apps (`ZZAPP_FREESTANDING=1`) link without newlib and need no sysroot.
`templates/README.md` and `docs/zzapp-developer-guide.md` have the details.

## Host runner

`host/` builds `libzzapp-host`, which runs an app as a native process with
SDL2 standing in for the card:

```sh
cmake -S host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

`host/README.md` lists the options and where the host differs from the card.

## Documentation

- [docs/zzapp-api.md](docs/zzapp-api.md): the API reference.
- [docs/zzapp-developer-guide.md](docs/zzapp-developer-guide.md): how an app
  is built, loaded, and run.

## License

MIT. See [LICENSE](LICENSE).
