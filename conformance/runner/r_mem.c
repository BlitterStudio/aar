/*
 * Runner side of C-MEM (conformance/TESTS.md): the private heap is judged by the
 * payload conf_mem from inside. This file runs it three times and is the peer of
 * one message:
 *
 *   main     C-MEM.1 to C-MEM.8, C-MEM.10, C-MEM.11, C-MEM.13. The payload sends
 *            CONF-MEM-LIMIT with heap_info.limit; the runner keeps it
 *   caps     C-MEM.9: AAR_TAG_PRIVATE_MIB is set to CONF_MEM_CAPS_MIB, and the
 *            payload gets conf:caps_mib and conf:cap_mib (the limit `main` saw,
 *            in MiB) so it can tell whether the platform's own cap is below the
 *            client's
 *   lowmem   C-MEM.12: the payload exhausts the pool twice and counts
 *            AAR_EV_LOW_MEMORY
 *
 * Assumptions for the card run: the platform's own limit is at least the 2 MiB of
 * the payload's note plus the 4 MiB cap; with no cap the limit is the pool (the
 * lowmem variant exhausts it); AARSetTagList accepts AAR_TAG_PRIVATE_MIB before
 * the load. This file belongs to the C-MEM author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_mem.h"

/* The limit `main` reported, MiB; 0 until it did. */
static ULONG main_limit_mib;

static void on_limit(struct conf_run *run, const AARMessage *msg)
{
    ULONG lo = conf_mem_le32(msg->bytes, CONF_MEM_LIMIT_OFFSET);
    ULONG hi = conf_mem_le32(msg->bytes, CONF_MEM_LIMIT_OFFSET + 4u);

    (void)run;
    /* MiB = (hi:lo) >> 20, saturated to 32 bits. */
    main_limit_mib = hi >= (1UL << 20) ? 0xFFFFFFFFUL : (lo >> 20) | (hi << 12);
}

static const struct TagItem caps_tags[] = {
    { AAR_TAG_PRIVATE_MIB, CONF_MEM_CAPS_MIB },
    { TAG_DONE, 0 },
};

static char caps_arg[2][32];

static void prepare_caps(struct conf_run *run)
{
    snprintf(caps_arg[0], sizeof caps_arg[0], "conf:caps_mib=%lu",
             (unsigned long)CONF_MEM_CAPS_MIB);
    snprintf(caps_arg[1], sizeof caps_arg[1], "conf:cap_mib=%lu",
             (unsigned long)main_limit_mib);
    run->tags = caps_tags;
    run->extra_args[0] = caps_arg[0];
    run->extra_args[1] = caps_arg[1];
    run->extra_count = 2;
}

static const struct conf_peer peers[] = {
    { CONF_MSG_MEM_LIMIT, on_limit },
};

static const struct conf_variant variants[] = {
    { "main", NULL },
    { "caps", prepare_caps },
    { "lowmem", NULL },
};

const struct conf_area conf_area_mem = {
    .name = "C-MEM",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
