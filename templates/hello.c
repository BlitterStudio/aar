/*
 * Logs one line and returns. crt0 turns that into exit (libc flavor) or
 * aar_exit (freestanding flavor).
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

static const char hello_text[] = "hello aar\n";
const char *aar_hello_msg = hello_text;

int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aar_log(aar_hello_msg, sizeof hello_text - 1u);
    return 0;
}
