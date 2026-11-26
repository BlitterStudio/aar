# Cross file for an AAR app, profile A32 (ARMv7-A, Cortex-A9, hard-float, NEON). Pass it as
# CMAKE_TOOLCHAIN_FILE.
#
#   AAR_SDK             SDK root (default: the directory above cmake/)
#   AAR_SYSROOT_A32     PIC newlib and C++ runtime sysroot from
#                       build_pic_newlib.sh and build_pic_libstdcxx.sh
#   AAR_FREESTANDING    ON makes every aar_add_executable target link without
#                       newlib and drops the AAR_SYSROOT_A32 requirement.
#                       Without it, a FREESTANDING target in a project still
#                       needs the sysroot for its siblings. C++ sources need
#                       newlib, so a freestanding build refuses them.
#   AAR_CXX_EXCEPTIONS  ON (default) compiles C++ with -fexceptions and keeps
#                       the unwind tables; OFF compiles with -fno-exceptions.
#   AAR_CROSS           toolchain prefix, default arm-none-eabi-
#
# The compilers are arm-none-eabi-gcc and arm-none-eabi-g++ (Arm GNU Toolchain
# 15.3.rel2). One build directory is one profile: configure a second directory
# with the other file for the other profile.
#
# SPDX-License-Identifier: MIT

set(AAR_PROFILE a32)
include("${CMAKE_CURRENT_LIST_DIR}/aar-toolchain-common.cmake")
