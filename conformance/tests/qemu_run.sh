#!/bin/sh
# Runs conformance payloads on a real instruction stream: builds them for a
# profile with mk/aar.mk, builds the test loader of libaar (libaar/tests/loader.c)
# for Linux user mode and runs each payload under qemu, checking its AAR-CONF
# lines against conformance/TESTS.md.
#
#   conformance/tests/qemu_run.sh <SDK directory> <a32|a64> <area>...
#
# The areas are the lower-case names of the payloads (time for conf_time). The
# loader is not a platform: its table is partial and it has no client, so a
# payload that needs one prints "SKIP host: ..." for those tests and the checker
# allows that. Needs the profile's bare-metal toolchain and sysroot, qemu-arm or
# qemu-aarch64 and the Linux cross gcc; it prints "skipped" and exits 0 when they
# are missing (see libaar/tests/runtime_test.sh).
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: qemu_run.sh <SDK directory> <a32|a64> <area>...}" && pwd)
PROFILE=${2:?usage: qemu_run.sh <SDK directory> <a32|a64> <area>...}
shift 2
case "$PROFILE" in
  a32) CROSS=${CROSS:-arm-none-eabi-}; HOSTCC=${LOADER_CC:-arm-linux-gnueabihf-gcc}; QEMU=${QEMU:-qemu-arm}; LOADER_FLAGS="-marm" ;;
  a64) CROSS=${CROSS:-aarch64-none-elf-}; HOSTCC=${LOADER_CC:-aarch64-linux-gnu-gcc}; QEMU=${QEMU:-qemu-aarch64}; LOADER_FLAGS="" ;;
  *) echo "conformance qemu run: profile must be a32 or a64" >&2; exit 2 ;;
esac
for tool in "$QEMU" "$HOSTCC"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "conformance qemu run ($PROFILE): skipped, $tool not found"
    exit 0
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$SDK"

# shellcheck disable=SC2086
"$HOSTCC" -static $LOADER_FLAGS -O1 -Wall -Wextra -Werror -I"$SDK/include" \
  "$SDK/libaar/tests/loader.c" "$SDK/loader/aar_load.c" "$SDK/loader/aar_image.c" -o "$WORK/loader"

# The loader enters the image as the runtime would (ENTRY_CONTRACT: fresh stack,
# zeroed registers, link register 0, a table with every core slot and a complete
# aar_info) and commits the 2 MiB that the payloads' notes ask for
# (HEAP_INIT_MIB). It reports every optional CPU feature of spec 8.7 as present
# (CPU_FEATURES; qemu's CPU has them), so C-TABLE.20 and C-TABLE.21 execute the
# instructions. None of it changes what the other payloads see.
ENTRY_CONTRACT=1
HEAP_INIT_MIB=2
case "$PROFILE" in
  a32) CPU_FEATURES=7 ;;
  a64) CPU_FEATURES=63 ;;
esac
export ENTRY_CONTRACT HEAP_INIT_MIB CPU_FEATURES

for area in "$@"; do
  # conf_<area>.ldlibs: extra linker words; conf_<area>.freestanding: no libc
  # (the Makefile reads the same markers).
  LDLIBS_AREA=
  if [ -f "$SDK/conformance/payloads/conf_$area.ldlibs" ]; then
    LDLIBS_AREA=$(cat "$SDK/conformance/payloads/conf_$area.ldlibs")
  fi
  FREESTANDING=0
  if [ -f "$SDK/conformance/payloads/conf_$area.freestanding" ]; then
    FREESTANDING=1
  fi
  make -s -f "$SDK/mk/aar.mk" AAR_SDK="$SDK" AAR_PROFILE="$PROFILE" \
    CROSS="$CROSS" APP="conf_$area" SRCS="conformance/payloads/conf_$area.c" \
    BUILD="$WORK/build" LDLIBS="$LDLIBS_AREA" AAR_FREESTANDING="$FREESTANDING" all
  AREA_ID="C-$(echo "$area" | tr '[:lower:]' '[:upper:]')"
  # The environment variables of conf_host.h, which the loader serves from its own
  # environment (names with the prefix AAR_). There is no runner to set them.
  if [ "$area" = host ]; then
    ENTRY_ARGS=conf:fixtures=1
    AAR_CONF_HELLO='hello world'
    AAR_CONF_LONG=$(awk 'BEGIN { for (i = 0; i < 300; i++) printf "%c", 65 + i % 26 }')
    export ENTRY_ARGS AAR_CONF_HELLO AAR_CONF_LONG
  fi
  python3 "$SDK/conformance/tools/check_results.py" payload --area "$AREA_ID" \
    --profile "$PROFILE" -- "$QEMU" "$WORK/loader" "$WORK/build/conf_$area.elf"
  unset ENTRY_ARGS AAR_CONF_HELLO AAR_CONF_LONG
done
