/*
 * Sends more app-to-host messages than the queue holds. With --msg-script
 * every send must succeed and be recorded; a wedged queue returns AAR_E_BUSY
 * and this exits non-zero.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"

#include <string.h>

int aar_main(int argc, char **argv)
{
    uint8_t msg[AAR_MSG_BYTES];
    uint32_t ok = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    memset(msg, 0xAA, sizeof msg);
    for (i = 0; i < 100u; i++) {
        if (aar_msg_send(msg) == AAR_OK)
            ok++;
    }
    return ok == 100u ? 0 : 1;
}
