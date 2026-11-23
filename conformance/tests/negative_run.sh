#!/bin/sh
# Runs the accepted images of the C-ELF corpus (conformance/negative/MANIFEST, load
# column OK) on a real instruction stream: they are entered by the test loader of
# libaar (libaar/tests/loader.c) under qemu user mode and must exit with code 3. The
# images that stand for a relocation rule (C-ELF.16, C-ELF.17, C-ELF.18) load a
# relocated pointer and exit with 3 only if it holds the right address, so they are
# run a second time with NO_RELOC=1 (the loader applies no relocation) and must then
# exit with 4: the test would catch a loader that ignores the relocations.
#
#   conformance/tests/negative_run.sh <SDK directory> <a32|a64>
#
# Needs qemu-arm or qemu-aarch64 and the Linux cross gcc (see qemu_run.sh); prints
# "skipped" and exits 0 when they are missing.
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: negative_run.sh <SDK directory> <a32|a64>}" && pwd)
PROFILE=${2:?usage: negative_run.sh <SDK directory> <a32|a64>}
case "$PROFILE" in
  a32) HOSTCC=${LOADER_CC:-arm-linux-gnueabihf-gcc}; QEMU=${QEMU:-qemu-arm}; LOADER_FLAGS="-marm" ;;
  a64) HOSTCC=${LOADER_CC:-aarch64-linux-gnu-gcc}; QEMU=${QEMU:-qemu-aarch64}; LOADER_FLAGS="" ;;
  *) echo "corpus qemu run: profile must be a32 or a64" >&2; exit 2 ;;
esac
for tool in "$QEMU" "$HOSTCC"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "corpus qemu run ($PROFILE): skipped, $tool not found"
    exit 0
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# shellcheck disable=SC2086
"$HOSTCC" -static $LOADER_FLAGS -O1 -Wall -Wextra -Werror -I"$SDK/include" \
  "$SDK/libaar/tests/loader.c" -o "$WORK/loader"

ran=0
checked=0
while read -r name prof ids load probe; do
  case "$name" in
    '#'*|'') continue ;;
  esac
  [ "$prof" = "$PROFILE" ] && [ "$load" = OK ] || continue
  image="$SDK/conformance/negative/$PROFILE/$name.aarimg"
  rc=0
  "$QEMU" "$WORK/loader" "$image" >/dev/null 2>&1 || rc=$?
  if [ "$rc" -ne 3 ]; then
    echo "corpus ($PROFILE): $name exited with $rc, want 3"
    exit 1
  fi
  ran=$((ran + 1))
  case ",$ids," in
    *,C-ELF.16,*|*,C-ELF.17,*|*,C-ELF.18,*)
      rc=0
      NO_RELOC=1 "$QEMU" "$WORK/loader" "$image" >/dev/null 2>&1 || rc=$?
      if [ "$rc" -ne 4 ]; then
        echo "corpus ($PROFILE): $name exited with $rc without relocations, want 4: the image does not check its relocation"
        exit 1
      fi
      checked=$((checked + 1))
      ;;
  esac
done < "$SDK/conformance/negative/MANIFEST"
echo "corpus ($PROFILE): $ran accepted images exit 3, $checked relocation images exit 4 without relocations"
