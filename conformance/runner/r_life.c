/*
 * Runner side of C-LIFE (conformance/TESTS.md): lifecycle. This file belongs to
 * the C-LIFE author.
 *
 * C-LIFE.1 to C-LIFE.3 are judged on every payload run of every area, through the
 * observers below: the claim state after AAROpen, the end state, and the exit
 * code a payload returned.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar-conf.h"

/* The first FAIL of a test stays, so every run is judged and one bad run shows. */
static void life_note(const char *id, int ok, const char *detail)
{
    conf_record(id, ok, detail);
}

/* C-LIFE.1 */
static void on_open(const struct conf_run *run, LONG rc, const AARStatusInfo *st)
{
    char d[CONF_DETAIL_MAX];

    (void)run;
    if (rc == AAR_OK && st->state == AAR_STATE_CLAIMED && st->generation != 0) {
        conf_detailf(d, "claimed, generation %lu", (unsigned long)st->generation);
        life_note("C-LIFE.1", 1, d);
    } else {
        conf_detailf(d, "after open: status %ld, state %lu, generation %lu", (long)rc,
                     (unsigned long)st->state, (unsigned long)st->generation);
        life_note("C-LIFE.1", 0, d);
    }
}

/* C-LIFE.2 and C-LIFE.3 */
static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    char d[CONF_DETAIL_MAX];
    const char *name = run->area;

    if (!out->ran) {
        conf_detailf(d, "%s did not run: %.80s", name, out->why);
        life_note("C-LIFE.2", 0, d);
        life_note("C-LIFE.3", 0, d);
        return;
    }
    if (out->status_rc == AAR_OK && out->state == AAR_STATE_IDLE &&
        out->end_reason == AAR_END_EXIT && out->fault_class == AAR_FAULT_NONE) {
        conf_detailf(d, "%s ended: idle, exit, no fault", name);
        life_note("C-LIFE.2", 1, d);
    } else {
        conf_detailf(d, "%s ended: status %ld, state %lu, end reason %lu, fault %lu",
                     name, (long)out->status_rc, (unsigned long)out->state,
                     (unsigned long)out->end_reason, (unsigned long)out->fault_class);
        life_note("C-LIFE.2", 0, d);
    }
    if (!pr->have_end) {
        conf_detailf(d, "%s printed no end line", name);
        life_note("C-LIFE.3", 0, d);
    } else if (out->exit_code == (pr->fail ? 10L : 0L)) {
        conf_detailf(d, "%s returned %ld", name, (long)out->exit_code);
        life_note("C-LIFE.3", 1, d);
    } else {
        conf_detailf(d, "%s returned %ld but reported %lu failures", name,
                     (long)out->exit_code, (unsigned long)pr->fail);
        life_note("C-LIFE.3", 0, d);
    }
}

const struct conf_area conf_area_life = {
    .name = "C-LIFE",
    .on_open = on_open,
    .on_end = on_end,
};
