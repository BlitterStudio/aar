#!/bin/sh
# Runs libaar on a real instruction stream: builds tests/runtime_test.c (libc
# flavour) and tests/freestanding_test.c for a profile, builds tests/loader.c
# for Linux user mode, and runs both apps under qemu with a few table shapes.
#
#   libaar/tests/runtime_test.sh <SDK directory> <a32|a64>
#
# Needs the profile's bare-metal toolchain and sysroot (the SDK image has
# both), plus qemu-arm or qemu-aarch64 and arm-linux-gnueabihf-gcc or
# aarch64-linux-gnu-gcc with their libc. The SDK image has neither, so the
# script prints "skipped" and exits 0 when they are missing. To run it, build
# an image FROM the SDK image and add
#   apt-get install qemu-user gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
#       gcc-aarch64-linux-gnu libc6-dev-arm64-cross
# (make check-runtime runs it for every profile).
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: runtime_test.sh <SDK directory> <a32|a64>}" && pwd)
PROFILE=${2:?usage: runtime_test.sh <SDK directory> <a32|a64>}
case "$PROFILE" in
  a32) CROSS=${CROSS:-arm-none-eabi-}; HOSTCC=${LOADER_CC:-arm-linux-gnueabihf-gcc}; QEMU=${QEMU:-qemu-arm}; LOADER_FLAGS="-marm" ;;
  a64) CROSS=${CROSS:-aarch64-none-elf-}; HOSTCC=${LOADER_CC:-aarch64-linux-gnu-gcc}; QEMU=${QEMU:-qemu-aarch64}; LOADER_FLAGS="" ;;
  *) echo "runtime test: profile must be a32 or a64" >&2; exit 2 ;;
esac
for tool in "$QEMU" "$HOSTCC"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "runtime test ($PROFILE): skipped, $tool not found"
    exit 0
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/root"
cd "$SDK"

build() { # app source freestanding
  make -s -f "$SDK/mk/aar.mk" AAR_SDK="$SDK" AAR_PROFILE="$PROFILE" \
    CROSS="$CROSS" APP="$1" SRCS="$2" BUILD="$WORK/build" \
    AAR_FREESTANDING="$3" all
}

build runtime_test libaar/tests/runtime_test.c 0
build freestanding_test libaar/tests/freestanding_test.c 1
# shellcheck disable=SC2086
"$HOSTCC" -static $LOADER_FLAGS -O1 -Wall -Wextra -Werror -I"$SDK/include" \
  "$SDK/libaar/tests/loader.c" -o "$WORK/loader"

run() { # label expected-text env... -- app
  label=$1; want=$2; shift 2
  out=$(env AAR_ROOT="$WORK/root" "$@" 2>&1) || { echo "$out"; echo "runtime test ($PROFILE): $label failed"; exit 1; }
  case "$out" in
    *"$want"*) ;;
    *) echo "$out"; echo "runtime test ($PROFILE): $label did not report '$want'"; exit 1 ;;
  esac
  echo "runtime test ($PROFILE): $label ok"
}

APP="$WORK/build/runtime_test.elf"
run "libc flavour" "runtime_test PASS" "$QEMU" "$WORK/loader" "$APP"
run "libc flavour, heap committed at start, table ends before ext" \
  "runtime_test PASS" HEAP_INIT_MIB=2 CORE_ONLY=1 "$QEMU" "$WORK/loader" "$APP"
# With a coherent runtime publish and acquire never reach the table.
run "libc flavour, coherent runtime" "publish=0" COHERENT=1 "$QEMU" "$WORK/loader" "$APP"
run "freestanding flavour" "freestanding_test PASS" "$QEMU" "$WORK/loader" \
  "$WORK/build/freestanding_test.elf"
