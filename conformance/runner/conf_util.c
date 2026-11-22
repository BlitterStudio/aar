/*
 * Helpers that several area files of the runner share. See conf_util.h.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <dos/dostags.h>

#include <stdio.h>
#include <string.h>

#include "conf_util.h"

int conf_util_payload_path(char *out, ULONG size, const char *file)
{
    snprintf(out, size, "%s", conf_g.dir);
    return AddPart((STRPTR)out, (CONST_STRPTR)conf_g.profile_name, size) &&
           AddPart((STRPTR)out, (CONST_STRPTR)file, size);
}

int conf_util_negative_path(char *out, ULONG size, const char *file)
{
    snprintf(out, size, "%s", conf_g.dir);
    return AddPart((STRPTR)out, (CONST_STRPTR)"negative", size) &&
           AddPart((STRPTR)out, (CONST_STRPTR)file, size);
}

int conf_util_image_path(char *out, ULONG size, const char *profile, const char *name)
{
    char file[64];

    snprintf(file, sizeof file, "%s.aarimg", name);
    snprintf(out, size, "%s", conf_g.dir);
    return AddPart((STRPTR)out, (CONST_STRPTR)"negative", size) &&
           AddPart((STRPTR)out, (CONST_STRPTR)profile, size) &&
           AddPart((STRPTR)out, (CONST_STRPTR)file, size);
}

LONG conf_util_read_file(const char *path, UBYTE **data, ULONG *len)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG size;
    UBYTE *buf;

    *data = NULL;
    *len = 0;
    if (!fh)
        return -1;
    (void)Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0) {
        Close(fh);
        return -2;
    }
    buf = AllocVec((ULONG)size + 1UL, MEMF_ANY | MEMF_CLEAR); /* a NUL follows the data */
    if (!buf) {
        Close(fh);
        return -2;
    }
    if (size > 0 && Read(fh, buf, size) != size) {
        FreeVec(buf);
        Close(fh);
        return -2;
    }
    Close(fh);
    *data = buf;
    *len = (ULONG)size;
    return 0;
}

void conf_util_wait_end(AARHandle *h, ULONG timeout_ticks, struct conf_util_end *e)
{
    ULONG start = conf_ticks();

    memset(e, 0, sizeof *e);
    for (;;) {
        ULONG now;

        memset(&e->st, 0, sizeof e->st);
        e->st.size = sizeof e->st;
        e->status_rc = AARStatus(h, &e->st);
        now = conf_ticks();
        e->waited_ticks = now - start;
        if (e->status_rc != AAR_OK)
            break;
        if (e->st.end_reason != AAR_END_NONE &&
            (e->st.state == AAR_STATE_IDLE || e->st.state == AAR_STATE_FAULTED)) {
            e->ended = 1;
            break;
        }
        if (e->waited_ticks > timeout_ticks) {
            e->timed_out = 1;
            break;
        }
        Delay(1);
    }
    e->state = e->st.state;
    e->end_reason = e->st.end_reason;
    e->fault_class = e->st.fault_class;
    e->exit_code = e->st.exit_code;
}

LONG conf_util_start_file(AARHandle **h, const char *path,
                          const char *const *argv, ULONG argc)
{
    ULONG reason = 0;
    LONG rc = AAROpen(h, NULL, &reason);

    if (rc != AAR_OK) {
        *h = NULL;
        return rc;
    }
    rc = AARLoadFile(*h, path);
    if (rc == AAR_OK)
        rc = AARStart(*h, argv, argc);
    if (rc != AAR_OK) {
        (void)AARClose(*h);
        *h = NULL;
    }
    return rc;
}

static char arg_name[48];
static char arg_variant[64];
static char arg_ms[32];

LONG conf_util_start_payload(AARHandle **h, const char *file, const char *variant,
                             ULONG ms)
{
    char path[256];
    const char *argv[3];
    ULONG argc = 0;

    *h = NULL;
    if (!conf_util_payload_path(path, sizeof path, file))
        return AAR_E_INVAL;
    snprintf(arg_name, sizeof arg_name, "%s", file);
    snprintf(arg_variant, sizeof arg_variant, "conf:variant=%s", variant);
    argv[argc++] = arg_name;
    argv[argc++] = arg_variant;
    if (ms) {
        snprintf(arg_ms, sizeof arg_ms, "conf:ms=%lu", (unsigned long)ms);
        argv[argc++] = arg_ms;
    }
    return conf_util_start_file(h, path, argv, argc);
}

int conf_util_free_signals(void)
{
    BYTE bits[32];
    int n = 0;
    int i;

    for (i = 0; i < 32; i++) {
        BYTE b = AllocSignal(-1);

        if (b < 0)
            break;
        bits[n++] = b;
    }
    for (i = 0; i < n; i++)
        FreeSignal(bits[i]);
    return n;
}

int conf_util_spawn(const char *name, void (*fn)(void))
{
    struct TagItem tags[4];

    tags[0].ti_Tag = NP_Entry;
    tags[0].ti_Data = (ULONG)fn;
    tags[1].ti_Tag = NP_Name;
    tags[1].ti_Data = (ULONG)name;
    tags[2].ti_Tag = NP_StackSize;
    tags[2].ti_Data = 16384UL;
    tags[3].ti_Tag = TAG_DONE;
    tags[3].ti_Data = 0;
    return CreateNewProc(tags) != NULL;
}

int conf_util_wait_flag(volatile LONG *flag, ULONG seconds)
{
    ULONG start = conf_ticks();

    while (!*flag && conf_ticks() - start < seconds * CONF_TICKS_PER_SECOND)
        Delay(1);
    return *flag != 0;
}
