/*
 * Echo every 32-byte client message until the client stops the app. The app
 * parks in aar_msg_wait between messages.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

int aar_main(int argc, char **argv)
{
    uint8_t message[AAR_MSG_BYTES];

    (void)argc;
    (void)argv;
    while (!aar_should_stop()) {
        int rc = aar_msg_wait(500000u);

        if (rc == AAR_E_TIMEDOUT)
            continue;
        if (rc != AAR_OK)
            break;              /* stop, or a corrupt queue */
        rc = aar_msg_poll(message);
        if (rc < 0)
            return 20;
        if (rc == 0) {
            aar_yield();
            continue;
        }
        while ((rc = aar_msg_send(message)) == AAR_E_BUSY)
            aar_yield();
        if (rc != AAR_OK)
            return 20;
    }
    return 0;
}
