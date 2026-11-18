/*
 * Allocates and touches 1 MiB heap blocks until allocation fails, then
 * holds them until stopped (Ctrl-C), so a second app can push the platform
 * below its low-memory mark. The low-memory event is a lifecycle event and
 * needs no group.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

#define MEMHOG_BLOCK_BYTES (1024u * 1024u)
#define MEMHOG_PAGE_BYTES 4096u

static void report_events(void)
{
    aar_event events[8];
    uint32_t count = 0u;
    uint32_t i;

    if (aar_event_poll(events, 8u, &count) != AAR_OK)
        return;
    for (i = 0u; i < count; i++) {
        if (events[i].type == AAR_EV_LOW_MEMORY)
            printf("memhog: low memory event\n");
    }
}

int aar_main(int argc, char **argv)
{
    void *head = NULL;
    uint64_t total_mib = 0u;

    (void)argc;
    (void)argv;
    for (;;) {
        uint8_t *block;
        uint32_t offset;

        if (aar_should_stop())
            goto release;
        block = malloc(MEMHOG_BLOCK_BYTES);
        if (block == NULL)
            break;
        for (offset = 0u; offset < MEMHOG_BLOCK_BYTES;
             offset += MEMHOG_PAGE_BYTES) {
            block[offset] = (uint8_t)total_mib;
            if (aar_should_stop()) {
                free(block);
                goto release;
            }
        }
        *(void **)block = head;
        head = block;
        total_mib++;
        if (total_mib % 16u == 0u)
            printf("memhog: %llu MiB\n", (unsigned long long)total_mib);
        report_events();
    }
    printf("memhog: total %llu MiB, holding until Ctrl-C\n",
           (unsigned long long)total_mib);
    while (!aar_should_stop()) {
        report_events();
        aar_sleep_us(100000u);
    }

release:
    while (head != NULL) {
        void *next = *(void **)head;

        free(head);
        head = next;
    }
    return 0;
}
