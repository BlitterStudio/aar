/*
 * Link fixture, not a template. Pulls newlib malloc so the reloc check
 * sees more than the hello pointer. Not shipped as an app.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

int aar_main(int argc, char **argv)
{
    void *p;

    (void)argc;
    (void)argv;
    p = malloc(16);
    return p ? 0 : 1;
}
