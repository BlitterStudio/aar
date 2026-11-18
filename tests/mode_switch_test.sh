#!/bin/sh
# Builds hello in both flavours into the SAME output file in one build
# directory, without deleting anything, through every flavour transition:
# libc, freestanding, libc, freestanding, libc. After each build the image
# must equal a clean build of that flavour, and no archive used by a libc link
# may hold freestanding.o. Needs the toolchain on PATH, and the sysroot
# variable for the profile (AAR_SYSROOT_A32 or AAR_SYSROOT_A64) for the libc
# flavour.
#
# usage: mode_switch_test.sh <SDK directory> [a32|a64]
#
# The test builds templates/hello.c. CROSS overrides the toolchain prefix.
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: mode_switch_test.sh <SDK directory> [a32|a64]}" && pwd)
PROFILE=${2:-a32}
case "$PROFILE" in
  a32) DEFAULT_CROSS=arm-none-eabi- ;;
  a64) DEFAULT_CROSS=aarch64-none-elf- ;;
  *) echo "mode switch: profile must be a32 or a64" >&2; exit 2 ;;
esac
CROSS=${CROSS:-$DEFAULT_CROSS}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$SDK"

build() { # build-dir freestanding
  if ! make -s -f "$SDK/mk/aar.mk" AAR_SDK="$SDK" AAR_PROFILE="$PROFILE" \
      CROSS="$CROSS" APP=hello SRCS=templates/hello.c BUILD="$1" \
      AAR_FREESTANDING="$2" all >"$WORK/log" 2>&1; then
    cat "$WORK/log"
    exit 1
  fi
}

build "$WORK/clean0" 0
build "$WORK/clean1" 1

step=0
for mode in 0 1 0 1 0; do
  step=$((step + 1))
  build "$WORK/sw" "$mode"
  if ! cmp -s "$WORK/clean$mode/hello.elf" "$WORK/sw/hello.elf"; then
    echo "mode switch ($PROFILE): step $step (AAR_FREESTANDING=$mode) returned the other flavour's image"
    exit 1
  fi
  for archive in $(find "$WORK/sw" -name libaar.a); do
    case "$archive" in
      */freestanding/*) continue ;;
    esac
    if "${CROSS}ar" t "$archive" | grep -q freestanding.o; then
      echo "mode switch ($PROFILE): $archive holds freestanding.o"
      exit 1
    fi
  done
done
echo "mode switch ($PROFILE): ok"
