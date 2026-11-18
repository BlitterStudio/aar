# Shared part of aar-toolchain-a32.cmake and aar-toolchain-a64.cmake. Not a
# toolchain file by itself: the profile file sets AAR_PROFILE first.
#
# SPDX-License-Identifier: MIT

if(NOT AAR_PROFILE STREQUAL "a32" AND NOT AAR_PROFILE STREQUAL "a64")
  message(FATAL_ERROR "AAR_PROFILE must be a32 or a64")
endif()

set(CMAKE_SYSTEM_NAME Generic)
if(AAR_PROFILE STREQUAL "a32")
  set(CMAKE_SYSTEM_PROCESSOR arm)
  set(AAR_TARGET_TRIPLE "arm-none-eabi")
  set(AAR_PROFILE_FLAGS "-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard")
  set(AAR_SYSROOT_VAR AAR_SYSROOT_A32)
else()
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
  set(AAR_TARGET_TRIPLE "aarch64-none-elf")
  set(AAR_PROFILE_FLAGS "-march=armv8-a")
  set(AAR_SYSROOT_VAR AAR_SYSROOT_A64)
endif()

# try_compile projects read this file too and do not see -D options.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  AAR_FREESTANDING AAR_SDK AAR_SYSROOT_A32 AAR_SYSROOT_A64 AAR_CROSS)

if(NOT AAR_SDK)
  if(DEFINED ENV{AAR_SDK} AND NOT "$ENV{AAR_SDK}" STREQUAL "")
    set(AAR_SDK "$ENV{AAR_SDK}")
  else()
    get_filename_component(AAR_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  endif()
endif()
set(AAR_SDK "${AAR_SDK}" CACHE PATH "AAR SDK root" FORCE)
set(AAR_PROFILE "${AAR_PROFILE}" CACHE STRING "AAR profile, a32 or a64" FORCE)

if(NOT ${AAR_SYSROOT_VAR} AND DEFINED ENV{${AAR_SYSROOT_VAR}})
  set(${AAR_SYSROOT_VAR} "$ENV{${AAR_SYSROOT_VAR}}")
endif()
if(NOT ${AAR_SYSROOT_VAR} AND NOT AAR_FREESTANDING)
  message(FATAL_ERROR
    "Set ${AAR_SYSROOT_VAR} to the PIC newlib sysroot for profile ${AAR_PROFILE}")
endif()
if(${AAR_SYSROOT_VAR})
  set(${AAR_SYSROOT_VAR} "${${AAR_SYSROOT_VAR}}" CACHE PATH "PIC newlib sysroot" FORCE)
  set(AAR_SYSROOT "${${AAR_SYSROOT_VAR}}")
  set(AAR_SYSROOT_LINK_FLAG " -L${AAR_SYSROOT}/${AAR_TARGET_TRIPLE}/lib")
else()
  set(AAR_SYSROOT "")
  set(AAR_SYSROOT_LINK_FLAG "")
endif()

if(NOT AAR_CROSS)
  set(AAR_CROSS "${AAR_TARGET_TRIPLE}-")
endif()

set(CMAKE_C_COMPILER "${AAR_CROSS}gcc" CACHE FILEPATH "AAR C compiler" FORCE)
set(CMAKE_ASM_COMPILER "${AAR_CROSS}gcc" CACHE FILEPATH "AAR assembler" FORCE)
set(CMAKE_AR "${AAR_CROSS}ar" CACHE FILEPATH "AAR ar" FORCE)
set(CMAKE_RANLIB "${AAR_CROSS}ranlib" CACHE FILEPATH "AAR ranlib" FORCE)
set(CMAKE_LINKER "${AAR_CROSS}gcc" CACHE FILEPATH "AAR linker" FORCE)

# Same flags as mk/aar.mk.
set(CMAKE_C_FLAGS_INIT "${AAR_PROFILE_FLAGS} -fPIE -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -ffp-contract=off -Wall -Wextra -Werror -std=c11")
set(CMAKE_ASM_FLAGS_INIT "${AAR_PROFILE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${AAR_PROFILE_FLAGS} -nostdlib -fPIE -pie -Wl,-T,${AAR_SDK}/libaar/${AAR_PROFILE}/aar.ld -Wl,--gc-sections -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096${AAR_SYSROOT_LINK_FLAG}")

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
if(AAR_SYSROOT)
  set(CMAKE_FIND_ROOT_PATH "${AAR_SYSROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_STANDARD_LIBRARIES "")
