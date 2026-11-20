/*
 * aar-run: the ZZ9000 specifics of the launcher. STATUS lists the claims on the
 * card and RELEASE ends a claim that is no longer running; both go through the
 * ZZ9000 client extension of aar.library (libraries/aar_zz9000.h, an MIT header),
 * which exists only in the ZZ9000 build of the library. The rest of aar-run is
 * platform neutral.
 *
 * This file is included by aar_run.c and is not compiled alone.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

static const aar_client_ext_zz9000 *zz_ext;

/* The extension table of the ZZ9000 build, or 0 when this library has none (or
 * not the calls aar-run needs). */
static int zz9000_init(void)
{
    zz_ext = (const aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000,
                                                   AAR_EXT_ZZ9000_CLIENT);
    return AAR_CLIENT_EXT_ZZ9000_COVERS(zz_ext, list) &&
           AAR_CLIENT_EXT_ZZ9000_COVERS(zz_ext, release_handle);
}

/* What the board reports about itself, after the "runtime api" line. */
static void zz9000_print_caps(const AARInfo *info)
{
    printf(" caps %08lx", (unsigned long)info->platform[AAR_ZZ9000_INFO_CAPS]);
}

static int zz9000_print_status(void)
{
    uint32_t cursor = 0;
    unsigned shown = 0;

    for (;;) {
        AARZZ9000List item;
        LONG rc;

        memset(&item, 0, sizeof(item));
        item.size = sizeof(item);
        rc = zz_ext->list(cursor, &item);
        if (rc < 0) {
            printf("aar-run: APP_LIST failed (%s)\n", aar_run_error_name((int)rc));
            return 20;
        }
        if (rc == 0 || item.handle == 0 || shown >= 4096u)
            break;
        printf("claim %08lx state %s name %s committed %lu MiB cpu %lu s "
               "block %s reason %s\n",
               (unsigned long)item.handle, state_name(item.state),
               item.name[0] ? (const char *)item.name : "-",
               (unsigned long)(item.committed / AAR_RUN_MIB_BYTES),
               (unsigned long)(item.cpu_ms / 1000u),
               aar_run_block_reason_name(item.block_reason),
               aar_run_end_reason_name(item.end_reason));
        shown++;
        /* index is the card's slot index: the next cursor is one past it. */
        cursor = item.index + 1u;
    }
    if (!shown)
        puts("aar-run: no claims");
    return 0;
}

/* RELEASE <handle>: ends a claim that is no longer running (its app ended,
 * or the card revoked it). A running app is not ended this way. */
static int zz9000_release_claim(uint32_t handle)
{
    uint32_t cursor = 0;
    unsigned guard;

    for (guard = 0; guard < 4096u; guard++) {
        AARZZ9000List item;
        LONG rc;

        memset(&item, 0, sizeof(item));
        item.size = sizeof(item);
        rc = zz_ext->list(cursor, &item);
        if (rc < 0) {
            printf("aar-run: APP_LIST failed (%s)\n", aar_run_error_name((int)rc));
            return 20;
        }
        if (rc == 0 || item.handle == 0)
            break;
        if (item.handle == handle) {
            if (!aar_run_release_allowed(item.state, item.end_reason)) {
                printf("aar-run: claim %08lx is still in use (%s)\n",
                       (unsigned long)handle, state_name(item.state));
                return 20;
            }
            rc = zz_ext->release_handle(handle);
            if (rc != AAR_OK) {
                printf("aar-run: release of claim %08lx failed (%s)\n",
                       (unsigned long)handle, aar_run_error_name((int)rc));
                return 20;
            }
            printf("aar-run: released claim %08lx\n", (unsigned long)handle);
            return 0;
        }
        cursor = item.index + 1u;
    }
    printf("aar-run: there is no claim %08lx\n", (unsigned long)handle);
    return 20;
}
