# Cross file for an AAR app, profile A64 (ARMv8-A AArch64). Pass it as
# CMAKE_TOOLCHAIN_FILE.
#
#   AAR_SDK             SDK root (default: the directory above cmake/)
#   AAR_SYSROOT_A64     PIC newlib sysroot from build_pic_newlib.sh
#   AAR_FREESTANDING    ON makes every aar_add_executable target link without
#                       newlib and drops the AAR_SYSROOT_A64 requirement.
#                       Without it, a FREESTANDING target in a project still
#                       needs the sysroot for its siblings.
#   AAR_CROSS           toolchain prefix, default aarch64-none-elf-
#
# The compiler is aarch64-none-elf-gcc (Arm GNU Toolchain 15.3.rel2). One build
# directory is one profile: configure a second directory with the other file
# for the other profile.
#
# SPDX-License-Identifier: MIT

set(AAR_PROFILE a64)
include("${CMAKE_CURRENT_LIST_DIR}/aar-toolchain-common.cmake")
