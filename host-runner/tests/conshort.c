/*
 * One console read with the input pipe still open. aar_console_read must
 * return the available bytes, not block waiting for the full length; a
 * blocked read never prints and this never exits.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"

#include <stdio.h>

int aar_main(int argc, char **argv)
{
    char buf[64];
    uint32_t n = 0;
    int rc;

    (void)argc;
    (void)argv;
    rc = aar_console_read(buf, sizeof buf, &n);
    if (rc != AAR_OK)
        return 1;
    printf("got %u: %.*s\n", n, (int)n, buf);
    fflush(stdout);
    return n == 6u ? 0 : 2;
}
