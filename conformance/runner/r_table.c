/*
 * Runner side of C-TABLE (conformance/TESTS.md): the table, aar_info, the groups
 * and aar_ext are judged by the payload conf_table from inside. The runner gives
 * it its own view of the platform, to compare with what the app sees: the
 * standard info: words (groups, platform_id, msg_capacity, stall_limit_ms,
 * stop_grace_ms, from the core) and three more: api_minor (C-TABLE.2),
 * shared_default_mib (C-TABLE.18) and aarinfo (C-TABLE.3, .10 and .13: the
 * client's flags, shared_granule and profiles in one word, because an app gets
 * at most 16 argument strings).
 *
 * Assumptions for the card run: AARInfo and the app's aar_info agree on the
 * groups, the platform id, the message capacity, the stall limit and the stop
 * grace (the spec's 3.3 and 3.4 say the same platform answers both); the client
 * sets no shared limit (aar_serve does not), so the app sees the default; the
 * platform has room for a heap_grow of 1 MiB. Needs at least 15 argument slots
 * (AARInfo.max_args 16): the payload's tests of the client words are skipped as
 * host-only when the platform drops the last words. This file belongs to the
 * C-TABLE author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"

static char info_arg[3][64];

static void prepare_main(struct conf_run *run)
{
    snprintf(info_arg[0], sizeof info_arg[0], "info:api_minor=%lu",
             (unsigned long)conf_g.info.api_minor);
    snprintf(info_arg[1], sizeof info_arg[1], "info:shared_default_mib=%lu",
             (unsigned long)conf_g.info.shared_default_mib);
    snprintf(info_arg[2], sizeof info_arg[2], "info:aarinfo=%lu,%lu,%lu",
             (unsigned long)conf_g.info.flags,
             (unsigned long)conf_g.info.shared_granule,
             (unsigned long)conf_g.info.profiles);
    run->extra_args[0] = info_arg[0];
    run->extra_args[1] = info_arg[1];
    run->extra_args[2] = info_arg[2];
    run->extra_count = 3;
}

static const struct conf_variant variants[] = {
    { "main", prepare_main },
};

const struct conf_area conf_area_table = {
    .name = "C-TABLE",
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
