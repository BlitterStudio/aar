/*
 * Deliberately faults after proving the app log path.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aar_log("fault aar: executing UDF\n",
            sizeof "fault aar: executing UDF\n" - 1u);
    /* UDF is architecturally undefined on both profiles, independent of any
     * mapping at address zero. A32 encodes it as 0xe7f000f0, A64 as 0. */
#if defined(__aarch64__)
    __asm__ volatile (".inst 0x00000000" ::: "memory");
#else
    __asm__ volatile (".word 0xe7f000f0" ::: "memory");
#endif
    return 1;
}
