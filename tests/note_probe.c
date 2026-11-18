/*
 * Compile-only probe for AAR_NOTE: tests/abi_check.sh builds it for the Arm
 * targets and checks the emitted note with readelf and objcopy. It is not
 * linked and never runs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0x20000, 8, AAR_GROUP_DISPLAY | AAR_GROUP_GL, AAR_GROUP_AUDIO)

int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return AAR_OK;
}
