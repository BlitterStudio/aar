/*
 * aar-conf: the conformance runner of the AAR standard (spec section 11,
 * conformance/README.md). An AmigaOS m68k program that uses only aar.library
 * and the serving loop (libaarserve.a): no platform extension, so any
 * platform's library can run it.
 *
 *   aar-conf [DIR <payload directory>] [PROFILE a32|a64] [TO <file>]
 *            [ONLY <id prefix>] [ROOT <directory>] [INTERACTIVE]
 *
 * It prints, to the Shell and to TO,
 *
 *   AAR-CONF-SUITE <version> <profile>
 *   AAR-CONF-INFO key=value            the platform's AARInfo
 *   AAR-CONF <id> PASS|FAIL|SKIP <detail>   one per test
 *   AAR-CONF-SUMMARY pass=N fail=N skip=N verdict=CONFORMS|DOES-NOT-CONFORM
 *
 * The tests are the rows of TESTS.md (conf_catalogue.h is generated from it).
 * Payload-judged tests run in the payloads <DIR>/<profile>/conf_<area>.elf: each
 * is loaded and started like any app, served by aar_serve with a console that
 * the runner captures, and its AAR-CONF lines are relayed. An id that a payload
 * should have reported and did not is FAIL. Runner-judged tests use the library
 * as a client. Operator tests are SKIP manual unless INTERACTIVE. The verdict is
 * the one of spec 11.1.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/aar.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libraries/aar_serve.h>

#include "conf_catalogue.h"
#include "../payloads/conf_proto.h"

/* The program's own libraries; aar_serve uses them through these. */
struct Library *AARBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static const char version[] __attribute__((used)) =
    "$VER: aar-conf " CONF_SUITE_VERSION " (22.11.2026)\r\n";

#define DETAIL_MAX 112
#define CAPTURE_MAX (256UL * 1024UL)
#define PAYLOAD_TIMEOUT_S 180UL
#define STOP_PATIENCE_S 10UL
#define DEFAULT_ROOT "T:aar-conf"

/* ---- Results ------------------------------------------------------------------ */

enum { R_NONE = 0, R_PASS, R_FAIL, R_SKIP };

struct result {
    UBYTE status;
    char detail[DETAIL_MAX];
};

static struct result results[CONF_TEST_COUNT];
static UBYTE selected[CONF_TEST_COUNT];

static struct {
    AARInfo info;
    LONG query_rc;
    ULONG profile_value;       /* AAR_PROFILE_A32 or AAR_PROFILE_A64 */
    ULONG profile_bit;         /* the bit of conf_test.profiles */
    const char *profile_name;
    const char *dir;
    const char *root;
    const char *only;
    int interactive;
    int aborted;
    ULONG suite_notes;
} G;

static BPTR out_fh;
static BPTR to_fh;

static void put_text(const char *text)
{
    LONG len = (LONG)strlen(text);

    if (out_fh)
        (void)Write(out_fh, (APTR)text, len);
    if (to_fh)
        (void)Write(to_fh, (APTR)text, len);
}

static void emitf(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void emitf(const char *format, ...)
{
    char line[512];
    va_list ap;
    size_t n;

    va_start(ap, format);
    (void)vsnprintf(line, sizeof line - 2, format, ap);
    va_end(ap);
    n = strlen(line);
    line[n] = '\n';
    line[n + 1] = '\0';
    put_text(line);
}

static void detailf(char *dst, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void detailf(char *dst, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    (void)vsnprintf(dst, DETAIL_MAX, format, ap);
    va_end(ap);
}

static int find_test(const char *id)
{
    int i;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (strcmp(conf_tests[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* The first FAIL stays; a PASS never replaces a FAIL or a PASS. */
static void set_result(int idx, int status, const char *detail)
{
    struct result *r = &results[idx];

    if (r->status == R_FAIL)
        return;
    if (r->status != R_NONE && status != R_FAIL)
        return;
    r->status = (UBYTE)status;
    snprintf(r->detail, DETAIL_MAX, "%s", detail);
}

static void record(const char *id, int ok, const char *detail)
{
    int idx = find_test(id);

    if (idx < 0) {
        G.suite_notes++;
        emitf("# suite error: %s is not in the catalogue", id);
        return;
    }
    set_result(idx, ok ? R_PASS : R_FAIL, detail);
}

static int group_declared(unsigned short group)
{
    if (group == CONF_GROUP_CORE)
        return 1;
    if (group == CONF_GROUP_ZZ9000)
        return G.info.platform_id == AAR_PLATFORM_ZZ9000;
    return (G.info.groups & group) != 0;
}

static int has_prefix_nocase(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;

        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
    }
    return 1;
}

static ULONG now_seconds(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL +
           (ULONG)ds.ds_Tick / 50UL;
}

static int is_pow2(ULONG v)
{
    return v != 0 && (v & (v - 1UL)) == 0;
}

/* ---- C-CLIENT: runner-judged tests on AARInfo ---------------------------------- */

static void t_lib_version(int idx)
{
    char d[DETAIL_MAX];
    int ok = AARBase->lib_Version == AAR_LIB_VERSION &&
             AAR_LIB_VERSION == AAR_API_MAJOR;

    detailf(d, "aar.library %u.%u, API major %u", (unsigned)AARBase->lib_Version,
            (unsigned)AARBase->lib_Revision, (unsigned)AAR_API_MAJOR);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

/* AARQuery with a small size must change only that many bytes; with a size
 * larger than the structure it must write no more than the structure. */
static void t_query_size(int idx)
{
    ULONG buf[40];
    UBYTE *b = (UBYTE *)buf;
    LONG rc;
    int i;
    char d[DETAIL_MAX];

    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 16;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        detailf(d, "AARQuery with size 16 returned %ld", (long)rc);
        set_result(idx, R_FAIL, d);
        return;
    }
    for (i = 16; i < 160; i++) {
        if (b[i] != 0xA5) {
            detailf(d, "size 16 changed byte %d", i);
            set_result(idx, R_FAIL, d);
            return;
        }
    }
    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 160;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        detailf(d, "AARQuery with size 160 returned %ld", (long)rc);
        set_result(idx, R_FAIL, d);
        return;
    }
    for (i = (int)sizeof(AARInfo); i < 160; i++) {
        if (b[i] != 0xA5) {
            detailf(d, "size 160 wrote beyond the structure, byte %d", i);
            set_result(idx, R_FAIL, d);
            return;
        }
    }
    if (G.info.reserved0 != 0 || G.info.reserved1[0] != 0 ||
        G.info.reserved1[1] != 0 || G.info.reserved1[2] != 0 ||
        G.info.reserved1[3] != 0) {
        set_result(idx, R_FAIL, "reserved words of AARInfo are not 0");
        return;
    }
    set_result(idx, R_PASS, "size 16 and size 160 respected, reserved words 0");
}

static void t_api_version(int idx)
{
    char d[DETAIL_MAX];
    int ok = G.info.api_major == AAR_API_MAJOR &&
             G.info.api_minor >= AAR_API_MINOR;

    detailf(d, "platform API %lu.%lu, suite %u.%u", (unsigned long)G.info.api_major,
            (unsigned long)G.info.api_minor, (unsigned)AAR_API_MAJOR,
            (unsigned)AAR_API_MINOR);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

static void t_profiles(int idx)
{
    char d[DETAIL_MAX];
    ULONG p = G.info.profiles;
    int ok = p != 0 && (p & ~(AAR_PROFILEF_A32 | AAR_PROFILEF_A64)) == 0 &&
             (p & (1UL << (G.profile_value - 1UL))) != 0;

    detailf(d, "profiles 0x%lx, testing %s", (unsigned long)p, G.profile_name);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

static void t_groups(int idx)
{
    char d[DETAIL_MAX];
    ULONG g = G.info.groups;
    int ok = (g & ~(ULONG)(AAR_GROUPS_DEFINED | AAR_GROUPS_EXPERIMENTAL)) == 0;

    detailf(d, "groups 0x%lx", (unsigned long)g);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

static void t_limits(int idx)
{
    char d[DETAIL_MAX];
    int ok = G.info.max_image_bytes >= 16UL * 1024UL * 1024UL &&
             G.info.max_args >= 16UL && G.info.max_arg_bytes >= 128UL;

    detailf(d, "max image %lu, args %lu of %lu bytes",
            (unsigned long)G.info.max_image_bytes, (unsigned long)G.info.max_args,
            (unsigned long)G.info.max_arg_bytes);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

static void t_granules(int idx)
{
    char d[DETAIL_MAX];
    int ok = is_pow2(G.info.shared_granule) && G.info.shared_granule >= 64UL &&
             G.info.msg_capacity >= 32UL && G.info.stall_limit_ms >= 10000UL &&
             G.info.shared_default_mib == AAR_SHARED_DEFAULT_MIB &&
             G.info.shared_max_mib >= G.info.shared_default_mib;

    detailf(d, "granule %lu, msg %lu, stall %lu ms, shared %lu of %lu MiB",
            (unsigned long)G.info.shared_granule, (unsigned long)G.info.msg_capacity,
            (unsigned long)G.info.stall_limit_ms,
            (unsigned long)G.info.shared_default_mib,
            (unsigned long)G.info.shared_max_mib);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

static void t_queue_depth(int idx)
{
    char d[DETAIL_MAX];
    ULONG depth = G.info.queue_depth;
    int ok = is_pow2(depth) && depth >= 64UL && depth <= 4096UL &&
             G.info.msg_capacity == depth - 1UL;

    detailf(d, "queue depth %lu, msg capacity %lu", (unsigned long)depth,
            (unsigned long)G.info.msg_capacity);
    set_result(idx, ok ? R_PASS : R_FAIL, d);
}

struct runner_test {
    const char *id;
    void (*fn)(int idx);
};

static const struct runner_test runner_tests[] = {
    { "C-CLIENT.1", t_lib_version },
    { "C-CLIENT.2", t_query_size },
    { "C-CLIENT.3", t_api_version },
    { "C-CLIENT.4", t_profiles },
    { "C-CLIENT.5", t_groups },
    { "C-CLIENT.6", t_limits },
    { "C-CLIENT.7", t_granules },
    { "C-CLIENT.8", t_queue_depth },
};

/* ---- Running a payload ------------------------------------------------------------- */

/* What the runner knows about the payload run, for the C-LIFE tests that every
 * run feeds. */
static const char *const life_ids[] = { "C-LIFE.1", "C-LIFE.2", "C-LIFE.3" };

static void life_note(int which, int ok, const char *detail)
{
    record(life_ids[which], ok, detail);
}

struct capture {
    UBYTE *buf;
    ULONG len;
    int over;
    int write_pending;
    LONG write_len;
};

struct run {
    aar_serve *s;
    struct capture cap;
    char diag[160];
    ULONG messages;
    ULONG logs;
    ULONG stops;
};

static void hook_status(APTR user, const char *text)
{
    struct run *r = (struct run *)user;
    size_t have = strlen(r->diag);

    if (have + 2 < sizeof r->diag) {
        if (have)
            r->diag[have++] = ' ';
        snprintf(r->diag + have, sizeof r->diag - have, "%s", text);
    }
}

static void hook_log(APTR user, const void *data, ULONG len)
{
    struct run *r = (struct run *)user;

    (void)data;
    (void)len;
    r->logs++;
}

/* The peer: a payload asks for a stop by sending CONF-STOP-NOW. */
static void hook_message(APTR user, const AARMessage *msg)
{
    struct run *r = (struct run *)user;

    r->messages++;
    if (memcmp(msg->bytes, CONF_MSG_STOP, sizeof CONF_MSG_STOP - 1) == 0) {
        r->stops++;
        aar_serve_stop(r->s);
    }
}

static int con_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    struct run *r = (struct run *)ctx;
    ULONG room = CAPTURE_MAX - r->cap.len;
    ULONG take = bytes < room ? (ULONG)bytes : room;

    memcpy(r->cap.buf + r->cap.len, data, take);
    r->cap.len += take;
    if (take < bytes)
        r->cap.over = 1;
    r->cap.write_len = (LONG)bytes;
    r->cap.write_pending = 1;
    return 1;
}

/* No console input: the payload sees the end of input. */
static int con_read_start(void *ctx, uint32_t max)
{
    (void)ctx;
    (void)max;
    return 0;
}

static int con_live(void *ctx)
{
    (void)ctx;
    return 1;
}

static struct aar_serve_console_ops con_ops;
static aar_serve_hooks serve_hooks;

/* One line of the capture that starts with "AAR-CONF ": id, verdict, detail. */
static void relay_line(const char *area, const char *line, int *relayed)
{
    char id[32];
    char verdict[8];
    const char *p = line + 9;
    size_t n;
    int idx;
    int status;

    n = strcspn(p, " ");
    if (n == 0 || n >= sizeof id) {
        emitf("# %s: malformed result line: %.60s", area, line);
        return;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    p += n;
    while (*p == ' ')
        p++;
    n = strcspn(p, " ");
    if (n == 0 || n >= sizeof verdict) {
        emitf("# %s: malformed result line: %.60s", area, line);
        return;
    }
    memcpy(verdict, p, n);
    verdict[n] = '\0';
    p += n;
    while (*p == ' ')
        p++;

    idx = find_test(id);
    if (idx < 0 || conf_tests[idx].judge != CONF_J_PAYLOAD ||
        strcmp(conf_areas[conf_tests[idx].area], area) != 0) {
        emitf("# %s: reported %s, which is not one of its tests", area, id);
        G.suite_notes++;
        return;
    }
    (*relayed)++;
    if (strcmp(verdict, "PASS") == 0) {
        status = R_PASS;
    } else if (strcmp(verdict, "FAIL") == 0) {
        status = R_FAIL;
    } else if (strcmp(verdict, "SKIP") == 0) {
        /* 11.1: a SKIP is for an undeclared group or a SHOULD test, with a
         * reason. The runner has already skipped the undeclared groups. */
        if (conf_tests[idx].level == CONF_MUST) {
            char d[DETAIL_MAX];

            detailf(d, "SKIP of a MUST test: %.80s", p);
            set_result(idx, R_FAIL, d);
            return;
        }
        if (*p == '\0') {
            set_result(idx, R_FAIL, "SKIP without a reason");
            return;
        }
        status = R_SKIP;
    } else {
        char d[DETAIL_MAX];

        detailf(d, "unknown verdict %.20s", verdict);
        set_result(idx, R_FAIL, d);
        return;
    }
    set_result(idx, status, *p ? p : "ok");
}

static ULONG number_after(const char *line, const char *key)
{
    const char *p = strstr(line, key);

    return p ? (ULONG)strtoul(p + strlen(key), NULL, 10) : 0UL;
}

struct parsed {
    int have_end;
    ULONG pass, fail, skip;
    int relayed;
};

static void parse_capture(const char *area, struct capture *cap, struct parsed *pr)
{
    char *p = (char *)cap->buf;
    char *end = p + cap->len;
    ULONG other = 0;

    memset(pr, 0, sizeof *pr);
    cap->buf[cap->len] = '\0';
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *next;

        if (nl) {
            *nl = '\0';
            next = nl + 1;
        } else {
            next = end;
        }
        if (nl && nl > p && nl[-1] == '\r')
            nl[-1] = '\0';
        if (strncmp(p, "AAR-CONF-END ", 13) == 0) {
            pr->have_end = 1;
            pr->pass = number_after(p, "pass=");
            pr->fail = number_after(p, "fail=");
            pr->skip = number_after(p, "skip=");
        } else if (strncmp(p, "AAR-CONF ", 9) == 0) {
            relay_line(area, p, &pr->relayed);
        } else if (*p && other < 12) {
            emitf("# %s: %.100s", area, p);
            other++;
        }
        p = next;
    }
}

struct payload_outcome {
    int ran;                /* the payload was started and served to its end */
    int timed_out;
    char why[DETAIL_MAX];   /* why it did not run, when it did not */
    LONG exit_code;
    ULONG end_reason;
    ULONG state;
    ULONG fault_class;
    LONG status_rc;
    ULONG serve_flags;
};

static BPTR lock_root(void)
{
    BPTR lock = Lock((STRPTR)G.root, SHARED_LOCK);

    if (!lock) {
        lock = CreateDir((STRPTR)G.root);
        if (lock) {
            UnLock(lock);
            lock = Lock((STRPTR)G.root, SHARED_LOCK);
        }
    }
    return lock;
}

/* The arguments every payload gets (conf.h). */
static char info_args[7][40];

static int build_args(const char *name, const char **argv)
{
    int n = 0;

    argv[n++] = name;
    argv[n++] = "alpha";
    argv[n++] = "beta gamma";
    argv[n++] = "conf:peer=1";
    snprintf(info_args[0], sizeof info_args[0], "info:groups=%lu",
             (unsigned long)G.info.groups);
    snprintf(info_args[1], sizeof info_args[1], "info:platform_id=%lu",
             (unsigned long)G.info.platform_id);
    snprintf(info_args[2], sizeof info_args[2], "info:msg_capacity=%lu",
             (unsigned long)G.info.msg_capacity);
    snprintf(info_args[3], sizeof info_args[3], "info:stall_limit_ms=%lu",
             (unsigned long)G.info.stall_limit_ms);
    snprintf(info_args[4], sizeof info_args[4], "info:stop_grace_ms=%lu",
             (unsigned long)G.info.stop_grace_ms);
    snprintf(info_args[5], sizeof info_args[5], "info:max_args=%lu",
             (unsigned long)G.info.max_args);
    snprintf(info_args[6], sizeof info_args[6], "info:max_arg_bytes=%lu",
             (unsigned long)G.info.max_arg_bytes);
    for (int i = 0; i < 7 && (ULONG)n < G.info.max_args; i++)
        argv[n++] = info_args[i];
    return n;
}

/* Open, load, start and serve one payload; capture its console. */
static void run_payload(const char *area, struct run *run,
                        struct payload_outcome *out)
{
    char file[40];
    char path[256];
    const char *argv[16];
    int argc;
    AARHandle *h = NULL;
    aar_serve *s = NULL;
    aar_serve_config cfg;
    struct aar_serve_result res;
    AARStatusInfo st;
    ULONG reason = 0;
    ULONG start;
    ULONG stop_at = 0;
    BPTR root;
    LONG rc;
    size_t i;

    memset(out, 0, sizeof *out);
    snprintf(file, sizeof file, "conf_%s.elf", area + 2);
    for (i = 0; file[i]; i++) {
        if (file[i] >= 'A' && file[i] <= 'Z')
            file[i] = (char)(file[i] - 'A' + 'a');
    }
    snprintf(path, sizeof path, "%s", G.dir);
    if (!AddPart((STRPTR)path, (CONST_STRPTR)G.profile_name, sizeof path) ||
        !AddPart((STRPTR)path, (CONST_STRPTR)file, sizeof path)) {
        detailf(out->why, "payload path too long");
        return;
    }
    argc = build_args(file, argv);

    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        detailf(out->why, "AAROpen returned %ld (reason %lu)", (long)rc,
                (unsigned long)reason);
        return;
    }
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    rc = AARStatus(h, &st);
    if (rc == AAR_OK && st.state == AAR_STATE_CLAIMED && st.generation != 0) {
        char d[DETAIL_MAX];

        detailf(d, "claimed, generation %lu", (unsigned long)st.generation);
        life_note(0, 1, d);
    } else {
        char d[DETAIL_MAX];

        detailf(d, "after open: status %ld, state %lu, generation %lu", (long)rc,
                (unsigned long)st.state, (unsigned long)st.generation);
        life_note(0, 0, d);
    }
    rc = AARLoadFile(h, path);
    if (rc != AAR_OK) {
        detailf(out->why, "load of %.40s refused: %ld", file, (long)rc);
        goto close_handle;
    }
    rc = AARStart(h, (const char *const *)argv, (ULONG)argc);
    if (rc != AAR_OK) {
        detailf(out->why, "start of %.40s refused: %ld", file, (long)rc);
        goto close_handle;
    }

    root = lock_root();
    aar_serve_config_default(&cfg);
    cfg.flags = AAR_SERVEF_NO_INPUT_WINDOW;
    cfg.root = root;
    cfg.screen_title = (CONST_STRPTR)"aar-conf";
    memset(&con_ops, 0, sizeof con_ops);
    con_ops.ctx = run;
    con_ops.write_start = con_write_start;
    con_ops.read_start = con_read_start;
    con_ops.live = con_live;
    memset(&serve_hooks, 0, sizeof serve_hooks);
    serve_hooks.size = sizeof serve_hooks;
    serve_hooks.status = hook_status;
    serve_hooks.log = hook_log;
    serve_hooks.message = hook_message;
    serve_hooks.console = &con_ops;
    cfg.hooks = &serve_hooks;
    cfg.user = run;
    rc = aar_serve_create(h, &cfg, &s);
    if (rc != AAR_OK) {
        detailf(out->why, "aar_serve_create returned %ld", (long)rc);
        (void)AARStop(h);
        if (root)
            UnLock(root);
        goto close_handle;
    }
    run->s = s;
    start = now_seconds();
    for (;;) {
        ULONG got = Wait(aar_serve_sigmask(s));
        ULONG now;

        rc = aar_serve_step(s, got);
        while (run->cap.write_pending) {
            run->cap.write_pending = 0;
            aar_serve_write_done(s, run->cap.write_len);
        }
        if (rc != AAR_SERVE_CONTINUE)
            break;
        now = now_seconds();
        if (!stop_at && now - start > PAYLOAD_TIMEOUT_S) {
            aar_serve_stop(s);
            stop_at = now;
            out->timed_out = 1;
        }
        if (stop_at && now - stop_at > STOP_PATIENCE_S) {
            detailf(out->why, "payload did not end %lu s after a stop",
                    (unsigned long)STOP_PATIENCE_S);
            break;
        }
    }
    out->ran = rc != AAR_SERVE_CONTINUE;
    memset(&res, 0, sizeof res);
    res.size = sizeof res;
    out->status_rc = aar_serve_result(s, &res);
    out->exit_code = res.exit_code;
    out->end_reason = res.end_reason;
    out->state = res.state;
    out->fault_class = res.status.fault_class;
    out->serve_flags = res.flags;
    if (res.flags & AAR_SERVE_R_USER_STOP)
        G.aborted = 1;
    if (out->ran) {
        /* The status stays readable until AARClose. */
        memset(&st, 0, sizeof st);
        st.size = sizeof st;
        rc = AARStatus(h, &st);
        if (rc == AAR_OK) {
            out->state = st.state;
            out->end_reason = st.end_reason;
            out->exit_code = st.exit_code;
            out->fault_class = st.fault_class;
        } else {
            out->status_rc = rc;
        }
    }
    aar_serve_destroy(s);
    if (root)
        UnLock(root);
close_handle:
    (void)AARClose(h);
}

/* Run the payload of one area and fill the results of its expected ids. */
static void run_area(int area)
{
    const char *name = conf_areas[area];
    struct run run;
    struct payload_outcome out;
    struct parsed pr;
    char why[DETAIL_MAX];
    int i;
    int expected = 0;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == R_NONE &&
            conf_tests[i].judge == CONF_J_PAYLOAD && conf_tests[i].area == area)
            expected++;
    }
    if (!expected)
        return;
    memset(&run, 0, sizeof run);
    run.cap.buf = AllocVec(CAPTURE_MAX + 1UL, MEMF_ANY | MEMF_CLEAR);
    if (!run.cap.buf) {
        for (i = 0; i < CONF_TEST_COUNT; i++) {
            if (selected[i] && conf_tests[i].area == area &&
                conf_tests[i].judge == CONF_J_PAYLOAD)
                set_result(i, R_FAIL, "no memory for the console capture");
        }
        return;
    }
    emitf("# running conf_%s", name + 2);
    run_payload(name, &run, &out);
    parse_capture(name, &run.cap, &pr);
    if (run.diag[0])
        emitf("# %s: client says: %.100s", name, run.diag);
    if (run.cap.over)
        emitf("# %s: console capture overflowed", name);

    /* The C-LIFE tests every run feeds. */
    if (out.ran) {
        char d[DETAIL_MAX];

        if (out.status_rc == AAR_OK && out.state == AAR_STATE_IDLE &&
            out.end_reason == AAR_END_EXIT && out.fault_class == AAR_FAULT_NONE) {
            detailf(d, "%s ended: idle, exit, no fault", name);
            life_note(1, 1, d);
        } else {
            detailf(d, "%s ended: status %ld, state %lu, end reason %lu, fault %lu",
                    name, (long)out.status_rc, (unsigned long)out.state,
                    (unsigned long)out.end_reason, (unsigned long)out.fault_class);
            life_note(1, 0, d);
        }
        if (!pr.have_end) {
            detailf(d, "%s printed no end line", name);
            life_note(2, 0, d);
        } else if (out.exit_code == (pr.fail ? 10L : 0L)) {
            detailf(d, "%s returned %ld", name, (long)out.exit_code);
            life_note(2, 1, d);
        } else {
            detailf(d, "%s returned %ld but reported %lu failures", name,
                    (long)out.exit_code, (unsigned long)pr.fail);
            life_note(2, 0, d);
        }
    } else {
        char d[DETAIL_MAX];

        detailf(d, "%s did not run: %.80s", name, out.why);
        life_note(1, 0, d);
        life_note(2, 0, d);
    }
    if (pr.have_end && pr.pass + pr.fail + pr.skip != (ULONG)pr.relayed) {
        emitf("# %s: end line counts %lu results, %d were relayed", name,
              (unsigned long)(pr.pass + pr.fail + pr.skip), pr.relayed);
        G.suite_notes++;
    }

    /* An expected id that was not reported fails: a payload that crashed or
     * was refused cannot pass by silence. */
    if (!out.ran)
        detailf(why, "payload not run: %.90s", out.why);
    else if (out.timed_out)
        detailf(why, "payload did not finish in %lu s", (unsigned long)PAYLOAD_TIMEOUT_S);
    else if (!pr.have_end)
        detailf(why, "payload ended without its end line (exit %ld, end reason %lu)",
                (long)out.exit_code, (unsigned long)out.end_reason);
    else
        detailf(why, "payload did not report this id");
    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == R_NONE &&
            conf_tests[i].judge == CONF_J_PAYLOAD && conf_tests[i].area == area)
            set_result(i, R_FAIL, why);
    }
    FreeVec(run.cap.buf);
}

/* ---- Operator tests ------------------------------------------------------------------- */

static void run_operator(int idx)
{
    char answer[16];
    BPTR in = Input();

    if (!G.interactive) {
        set_result(idx, R_SKIP, "manual");
        return;
    }
    emitf("# [%s] %s", conf_tests[idx].id, conf_tests[idx].prompt);
    put_text("# Did it happen as described? (y/n) ");
    answer[0] = '\0';
    if (!in || !FGets(in, (STRPTR)answer, sizeof answer)) {
        set_result(idx, R_SKIP, "manual: no answer");
        return;
    }
    if (answer[0] == 'y' || answer[0] == 'Y')
        set_result(idx, R_PASS, "operator confirmed");
    else
        set_result(idx, R_FAIL, "operator said no");
}

/* ---- The run ------------------------------------------------------------------------------ */

static void dump_info(void)
{
    const AARInfo *i = &G.info;
    int k;

    emitf("AAR-CONF-SUITE %s %s", CONF_SUITE_VERSION, G.profile_name);
    emitf("AAR-CONF-INFO query=%ld", (long)G.query_rc);
    emitf("AAR-CONF-INFO library=%u.%u", (unsigned)AARBase->lib_Version,
          (unsigned)AARBase->lib_Revision);
    emitf("AAR-CONF-INFO api_major=%lu", (unsigned long)i->api_major);
    emitf("AAR-CONF-INFO api_minor=%lu", (unsigned long)i->api_minor);
    emitf("AAR-CONF-INFO platform_id=%lu", (unsigned long)i->platform_id);
    emitf("AAR-CONF-INFO profiles=0x%lx", (unsigned long)i->profiles);
    emitf("AAR-CONF-INFO groups=0x%lx", (unsigned long)i->groups);
    emitf("AAR-CONF-INFO flags=0x%lx", (unsigned long)i->flags);
    emitf("AAR-CONF-INFO max_apps=%lu", (unsigned long)i->max_apps);
    emitf("AAR-CONF-INFO max_image_bytes=%lu", (unsigned long)i->max_image_bytes);
    emitf("AAR-CONF-INFO max_args=%lu", (unsigned long)i->max_args);
    emitf("AAR-CONF-INFO max_arg_bytes=%lu", (unsigned long)i->max_arg_bytes);
    emitf("AAR-CONF-INFO shared_granule=%lu", (unsigned long)i->shared_granule);
    emitf("AAR-CONF-INFO msg_capacity=%lu", (unsigned long)i->msg_capacity);
    emitf("AAR-CONF-INFO stall_limit_ms=%lu", (unsigned long)i->stall_limit_ms);
    emitf("AAR-CONF-INFO stop_grace_ms=%lu", (unsigned long)i->stop_grace_ms);
    emitf("AAR-CONF-INFO shared_default_mib=%lu", (unsigned long)i->shared_default_mib);
    emitf("AAR-CONF-INFO shared_max_mib=%lu", (unsigned long)i->shared_max_mib);
    emitf("AAR-CONF-INFO weight_max=%lu", (unsigned long)i->weight_max);
    emitf("AAR-CONF-INFO queue_depth=%lu", (unsigned long)i->queue_depth);
    for (k = 0; k < 8; k++)
        emitf("AAR-CONF-INFO platform%d=0x%lx", k, (unsigned long)i->platform[k]);
}

static int stack_ok(void)
{
    struct Task *task = FindTask(NULL);

    return (ULONG)task->tc_SPUpper - (ULONG)task->tc_SPLower >=
           AAR_SERVE_MIN_STACK;
}

/* Decide what each test needs: not printed (profile, filter, not tested),
 * skipped by the runner (undeclared group, not implemented) or run. */
static void plan(void)
{
    int i;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        const struct conf_test *t = &conf_tests[i];

        if (t->judge == CONF_J_NONE || !(t->profiles & G.profile_bit))
            continue;
        if (G.only && !has_prefix_nocase(t->id, G.only))
            continue;
        selected[i] = 1;
        if (!group_declared(t->group))
            set_result(i, R_SKIP, "group not declared");
        else if (!t->impl)
            set_result(i, R_SKIP, "suite: not implemented in this version");
    }
}

static void run_tests(void)
{
    int i;
    size_t k;

    for (k = 0; k < sizeof runner_tests / sizeof runner_tests[0]; k++) {
        int idx = find_test(runner_tests[k].id);

        if (idx >= 0 && selected[idx] && results[idx].status == R_NONE)
            runner_tests[k].fn(idx);
    }
    for (i = 0; i < CONF_AREA_COUNT && !G.aborted; i++)
        run_area(i);
    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == R_NONE &&
            conf_tests[i].judge == CONF_J_OPERATOR)
            run_operator(i);
    }
}

static int report(void)
{
    ULONG pass = 0, fail = 0, skip = 0;
    ULONG must_fail = 0, must_skip = 0;
    int i;
    int ok;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        const struct conf_test *t = &conf_tests[i];
        const char *word;
        int status;

        if (!selected[i])
            continue;
        status = results[i].status;
        if (status == R_NONE) {
            set_result(i, R_FAIL, G.aborted ? "run aborted"
                                            : "the runner has no result for it");
            status = R_FAIL;
        }
        switch (status) {
        case R_PASS:
            pass++;
            word = "PASS";
            break;
        case R_FAIL:
            fail++;
            word = "FAIL";
            if (t->level == CONF_MUST)
                must_fail++;
            break;
        default:
            skip++;
            word = "SKIP";
            /* 11.1: only an undeclared group or a SHOULD test may be skipped. */
            if (t->level == CONF_MUST && group_declared(t->group))
                must_skip++;
            break;
        }
        emitf("AAR-CONF %s %s %s", t->id, word, results[i].detail);
    }
    ok = must_fail == 0 && must_skip == 0 && G.suite_notes == 0;
    if (G.only) {
        emitf("# filtered run (ONLY %s): %lu MUST failed, %lu MUST skipped", G.only,
              (unsigned long)must_fail, (unsigned long)must_skip);
    } else if (!ok) {
        emitf("# %lu MUST failed, %lu MUST skipped, %lu suite notes",
              (unsigned long)must_fail, (unsigned long)must_skip,
              (unsigned long)G.suite_notes);
    }
    emitf("AAR-CONF-SUMMARY pass=%lu fail=%lu skip=%lu verdict=%s",
          (unsigned long)pass, (unsigned long)fail, (unsigned long)skip,
          G.only ? "PARTIAL" : (ok ? "CONFORMS" : "DOES-NOT-CONFORM"));
    return ok || (G.only && must_fail == 0) ? RETURN_OK : RETURN_ERROR;
}

enum { A_DIR, A_PROFILE, A_TO, A_ONLY, A_ROOT, A_INTERACTIVE, A_COUNT };

int main(void)
{
    LONG args[A_COUNT];
    struct RDArgs *rda;
    int code = RETURN_FAIL;

    memset(args, 0, sizeof args);
    rda = ReadArgs((CONST_STRPTR)"DIR,PROFILE/K,TO/K,ONLY/K,ROOT/K,INTERACTIVE/S",
                   args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"aar-conf");
        return RETURN_FAIL;
    }
    G.dir = args[A_DIR] ? (const char *)args[A_DIR] : "PROGDIR:";
    G.root = args[A_ROOT] ? (const char *)args[A_ROOT] : DEFAULT_ROOT;
    G.only = args[A_ONLY] ? (const char *)args[A_ONLY] : NULL;
    G.interactive = args[A_INTERACTIVE] != 0;
    out_fh = Output();
    if (args[A_TO]) {
        to_fh = Open((CONST_STRPTR)args[A_TO], MODE_NEWFILE);
        if (!to_fh)
            PrintFault(IoErr(), (CONST_STRPTR)"aar-conf: cannot write TO");
    }
    if (!stack_ok()) {
        put_text("aar-conf: run Stack 32768 first.\n");
        goto done;
    }
    AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 36);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 36);
    if (!AARBase) {
        put_text("aar-conf: aar.library does not open.\n");
        goto done;
    }
    G.info.size = sizeof G.info;
    G.query_rc = AARQuery(&G.info);
    G.profile_value = AAR_PROFILE_A32;
    if (args[A_PROFILE]) {
        if (has_prefix_nocase((const char *)args[A_PROFILE], "a64"))
            G.profile_value = AAR_PROFILE_A64;
    } else if (!(G.info.profiles & AAR_PROFILEF_A32) &&
               (G.info.profiles & AAR_PROFILEF_A64)) {
        G.profile_value = AAR_PROFILE_A64;
    }
    G.profile_bit = G.profile_value == AAR_PROFILE_A64 ? 2UL : 1UL;
    G.profile_name = G.profile_value == AAR_PROFILE_A64 ? "a64" : "a32";

    dump_info();
    plan();
    run_tests();
    code = report();
done:
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (AARBase)
        CloseLibrary(AARBase);
    if (to_fh)
        Close(to_fh);
    FreeArgs(rda);
    return code;
}
