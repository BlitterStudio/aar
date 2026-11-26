/*
 * Runner side of C-CLIENT (conformance/TESTS.md): aar.library as a client sees
 * it. This file belongs to the C-CLIENT author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <exec/libraries.h>
#include <exec/ports.h>

#include <stdio.h>
#include <string.h>

#include <libraries/aar_zz9000.h>

#include "conf_util.h"
#include "../payloads/conf_life.h"

static int is_pow2(ULONG v)
{
    return v != 0 && (v & (v - 1UL)) == 0;
}

static void pass_fail(int idx, int ok, const char *detail)
{
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, detail);
}

/* C-CLIENT.1 */
static void t_lib_version(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = AARBase->lib_Version == AAR_LIB_VERSION &&
             AAR_LIB_VERSION == AAR_API_MAJOR;

    conf_detailf(d, "aar.library %u.%u, API major %u", (unsigned)AARBase->lib_Version,
                 (unsigned)AARBase->lib_Revision, (unsigned)AAR_API_MAJOR);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.2: AARQuery with a small size must change only that many bytes; with
 * a size larger than the structure it must write no more than the structure. */
static void t_query_size(int idx)
{
    ULONG buf[40];
    UBYTE *b = (UBYTE *)buf;
    LONG rc;
    int i;
    char d[CONF_DETAIL_MAX];

    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 16;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARQuery with size 16 returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    for (i = 16; i < 160; i++) {
        if (b[i] != 0xA5) {
            conf_detailf(d, "size 16 changed byte %d", i);
            pass_fail(idx, 0, d);
            return;
        }
    }
    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 160;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARQuery with size 160 returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    for (i = (int)sizeof(AARInfo); i < 160; i++) {
        if (b[i] != 0xA5) {
            conf_detailf(d, "size 160 wrote beyond the structure, byte %d", i);
            pass_fail(idx, 0, d);
            return;
        }
    }
    if (conf_g.info.reserved0 != 0 || conf_g.info.reserved1[0] != 0 ||
        conf_g.info.reserved1[1] != 0 || conf_g.info.reserved1[2] != 0 ||
        conf_g.info.reserved1[3] != 0) {
        pass_fail(idx, 0, "reserved words of AARInfo are not 0");
        return;
    }
    pass_fail(idx, 1, "size 16 and size 160 respected, reserved words 0");
}

/* C-CLIENT.3 */
static void t_api_version(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = conf_g.info.api_major == AAR_API_MAJOR &&
             conf_g.info.api_minor >= AAR_API_MINOR;

    conf_detailf(d, "platform API %lu.%lu, suite %u.%u",
                 (unsigned long)conf_g.info.api_major,
                 (unsigned long)conf_g.info.api_minor, (unsigned)AAR_API_MAJOR,
                 (unsigned)AAR_API_MINOR);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.4 */
static void t_profiles(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG p = conf_g.info.profiles;
    int ok = p != 0 && (p & ~(AAR_PROFILEF_A32 | AAR_PROFILEF_A64)) == 0 &&
             (p & (1UL << (conf_g.profile_value - 1UL))) != 0;

    conf_detailf(d, "profiles 0x%lx, testing %s", (unsigned long)p,
                 conf_g.profile_name);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.5 */
static void t_groups(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG g = conf_g.info.groups;
    int ok = (g & ~(ULONG)(AAR_GROUPS_DEFINED | AAR_GROUPS_EXPERIMENTAL)) == 0;

    conf_detailf(d, "groups 0x%lx", (unsigned long)g);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.6 */
static void t_limits(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = conf_g.info.max_image_bytes >= 16UL * 1024UL * 1024UL &&
             conf_g.info.max_args >= 16UL && conf_g.info.max_arg_bytes >= 128UL;

    conf_detailf(d, "max image %lu, args %lu of %lu bytes",
                 (unsigned long)conf_g.info.max_image_bytes,
                 (unsigned long)conf_g.info.max_args,
                 (unsigned long)conf_g.info.max_arg_bytes);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.7 */
static void t_granules(int idx)
{
    char d[CONF_DETAIL_MAX];
    const AARInfo *i = &conf_g.info;
    int ok = is_pow2(i->shared_granule) && i->shared_granule >= 64UL &&
             i->msg_capacity >= 32UL && i->stall_limit_ms >= 10000UL &&
             i->shared_default_mib == AAR_SHARED_DEFAULT_MIB &&
             i->shared_max_mib >= i->shared_default_mib;

    conf_detailf(d, "granule %lu, msg %lu, stall %lu ms, shared %lu of %lu MiB",
                 (unsigned long)i->shared_granule, (unsigned long)i->msg_capacity,
                 (unsigned long)i->stall_limit_ms, (unsigned long)i->shared_default_mib,
                 (unsigned long)i->shared_max_mib);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.8 */
static void t_queue_depth(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG depth = conf_g.info.queue_depth;
    int ok = is_pow2(depth) && depth >= 64UL && depth <= 4096UL &&
             conf_g.info.msg_capacity == depth - 1UL;

    conf_detailf(d, "queue depth %lu, msg capacity %lu", (unsigned long)depth,
                 (unsigned long)conf_g.info.msg_capacity);
    pass_fail(idx, ok, d);
}

/* ---- Helpers ------------------------------------------------------------------------------- */

static int image_path(char *out, ULONG size, const char *name)
{
    return conf_util_image_path(out, size, conf_g.profile_name, name);
}

static const char *const image_argv[] = { "conf_client" };

/* Opens a claim, loads the image (from the file path or from memory), starts it and
 * waits for its end. Returns AAR_OK, or the first call that failed. */
static LONG run_image(const char *path, const UBYTE *data, ULONG len,
                      const char *const *argv, ULONG argc, struct conf_util_end *e)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    LONG rc;
    LONG closed;

    memset(e, 0, sizeof *e);
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK)
        return rc;
    rc = path ? AARLoadFile(h, path) : AARLoadMemory(h, data, len);
    if (rc == AAR_OK)
        rc = AARStart(h, argv, argc);
    if (rc == AAR_OK) {
        conf_util_wait_end(h, 10UL * CONF_TICKS_PER_SECOND, e);
        if (!e->ended)
            (void)AARStop(h);
    }
    closed = AARClose(h);
    return rc != AAR_OK ? rc : closed;
}

static int ran_ok(const struct conf_util_end *e)
{
    return e->ended && e->end_reason == AAR_END_EXIT && e->exit_code == CONF_IMAGE_EXIT;
}

static LONG set_tag(AARHandle *h, ULONG tag, ULONG data)
{
    struct TagItem t[2];

    t[0].ti_Tag = tag;
    t[0].ti_Data = data;
    t[1].ti_Tag = TAG_DONE;
    t[1].ti_Data = 0;
    return AARSetTagList(h, t);
}

/* ---- C-CLIENT.9 and C-CLIENT.10: the probe ---------------------------------------------------- */

/* The note of negative/<profile>/probe-note.aarimg (gen_negative.py). */
#define NOTE_STACK 131072UL
#define NOTE_HEAP 3UL
#define NOTE_REQUIRED 0x3UL
#define NOTE_OPTIONAL 0x14UL
#define NOTE_MINOR 5UL

static int info_matches(const AARImageInfo *ii, ULONG bytes, char *d)
{
    if (ii->profile != conf_g.profile_value || ii->api_major != AAR_API_MAJOR ||
        ii->api_minor_required != NOTE_MINOR || ii->note_version != 1UL) {
        conf_detailf(d, "profile %lu, API %lu.%lu, note version %lu",
                     (unsigned long)ii->profile, (unsigned long)ii->api_major,
                     (unsigned long)ii->api_minor_required, (unsigned long)ii->note_version);
        return 0;
    }
    if (ii->stack_bytes != NOTE_STACK || ii->min_heap_mib != NOTE_HEAP) {
        conf_detailf(d, "stack %lu, heap %lu MiB", (unsigned long)ii->stack_bytes,
                     (unsigned long)ii->min_heap_mib);
        return 0;
    }
    if (ii->groups_required != NOTE_REQUIRED || ii->groups_optional != NOTE_OPTIONAL) {
        conf_detailf(d, "groups required 0x%lx, optional 0x%lx",
                     (unsigned long)ii->groups_required, (unsigned long)ii->groups_optional);
        return 0;
    }
    if (ii->image_bytes != bytes || ii->reserved[0] != 0 || ii->reserved[1] != 0) {
        conf_detailf(d, "image_bytes %lu, the file has %lu", (unsigned long)ii->image_bytes,
                     (unsigned long)bytes);
        return 0;
    }
    return 1;
}

/* C-CLIENT.9 */
static void t_probe(int idx)
{
    char path[256];
    char d[CONF_DETAIL_MAX];
    AARImageInfo a;
    AARImageInfo b;
    UBYTE *data = NULL;
    ULONG len = 0;
    LONG rc;

    if (!image_path(path, sizeof path, "probe-note")) {
        pass_fail(idx, 0, "image path too long");
        return;
    }
    if (conf_util_read_file(path, &data, &len) != 0) {
        conf_detailf(d, "cannot read %.70s: copy conformance/negative to DIR", path);
        pass_fail(idx, 0, d);
        return;
    }
    memset(&a, 0xA5, sizeof a);
    a.size = sizeof a;
    rc = AARProbeFile(path, &a);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARProbeFile returned %ld", (long)rc);
        pass_fail(idx, 0, d);
    } else if (!info_matches(&a, len, d)) {
        char e[CONF_DETAIL_MAX];

        conf_detailf(e, "AARProbeFile: %.90s", d);
        pass_fail(idx, 0, e);
    } else {
        memset(&b, 0xA5, sizeof b);
        b.size = sizeof b;
        rc = AARProbeMemory(data, len, &b);
        if (rc != AAR_OK) {
            conf_detailf(d, "AARProbeMemory returned %ld", (long)rc);
            pass_fail(idx, 0, d);
        } else if (memcmp(&a, &b, sizeof a) != 0) {
            pass_fail(idx, 0, "AARProbeMemory and AARProbeFile disagree");
        } else {
            conf_detailf(d, "profile %lu, API 0.%lu, stack %lu, heap %lu, groups 0x%lx/0x%lx",
                         (unsigned long)a.profile, (unsigned long)a.api_minor_required,
                         (unsigned long)a.stack_bytes, (unsigned long)a.min_heap_mib,
                         (unsigned long)a.groups_required, (unsigned long)a.groups_optional);
            pass_fail(idx, 1, d);
        }
    }
    FreeVec(data);
}

/* A probe that has to fail with the code `want` (spec 9.2: AAR_E_INVAL for anything that is
 * not a valid image, AAR_E_NOENT for a file that cannot be opened) and leave the caller's
 * structure alone. */
static int bad_probe(const char *what, const char *path, const UBYTE *data, ULONG len,
                     LONG want, char *d)
{
    AARImageInfo info;
    AARImageInfo before;
    LONG rc;

    memset(&info, 0xA5, sizeof info);
    info.size = sizeof info;
    before = info;
    rc = path ? AARProbeFile(path, &info) : AARProbeMemory(data, len, &info);
    if (rc != want) {
        conf_detailf(d, "%s: the probe returned %ld, %ld is due", what, (long)rc, (long)want);
        return 0;
    }
    if (memcmp(&info, &before, sizeof info) != 0) {
        conf_detailf(d, "%s: the probe changed the structure of the caller", what);
        return 0;
    }
    return 1;
}

/* C-CLIENT.10 */
static void t_probe_bad(int idx)
{
    static const char *const names[] = { "not-elf", "truncated-20", "truncated-header" };
    char path[256];
    char d[CONF_DETAIL_MAX];
    unsigned i;

    for (i = 0; i < CONF_COUNT(names); i++) {
        UBYTE *data = NULL;
        ULONG len = 0;
        int ok;

        if (!image_path(path, sizeof path, names[i]) ||
            conf_util_read_file(path, &data, &len) != 0) {
            conf_detailf(d, "cannot read %.40s: copy conformance/negative to DIR", names[i]);
            pass_fail(idx, 0, d);
            return;
        }
        ok = bad_probe(names[i], path, NULL, 0, AAR_E_INVAL, d) &&
             bad_probe(names[i], NULL, data, len, AAR_E_INVAL, d);
        FreeVec(data);
        if (!ok) {
            pass_fail(idx, 0, d);
            return;
        }
    }
    if (!image_path(path, sizeof path, "no-such-image") ||
        !bad_probe("a missing file", path, NULL, 0, AAR_E_NOENT, d)) {
        pass_fail(idx, 0, d);
        return;
    }
    pass_fail(idx, 1, "not an ELF and truncated: AAR_E_INVAL; a missing file: AAR_E_NOENT; nothing changed");
}

/* ---- C-CLIENT.11 to C-CLIENT.14: open and options ------------------------------------------------ */

/* C-CLIENT.11 */
static void t_open(int idx)
{
    AARHandle *a = NULL;
    AARHandle *b = NULL;
    char d[CONF_DETAIL_MAX];
    LONG ra = AAROpen(&a, NULL, NULL);
    LONG rb = ra == AAR_OK ? AAROpen(&b, NULL, NULL) : ra;
    int ok = ra == AAR_OK && rb == AAR_OK && a && b && a != b;

    conf_detailf(d, "two opens: %ld and %ld, handles %s", (long)ra, (long)rb,
                 a != b ? "differ" : "are the same");
    if (rb == AAR_OK)
        (void)AARClose(b);
    if (ra == AAR_OK)
        (void)AARClose(a);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.12 */
static void t_tags(int idx)
{
    static const struct {
        ULONG tag;
        ULONG data;
        const char *name;
    } accepted[] = {
        { AAR_TAG_PRIVATE_MIB, 8UL, "AAR_TAG_PRIVATE_MIB" },
        { AAR_TAG_SHARED_MIB, 8UL, "AAR_TAG_SHARED_MIB" },
        { AAR_TAG_AUDIO_ROUTE, AAR_AUDIO_ROUTE_PLATFORM, "AAR_TAG_AUDIO_ROUTE" },
        { AAR_TAG_LEASE_EXTENDED, 1UL, "AAR_TAG_LEASE_EXTENDED" },
        { AAR_TAG_CLIENT_CAPS, 0UL, "AAR_TAG_CLIENT_CAPS" },
    };
    AARHandle *h = NULL;
    ULONG reason = 0;
    char d[CONF_DETAIL_MAX];
    LONG rc = AAROpen(&h, NULL, &reason);
    unsigned i;

    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    for (i = 0; i < CONF_COUNT(accepted); i++) {
        rc = set_tag(h, accepted[i].tag, accepted[i].data);
        if (rc != AAR_OK) {
            conf_detailf(d, "%s returned %ld", accepted[i].name, (long)rc);
            (void)AARClose(h);
            pass_fail(idx, 0, d);
            return;
        }
    }
    rc = set_tag(h, AAR_TAG_WEIGHT, 1UL);
    (void)AARClose(h);
    if (rc != AAR_OK && rc != AAR_E_NOSYS) {
        conf_detailf(d, "AAR_TAG_WEIGHT returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    conf_detailf(d, "five tags accepted, AAR_TAG_WEIGHT %s", rc == AAR_OK ? "accepted" : "AAR_E_NOSYS");
    pass_fail(idx, 1, d);
}

/* C-CLIENT.13 */
static void t_tags_late(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    char path[256];
    char d[CONF_DETAIL_MAX];
    LONG rc = AAROpen(&h, NULL, &reason);
    LONG shared;
    LONG priv;
    LONG late;
    LONG load;

    if (rc != AAR_OK || !image_path(path, sizeof path, "ok-stack0")) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        if (rc == AAR_OK)
            (void)AARClose(h);
        pass_fail(idx, 0, d);
        return;
    }
    shared = set_tag(h, AAR_TAG_SHARED_MIB, 4096UL);
    priv = set_tag(h, AAR_TAG_PRIVATE_MIB, 4096UL);
    load = AARLoadFile(h, path);
    late = set_tag(h, AAR_TAG_PRIVATE_MIB, 8UL);
    (void)AARClose(h);
    if (shared != AAR_E_INVAL || priv != AAR_E_INVAL) {
        conf_detailf(d, "4096 MiB: shared %ld, private %ld, AAR_E_INVAL is due", (long)shared,
                     (long)priv);
        pass_fail(idx, 0, d);
    } else if (load != AAR_OK) {
        conf_detailf(d, "AARLoadFile of %.40s returned %ld", path, (long)load);
        pass_fail(idx, 0, d);
    } else if (late != AAR_E_BUSY) {
        conf_detailf(d, "AARSetTagList after the load returned %ld, AAR_E_BUSY is due", (long)late);
        pass_fail(idx, 0, d);
    } else {
        pass_fail(idx, 1, "4096 MiB is AAR_E_INVAL, a tag after the load AAR_E_BUSY");
    }
}

/* C-CLIENT.14 */
static void t_weight(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    char d[CONF_DETAIL_MAX];
    ULONG max = conf_g.info.weight_max;
    LONG rc = AAROpen(&h, NULL, &reason);
    LONG good;
    LONG over;

    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    if (max == 0UL) {
        good = set_tag(h, AAR_TAG_WEIGHT, 1UL);
        (void)AARClose(h);
        conf_detailf(d, "weight_max is 0: AAR_TAG_WEIGHT returned %ld", (long)good);
        pass_fail(idx, good == AAR_E_NOSYS, d);
        return;
    }
    good = set_tag(h, AAR_TAG_WEIGHT, max);
    over = set_tag(h, AAR_TAG_WEIGHT, max + 1UL);
    (void)AARClose(h);
    conf_detailf(d, "weight_max %lu: %lu gives %ld, %lu gives %ld", (unsigned long)max,
                 (unsigned long)max, (long)good, (unsigned long)(max + 1UL), (long)over);
    pass_fail(idx, good == AAR_OK && over == AAR_E_INVAL, d);
}

/* ---- C-CLIENT.15 to C-CLIENT.17: load, start, status -------------------------------------------- */

/* C-CLIENT.15 */
static void t_load_memory(int idx)
{
    char path[256];
    char d[CONF_DETAIL_MAX];
    UBYTE *data = NULL;
    ULONG len = 0;
    struct conf_util_end ef;
    struct conf_util_end em;
    LONG rf;
    LONG rm;

    if (!image_path(path, sizeof path, "ok-stack0") ||
        conf_util_read_file(path, &data, &len) != 0) {
        conf_detailf(d, "cannot read %.60s: copy conformance/negative to DIR", path);
        pass_fail(idx, 0, d);
        return;
    }
    rf = run_image(path, NULL, 0, image_argv, 1, &ef);
    rm = run_image(NULL, data, len, image_argv, 1, &em);
    FreeVec(data);
    if (rf != AAR_OK || rm != AAR_OK || !ran_ok(&ef) || !ran_ok(&em)) {
        conf_detailf(d, "file: %ld, reason %lu, exit %ld; memory: %ld, reason %lu, exit %ld",
                     (long)rf, (unsigned long)ef.end_reason, (long)ef.exit_code, (long)rm,
                     (unsigned long)em.end_reason, (long)em.exit_code);
        pass_fail(idx, 0, d);
    } else {
        conf_detailf(d, "both runs exited with %ld", (long)ef.exit_code);
        pass_fail(idx, 1, d);
    }
}

/* C-CLIENT.16 */
static void t_start_args(int idx)
{
    char path[256];
    char d[CONF_DETAIL_MAX];
    ULONG n = conf_g.info.max_args;
    ULONG sz = conf_g.info.max_arg_bytes;
    ULONG i;
    UBYTE *buf;
    char *text;
    const char **argv;
    struct conf_util_end e;
    LONG rc;

    if (n == 0UL || sz < 2UL || n > 4096UL || sz > 4096UL || n * sz > 8UL * 1024UL * 1024UL ||
        !image_path(path, sizeof path, "ok-stack0")) {
        conf_detailf(d, "max_args %lu of %lu bytes cannot be built here", (unsigned long)n,
                     (unsigned long)sz);
        pass_fail(idx, 0, d);
        return;
    }
    buf = AllocVec(n * sizeof(char *) + n * sz, MEMF_ANY | MEMF_CLEAR);
    if (!buf) {
        pass_fail(idx, 0, "no memory for the argument strings");
        return;
    }
    argv = (const char **)buf;
    text = (char *)buf + n * sizeof(char *);
    for (i = 0; i < n; i++) {
        char *s = text + i * sz;
        ULONG k;

        for (k = 0; k + 1UL < sz; k++)
            s[k] = (char)('a' + (i % 26UL));
        s[sz - 1UL] = '\0';
        argv[i] = s;
    }
    snprintf(text, sz, "conf_client"); /* the first string is the program name */
    rc = run_image(path, NULL, 0, argv, n, &e);
    FreeVec(buf);
    if (rc != AAR_OK || !ran_ok(&e)) {
        conf_detailf(d, "%lu strings of %lu bytes: result %ld, end reason %lu, exit %ld",
                     (unsigned long)n, (unsigned long)sz, (long)rc,
                     (unsigned long)e.end_reason, (long)e.exit_code);
        pass_fail(idx, 0, d);
    } else {
        conf_detailf(d, "%lu strings of %lu bytes accepted", (unsigned long)n,
                     (unsigned long)sz);
        pass_fail(idx, 1, d);
    }
}

/* C-CLIENT.17 */
static void t_status(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    char path[256];
    char d[CONF_DETAIL_MAX];
    UBYTE raw[sizeof(AARStatusInfo) + 16];
    AARStatusInfo *small = (AARStatusInfo *)raw;
    AARStatusInfo full;
    LONG rc;
    int i;

    if (!image_path(path, sizeof path, "ok-stack0")) {
        pass_fail(idx, 0, "image path too long");
        return;
    }
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    memset(raw, 0xA5, sizeof raw);
    small->size = 16;
    rc = AARStatus(h, small);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARStatus with size 16 returned %ld", (long)rc);
    } else if (small->state != AAR_STATE_CLAIMED || small->generation == 0UL ||
               small->end_reason != AAR_END_NONE) {
        conf_detailf(d, "size 16: state %lu, generation %lu, end reason %lu",
                     (unsigned long)small->state, (unsigned long)small->generation,
                     (unsigned long)small->end_reason);
    } else {
        for (i = 16; i < (int)sizeof raw; i++) {
            if (raw[i] != 0xA5) {
                conf_detailf(d, "size 16 changed byte %d", i);
                goto bad;
            }
        }
        rc = AARLoadFile(h, path);
        if (rc != AAR_OK) {
            conf_detailf(d, "AARLoadFile of the ok image returned %ld", (long)rc);
            goto bad;
        }
        memset(&full, 0, sizeof full);
        full.size = sizeof full;
        rc = AARStatus(h, &full);
        if (rc != AAR_OK) {
            conf_detailf(d, "AARStatus after the load returned %ld", (long)rc);
        } else if (full.generation != small->generation || full.generation == 0UL) {
            conf_detailf(d, "generation %lu, then %lu", (unsigned long)small->generation,
                         (unsigned long)full.generation);
        } else if (full.load_base.hi == 0UL && full.load_base.lo == 0UL) {
            conf_detailf(d, "load_base is 0 after the load");
        } else if (full.profile != conf_g.profile_value) {
            conf_detailf(d, "profile %lu, the image is %lu", (unsigned long)full.profile,
                         (unsigned long)conf_g.profile_value);
        } else {
            (void)AARClose(h);
            conf_detailf(d, "generation %lu, load_base %lx, profile %lu",
                         (unsigned long)full.generation, (unsigned long)full.load_base.lo,
                         (unsigned long)full.profile);
            pass_fail(idx, 1, d);
            return;
        }
    }
bad:
    (void)AARClose(h);
    pass_fail(idx, 0, d);
}

/* ---- C-CLIENT.18 to C-CLIENT.20: the signal ---------------------------------------------------- */

/* C-CLIENT.18 */
static void t_signal_mask(int idx)
{
    AARHandle *a = NULL;
    AARHandle *b = NULL;
    char d[CONF_DETAIL_MAX];
    ULONG ma;
    ULONG mb;
    ULONG null_mask;
    ULONG closed_mask;
    LONG ra = AAROpen(&a, NULL, NULL);
    LONG rb = ra == AAR_OK ? AAROpen(&b, NULL, NULL) : ra;

    if (ra != AAR_OK || rb != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld and %ld", (long)ra, (long)rb);
        if (ra == AAR_OK)
            (void)AARClose(a);
        pass_fail(idx, 0, d);
        return;
    }
    ma = AARMessageSignal(a);
    mb = AARMessageSignal(b);
    null_mask = AARMessageSignal(NULL);
    (void)AARClose(b);
    closed_mask = AARMessageSignal(b);
    (void)AARClose(a);
    conf_detailf(d, "masks 0x%lx and 0x%lx, NULL gives 0x%lx, a closed handle 0x%lx",
                 (unsigned long)ma, (unsigned long)mb, (unsigned long)null_mask,
                 (unsigned long)closed_mask);
    pass_fail(idx, ma != 0UL && ma == mb && null_mask == 0UL && closed_mask == 0UL, d);
}

static int wait_signal(ULONG mask, ULONG ticks)
{
    ULONG t0 = conf_ticks();

    while (!(SetSignal(0, 0) & mask)) {
        if (conf_ticks() - t0 > ticks)
            return 0;
        Delay(1);
    }
    return 1;
}

static void clear_signal(ULONG mask)
{
    (void)SetSignal(0, mask);
}

/* The next request of the app (a file stat: it parks the app) and its completion. clear_mask: the
 * signal bits to clear between taking the request and replying to it. */
static int serve_one(AARHandle *h, ULONG ticks, char *d, ULONG clear_mask)
{
    AARServiceReq req;
    ULONG t0 = conf_ticks();
    LONG rc;

    for (;;) {
        memset(&req, 0, sizeof req);
        req.size = sizeof req;
        rc = AARServiceNext(h, &req);
        if (rc == AAR_OK)
            break;
        if (rc != AAR_E_BUSY || conf_ticks() - t0 > ticks) {
            conf_detailf(d, "AARServiceNext returned %ld", (long)rc);
            return 0;
        }
        Delay(1);
    }
    if (req.op != AAR_OP_FILE_STAT) {
        conf_detailf(d, "the request is op 0x%lx, a file stat is due", (unsigned long)req.op);
        return 0;
    }
    /* The signal for the end of the app can come as soon as the reply is posted:
     * clear what is pending before it, not after. */
    if (clear_mask)
        clear_signal(clear_mask);
    req.result = AAR_E_NOENT; /* the name does not exist */
    rc = AARServiceReply(h, &req);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARServiceReply returned %ld", (long)rc);
        return 0;
    }
    return 1;
}

static LONG send_go(AARHandle *h)
{
    AARMessage m;

    memset(&m, 0, sizeof m);
    conf_life_build(m.bytes, CONF_LIFE_GO, 0, 0, 0, 0);
    return AARSendMessage(h, &m);
}

/* C-CLIENT.19 */
static void t_signal_events(int idx)
{
    AARHandle *h = NULL;
    AARMessage m;
    struct conf_util_end e;
    char d[CONF_DETAIL_MAX];
    ULONG mask;
    LONG rc;
    int got_msg = 0;
    int sig_msg;
    int sig_req;
    int sig_exit;

    rc = conf_util_start_payload(&h, "conf_life.elf", CONF_LIFE_V_BURST, 1UL);
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    mask = AARMessageSignal(h); /* arms the handle */
    clear_signal(mask);
    sig_msg = wait_signal(mask, 5UL * CONF_TICKS_PER_SECOND);
    if (sig_msg) {
        ULONG t0 = conf_ticks();

        while (!got_msg && conf_ticks() - t0 < CONF_TICKS_PER_SECOND) {
            if (AARGetMessage(h, &m) == AAR_OK && conf_life_is(m.bytes, CONF_LIFE_MSG))
                got_msg = 1;
            else
                Delay(1);
        }
    }
    clear_signal(mask);
    rc = send_go(h);
    sig_req = rc == AAR_OK && wait_signal(mask, 5UL * CONF_TICKS_PER_SECOND);
    if (!sig_req || !serve_one(h, 5UL * CONF_TICKS_PER_SECOND, d, mask)) {
        if (!sig_req)
            conf_detailf(d, "no signal for the request (send %ld)", (long)rc);
        (void)AARStop(h);
        (void)AARClose(h);
        pass_fail(idx, 0, sig_msg ? d : "no signal for the app's message");
        return;
    }
    sig_exit = wait_signal(mask, 5UL * CONF_TICKS_PER_SECOND);
    conf_util_wait_end(h, 5UL * CONF_TICKS_PER_SECOND, &e);
    (void)AARClose(h);
    conf_detailf(d, "signal for the message %s, the request %s, the exit %s",
                 sig_msg ? "yes" : "NO", sig_req ? "yes" : "NO", sig_exit ? "yes" : "NO");
    pass_fail(idx, sig_msg && got_msg && sig_req && sig_exit && e.ended, d);
}

/* C-CLIENT.20 (SHOULD): the owner wakes within 250 ms of an app message. Each round
 * sends a ping to the app, which answers at once. */
static void t_wake(int idx)
{
    AARHandle *h = NULL;
    AARMessage m;
    char d[CONF_DETAIL_MAX];
    ULONG mask;
    ULONG worst = 0;
    ULONG sum = 0;
    ULONG rounds = 0;
    LONG rc;
    ULONG r;

    rc = conf_util_start_payload(&h, "conf_life.elf", CONF_LIFE_V_PING, 0);
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    mask = AARMessageSignal(h);
    Delay(25);
    for (r = 0; r < 20UL; r++) {
        ULONG t0;
        ULONG ms;
        int woke;

        while (AARGetMessage(h, &m) == AAR_OK)
            ;
        clear_signal(mask);
        memset(&m, 0, sizeof m);
        conf_life_build(m.bytes, CONF_LIFE_PING, r, 0, 0, 0);
        t0 = conf_ticks();
        if (AARSendMessage(h, &m) != AAR_OK)
            break;
        woke = wait_signal(mask, 2UL * CONF_TICKS_PER_SECOND);
        ms = (conf_ticks() - t0) * 1000UL / CONF_TICKS_PER_SECOND;
        if (!woke)
            ms = 2000UL;
        if (ms > worst)
            worst = ms;
        sum += ms;
        rounds++;
        Delay(5);
    }
    (void)AARStop(h);
    (void)AARClose(h);
    conf_detailf(d, "%lu rounds, worst %lu ms, mean %lu ms", (unsigned long)rounds,
                 (unsigned long)worst, (unsigned long)(rounds ? sum / rounds : 0));
    pass_fail(idx, rounds == 20UL && worst <= 250UL + 40UL, d);
}
/* ---- C-CLIENT.21 and C-CLIENT.22: the notify port ---------------------------------------------- */

struct evinfo {
    ULONG type;
    APTR handle;
    APTR reply;
    ULONG length;
    ULONG end_reason; /* of the status the event carries */
};

/* Takes the next event off the port, waiting for it for ticks. */
static int next_event(struct MsgPort *port, ULONG ticks, struct evinfo *ev)
{
    ULONG t0 = conf_ticks();

    for (;;) {
        AAREvent *e = (AAREvent *)GetMsg(port);

        if (e) {
            ev->type = e->type;
            ev->handle = e->handle;
            ev->reply = e->message.mn_ReplyPort;
            ev->length = e->message.mn_Length;
            ev->end_reason = e->status.end_reason;
            return 1;
        }
        if (conf_ticks() - t0 > ticks)
            return 0;
        Delay(1);
    }
}

static int port_count(struct MsgPort *port)
{
    struct Node *n;
    int c = 0;

    Forbid();
    for (n = port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        c++;
    Permit();
    return c;
}

/* The most events the port held at once over about 100 ms. */
static int port_peak(struct MsgPort *port)
{
    int peak = 0;
    int i;

    for (i = 0; i < 6; i++) {
        int c = port_count(port);

        if (c > peak)
            peak = c;
        Delay(1);
    }
    return peak;
}

static void drain_port(struct MsgPort *port)
{
    while (GetMsg(port))
        ;
}

static ULONG drain_messages(AARHandle *h)
{
    AARMessage m;
    ULONG n = 0;

    while (AARGetMessage(h, &m) == AAR_OK)
        n++;
    return n;
}

static const char *type_name(ULONG t)
{
    switch (t) {
    case AAR_NOTIFY_MESSAGE: return "MESSAGE";
    case AAR_NOTIFY_COMPLETE: return "COMPLETE";
    case AAR_NOTIFY_FAULT: return "FAULT";
    case AAR_NOTIFY_REQUEST: return "REQUEST";
    default: return "an unknown type";
    }
}

/* The next event has to be of type want and name h. */
static int expect_event(struct MsgPort *port, AARHandle *h, ULONG want, const char *when,
                        char *d)
{
    struct evinfo ev;

    if (!next_event(port, 2UL * CONF_TICKS_PER_SECOND, &ev)) {
        conf_detailf(d, "no event %s (a %s is due)", when, type_name(want));
        return 0;
    }
    if (ev.type != want) {
        conf_detailf(d, "the event %s is %s, %s is due", when, type_name(ev.type),
                     type_name(want));
        return 0;
    }
    if (ev.handle != (APTR)h) {
        conf_detailf(d, "the event %s names another handle", when);
        return 0;
    }
    return 1;
}

/* Three messages that stay unread while the app goes on and ends. C-CLIENT.21: one event
 * on the port at a time. C-CLIENT.22: while a message waits every event is MESSAGE. */
static int events_messages(struct MsgPort *port, char *d, int *one_at_a_time)
{
    AARHandle *h = NULL;
    struct conf_util_end e;
    struct evinfo ev;
    int i;
    LONG rc = conf_util_start_payload_port(&h, port, "conf_life.elf", CONF_LIFE_V_BURST, 3UL);
    int ok = 0;

    *one_at_a_time = 0;
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        return 0;
    }
    Delay(2UL * CONF_TICKS_PER_SECOND);
    if (port_peak(port) != 1) {
        conf_detailf(d, "the port holds %d events after three messages, one is due",
                     port_count(port));
        goto done;
    }
    if (!next_event(port, CONF_TICKS_PER_SECOND, &ev) || ev.type != AAR_NOTIFY_MESSAGE ||
        ev.handle != (APTR)h || ev.reply != NULL) {
        conf_detailf(d, "first event: type %s, handle %s, reply port %s",
                     type_name(ev.type), ev.handle == (APTR)h ? "right" : "wrong",
                     ev.reply ? "set" : "NULL");
        goto done;
    }
    /* The next one is posted now; it stays on the port while the app goes on. */
    *one_at_a_time = port_peak(port) <= 1;
    rc = send_go(h);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARSendMessage returned %ld", (long)rc);
        goto done;
    }
    if (!serve_one(h, 5UL * CONF_TICKS_PER_SECOND, d, 0UL))
        goto done;
    conf_util_wait_end(h, 5UL * CONF_TICKS_PER_SECOND, &e);
    if (!e.ended) {
        conf_detailf(d, "the app did not end after its request was served");
        goto done;
    }
    /* A message is waiting: every event is MESSAGE, also after the app has ended, until
     * one of them carries the end (its status). COMPLETE is due only when nothing
     * waits. */
    for (i = 0; i < 6; i++) {
        if (!next_event(port, 2UL * CONF_TICKS_PER_SECOND, &ev)) {
            conf_detailf(d, "no event after the app ended (the end is due in a MESSAGE event)");
            goto done;
        }
        if (ev.type != AAR_NOTIFY_MESSAGE) {
            conf_detailf(d, "an event while messages wait is %s, MESSAGE is due",
                         type_name(ev.type));
            goto done;
        }
        if (ev.end_reason != AAR_END_NONE) {
            ok = 1;
            break;
        }
    }
    if (!ok)
        conf_detailf(d, "no event carried the end of the app");
done:
    drain_port(port);
    (void)AARClose(h);
    return ok;
}

/* One message that is taken at once, then the app queues a request: REQUEST; then
 * the app ends: COMPLETE. */
static int events_request(struct MsgPort *port, char *d)
{
    AARHandle *h = NULL;
    struct conf_util_end e;
    LONG rc = conf_util_start_payload_port(&h, port, "conf_life.elf", CONF_LIFE_V_BURST, 1UL);
    int ok = 0;

    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        return 0;
    }
    Delay(2UL * CONF_TICKS_PER_SECOND);
    if (drain_messages(h) == 0UL) {
        conf_detailf(d, "the app's message was not there");
        goto done;
    }
    if (!expect_event(port, h, AAR_NOTIFY_MESSAGE, "for the message", d))
        goto done;
    rc = send_go(h);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARSendMessage returned %ld", (long)rc);
        goto done;
    }
    if (!expect_event(port, h, AAR_NOTIFY_REQUEST, "for the request, no message waiting", d))
        goto done;
    if (!serve_one(h, 5UL * CONF_TICKS_PER_SECOND, d, 0UL))
        goto done;
    conf_util_wait_end(h, 5UL * CONF_TICKS_PER_SECOND, &e);
    ok = expect_event(port, h, AAR_NOTIFY_COMPLETE, "after the app exited", d);
done:
    drain_port(port);
    (void)AARClose(h);
    return ok;
}

/* A fault ends an app that sent nothing: FAULT. */
static int events_fault(struct MsgPort *port, char *d)
{
    AARHandle *h = NULL;
    struct conf_util_end e;
    LONG rc = conf_util_start_payload_port(&h, port, "conf_life.elf", CONF_LIFE_V_FAULT_QUIET, 0);
    int ok = 0;

    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        return 0;
    }
    conf_util_wait_end(h, 10UL * CONF_TICKS_PER_SECOND, &e);
    if (!e.ended || e.end_reason != AAR_END_USER_FAULT) {
        conf_detailf(d, "the app did not fault: end reason %lu", (unsigned long)e.end_reason);
        goto done;
    }
    ok = expect_event(port, h, AAR_NOTIFY_FAULT, "after a fault, nothing waiting", d);
done:
    drain_port(port);
    (void)AARClose(h);
    return ok;
}

static void g_events(void)
{
    struct MsgPort *port = CreateMsgPort();
    char d[CONF_DETAIL_MAX];
    int one = 0;
    int ok;

    if (!port) {
        conf_record("C-CLIENT.21", 0, "no message port");
        conf_record("C-CLIENT.22", 0, "no message port");
        return;
    }
    ok = events_messages(port, d, &one);
    conf_record("C-CLIENT.21", one, one ? "one event on the port at a time, naming the handle, no reply port"
                                        : d);
    if (ok)
        ok = events_request(port, d);
    if (ok)
        ok = events_fault(port, d);
    conf_record("C-CLIENT.22", ok, ok ? "MESSAGE, then COMPLETE or FAULT, else REQUEST" : d);
    DeleteMsgPort(port);
}

static int events_done;

/* C-CLIENT.21 and C-CLIENT.22 share the runs of the notify port. */
static void t_events(int idx)
{
    (void)idx;
    if (!events_done) {
        events_done = 1;
        g_events();
    }
}

/* ---- C-CLIENT.23 to C-CLIENT.26: signals, owners, handles --------------------------------------- */

/* The signal bit is the library's: allocated at the owner task's first AAROpen and
 * returned when the task has no handle left, by its last AARClose or at the latest by
 * its last CloseLibrary [spec question 59]. The runner's own task keeps its base open
 * for the whole run, so a child task that opens the library itself does the count. */
static struct sigctl {
    volatile LONG done;
    LONG open_rc;
    int n0, n1, n2, n3;
} sigc;

static void signal_child(void)
{
    struct Library *own = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    struct Library *saved = AARBase;
    AARHandle *hs[3];
    ULONG reason = 0;
    int n = 0;

    if (!own) {
        sigc.open_rc = AAR_E_NOSYS;
        sigc.done = 1;
        return;
    }
    AARBase = own; /* the parent is waiting and calls nothing meanwhile */
    sigc.n0 = conf_util_free_signals();
    while (n < 3 && (sigc.open_rc = AAROpen(&hs[n], NULL, &reason)) == AAR_OK)
        n++;
    sigc.n1 = conf_util_free_signals();
    while (n > 0)
        (void)AARClose(hs[--n]);
    sigc.n2 = conf_util_free_signals();
    AARBase = saved;
    CloseLibrary(own);
    sigc.n3 = conf_util_free_signals();
    sigc.done = 1;
}

/* C-CLIENT.23 */
static void t_signal_bit(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok;

    memset(&sigc, 0, sizeof sigc);
    if (!conf_util_spawn("aar-conf-child", signal_child) || !conf_util_wait_flag(&sigc.done, 20)) {
        pass_fail(idx, 0, "the child task did not run");
        return;
    }
    conf_detailf(d, "child task, free signal bits: %d before, %d with three handles, %d after "
                    "AARClose, %d after CloseLibrary", sigc.n0, sigc.n1, sigc.n2, sigc.n3);
    ok = sigc.open_rc == AAR_OK && sigc.n0 > 0 && sigc.n1 == sigc.n0 - 1 &&
         (sigc.n2 == sigc.n0 - 1 || sigc.n2 == sigc.n0) && sigc.n3 == sigc.n0;
    pass_fail(idx, ok, d);
}

static struct owner_ctl {
    volatile LONG done;
    AARHandle *h;
    LONG status, stop, send, close;
} owner;

/* The child task: a task that does not own the handle. */
static void owner_child(void)
{
    AARStatusInfo st;
    AARMessage m;

    memset(&st, 0, sizeof st);
    st.size = 16;
    memset(&m, 0, sizeof m);
    owner.status = AARStatus(owner.h, &st);
    owner.stop = AARStop(owner.h);
    owner.send = AARSendMessage(owner.h, &m);
    owner.close = AARClose(owner.h);
    owner.done = 1;
}

/* C-CLIENT.24 */
static void t_owner(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    AARStatusInfo st;
    char d[CONF_DETAIL_MAX];
    LONG own;
    LONG rc = AAROpen(&h, NULL, &reason);

    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    memset(&owner, 0, sizeof owner);
    owner.h = h;
    if (!conf_util_spawn("aar-conf-child", owner_child) || !conf_util_wait_flag(&owner.done, 10)) {
        (void)AARClose(h);
        pass_fail(idx, 0, "the other task did not run");
        return;
    }
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    own = AARStatus(h, &st);
    if (owner.status != AAR_E_OWNER || owner.stop != AAR_E_OWNER ||
        owner.send != AAR_E_OWNER || owner.close != AAR_E_OWNER) {
        conf_detailf(d, "another task: status %ld, stop %ld, send %ld, close %ld "
                        "(AAR_E_OWNER is due)", (long)owner.status, (long)owner.stop,
                     (long)owner.send, (long)owner.close);
        pass_fail(idx, 0, d);
    } else if (own != AAR_OK || st.state != AAR_STATE_CLAIMED) {
        conf_detailf(d, "the owner's status after the other task's calls: %ld, state %lu",
                     (long)own, (unsigned long)st.state);
        pass_fail(idx, 0, d);
    } else {
        pass_fail(idx, 1, "status, stop, send and close from another task: AAR_E_OWNER, the handle still works");
    }
    (void)AARClose(h);
}

/* C-CLIENT.25 */
static void t_many_handles(int idx)
{
    AARHandle *hs[20];
    ULONG reason = 0;
    char d[CONF_DETAIL_MAX];
    ULONG mask;
    LONG rc = AAR_OK;
    int n = 0;
    int i;
    int ok = 1;

    while (n < 20 && (rc = AAROpen(&hs[n], NULL, &reason)) == AAR_OK)
        n++;
    if (n < 20) {
        int ceiling = rc == AAR_E_NORESOURCE && conf_g.info.max_apps != 0UL &&
                      (ULONG)n >= conf_g.info.max_apps;

        conf_detailf(d, "%d handles, then %ld (reason %lu), max_apps %lu", n, (long)rc,
                     (unsigned long)reason, (unsigned long)conf_g.info.max_apps);
        while (n > 0)
            (void)AARClose(hs[--n]);
        pass_fail(idx, ceiling, d);
        return;
    }
    mask = AARMessageSignal(hs[0]);
    for (i = 0; i < 20; i++) {
        AARStatusInfo st;
        int k;

        memset(&st, 0, sizeof st);
        st.size = 16;
        if (AARStatus(hs[i], &st) != AAR_OK || st.state != AAR_STATE_CLAIMED ||
            AARMessageSignal(hs[i]) != mask)
            ok = 0;
        for (k = 0; k < i; k++)
            ok &= hs[k] != hs[i];
    }
    for (i = 19; i >= 0; i--)
        (void)AARClose(hs[i]);
    conf_detailf(d, "20 handles of one task, one signal mask 0x%lx", (unsigned long)mask);
    pass_fail(idx, ok && mask != 0UL, d);
}

/* C-CLIENT.26: a NULL handle is refused and nothing is written. */
static void t_null_handle(int idx)
{
    AARStatusInfo st;
    AARStatusInfo st_before;
    AARMessage m;
    AARMessage m_before;
    AARServiceReq req;
    struct AARSharedMap map;
    struct TagItem none[1];
    char d[CONF_DETAIL_MAX];
    LONG rc[13];
    static const char *const names[13] = {
        "AARClose", "AARStatus", "AARStop", "AARStart", "AARLoadFile", "AARLoadMemory",
        "AARSetTagList", "AARSendMessage", "AARGetMessage", "AARSharedMap",
        "AARSharedUnmap", "AARServiceNext", "AARServiceReply"
    };
    int i;

    memset(&st, 0xA5, sizeof st);
    st.size = sizeof st;
    st_before = st;
    memset(&m, 0xA5, sizeof m);
    m_before = m;
    memset(&req, 0, sizeof req);
    req.size = sizeof req;
    memset(&map, 0, sizeof map);
    map.size = sizeof map;
    none[0].ti_Tag = TAG_DONE;
    none[0].ti_Data = 0;
    rc[0] = AARClose(NULL);
    rc[1] = AARStatus(NULL, &st);
    rc[2] = AARStop(NULL);
    rc[3] = AARStart(NULL, image_argv, 1);
    rc[4] = AARLoadFile(NULL, "NIL:");
    rc[5] = AARLoadMemory(NULL, "ab", 2);
    rc[6] = AARSetTagList(NULL, none);
    rc[7] = AARSendMessage(NULL, &m);
    rc[8] = AARGetMessage(NULL, &m);
    rc[9] = AARSharedMap(NULL, 0x1000UL, 16UL, 0UL, &map);
    rc[10] = AARSharedUnmap(NULL, &map);
    rc[11] = AARServiceNext(NULL, &req);
    rc[12] = AARServiceReply(NULL, &req);
    {
        char codes[96];
        size_t used = 0;

        codes[0] = '\0';
        for (i = 0; i < 13; i++) {
            int n = snprintf(codes + used, sizeof codes - used, "%s%ld", i ? " " : "", (long)rc[i]);

            if (n > 0 && (size_t)n < sizeof codes - used)
                used += (size_t)n;
        }
        for (i = 0; i < 13; i++) {
            if (rc[i] != AAR_E_STALE) {
                conf_detailf(d, "%s(NULL) returned %ld, AAR_E_STALE is due (spec 10.2); the 13 "
                                "codes: %s", names[i], (long)rc[i], codes);
                pass_fail(idx, 0, d);
                return;
            }
        }
        if (memcmp(&st, &st_before, sizeof st) != 0 || memcmp(&m, &m_before, sizeof m) != 0 ||
            map.base != NULL) {
            conf_detailf(d, "a call with a NULL handle changed the caller's structure (the 13 "
                            "codes: %s)", codes);
            pass_fail(idx, 0, d);
            return;
        }
        conf_detailf(d, "13 calls with a NULL handle returned AAR_E_STALE, nothing written "
                        "(codes %s)", codes);
        pass_fail(idx, 1, d);
    }
}

/* ---- C-CLIENT.27 to C-CLIENT.29: extensions and tokens --------------------------------------------- */

/* C-CLIENT.27 */
static void t_ext_unknown(int idx)
{
    APTR a = AARExt(0x7FFF0000UL, 1UL);
    APTR b = AARExt(conf_g.info.platform_id, 0xFFFFUL);
    APTR c = AARExt(0xFFFFFFFFUL, 0xFFFFFFFFUL);
    char d[CONF_DETAIL_MAX];

    conf_detailf(d, "unknown platform %p, unknown extension %p, both unknown %p", a, b, c);
    pass_fail(idx, a == NULL && b == NULL && c == NULL, d);
}

/* C-CLIENT.28 */
static void t_ext_zz9000(int idx)
{
    const aar_client_ext_zz9000 *x =
        (const aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    char d[CONF_DETAIL_MAX];

    if (!x) {
        pass_fail(idx, 0, "AARExt(ZZ9000, CLIENT) returned NULL");
        return;
    }
    conf_detailf(d, "table of %lu bytes", (unsigned long)x->size);
    pass_fail(idx, x->size >= 2UL * sizeof(ULONG) && AAR_CLIENT_EXT_ZZ9000_COVERS(x, list), d);
}

/* C-CLIENT.29: the token of a mapped buffer. */
static void t_token(int idx)
{
    AARHandle *h = NULL;
    AARMessage msg;
    struct AARSharedMap map;
    struct conf_util_end e;
    char d[CONF_DETAIL_MAX];
    ULONG token = 0;
    ULONG t0;
    LONG rc = conf_util_start_payload(&h, "conf_life.elf", CONF_LIFE_V_HOLD_SHARED, 0);
    LONG rt;
    int got = 0;

    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    t0 = conf_ticks();
    while (!got && conf_ticks() - t0 < 10UL * CONF_TICKS_PER_SECOND) {
        if (AARGetMessage(h, &msg) == AAR_OK && conf_life_is(msg.bytes, CONF_LIFE_TOKEN))
            got = 1;
        else
            Delay(2);
    }
    memset(&map, 0, sizeof map);
    map.size = sizeof map;
    if (!got) {
        pass_fail(idx, 0, "the app sent no token");
    } else if ((rc = AARSharedMap(h, conf_life_get(msg.bytes, 0), conf_life_get(msg.bytes, 2),
                                  0UL, &map)) != AAR_OK) {
        conf_detailf(d, "AARSharedMap returned %ld", (long)rc);
        pass_fail(idx, 0, d);
    } else {
        rt = AARSharedToken(h, map.base, map.bytes, &token);
        if (rt == AAR_E_NOSYS) {
            pass_fail(idx, 1, "AAR_E_NOSYS: the platform cannot name host memory");
        } else if (rt != AAR_OK) {
            conf_detailf(d, "AARSharedToken returned %ld", (long)rt);
            pass_fail(idx, 0, d);
        } else if (token != map.haddr) {
            conf_detailf(d, "the token of the map is 0x%lx, the app gave 0x%lx",
                         (unsigned long)token, (unsigned long)map.haddr);
            pass_fail(idx, 0, d);
        } else {
            conf_detailf(d, "token 0x%lx is the buffer's own", (unsigned long)token);
            pass_fail(idx, 1, d);
        }
        (void)AARSharedUnmap(h, &map);
    }
    (void)AARStop(h);
    conf_util_wait_end(h, 5UL * CONF_TICKS_PER_SECOND, &e);
    (void)AARClose(h);
}

/* ---- C-CLIENT.30 and C-CLIENT.31: the serving loop ---------------------------------------------- */

static struct conf_run run;

static void client_run(const char *variant, ULONG serve_groups, struct conf_outcome *out)
{
    /* The app is conf_life.elf, which the area name C-LIFE selects. */
    conf_run_init(&run, "C-LIFE");
    run.variant = variant;
    run.norelay = 1;
    run.timeout_s = 60;
    run.serve_groups = serve_groups;
    conf_run_payload(&run, out);
}

/* C-CLIENT.30 */
static void t_unserved_group(int idx)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];

    if (!(conf_g.info.groups & AAR_GROUP_DISPLAY)) {
        pass_fail(idx, 1, "no display group declared: the app makes no display request");
        return;
    }
    client_run(CONF_LIFE_V_DISPLAY, AAR_GROUP_INPUT | AAR_GROUP_AUDIO, &out);
    conf_run_free(&run);
    if (!out.ran) {
        conf_detailf(d, "conf_life did not run: %.80s", out.why);
        pass_fail(idx, 0, d);
    } else if (out.end_reason != AAR_END_EXIT || out.exit_code != 0) {
        conf_detailf(d, "display open with the display group unserved: end reason %lu, exit %ld "
                        "(AAR_E_NOSYS is due)", (unsigned long)out.end_reason,
                     (long)out.exit_code);
        pass_fail(idx, 0, d);
    } else {
        pass_fail(idx, 1, "the display open of an unserved group completed with AAR_E_NOSYS");
    }
}

/* C-CLIENT.31: files, directory lists, the console, the log and messages. */
static void t_serve_all(int idx)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];
    char path[256];
    char text[8];
    BPTR fh;
    LONG got = 0;
    int console;

    client_run(CONF_LIFE_V_SERVE_ALL, 0, &out);
    console = run.cap.buf && strstr((const char *)run.cap.buf, "serve-all console") != NULL;
    conf_run_free(&run);
    if (!out.ran) {
        conf_detailf(d, "conf_life did not run: %.80s", out.why);
        pass_fail(idx, 0, d);
        return;
    }
    if (out.end_reason != AAR_END_EXIT || out.exit_code != 0) {
        conf_detailf(d, "serve-all failed at step %ld (end reason %lu)", (long)out.exit_code,
                     (unsigned long)out.end_reason);
        pass_fail(idx, 0, d);
        return;
    }
    memset(text, 0, sizeof text);
    snprintf(path, sizeof path, "%s", conf_g.root);
    if (AddPart((STRPTR)path, (CONST_STRPTR)"conf-life-serve.tmp", sizeof path) &&
        (fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) != 0) {
        got = Read(fh, text, 5);
        Close(fh);
    }
    if (!console) {
        pass_fail(idx, 0, "the console text did not arrive");
    } else if (got != 5 || memcmp(text, "hello", 5) != 0) {
        pass_fail(idx, 0, "the file the app wrote is not under ROOT");
    } else {
        pass_fail(idx, 1, "file, list, console, log and message requests served (display, audio and input: their areas)");
    }
}

static const struct conf_r_test tests[] = {
    { "C-CLIENT.1", t_lib_version },
    { "C-CLIENT.2", t_query_size },
    { "C-CLIENT.3", t_api_version },
    { "C-CLIENT.4", t_profiles },
    { "C-CLIENT.5", t_groups },
    { "C-CLIENT.6", t_limits },
    { "C-CLIENT.7", t_granules },
    { "C-CLIENT.8", t_queue_depth },
    { "C-CLIENT.9", t_probe },
    { "C-CLIENT.10", t_probe_bad },
    { "C-CLIENT.11", t_open },
    { "C-CLIENT.12", t_tags },
    { "C-CLIENT.13", t_tags_late },
    { "C-CLIENT.14", t_weight },
    { "C-CLIENT.15", t_load_memory },
    { "C-CLIENT.16", t_start_args },
    { "C-CLIENT.17", t_status },
    { "C-CLIENT.18", t_signal_mask },
    { "C-CLIENT.19", t_signal_events },
    { "C-CLIENT.20", t_wake },
    { "C-CLIENT.21", t_events },
    { "C-CLIENT.22", t_events },
    { "C-CLIENT.23", t_signal_bit },
    { "C-CLIENT.24", t_owner },
    { "C-CLIENT.25", t_many_handles },
    { "C-CLIENT.26", t_null_handle },
    { "C-CLIENT.27", t_ext_unknown },
    { "C-CLIENT.28", t_ext_zz9000 },
    { "C-CLIENT.29", t_token },
    { "C-CLIENT.30", t_unserved_group },
    { "C-CLIENT.31", t_serve_all },
};

const struct conf_area conf_area_client = {
    .name = "C-CLIENT",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
};
