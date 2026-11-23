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
 * This file is the core: options, catalogue, result lines, verdict, the payload
 * run through aar_serve with a captured console, the peer message dispatch. What
 * belongs to one area lives in r_<area>.c (aar-conf.h, r_areas.h,
 * conformance/CONTRIBUTING.md).
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
#include <exec/tasks.h>
#include <exec/libraries.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF_CATALOGUE_DEFINE
#include "aar-conf.h"

/* The program's own libraries; aar_serve uses them through these. */
struct Library *AARBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static const char version[] __attribute__((used)) =
    "$VER: aar-conf " CONF_SUITE_VERSION " (22.11.2026)\r\n";

#define PAYLOAD_TIMEOUT_S 180UL
#define STOP_PATIENCE_S 10UL
#define DEFAULT_ROOT "T:aar-conf"

/* ---- The registered areas (r_areas.h: one line per area) ----------------------------- */

#define CONF_R_AREA(x) extern const struct conf_area conf_area_##x;
#include "r_areas.h"
#undef CONF_R_AREA

static const struct conf_area *const areas[] = {
#define CONF_R_AREA(x) &conf_area_##x,
#include "r_areas.h"
#undef CONF_R_AREA
};

/* ---- Results ------------------------------------------------------------------ */

struct result {
    UBYTE status;
    char detail[CONF_DETAIL_MAX];
};

static struct result results[CONF_TEST_COUNT];
static UBYTE selected[CONF_TEST_COUNT];

struct conf_globals conf_g;

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

void conf_emitf(const char *format, ...)
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

void conf_detailf(char *dst, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    (void)vsnprintf(dst, CONF_DETAIL_MAX, format, ap);
    va_end(ap);
}

int conf_find_test(const char *id)
{
    int i;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (strcmp(conf_tests[i].id, id) == 0)
            return i;
    }
    return -1;
}

int conf_selected(int idx)
{
    return selected[idx];
}

void conf_set_result(int idx, int status, const char *detail)
{
    struct result *r = &results[idx];

    if (r->status == CONF_R_FAIL)
        return;
    if (r->status != CONF_R_NONE && status != CONF_R_FAIL)
        return;
    r->status = (UBYTE)status;
    snprintf(r->detail, CONF_DETAIL_MAX, "%s", detail);
}

void conf_record(const char *id, int ok, const char *detail)
{
    int idx = conf_find_test(id);

    if (idx < 0) {
        conf_g.suite_notes++;
        conf_emitf("# suite error: %s is not in the catalogue", id);
        return;
    }
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, detail);
}

int conf_group_declared(unsigned short group)
{
    if (group == CONF_GROUP_CORE)
        return 1;
    if (group == CONF_GROUP_ZZ9000)
        return conf_g.info.platform_id == AAR_PLATFORM_ZZ9000;
    return (conf_g.info.groups & group) != 0;
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

ULONG conf_ticks(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400UL * CONF_TICKS_PER_SECOND +
           (ULONG)ds.ds_Minute * 60UL * CONF_TICKS_PER_SECOND + (ULONG)ds.ds_Tick;
}

/* ---- The hooks of a payload run --------------------------------------------------------- */

static void hook_status(APTR user, const char *text)
{
    struct conf_run *r = (struct conf_run *)user;
    size_t have = strlen(r->diag);

    if (have + 2 < sizeof r->diag) {
        if (have)
            r->diag[have++] = ' ';
        snprintf(r->diag + have, sizeof r->diag - have, "%s", text);
    }
}

static void hook_log(APTR user, const void *data, ULONG len)
{
    struct conf_run *r = (struct conf_run *)user;

    r->logs++;
    if (r->on_log)
        r->on_log(r, data, len);
}

/* The peer: keywords of the core and of every registered area. */
static void hook_message(APTR user, const AARMessage *msg)
{
    struct conf_run *r = (struct conf_run *)user;
    unsigned a;
    unsigned k;

    r->messages++;
    if (memcmp(msg->bytes, CONF_MSG_STOP, sizeof CONF_MSG_STOP - 1) == 0) {
        r->stops++;
        aar_serve_stop(r->s);
        return;
    }
    for (a = 0; a < CONF_COUNT(areas); a++) {
        for (k = 0; k < areas[a]->peer_count; k++) {
            const struct conf_peer *p = &areas[a]->peers[k];

            if (memcmp(msg->bytes, p->keyword, strlen(p->keyword)) == 0) {
                p->fn(r, msg);
                return;
            }
        }
    }
    if (r->on_message)
        r->on_message(r, msg);
}

static int con_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    struct conf_run *r = (struct conf_run *)ctx;
    ULONG room = CONF_CAPTURE_MAX - r->cap.len;
    ULONG take = bytes < room ? (ULONG)bytes : room;

    memcpy(r->cap.buf + r->cap.len, data, take);
    r->cap.len += take;
    if (take < bytes)
        r->cap.over = 1;
    r->write_len = (LONG)bytes;
    r->write_due = conf_ticks() + r->con.write_hold_ticks;
    r->write_pending = 1;
    return 1;
}

/* Scripted input: bytes while the script lasts, then the end of input (0: it
 * cannot start), or with input_block a read that never ends. */
static int con_read_start(void *ctx, uint32_t max)
{
    struct conf_run *r = (struct conf_run *)ctx;

    if (r->input_pos >= r->con.input_len && !r->con.input_block)
        return 0;
    r->read_max = (ULONG)max;
    r->read_due = conf_ticks() + (r->input_pos == 0 ? r->con.input_hold_ticks : 0UL);
    r->read_pending = 1;
    return 1;
}

static int con_live(void *ctx)
{
    (void)ctx;
    return 1;
}

static struct aar_serve_console_ops con_ops;
static aar_serve_hooks serve_hooks;

/* Completions the console owes aar_serve, outside its calls. */
static void console_pump(struct conf_run *r, ULONG now)
{
    while (r->write_pending && (LONG)(now - r->write_due) >= 0) {
        r->write_pending = 0;
        aar_serve_write_done(r->s, r->write_len);
    }
    if (r->read_pending && r->input_pos < r->con.input_len &&
        (LONG)(now - r->read_due) >= 0) {
        ULONG left = r->con.input_len - r->input_pos;
        ULONG take = left < r->read_max ? left : r->read_max;

        r->read_pending = 0;
        aar_serve_read_done(r->s, (LONG)take, r->con.input + r->input_pos);
        r->input_pos += take;
    }
}

void conf_run_init(struct conf_run *run, const char *area)
{
    memset(run, 0, sizeof *run);
    run->area = area;
}

void conf_run_free(struct conf_run *run)
{
    if (run->cap.buf)
        FreeVec(run->cap.buf);
    run->cap.buf = NULL;
}

void conf_run_stop(struct conf_run *run)
{
    if (run->s)
        aar_serve_stop(run->s);
}

/* ---- Relaying result lines ---------------------------------------------------------------- */

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
        conf_emitf("# %s: malformed result line: %.60s", area, line);
        return;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    p += n;
    while (*p == ' ')
        p++;
    n = strcspn(p, " ");
    if (n == 0 || n >= sizeof verdict) {
        conf_emitf("# %s: malformed result line: %.60s", area, line);
        return;
    }
    memcpy(verdict, p, n);
    verdict[n] = '\0';
    p += n;
    while (*p == ' ')
        p++;

    idx = conf_find_test(id);
    if (idx < 0 || conf_tests[idx].judge != CONF_J_PAYLOAD ||
        strcmp(conf_areas[conf_tests[idx].area], area) != 0) {
        conf_emitf("# %s: reported %s, which is not one of its tests", area, id);
        conf_g.suite_notes++;
        return;
    }
    (*relayed)++;
    if (strcmp(verdict, "PASS") == 0) {
        status = CONF_R_PASS;
    } else if (strcmp(verdict, "FAIL") == 0) {
        status = CONF_R_FAIL;
    } else if (strcmp(verdict, "SKIP") == 0) {
        /* 11.1: a SKIP is for an undeclared group or a SHOULD test, with a
         * reason. The runner has already skipped the undeclared groups. */
        if (conf_tests[idx].level == CONF_MUST) {
            char d[CONF_DETAIL_MAX];

            conf_detailf(d, "SKIP of a MUST test: %.80s", p);
            conf_set_result(idx, CONF_R_FAIL, d);
            return;
        }
        if (*p == '\0') {
            conf_set_result(idx, CONF_R_FAIL, "SKIP without a reason");
            return;
        }
        status = CONF_R_SKIP;
    } else {
        char d[CONF_DETAIL_MAX];

        conf_detailf(d, "unknown verdict %.20s", verdict);
        conf_set_result(idx, CONF_R_FAIL, d);
        return;
    }
    conf_set_result(idx, status, *p ? p : "ok");
}

static ULONG number_after(const char *line, const char *key)
{
    const char *p = strstr(line, key);

    return p ? (ULONG)strtoul(p + strlen(key), NULL, 10) : 0UL;
}

void conf_parse_capture(struct conf_run *run, struct conf_parsed *pr)
{
    char *p = (char *)run->cap.buf;
    char *end = p + run->cap.len;
    ULONG other = 0;

    memset(pr, 0, sizeof *pr);
    if (!p)
        return;
    run->cap.buf[run->cap.len] = '\0';
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
            if (run->norelay)
                pr->relayed++;
            else
                relay_line(run->area, p, &pr->relayed);
        } else if (*p && other < 12) {
            conf_emitf("# %s: %.100s", run->area, p);
            other++;
        }
        p = next;
    }
}

/* ---- Running a payload ---------------------------------------------------------------------- */

static BPTR lock_root(void)
{
    BPTR lock = Lock((STRPTR)conf_g.root, SHARED_LOCK);

    if (!lock) {
        lock = CreateDir((STRPTR)conf_g.root);
        if (lock) {
            UnLock(lock);
            lock = Lock((STRPTR)conf_g.root, SHARED_LOCK);
        }
    }
    return lock;
}

/* The arguments every payload gets (payloads/conf.h). */
static char info_args[7][40];
static char variant_arg[48];

static ULONG build_args(const struct conf_run *run, const char *name,
                        const char **argv)
{
    ULONG n = 0;
    ULONG i;
    ULONG max = conf_g.info.max_args < 16UL ? conf_g.info.max_args : 16UL;

    if (run->argv_override) {
        for (i = 0; i < run->argc_override && i < 16UL; i++)
            argv[n++] = run->argv_override[i];
        return n;
    }
    argv[n++] = name;
    argv[n++] = "alpha";
    argv[n++] = "beta gamma";
    argv[n++] = "conf:peer=1";
    snprintf(info_args[0], sizeof info_args[0], "info:groups=%lu",
             (unsigned long)conf_g.info.groups);
    snprintf(info_args[1], sizeof info_args[1], "info:platform_id=%lu",
             (unsigned long)conf_g.info.platform_id);
    snprintf(info_args[2], sizeof info_args[2], "info:msg_capacity=%lu",
             (unsigned long)conf_g.info.msg_capacity);
    snprintf(info_args[3], sizeof info_args[3], "info:stall_limit_ms=%lu",
             (unsigned long)conf_g.info.stall_limit_ms);
    snprintf(info_args[4], sizeof info_args[4], "info:stop_grace_ms=%lu",
             (unsigned long)conf_g.info.stop_grace_ms);
    snprintf(info_args[5], sizeof info_args[5], "info:max_args=%lu",
             (unsigned long)conf_g.info.max_args);
    snprintf(info_args[6], sizeof info_args[6], "info:max_arg_bytes=%lu",
             (unsigned long)conf_g.info.max_arg_bytes);
    for (i = 0; i < 7 && n < max; i++)
        argv[n++] = info_args[i];
    if (run->variant && n < max) {
        snprintf(variant_arg, sizeof variant_arg, "conf:variant=%s", run->variant);
        argv[n++] = variant_arg;
    }
    for (i = 0; i < run->extra_count && n < max; i++)
        argv[n++] = run->extra_args[i];
    return n;
}

void conf_run_payload(struct conf_run *run, struct conf_outcome *out)
{
    char file[40];
    char path[256];
    const char *argv[16];
    ULONG argc;
    AARHandle *h = NULL;
    aar_serve *s = NULL;
    aar_serve_config cfg;
    struct aar_serve_result res;
    AARStatusInfo st;
    ULONG reason = 0;
    ULONG start;
    ULONG stop_at = 0;
    ULONG timeout = run->timeout_s ? run->timeout_s : PAYLOAD_TIMEOUT_S;
    BPTR root = 0;
    LONG rc;
    size_t i;
    unsigned a;

    memset(out, 0, sizeof *out);
    run->s = NULL;
    run->h = NULL;
    if (!run->cap.buf) {
        run->cap.buf = AllocVec(CONF_CAPTURE_MAX + 1UL, MEMF_ANY | MEMF_CLEAR);
        if (!run->cap.buf) {
            conf_detailf(out->why, "no memory for the console capture");
            return;
        }
    }
    snprintf(file, sizeof file, "conf_%s.elf", run->area + 2);
    for (i = 0; file[i]; i++) {
        if (file[i] >= 'A' && file[i] <= 'Z')
            file[i] = (char)(file[i] - 'A' + 'a');
    }
    snprintf(path, sizeof path, "%s", conf_g.dir);
    if (!AddPart((STRPTR)path, (CONST_STRPTR)conf_g.profile_name, sizeof path) ||
        !AddPart((STRPTR)path, (CONST_STRPTR)file, sizeof path)) {
        conf_detailf(out->why, "payload path too long");
        return;
    }
    argc = build_args(run, file, argv);

    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        conf_detailf(out->why, "AAROpen returned %ld (reason %lu)", (long)rc,
                     (unsigned long)reason);
        return;
    }
    run->h = h;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    rc = AARStatus(h, &st);
    for (a = 0; a < CONF_COUNT(areas); a++) {
        if (areas[a]->on_open)
            areas[a]->on_open(run, rc, &st);
    }
    if (run->tags) {
        rc = AARSetTagList(h, (struct TagItem *)run->tags);
        if (rc != AAR_OK) {
            conf_detailf(out->why, "AARSetTagList returned %ld", (long)rc);
            goto close_handle;
        }
    }
    rc = AARLoadFile(h, path);
    if (rc != AAR_OK) {
        conf_detailf(out->why, "load of %.40s refused: %ld", file, (long)rc);
        goto close_handle;
    }
    rc = AARStart(h, (const char *const *)argv, argc);
    if (rc != AAR_OK) {
        conf_detailf(out->why, "start of %.40s refused: %ld", file, (long)rc);
        goto close_handle;
    }

    root = lock_root();
    aar_serve_config_default(&cfg);
    cfg.flags = (AAR_SERVEF_NO_INPUT_WINDOW | run->serve_flags) & ~run->serve_flags_off;
    cfg.groups = run->serve_groups;
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
    serve_hooks.display = run->display;
    serve_hooks.console = &con_ops;
    cfg.hooks = &serve_hooks;
    cfg.user = run;
    rc = aar_serve_create(h, &cfg, &s);
    if (rc != AAR_OK) {
        conf_detailf(out->why, "aar_serve_create returned %ld", (long)rc);
        (void)AARStop(h);
        goto close_handle;
    }
    run->s = s;
    start = conf_ticks();
    for (;;) {
        ULONG got = Wait(aar_serve_sigmask(s));
        ULONG now_ticks;

        rc = aar_serve_step(s, got);
        now_ticks = conf_ticks();
        console_pump(run, now_ticks);
        if (rc != AAR_SERVE_CONTINUE)
            break;
        if (run->poll)
            run->poll(run, now_ticks);
        if (!stop_at && (LONG)(now_ticks - start) > (LONG)(timeout * CONF_TICKS_PER_SECOND)) {
            aar_serve_stop(s);
            stop_at = now_ticks;
            out->timed_out = 1;
        }
        if (stop_at && (LONG)(now_ticks - stop_at) > (LONG)(STOP_PATIENCE_S * CONF_TICKS_PER_SECOND)) {
            conf_detailf(out->why, "payload did not end %lu s after a stop",
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
    out->status = res.status;
    if (res.flags & AAR_SERVE_R_USER_STOP)
        conf_g.aborted = 1;
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
            out->status = st;
        } else {
            out->status_rc = rc;
        }
    }
    if (out->ran && run->on_finished)
        run->on_finished(run);
    aar_serve_destroy(s);
    run->s = NULL;
close_handle:
    if (root)
        UnLock(root);
    (void)AARClose(h);
    run->h = NULL;
}

/* The ids the payload of `area` is expected to report in this run set. */
static int expected_count(int area)
{
    int i;
    int n = 0;

    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == CONF_R_NONE &&
            conf_tests[i].judge == CONF_J_PAYLOAD && conf_tests[i].area == area)
            n++;
    }
    return n;
}

/* ---- Operator tests ------------------------------------------------------------------- */

/* Questions to a person, asked through the Shell. A test that shows something (a
 * picture, a grey screen, a tone) is a variant of its area's payload run with an
 * entry in the area's `operators` table: the runner prints what to look for, waits
 * for Return so that the person is looking, runs the variant, and asks right after
 * it. A test that is a procedure done before the run (C-LIFE.24) has no variant and
 * is asked at the start of the run. Without INTERACTIVE they are SKIP manual. */

/* The console echoes what the operator types, the report file does not: the
 * answer goes into it as typed, so the prompt line ends there too. */
static int read_answer(char *buf, ULONG size)
{
    BPTR in = Input();
    int got;
    size_t n;

    buf[0] = '\0';
    got = in && FGets(in, (STRPTR)buf, size) != NULL;
    if (to_fh) {
        n = strlen(buf);
        if (n)
            (void)Write(to_fh, (APTR)buf, (LONG)n);
        if (!n || buf[n - 1] != '\n')
            (void)Write(to_fh, (APTR)"\n", 1);
    }
    return got;
}

/* Waits for Return. 0 when there is no input. */
static int wait_ready(void)
{
    char line[16];

    put_text("# Get ready. The test starts when you press Return: ");
    return read_answer(line, sizeof line);
}

/* 1 yes, 0 no, -1 no input. */
static int ask_yes_no(const char *question)
{
    char line[16];

    put_text("# ");
    put_text(question);
    put_text(" (y/n) ");
    if (!read_answer(line, sizeof line))
        return -1;
    return line[0] == 'y' || line[0] == 'Y';
}

static const struct conf_operator *find_operator(const char *id)
{
    unsigned a;
    unsigned k;

    for (a = 0; a < CONF_COUNT(areas); a++) {
        for (k = 0; k < areas[a]->operator_count; k++) {
            if (strcmp(areas[a]->operators[k].id, id) == 0)
                return &areas[a]->operators[k];
        }
    }
    return NULL;
}

/* The answer to the operator question of test idx. `op` may be NULL; its accept
 * hook can refuse a yes that the app's own evidence contradicts. */
static void judge_operator(int idx, const struct conf_operator *op, const char *question)
{
    int yes = ask_yes_no(question);

    if (yes < 0) {
        conf_set_result(idx, CONF_R_SKIP, "manual: no answer");
    } else if (!yes) {
        conf_set_result(idx, CONF_R_FAIL, "operator said no");
    } else {
        char why[CONF_DETAIL_MAX];

        if (op && op->accept && !op->accept(why))
            conf_set_result(idx, CONF_R_FAIL, why);
        else
            conf_set_result(idx, CONF_R_PASS, "operator confirmed");
    }
}

/* Operator tests that are procedures done before the run: asked at the start. */
static void ask_procedures(void)
{
    int i;

    if (!conf_g.interactive)
        return;
    for (i = 0; i < CONF_TEST_COUNT && !conf_g.aborted; i++) {
        if (!selected[i] || results[i].status != CONF_R_NONE ||
            conf_tests[i].judge != CONF_J_OPERATOR ||
            find_operator(conf_tests[i].id))
            continue;
        conf_emitf("# [%s] Procedure, to be done before this run: %s", conf_tests[i].id,
                   conf_tests[i].prompt);
        judge_operator(i, NULL,
                       "Did you do this before this run, and did it behave as described?");
    }
}

/* Run the payload of one area (once per variant) and fill the results of its
 * expected ids. A variant that shows something to a person runs only with
 * INTERACTIVE and only when its operator test is selected; the other variants run
 * when a payload test of the area is selected. */
static void run_area(int area)
{
    const char *name = conf_areas[area];
    const struct conf_area *def = NULL;
    unsigned nvariants = 1;
    unsigned v;
    unsigned a;
    char why[CONF_DETAIL_MAX] = "";
    int i;
    int normal = expected_count(area) > 0;
    int stop_area = 0;

    for (a = 0; a < CONF_COUNT(areas); a++) {
        if (strcmp(areas[a]->name, name) == 0)
            def = areas[a];
    }
    if (def && def->variant_count)
        nvariants = def->variant_count;
    if (!normal && !(def && def->operator_count && conf_g.interactive))
        return;

    for (v = 0; v < nvariants && !conf_g.aborted && !stop_area; v++) {
        const struct conf_variant *var = def && def->variant_count ? &def->variants[v] : NULL;
        const struct conf_operator *op = NULL;
        int opidx = -1;
        int repeated = 0;
        int again;
        unsigned k;

        for (k = 0; var && def && k < def->operator_count; k++) {
            if (strcmp(def->operators[k].variant, var->name) == 0)
                op = &def->operators[k];
        }
        if (op) {
            opidx = conf_find_test(op->id);
            if (!conf_g.interactive || opidx < 0 || !selected[opidx] ||
                results[opidx].status != CONF_R_NONE)
                continue;
        } else if (!normal) {
            continue;
        }
        do {
            struct conf_run run;
            struct conf_outcome out;
            struct conf_parsed pr;

            again = 0;
            conf_run_init(&run, name);
            if (var) {
                run.variant = var->name;
                if (var->prepare)
                    var->prepare(&run);
            }
            if (op) {
                conf_emitf("# [%s] %s", op->id, conf_tests[opidx].prompt);
                if (!wait_ready()) {
                    conf_set_result(opidx, CONF_R_SKIP, "manual: no answer");
                    conf_run_free(&run);
                    break;
                }
            }
            conf_emitf("# running conf_%s%s%s", name + 2, run.variant ? " variant " : "",
                       run.variant ? run.variant : "");
            conf_run_payload(&run, &out);
            conf_parse_capture(&run, &pr);
            if (run.diag[0])
                conf_emitf("# %s: client says: %.100s", name, run.diag);
            if (run.cap.over)
                conf_emitf("# %s: console capture overflowed", name);
            if (pr.have_end && pr.pass + pr.fail + pr.skip != (ULONG)pr.relayed) {
                conf_emitf("# %s: end line counts %lu results, %d were relayed", name,
                           (unsigned long)(pr.pass + pr.fail + pr.skip), pr.relayed);
                conf_g.suite_notes++;
            }
            for (a = 0; a < CONF_COUNT(areas); a++) {
                if (areas[a]->on_end)
                    areas[a]->on_end(&run, &out, &pr);
            }
            /* The reason an expected id will fail with, if it is never reported. */
            if (!out.ran)
                conf_detailf(why, "payload not run: %.90s", out.why);
            else if (out.timed_out)
                conf_detailf(why, "payload did not finish in %lu s",
                             (unsigned long)(run.timeout_s ? run.timeout_s
                                                           : PAYLOAD_TIMEOUT_S));
            else if (!pr.have_end && out.end_reason == AAR_END_FORCED)
                conf_detailf(why, "app ended by force (end reason FORCED, exit %ld) before its end line%s",
                             (long)out.exit_code,
                             run.stops ? ": it did not leave within the grace period after the stop it asked for" : "");
            else if (!pr.have_end)
                conf_detailf(why, "payload ended without its end line (exit %ld, end reason %lu)",
                             (long)out.exit_code, (unsigned long)out.end_reason);
            else
                conf_detailf(why, "payload did not report this id");
            if (op) {
                /* Right after the run, while the person remembers it. */
                if (!out.ran) {
                    char d[CONF_DETAIL_MAX];

                    conf_detailf(d, "payload not run: %.90s", out.why);
                    conf_set_result(opidx, CONF_R_FAIL, d);
                } else {
                    if (op->review && op->review(&run) && !repeated &&
                        ask_yes_no("Repeat the test once?") == 1) {
                        repeated = 1;
                        again = 1;
                    }
                    if (!again)
                        judge_operator(opidx, op, "Did it happen as described?");
                }
            } else if (!out.ran || !pr.have_end) {
                stop_area = 1; /* the other variants would fail the same way */
            }
            conf_run_free(&run);
        } while (again && !conf_g.aborted);
    }
    /* An expected id that was not reported fails: a payload that crashed or was
     * refused cannot pass by silence. */
    if (!why[0])
        conf_detailf(why, "payload did not report this id");
    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == CONF_R_NONE &&
            conf_tests[i].judge == CONF_J_PAYLOAD && conf_tests[i].area == area)
            conf_set_result(i, CONF_R_FAIL, why);
    }
}

/* Operator tests that are left: SKIP manual without INTERACTIVE. */
static void run_operator(int idx)
{
    if (!conf_g.interactive) {
        conf_set_result(idx, CONF_R_SKIP, "manual");
        return;
    }
    conf_emitf("# [%s] %s", conf_tests[idx].id, conf_tests[idx].prompt);
    judge_operator(idx, NULL, "Did it happen as described?");
}

/* ---- The run ------------------------------------------------------------------------------ */

static void dump_info(void)
{
    const AARInfo *i = &conf_g.info;
    int k;

    conf_emitf("AAR-CONF-SUITE %s %s", CONF_SUITE_VERSION, conf_g.profile_name);
    conf_emitf("AAR-CONF-INFO query=%ld", (long)conf_g.query_rc);
    conf_emitf("AAR-CONF-INFO library=%u.%u", (unsigned)AARBase->lib_Version,
               (unsigned)AARBase->lib_Revision);
    conf_emitf("AAR-CONF-INFO api_major=%lu", (unsigned long)i->api_major);
    conf_emitf("AAR-CONF-INFO api_minor=%lu", (unsigned long)i->api_minor);
    conf_emitf("AAR-CONF-INFO platform_id=%lu", (unsigned long)i->platform_id);
    conf_emitf("AAR-CONF-INFO profiles=0x%lx", (unsigned long)i->profiles);
    conf_emitf("AAR-CONF-INFO groups=0x%lx", (unsigned long)i->groups);
    conf_emitf("AAR-CONF-INFO flags=0x%lx", (unsigned long)i->flags);
    conf_emitf("AAR-CONF-INFO max_apps=%lu", (unsigned long)i->max_apps);
    conf_emitf("AAR-CONF-INFO max_image_bytes=%lu", (unsigned long)i->max_image_bytes);
    conf_emitf("AAR-CONF-INFO max_args=%lu", (unsigned long)i->max_args);
    conf_emitf("AAR-CONF-INFO max_arg_bytes=%lu", (unsigned long)i->max_arg_bytes);
    conf_emitf("AAR-CONF-INFO shared_granule=%lu", (unsigned long)i->shared_granule);
    conf_emitf("AAR-CONF-INFO msg_capacity=%lu", (unsigned long)i->msg_capacity);
    conf_emitf("AAR-CONF-INFO stall_limit_ms=%lu", (unsigned long)i->stall_limit_ms);
    conf_emitf("AAR-CONF-INFO stop_grace_ms=%lu", (unsigned long)i->stop_grace_ms);
    conf_emitf("AAR-CONF-INFO shared_default_mib=%lu",
               (unsigned long)i->shared_default_mib);
    conf_emitf("AAR-CONF-INFO shared_max_mib=%lu", (unsigned long)i->shared_max_mib);
    conf_emitf("AAR-CONF-INFO weight_max=%lu", (unsigned long)i->weight_max);
    conf_emitf("AAR-CONF-INFO queue_depth=%lu", (unsigned long)i->queue_depth);
    for (k = 0; k < 8; k++)
        conf_emitf("AAR-CONF-INFO platform%d=0x%lx", k, (unsigned long)i->platform[k]);
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

        if (t->judge == CONF_J_NONE || !(t->profiles & conf_g.profile_bit))
            continue;
        if (conf_g.only && !has_prefix_nocase(t->id, conf_g.only))
            continue;
        selected[i] = 1;
        if (!conf_group_declared(t->group))
            conf_set_result(i, CONF_R_SKIP, "group not declared");
        else if (!t->impl)
            conf_set_result(i, CONF_R_SKIP, "suite: not implemented in this version");
    }
}

/* The observer tests (C-LIFE.1 to C-LIFE.3) are judged on every payload run of a
 * run_area. When the selection (ONLY) leaves them without a result because no
 * payload area was selected, the core runs the payload of OBSERVER_AREA once with
 * its result lines not taken, so that the observers see a run. A full run never
 * needs it: the observers have their results by then. */
#define OBSERVER_AREA "C-TIME"

static void run_observer_fallback(void)
{
    struct conf_run run;
    struct conf_outcome out;
    struct conf_parsed pr;
    unsigned a;
    int i;
    int need = 0;

    for (a = 0; a < CONF_COUNT(areas) && !need; a++) {
        if (!areas[a]->on_open && !areas[a]->on_end)
            continue;
        for (i = 0; i < CONF_TEST_COUNT; i++) {
            if (selected[i] && results[i].status == CONF_R_NONE &&
                conf_tests[i].judge == CONF_J_RUNNER &&
                strcmp(conf_areas[conf_tests[i].area], areas[a]->name) == 0) {
                need = 1;
                break;
            }
        }
    }
    if (!need || conf_g.aborted)
        return;
    conf_run_init(&run, OBSERVER_AREA);
    run.norelay = 1;
    conf_emitf("# running conf_%s for the observer tests", OBSERVER_AREA + 2);
    conf_run_payload(&run, &out);
    conf_parse_capture(&run, &pr);
    for (a = 0; a < CONF_COUNT(areas); a++) {
        if (areas[a]->on_end)
            areas[a]->on_end(&run, &out, &pr);
    }
    conf_run_free(&run);
}

static void run_tests(void)
{
    int i;
    unsigned a;
    unsigned k;

    /* Procedures the operator did before the run are asked first. */
    ask_procedures();

    /* Runner-judged tests of every registered area. */
    for (a = 0; a < CONF_COUNT(areas); a++) {
        for (k = 0; k < areas[a]->test_count && !conf_g.aborted; k++) {
            int idx = conf_find_test(areas[a]->tests[k].id);

            if (idx >= 0 && selected[idx] && results[idx].status == CONF_R_NONE)
                areas[a]->tests[k].fn(idx);
        }
    }
    for (i = 0; i < CONF_AREA_COUNT && !conf_g.aborted; i++)
        run_area(i);
    run_observer_fallback();
    for (i = 0; i < CONF_TEST_COUNT; i++) {
        if (selected[i] && results[i].status == CONF_R_NONE &&
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
        if (status == CONF_R_NONE) {
            conf_set_result(i, CONF_R_FAIL, conf_g.aborted ? "run aborted"
                                                           : "the runner has no result for it");
            status = CONF_R_FAIL;
        }
        switch (status) {
        case CONF_R_PASS:
            pass++;
            word = "PASS";
            break;
        case CONF_R_FAIL:
            fail++;
            word = "FAIL";
            if (t->level == CONF_MUST)
                must_fail++;
            break;
        default:
            skip++;
            word = "SKIP";
            /* 11.1: only an undeclared group or a SHOULD test may be skipped. */
            if (t->level == CONF_MUST && conf_group_declared(t->group))
                must_skip++;
            break;
        }
        conf_emitf("AAR-CONF %s %s %s", t->id, word, results[i].detail);
    }
    ok = must_fail == 0 && must_skip == 0 && conf_g.suite_notes == 0;
    if (conf_g.only) {
        conf_emitf("# filtered run (ONLY %s): %lu MUST failed, %lu MUST skipped",
                   conf_g.only, (unsigned long)must_fail, (unsigned long)must_skip);
    } else if (!ok) {
        conf_emitf("# %lu MUST failed, %lu MUST skipped, %lu suite notes",
                   (unsigned long)must_fail, (unsigned long)must_skip,
                   (unsigned long)conf_g.suite_notes);
    }
    conf_emitf("AAR-CONF-SUMMARY pass=%lu fail=%lu skip=%lu verdict=%s",
               (unsigned long)pass, (unsigned long)fail, (unsigned long)skip,
               conf_g.only ? "PARTIAL" : (ok ? "CONFORMS" : "DOES-NOT-CONFORM"));
    return ok || (conf_g.only && must_fail == 0) ? RETURN_OK : RETURN_ERROR;
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
    conf_g.dir = args[A_DIR] ? (const char *)args[A_DIR] : "PROGDIR:";
    conf_g.root = args[A_ROOT] ? (const char *)args[A_ROOT] : DEFAULT_ROOT;
    conf_g.only = args[A_ONLY] ? (const char *)args[A_ONLY] : NULL;
    conf_g.interactive = args[A_INTERACTIVE] != 0;
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
    conf_g.info.size = sizeof conf_g.info;
    conf_g.query_rc = AARQuery(&conf_g.info);
    conf_g.profile_value = AAR_PROFILE_A32;
    if (args[A_PROFILE]) {
        if (has_prefix_nocase((const char *)args[A_PROFILE], "a64"))
            conf_g.profile_value = AAR_PROFILE_A64;
    } else if (!(conf_g.info.profiles & AAR_PROFILEF_A32) &&
               (conf_g.info.profiles & AAR_PROFILEF_A64)) {
        conf_g.profile_value = AAR_PROFILE_A64;
    }
    conf_g.profile_bit = conf_g.profile_value == AAR_PROFILE_A64 ? 2UL : 1UL;
    conf_g.profile_name = conf_g.profile_value == AAR_PROFILE_A64 ? "a64" : "a32";

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
