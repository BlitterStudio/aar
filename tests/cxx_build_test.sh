#!/bin/sh
# Build-system checks for C++ apps, for one profile. Needs the profile's
# toolchain and sysroot with the PIC C++ runtime (the SDK image has both).
#
#   tests/cxx_build_test.sh <SDK directory> <a32|a64>
#
# 1. mk/aar.mk: building with AAR_CXX_EXCEPTIONS 1, then 0, then 1 in one
#    build directory must relink each time, so the last image is the first
#    image again and the middle one differs.
# 2. mk/aar.mk: main.c and main.cpp in one app are two objects, and both end
#    up in the image (the link has --no-undefined, so a dropped one fails it).
# 3. cmake: reconfiguring an existing build directory with
#    -DAAR_CXX_EXCEPTIONS=OFF changes the C++ compile flags and the image. Run
#    when cmake is installed.
#
# SPDX-License-Identifier: MIT
set -eu

SDK=$(CDPATH= cd -- "${1:?usage: cxx_build_test.sh <SDK directory> <a32|a64>}" && pwd)
PROFILE=${2:?usage: cxx_build_test.sh <SDK directory> <a32|a64>}
case "$PROFILE" in
  a32) CROSS=${CROSS:-arm-none-eabi-} ;;
  a64) CROSS=${CROSS:-aarch64-none-elf-} ;;
  *) echo "C++ build test: profile must be a32 or a64" >&2; exit 2 ;;
esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mk() { # srcdir app srcs exceptions
  (cd "$1" && make -s -f "$SDK/mk/aar.mk" AAR_SDK="$SDK" AAR_PROFILE="$PROFILE" \
    CROSS="$CROSS" APP="$2" SRCS="$3" BUILD="$WORK/build" AAR_CXX_EXCEPTIONS="$4" all)
}

# 1. The exception mode is part of what the image was linked from.
mk "$SDK" relink templates/cxx_hello.cpp 1
cp "$WORK/build/relink.elf" "$WORK/first.elf"
mk "$SDK" relink templates/cxx_hello.cpp 0
cp "$WORK/build/relink.elf" "$WORK/second.elf"
mk "$SDK" relink templates/cxx_hello.cpp 1
if cmp -s "$WORK/first.elf" "$WORK/second.elf"; then
  echo "C++ build test ($PROFILE): the exception modes gave the same image"; exit 1
fi
if ! cmp -s "$WORK/first.elf" "$WORK/build/relink.elf"; then
  echo "C++ build test ($PROFILE): 1, 0, 1 did not end with the exceptions image"; exit 1
fi
echo "C++ build test ($PROFILE): exception mode switch relinks ok"

# 2. Same base name, different language.
mkdir "$WORK/src"
cat > "$WORK/src/main.c" <<'EOF'
int dup_from_c(void) { return 1; }
EOF
cat > "$WORK/src/main.cpp" <<'EOF'
#include "aar/app.h"
#include "aar/note.h"
AAR_NOTE(0, 0, 0, 0)
extern "C" int dup_from_c(void);
extern "C" int aar_main(int, char **) { return dup_from_c() == 1 ? 0 : 1; }
EOF
mk "$WORK/src" dupname "main.c main.cpp" 1
echo "C++ build test ($PROFILE): main.c and main.cpp both linked ok"

# 3. CMake reconfigure.
if command -v cmake >/dev/null 2>&1; then
  CM="$WORK/cmake"
  cmake -S "$SDK/cmake/example" -B "$CM" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$SDK/cmake/aar-toolchain-$PROFILE.cmake" >/dev/null
  cmake --build "$CM" --target cxx_hello >/dev/null
  cp "$CM/cxx_hello.elf" "$WORK/cm-on.elf"
  grep -q -- '-fexceptions' "$CM/compile_commands.json"
  cmake -S "$SDK/cmake/example" -B "$CM" -DAAR_CXX_EXCEPTIONS=OFF >/dev/null
  cmake --build "$CM" --target cxx_hello >/dev/null
  if grep -q -- '-fexceptions' "$CM/compile_commands.json" || cmp -s "$WORK/cm-on.elf" "$CM/cxx_hello.elf"; then
    echo "C++ build test ($PROFILE): reconfiguring with AAR_CXX_EXCEPTIONS=OFF changed nothing"; exit 1
  fi
  echo "C++ build test ($PROFILE): cmake reconfigure applies AAR_CXX_EXCEPTIONS ok"
else
  echo "C++ build test ($PROFILE): cmake not found, reconfigure check skipped"
fi
