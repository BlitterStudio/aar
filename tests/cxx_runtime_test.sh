#!/bin/sh
# Runs the C++ template on a real instruction stream: builds
# templates/cxx_hello.cpp for a profile with exceptions (the default) and with
# AAR_CXX_EXCEPTIONS=0, builds the test loader of libaar for Linux user mode and
# runs both images under qemu. The template checks its own results (global
# constructor, containers, new and delete, virtual calls, RTTI, and throw and
# catch when the build has exceptions) and logs "cxx_hello PASS"; its global
# destructor runs from exit() and logs after that. libaar/tests/cxx_env_test.cpp
# then reads the host environment with std::getenv and _getenv_r from a C++
# image, where libstdc++ also references getenv: it fails if the link took
# newlib's getenv instead of libaar's.
#
#   tests/cxx_runtime_test.sh <SDK directory> <a32|a64>
#
# Needs the profile's bare-metal toolchain and sysroot with the PIC C++ runtime
# (the SDK image has both), plus qemu-arm or qemu-aarch64 and
# arm-linux-gnueabihf-gcc or aarch64-linux-gnu-gcc with their libc. The SDK
# image has neither, so the script prints "skipped" and exits 0 when they are
# missing (make check-runtime runs it for every profile).
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: cxx_runtime_test.sh <SDK directory> <a32|a64>}" && pwd)
PROFILE=${2:?usage: cxx_runtime_test.sh <SDK directory> <a32|a64>}
case "$PROFILE" in
  a32) CROSS=${CROSS:-arm-none-eabi-}; HOSTCC=${LOADER_CC:-arm-linux-gnueabihf-gcc}; QEMU=${QEMU:-qemu-arm}; LOADER_FLAGS="-marm" ;;
  a64) CROSS=${CROSS:-aarch64-none-elf-}; HOSTCC=${LOADER_CC:-aarch64-linux-gnu-gcc}; QEMU=${QEMU:-qemu-aarch64}; LOADER_FLAGS="" ;;
  *) echo "C++ runtime test: profile must be a32 or a64" >&2; exit 2 ;;
esac
for tool in "$QEMU" "$HOSTCC"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "C++ runtime test ($PROFILE): skipped, $tool not found"
    exit 0
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/root"
cd "$SDK"

build() { # app exceptions [source]
  make -s -f "$SDK/mk/aar.mk" AAR_SDK="$SDK" AAR_PROFILE="$PROFILE" \
    CROSS="$CROSS" APP="$1" SRCS="${3:-templates/cxx_hello.cpp}" BUILD="$WORK/build" \
    AAR_CXX_EXCEPTIONS="$2" all
}

build cxx_hello 1
build cxx_hello_noexc 0
build cxx_env 1 libaar/tests/cxx_env_test.cpp
build cxx_env_noexc 0 libaar/tests/cxx_env_test.cpp
# shellcheck disable=SC2086
"$HOSTCC" -static $LOADER_FLAGS -O1 -Wall -Wextra -Werror -I"$SDK/include" \
  "$SDK/libaar/tests/loader.c" "$SDK/loader/aar_load.c" "$SDK/loader/aar_image.c" -o "$WORK/loader"

run() { # label app expected-text...
  label=$1; app=$2; shift 2
  rm -rf "$WORK/root"
  mkdir "$WORK/root"
  out=$(env AAR_ROOT="$WORK/root" ENTRY_CONTRACT=1 HEAP_INIT_MIB=2 AAR_TEST_VAR=hello \
    "$QEMU" "$WORK/loader" "$WORK/build/$app.elf" 2>&1) \
    || { echo "$out"; echo "C++ runtime test ($PROFILE): $label failed"; exit 1; }
  for want in "$@"; do
    case "$out" in
      *"$want"*) ;;
      *) echo "$out"; echo "C++ runtime test ($PROFILE): $label did not report '$want'"; exit 1 ;;
    esac
  done
  case "$out" in
    *FAIL*) echo "$out"; echo "C++ runtime test ($PROFILE): $label reported a failure"; exit 1 ;;
  esac
  echo "C++ runtime test ($PROFILE): $label ok"
}

run "with exceptions" cxx_hello "global constructor ran" "hello from c++" \
  "exceptions: on, 3 caught" "cxx_hello PASS" "global destructor ran"
run "without exceptions" cxx_hello_noexc "global constructor ran" "hello from c++" \
  "exceptions: off" "cxx_hello PASS" "global destructor ran"
run "environment, with exceptions" cxx_env "cxx_env PASS"
run "environment, without exceptions" cxx_env_noexc "cxx_env PASS"
