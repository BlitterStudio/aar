# Builds templates/hello.c, templates/freestanding.c and templates/cxx_hello.cpp
# with an SDK toolchain file. -DAAR_FREESTANDING=ON builds the C templates
# without newlib and needs no sysroot; the C++ template needs the libc flavour
# and is left out then.
#
#   cmake -S cmake/example -B build/cmake-hello-a32 \
#     -DCMAKE_TOOLCHAIN_FILE=$AAR_SDK/cmake/aar-toolchain-a32.cmake
#   cmake --build build/cmake-hello-a32
#
# Use aar-toolchain-a64.cmake in a second build directory for profile A64.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(aar_hello C CXX ASM)

if(NOT AAR_SDK)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=.../cmake/aar-toolchain-a32.cmake (or -a64.cmake)")
endif()

include("${AAR_SDK}/cmake/aar.cmake")
aar_add_executable(hello SOURCES "${AAR_SDK}/templates/hello.c")
aar_add_executable(freestanding FREESTANDING
  SOURCES "${AAR_SDK}/templates/freestanding.c")
if(NOT AAR_FREESTANDING)
  aar_add_executable(cxx_hello SOURCES "${AAR_SDK}/templates/cxx_hello.cpp")
endif()
