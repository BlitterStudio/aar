/*
 * aar_serve: the serving loop of an aar.library client (spec section 9.6).
 *
 * An app that runs on a platform behind aar.library asks its client for files,
 * the console, a display, audio and input. This header is the interface of the
 * code that answers those requests, so that every client, the reference
 * launcher aar-run (client/run) and a program with its own window and event
 * loop alike, serves the same way. The calls are plain C functions linked into
 * the program (client/serve/aar_serve.c or libaarserve.a), not library
 * vectors. docs/aar-serve-api.md describes each one.
 *
 * One aar_serve serves one AARHandle. The caller opens the handle, gives it its
 * options (AARSetTagList), loads and starts the app, creates the aar_serve,
 * steps it until the app has ended, and then destroys it and closes the
 * handle. aar_serve never opens or closes the handle and never loads or starts
 * an app. Every call must come from the task that opened the handle
 * (AAR_E_OWNER otherwise) and the hooks run in that task, inside
 * aar_serve_step; nothing here runs in an interrupt.
 *
 * The program supplies the library bases it already has: AARBase, and for a
 * display IntuitionBase and GfxBase (as any program that uses those protos
 * does). aar_serve opens timer.device, Picasso96API.library and ahi.device
 * itself and keeps those bases to itself.
 *
 * The task calling aar_serve_create, aar_serve_step and aar_serve_run needs at
 * least AAR_SERVE_MIN_STACK bytes of stack: a palette update holds a 3 KiB
 * table, the packet engine and the console need the rest.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBRARIES_AAR_SERVE_H
#define LIBRARIES_AAR_SERVE_H

#include <stdint.h>

#include <exec/types.h>
#include <dos/dos.h>

#include <libraries/aar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The least stack the calling task needs, in bytes. aar_serve does not check it;
 * a launcher that wants a message instead of a crash tests the stack itself. */
#define AAR_SERVE_MIN_STACK 16384UL

typedef struct aar_serve aar_serve;

/* ---- Requests and completions (the types of the hooks) ---------------------- */

/* A request as aar.library hands it out, with its payload buffer already mapped
 * into this process. buf is the mapped address of (buf_token, buf_off) and is
 * NULL when the request names no payload. A request's buffer is touched only
 * between the request and its completion, and only while the claim is live. */
struct aar_serve_req {
    uint16_t op;                    /* AAR_OP_* */
    uint16_t flags;
    uint32_t id;
    uint32_t arg[AAR_ENTRY_ARGS];   /* AAR_A_* */
    uint32_t buf_token;
    uint32_t buf_off;
    uint32_t buf_len;
    uint8_t *buf;
};

/* The completion a handler produces: AAR_OK or a negative AAR_E_* in result,
 * the entry flags and the result words (AAR_R_*). */
struct aar_serve_done {
    int32_t result;
    uint16_t flags;
    uint32_t out[AAR_ENTRY_OUTS];
};

/* ---- Replacing the display and the console ---------------------------------- */

/* The calls under the display handlers. The built-in table drives a Picasso96
 * screen with two screen buffers, a three or two page tall screen that is
 * panned, or a single buffer (AAR_SERVE_DISPLAY_*). A program that draws into
 * its own window supplies its own table and the pixel memory through
 * AARSharedToken. */
struct aar_serve_display_ops {
    void *ctx;                      /* the first argument of every call */
    /* DISPLAY_OPEN: opens the screen, finds the tokens of its bitmaps and fills
     * *done (the completion carries the pitch, the layout, front, back and
     * spare tokens and the AAR_DISPCAP_* flags). */
    void (*open)(void *ctx, const struct aar_serve_req *req,
                 struct aar_serve_done *done);
    /* Show the back (1) or the front (0) buffer. 0: the change failed. A flip
     * that succeeds is completed when the program calls aar_serve_flip_shown
     * (the built-in table does so when the SafeMessage of the change comes back). */
    int (*change_buffer)(void *ctx, int to_back);
    /* Pan the screen to page (page * screen height rows down). */
    void (*pan)(void *ctx, uint32_t page);
    void (*wait_tof)(void *ctx);
    /* A palette: (count << 16) | first, then three 32 bit words per colour,
     * then 0 (the LoadRGB32 table). */
    void (*load_rgb32)(void *ctx, const uint32_t *table);
    void (*close)(void *ctx);
};

/* The calls under the console handlers. Writes and reads are asynchronous: a
 * start returns at once, and the program tells aar_serve how it ended. */
struct aar_serve_console_ops {
    void *ctx;
    /* Open the Workbench console, if there is one to open. 0: it cannot open. */
    int (*open)(void *ctx);
    /* Start one write of bytes (copy them before returning). 0: it could not
     * start. */
    int (*write_start)(void *ctx, const uint8_t *data, uint32_t bytes);
    /* Start one read of at most max bytes. 0: it could not start (end of input). */
    int (*read_start)(void *ctx, uint32_t max);
    /* The app is RUNNING and no stop was asked: a read may start. */
    int (*read_allowed)(void *ctx);
    /* The claim is still served; a request's buffer is not touched after it is
     * not. */
    int (*live)(void *ctx);
    /* Output cannot be written: stop the app and say so once. */
    void (*output_error)(void *ctx);
};

/* ---- Configuration ------------------------------------------------------------ */

/* aar_serve_config.flags */
#define AAR_SERVEF_ABS_PATHS       (1UL << 0)  /* paths with a volume or assign colon are allowed;
                                                  ROOT does not confine them */
#define AAR_SERVEF_KEEP_MESSAGES   (1UL << 1)  /* do not drain app messages: the program calls
                                                  AARGetMessage itself */
#define AAR_SERVEF_NO_INPUT_WINDOW (1UL << 2)  /* no backdrop input window: the program feeds
                                                  aar_serve_post_event */
#define AAR_SERVEF_USE_SIGNAL      (1UL << 3)  /* arm the handle and add the library signal to
                                                  aar_serve_sigmask */

/* aar_serve_config.display_strategy: the first strategy to try; the later ones
 * are the fallbacks. */
#define AAR_SERVE_DISPLAY_BUFFERS 0UL  /* screen buffers, then a 3 page tall screen, a 2 page one, one buffer */
#define AAR_SERVE_DISPLAY_PAN3    1UL  /* start at the 3 page tall screen */
#define AAR_SERVE_DISPLAY_PAN2    2UL  /* start at the 2 page tall screen */
#define AAR_SERVE_DISPLAY_SINGLE  3UL  /* one buffer, the platform copies */

/* Why the loop wants the app stopped (the second argument of stop_wanted). */
#define AAR_SERVE_WHY_BREAK   1UL  /* Ctrl-C, or the raw key Ctrl-C in the display window */
#define AAR_SERVE_WHY_WINDOW  2UL  /* the input window was closed */
#define AAR_SERVE_WHY_CONSOLE 3UL  /* the console cannot be opened or written */
#define AAR_SERVE_WHY_DISPLAY 4UL  /* the display cannot be opened */
#define AAR_SERVE_WHY_AUDIO   5UL  /* AHI cannot be opened */
#define AAR_SERVE_WHY_QUEUE   6UL  /* the request queue is corrupt */
#define AAR_SERVE_WHY_CALLER  7UL  /* aar_serve_stop was called */

/* The default AHI, joystick and heartbeat settings of aar_serve_config_default. */
#define AAR_SERVE_DEFAULT_POLL_US    1000UL
#define AAR_SERVE_DEFAULT_JOY_PORTS  2UL  /* port 1 only */
#define AAR_SERVE_BREAK_ALL (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F)

struct aar_serve_hooks {
    ULONG size;                     /* sizeof; set by the caller */
    /* A launcher diagnostic or error line (no newline). Default: queued to the
     * console writer. */
    void (*status)(APTR user, const char *text);
    /* One app log record (AAR_OP_LOG), valid for the call. Default: queued as a
     * diagnostic line. */
    void (*log)(APTR user, const void *data, ULONG len);
    /* One app message. Only with AAR_SERVEF_KEEP_MESSAGES unset. Default: drop
     * and count. */
    void (*message)(APTR user, const AARMessage *msg);
    /* The loop wants the app stopped; why is AAR_SERVE_WHY_*. Default:
     * aar_serve_stop. Lets a program close its own window first. */
    void (*stop_wanted)(APTR user, ULONG why);
    /* Replacements for the P96 screen and for the console. NULL: the built-in
     * ones. */
    const struct aar_serve_display_ops *display;
    const struct aar_serve_console_ops *console;
};
typedef struct aar_serve_hooks aar_serve_hooks;

typedef struct aar_serve_config {
    ULONG size;                     /* sizeof; set by aar_serve_config_default; grows at the end only */
    ULONG flags;                    /* AAR_SERVEF_* */
    BPTR root;                      /* a lock the app's relative paths resolve under; aar_serve
                                       duplicates it. 0: a duplicate of the current directory */
    ULONG screen_mode;              /* the P96 DisplayID to force; 0: choose by size and format */
    CONST_STRPTR screen_title;      /* title of the P96 screen and of requesters; NULL: "aar-serve" */
    BPTR console_in;                /* 0: Input() */
    BPTR console_out;               /* 0: Output(). A stream the caller gives is the caller's to close */
    CONST_STRPTR console_spec;      /* opened at the first console use when console_out is 0 and the
                                       program has no Shell stream (a Workbench start, for example
                                       "CON:0/0/640/160/aar-run/CLOSE/WAIT"); NULL: none, and console
                                       I/O then fails as AAR_E_IO */
    ULONG audio_route;              /* AAR_AUDIO_ROUTE_PLATFORM or AAR_AUDIO_ROUTE_AHI: what this loop
                                       serves; it must equal the AAR_TAG_AUDIO_ROUTE the handle was
                                       given */
    ULONG groups;                   /* AAR_GROUP_* to serve; the other groups' ops complete with
                                       AAR_E_NOSYS. 0: every group */
    ULONG joy_ports;                /* bit 0 port 0, bit 1 port 1; default AAR_SERVE_DEFAULT_JOY_PORTS */
    ULONG poll_us;                  /* the heartbeat of the timer in microseconds; 0: none. Default
                                       AAR_SERVE_DEFAULT_POLL_US */
    ULONG break_mask;               /* the task signals the loop consumes: CTRL_C stops the app,
                                       CTRL_D, CTRL_E and CTRL_F become AAR_EV_BREAK. Default
                                       AAR_SERVE_BREAK_ALL; 0: the program owns its signals */
    ULONG display_strategy;         /* AAR_SERVE_DISPLAY_*, the first strategy to try */
    const aar_serve_hooks *hooks;   /* may be NULL */
    APTR user;                      /* passed to every hook */
    APTR platform;                  /* reserved, 0 */
} aar_serve_config;

/* ---- The result ---------------------------------------------------------------- */

/* aar_serve_result.flags */
#define AAR_SERVE_R_FAULTED        (1UL << 0)  /* the app faulted */
#define AAR_SERVE_R_USER_STOP      (1UL << 1)  /* the user stopped it with Ctrl-C */
#define AAR_SERVE_R_STOP_ASKED     (1UL << 2)  /* a stop was requested, by whoever */
#define AAR_SERVE_R_CLAIM_LOST     (1UL << 3)  /* the claim was revoked, quarantined or reset */
#define AAR_SERVE_R_DISPLAY_FAILED (1UL << 4)
#define AAR_SERVE_R_CONSOLE_FAILED (1UL << 5)
#define AAR_SERVE_R_AUDIO_FAILED   (1UL << 6)
#define AAR_SERVE_R_QUEUE_CORRUPT  (1UL << 7)

struct aar_serve_result {
    ULONG size;                     /* sizeof; set by the caller */
    ULONG flags;                    /* AAR_SERVE_R_*; valid at any time */
    ULONG state;                    /* AAR_STATE_* of the final read */
    ULONG end_reason;               /* AAR_END_* of the final read */
    LONG  exit_code;                /* the app's exit code of the final read */
    ULONG dropped_logs;             /* log records the console queue had no room for */
    ULONG dropped_messages;         /* app messages drained and dropped */
    AARStatusInfo status;           /* the final AARStatus read */
};

/* ---- Calls --------------------------------------------------------------------- */

/* What aar_serve_step and aar_serve_run return. */
#define AAR_SERVE_CONTINUE 0L  /* keep stepping */
#define AAR_SERVE_ENDED    1L  /* the app ended and the output that must be written is written */
#define AAR_SERVE_LOST     2L  /* the claim is gone (revoked, quarantined, reset): nothing more to serve */

/* Fill *cfg with the defaults and its size. Change the fields you need. */
void aar_serve_config_default(aar_serve_config *cfg);

/* Create the serving state for handle h. AAR_OK, or: AAR_E_INVAL (a bad
 * configuration), AAR_E_NOMEM, AAR_E_NOSYS (no timer.device), AAR_E_OWNER,
 * AAR_E_STALE. On an error *out is NULL and nothing is left behind. */
LONG aar_serve_create(AARHandle *h, const aar_serve_config *cfg, aar_serve **out);

/* Wait and step until the app has ended. AAR_SERVE_ENDED or AAR_SERVE_LOST. */
LONG aar_serve_run(aar_serve *s);

/* Every signal the loop waits on now. It changes while the app runs (the console,
 * the input window and the audio port appear when they are first used): ask
 * again before each Wait. */
ULONG aar_serve_sigmask(const aar_serve *s);

/* One pass of the loop; signals is what Wait returned. AAR_SERVE_CONTINUE until
 * the app has ended. */
LONG aar_serve_step(aar_serve *s, ULONG signals);

/* Post an input event for the app (a program that has its own window). */
LONG aar_serve_post_event(aar_serve *s, const aar_event *ev);

/* A replacement display table tells the loop that the flip it started with
 * change_buffer is on the screen and the old buffer is free to draw into. */
void aar_serve_flip_shown(aar_serve *s);

/* Ask the app to stop. Idempotent. */
void aar_serve_stop(aar_serve *s);

/* Queue a launcher diagnostic line (no newline) before the app's console text. */
void aar_serve_status(aar_serve *s, const char *text);

/* Find a file the way the app's own paths are found, through every directory of
 * a multi-directory assign, and give its full name. dir is the directory a
 * relative name is searched from (0: name is a volume or assign path). AAR_OK;
 * AAR_E_NOENT: not found; AAR_E_IO: found but the name did not fit or could not
 * be built. A Ctrl-C during the search counts as a user stop. */
LONG aar_serve_locate(aar_serve *s, BPTR dir, const char *name, char *full, ULONG size);

/* The outcome. flags are valid at any time. When the claim is still ours the
 * call reads the final status into status and returns AAR_OK; otherwise the
 * library's error (AAR_E_STALE after AAR_SERVE_LOST). */
LONG aar_serve_result(aar_serve *s, struct aar_serve_result *out);

/* Finish everything that is pending (aborts and waits for AHI packets, restores
 * the public screen, finishes the console writes and the launcher text),
 * releases every file and lock, closes timer.device and the libraries aar_serve
 * opened, and frees the state. The handle stays open: close it afterwards. */
void aar_serve_destroy(aar_serve *s);

#ifdef __cplusplus
}
#endif

#endif /* LIBRARIES_AAR_SERVE_H */
