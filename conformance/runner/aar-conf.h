/*
 * The runner's core interface for the area files (r_<area>.c). aar-conf.c is the
 * core: options, the catalogue, the AARInfo dump, result lines, the verdict, the
 * payload run through aar_serve with a captured console, the peer message
 * dispatch. An area file adds what is specific to its area: runner-judged tests,
 * peer keywords, variants of its payload run, observers of every run. See
 * conformance/CONTRIBUTING.md.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_RUNNER_H
#define AAR_CONF_RUNNER_H

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/aar.h>

#include <libraries/aar_serve.h>

#include "conf_catalogue.h"
#include "../payloads/conf_proto.h"

#define CONF_DETAIL_MAX 200
#define CONF_CAPTURE_MAX (256UL * 1024UL)
#define CONF_MAX_EXTRA_ARGS 8
#define CONF_TICKS_PER_SECOND 50UL

#define CONF_COUNT(array) ((unsigned)(sizeof(array) / sizeof((array)[0])))

/* ---- Globals ------------------------------------------------------------------ */

/* CONF_R_NONE, CONF_R_PASS, CONF_R_FAIL, CONF_R_SKIP and conf_update_allowed. */
#include "conf_select.h"

struct conf_globals {
    AARInfo info;              /* AARQuery of the platform */
    LONG query_rc;
    ULONG profile_value;       /* AAR_PROFILE_A32 or AAR_PROFILE_A64 */
    ULONG profile_bit;         /* the bit of conf_test.profiles */
    const char *profile_name;  /* "a32" or "a64" */
    const char *dir;           /* DIR: the directory that holds a32/ and a64/ */
    const char *root;          /* ROOT: files of the payloads resolve here */
    const char *only;          /* ONLY filter or NULL */
    int interactive;
    int aborted;               /* the user pressed Ctrl-C during a payload run */
    ULONG suite_notes;         /* suite errors: any makes the verdict negative */
};
extern struct conf_globals conf_g;

/* ---- Results ------------------------------------------------------------------- */

/* Output, to the Shell and to TO. Lines that start with "# " are comments. */
void conf_emitf(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* snprintf into a detail buffer of CONF_DETAIL_MAX bytes. */
void conf_detailf(char *dst, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Index into conf_tests[] of an id, or -1. */
int conf_find_test(const char *id);
/* The test is part of this run (profile, ONLY filter, not "none"). */
int conf_selected(int idx);
/* Sets a result. The first FAIL stays; a PASS never replaces a result. */
void conf_set_result(int idx, int status, const char *detail);
/* conf_set_result by id: PASS when ok, else FAIL. An unknown id is a suite
 * error. */
void conf_record(const char *id, int ok, const char *detail);
/* The platform declares the group (core always; zz9000 by platform id). */
int conf_group_declared(unsigned short group);

/* Time in 1/50 s since the DOS epoch: monotonic, 20 ms resolution. */
ULONG conf_ticks(void);

/* ---- Running a payload ----------------------------------------------------------- */

struct conf_run;

/* A peer keyword handler: a payload sent a message whose first bytes are the
 * keyword. It runs inside aar_serve_step in the runner task and may call
 * AARSendMessage(run->h, ...), aar_serve_post_event(run->s, ...), conf_run_stop
 * and so on. Keywords start with CONF-<AREA>- (CONF-STOP-NOW is the core's). */
struct conf_peer {
    const char *keyword;
    void (*fn)(struct conf_run *run, const AARMessage *msg);
};

struct conf_capture {
    UBYTE *buf;                /* CONF_CAPTURE_MAX + 1 bytes, NUL after len */
    ULONG len;
    int over;                  /* more output than fits: the rest was dropped */
};

/* One run of one payload. conf_run_init fills the defaults; an area changes the
 * "inputs" before conf_run_payload (usually in a variant's prepare). */
struct conf_run {
    /* inputs */
    const char *area;          /* "C-TIME": the payload is conf_time.elf */
    const char *variant;       /* NULL, or the name passed as conf:variant=<name> */
    const char *extra_args[CONF_MAX_EXTRA_ARGS]; /* after the standard arguments */
    ULONG extra_count;
    const char *const *argv_override; /* replaces every argument (argv[0] too) */
    ULONG argc_override;
    const struct TagItem *tags;      /* AARSetTagList before the load, or NULL */
    ULONG timeout_s;                 /* 0: the default (180 s) */
    ULONG serve_flags;               /* ORed into aar_serve_config.flags */
    ULONG serve_groups;              /* aar_serve_config.groups, 0: all */
    ULONG serve_flags_off;           /* cleared from aar_serve_config.flags after the
                                        core's own (NO_INPUT_WINDOW) and serve_flags */
    const struct aar_serve_display_ops *display; /* replaces the P96 display, or NULL */
    /* Console modes. The console is a capture; these shape it. Slow output: a
     * write completes write_hold_ticks (1/50 s) after it started. Scripted
     * input: console_read gets input[] in pieces; after the script it gets the
     * end of input, or with input_block nothing, ever (a read that only a stop
     * ends). The first read waits input_hold_ticks. */
    struct {
        const UBYTE *input;
        ULONG input_len;
        ULONG input_hold_ticks;
        int input_block;
        ULONG write_hold_ticks;
    } con;
    /* Callbacks, all in the runner task inside the serving loop. */
    void (*poll)(struct conf_run *run, ULONG now_ticks);          /* every pass */
    void (*on_message)(struct conf_run *run, const AARMessage *m); /* not a keyword */
    void (*on_log)(struct conf_run *run, const void *data, ULONG len);
    void *user;                      /* for the area's callbacks */
    /* The app has ended and the serving loop returned; the handle is still open
     * (the status and the messages are readable), AARClose follows. */
    void (*on_finished)(struct conf_run *run);
    int norelay;                     /* do not take AAR-CONF lines into results */

    /* state, read by the area */
    aar_serve *s;
    AARHandle *h;
    struct conf_capture cap;
    char diag[160];                  /* what the client said (aar_serve status) */
    ULONG messages;                  /* messages the payload sent */
    ULONG logs;                      /* log records */
    ULONG stops;                     /* CONF-STOP-NOW requests */

    /* console state, the core's */
    ULONG input_pos;
    int write_pending;
    LONG write_len;
    ULONG write_due;
    int read_pending;
    ULONG read_max;
    ULONG read_due;
};

struct conf_outcome {
    int ran;                   /* started and served to its end */
    int timed_out;
    char why[CONF_DETAIL_MAX]; /* why it did not run, when it did not */
    LONG exit_code;
    ULONG state, end_reason, fault_class;
    LONG status_rc;            /* the status read after the end */
    ULONG serve_flags;         /* AAR_SERVE_R_* */
    AARStatusInfo status;      /* valid when status_rc is AAR_OK */
};

/* What the AAR-CONF lines of a capture said. */
struct conf_parsed {
    int have_end;
    ULONG pass, fail, skip;    /* from the end line */
    int relayed;               /* result lines taken */
};

/* Defaults for a run of the payload of `area`. Frees nothing. */
void conf_run_init(struct conf_run *run, const char *area);
void conf_run_free(struct conf_run *run);
/* Open, load, start, serve to the end, read the status, close. Never fails the
 * caller: a problem is in out->why. */
void conf_run_payload(struct conf_run *run, struct conf_outcome *out);
/* Splits the capture into lines, takes the AAR-CONF lines into the results (not
 * with run->norelay) and prints the other lines as comments. */
void conf_parse_capture(struct conf_run *run, struct conf_parsed *pr);
/* Ask the app to stop, as a client does. */
void conf_run_stop(struct conf_run *run);

/* ---- Area files ---------------------------------------------------------------------- */

/* A runner-judged test: fn records its result with conf_set_result(idx, ...). */
struct conf_r_test {
    const char *id;
    void (*fn)(int idx);
};

/* A variant of the area's payload run: the payload runs once per variant, with
 * its prepare applied (tags, extra arguments, console modes, fixtures) and
 * conf:variant=<name> as an argument. A payload prints the ids of a variant only
 * in that variant. Without variants the payload runs once. */
struct conf_variant {
    const char *name;
    void (*prepare)(struct conf_run *run);
};

/* An operator test (judge `operator` in TESTS.md) that a variant of the payload run
 * shows to a person: a picture, a screen to touch, a tone. The core runs the variant
 * only with INTERACTIVE and only when the test is selected (so ONLY=<id> runs just
 * this variant): it prints the test's text, waits for Return so that the person is
 * looking, runs the variant and asks the question right after it. An operator test
 * with no entry here is a procedure done before the run and is asked at the start.
 *   review  after the run, before the question; it prints what the app saw and
 *           returns 1 when the run gave no evidence and may be repeated once (the
 *           core then offers a repeat). May be NULL.
 *   accept  after a yes; returns 0 and fills why (a result detail) when the app's
 *           own evidence contradicts the person, so that the test fails. May be NULL. */
struct conf_operator {
    const char *id;                          /* "C-INPUT.11" */
    const char *variant;                     /* "show" */
    int (*review)(const struct conf_run *run);
    int (*accept)(char *why);
};

/* What an area registers (r_areas.h). Everything is optional. */
struct conf_area {
    const char *name;                       /* "C-CLIENT" */
    const struct conf_r_test *tests;        /* runner-judged tests of the area */
    unsigned test_count;
    const struct conf_peer *peers;          /* peer keywords of its payload */
    unsigned peer_count;
    const struct conf_variant *variants;    /* runs of its payload */
    unsigned variant_count;
    const struct conf_operator *operators;  /* operator tests shown by a variant */
    unsigned operator_count;
    /* Observers: called for every payload run of every area. */
    void (*on_open)(const struct conf_run *run, LONG status_rc,
                    const AARStatusInfo *status);        /* after AAROpen */
    void (*on_end)(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr);        /* after the run */
};

#endif
