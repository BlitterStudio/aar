/*
 * Runner side of C-ENTRY (conformance/TESTS.md): the entry contract is judged by
 * the payload conf_entry from inside. This file runs it twice:
 *
 *   main   the standard arguments (name, alpha, beta gamma, conf:peer=1, info:
 *          words): everything but C-ENTRY.4
 *   long   16 arguments of 127 bytes each, argv[0] included (spec 3.2: the
 *          runtime supports at least that many), for C-ENTRY.4; the payload
 *          prints no other id in this run
 *
 * Assumptions for the card run: AARStart accepts 16 strings of 127 bytes (127
 * plus the NUL is AARInfo.max_arg_bytes 128) and an argv[0] path of that length;
 * a refusal at the start makes C-ENTRY.4 fail with the code, which is the
 * spec's MUST (3.2). This file belongs to the C-ENTRY author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_entry.h"

static char long_store[CONF_ENTRY_LONG_ARGS][CONF_ENTRY_LONG_BYTES + 1UL];
static const char *long_argv[CONF_ENTRY_LONG_ARGS];

static void prepare_long(struct conf_run *run)
{
    unsigned i;
    unsigned j;
    static const char file[] = CONF_ENTRY_FILE;
    unsigned prefix = CONF_ENTRY_LONG_BYTES - (unsigned)(sizeof file - 1);

    /* argv[0]: p...p/conf_entry.elf, 127 bytes. */
    memset(long_store[0], 'p', prefix - 1);
    long_store[0][prefix - 1] = '/';
    memcpy(long_store[0] + prefix, file, sizeof file);
    for (i = 1; i < CONF_ENTRY_LONG_ARGS; i++) {
        for (j = 0; j < CONF_ENTRY_LONG_BYTES; j++)
            long_store[i][j] = conf_entry_long_char(i, j);
        long_store[i][CONF_ENTRY_LONG_BYTES] = '\0';
    }
    for (i = 0; i < CONF_ENTRY_LONG_ARGS; i++)
        long_argv[i] = long_store[i];
    run->argv_override = long_argv;
    run->argc_override = CONF_ENTRY_LONG_ARGS;
    if (conf_g.info.max_args < CONF_ENTRY_LONG_ARGS ||
        conf_g.info.max_arg_bytes < CONF_ENTRY_LONG_BYTES + 1UL) {
        /* The start is then refused and C-ENTRY.4 fails with its code. */
        conf_emitf("# C-ENTRY: AARInfo allows %lu arguments of %lu bytes, the spec's least is 16 of 128",
                   (unsigned long)conf_g.info.max_args,
                   (unsigned long)conf_g.info.max_arg_bytes);
    }
}

static const struct conf_variant variants[] = {
    { "main", NULL },
    { "long", prepare_long },
};

const struct conf_area conf_area_entry = {
    .name = "C-ENTRY",
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
