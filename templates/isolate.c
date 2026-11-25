/*
 * Exercises the app address-space boundary of an isolated runtime without
 * touching app resources.
 * ZZ9000 note: the address probes below target its firmware memory and an
 * ungranted host heap page. Select addresses for the platform under test.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

#define FIRMWARE_ADDRESS 0x00100000u
#define UNGRANTED_HOST_ADDRESS 0x03400000u

typedef void (*firmware_entry_fn)(void);

static void text_target(void)
{
}

static int fault_store(void)
{
    volatile uint32_t *const address =
        (volatile uint32_t *)(uintptr_t)FIRMWARE_ADDRESS;

    printf("isolate store: writing firmware memory\n");
    fflush(stdout);
    *address = 0u;
    return 20;
}

static int fault_exec(void)
{
    firmware_entry_fn const entry =
        (firmware_entry_fn)(uintptr_t)FIRMWARE_ADDRESS;

    printf("isolate exec: entering firmware text\n");
    fflush(stdout);
    entry();
    return 20;
}

static int fault_window(void)
{
    volatile uint32_t *const address =
        (volatile uint32_t *)(uintptr_t)UNGRANTED_HOST_ADDRESS;

    printf("isolate window: writing ungranted host heap\n");
    fflush(stdout);
    *address = 0u;
    return 20;
}

/* A pointer into the app's own text is not a writable output buffer. An
 * isolated runtime answers AAR_E_FAULT. */
static int check_svc_pointer(void)
{
    int rc;

    printf("isolate svc: passing text pointer to aar_get_info\n");
    fflush(stdout);
    rc = aar_get_info((aar_info *)(uintptr_t)text_target);
    printf("isolate svc: aar_get_info returned %d\n", rc);
    return rc == AAR_E_FAULT ? 0 : 20;
}

int aar_main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: isolate <store|exec|window|svc>\n");
        return 20;
    }
    if (strcmp(argv[1], "store") == 0)
        return fault_store();
    if (strcmp(argv[1], "exec") == 0)
        return fault_exec();
    if (strcmp(argv[1], "window") == 0)
        return fault_window();
    if (strcmp(argv[1], "svc") == 0)
        return check_svc_pointer();
    fprintf(stderr, "usage: isolate <store|exec|window|svc>\n");
    return 20;
}
