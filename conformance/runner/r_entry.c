/*
 * Runner side of C-ENTRY (conformance/TESTS.md): the entry contract is judged by
 * the payload conf_entry from inside. This file runs it twice:
 *
 *   main   the standard arguments (name, alpha, beta gamma, conf:peer=1, info:
 *          words): everything but C-ENTRY.4, C-ENTRY.23 and C-ENTRY.24
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
#include "../payloads/conf_life.h"
#include "conf_util.h"

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

/* C-ENTRY.23 and C-ENTRY.24 (spec 3.2, "Mode"): the app runs unprivileged, which shows in one
 * effect. Where the platform reports AAR_INFOF_ISOLATED, an instruction that needs privilege
 * (the variant fault-privileged of conf_life: A32 mcr p15 to SCTLR, A64 msr vbar_el1) ends the
 * app with AAR_END_USER_FAULT and the class ILLEGAL or OTHER (a trapped system register access
 * is classed either way). Without the flag the row passes with "not isolated": a runtime that
 * does not isolate apps may run them with more privilege (spec 3.2, 4.4). */
static void priv_run(int idx)
{
    AARHandle *h = NULL;
    struct conf_util_end e;
    char d[CONF_DETAIL_MAX];
    LONG rc;

    if (!(conf_g.info.flags & AAR_INFOF_ISOLATED)) {
        conf_set_result(idx, CONF_R_PASS, "not isolated");
        return;
    }
    rc = conf_util_start_payload(&h, "conf_life.elf", CONF_LIFE_V_FAULT_PRIVILEGED, 0);
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        conf_set_result(idx, CONF_R_FAIL, d);
        return;
    }
    conf_util_wait_end(h, 20UL * CONF_TICKS_PER_SECOND, &e);
    (void)AARClose(h);
    if (!e.ended) {
        conf_detailf(d, "the app did not end after a privileged instruction (state %lu)",
                     (unsigned long)e.state);
        conf_set_result(idx, CONF_R_FAIL, d);
    } else if (e.end_reason != AAR_END_USER_FAULT ||
               (e.fault_class != AAR_FAULT_ILLEGAL && e.fault_class != AAR_FAULT_OTHER)) {
        conf_detailf(d, "a privileged instruction ended the app with reason %lu, class %lu; "
                        "USER_FAULT with ILLEGAL (%lu) or OTHER (%lu) is due",
                     (unsigned long)e.end_reason, (unsigned long)e.fault_class,
                     (unsigned long)AAR_FAULT_ILLEGAL, (unsigned long)AAR_FAULT_OTHER);
        conf_set_result(idx, CONF_R_FAIL, d);
    } else {
        conf_detailf(d, "isolated: a privileged instruction ended the app, USER_FAULT, class %lu",
                     (unsigned long)e.fault_class);
        conf_set_result(idx, CONF_R_PASS, d);
    }
}

static const struct conf_r_test tests[] = {
    { "C-ENTRY.23", priv_run },
    { "C-ENTRY.24", priv_run },
};

static const struct conf_variant variants[] = {
    { "main", NULL },
    { "long", prepare_long },
};

const struct conf_area conf_area_entry = {
    .name = "C-ENTRY",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
