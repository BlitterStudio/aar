/*
 * The private layout of aar_serve: the instance (struct aar_serve) with one section per
 * service, the X_* bindings of every Exec, DOS, Intuition, graphics, Picasso96 and
 * aar.library call the backends make, and the services the backends take from the
 * loop. aar_serve.c includes this file and then the backends (aar_serve_dos.c,
 * aar_serve_con.c, aar_serve_gfx.c, aar_serve_ahi.c, aar_serve_input.c); they are
 * pieces of one translation unit and are not compiled alone.
 *
 * Every operating system call goes through an X_* binding, so that a host test can
 * drive the backends with a fake system: the test defines AAR_SERVE_HOST and puts
 * its fake system, serve_host.h, on the include path. On the Amiga the bindings
 * below are the real calls.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_SERVE_PRIV_H
#define AAR_SERVE_PRIV_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libraries/aar.h>
#include <libraries/aar_serve.h>

#include "aar_serve_core.h"

#ifdef AAR_SERVE_HOST
/* ---- The host build: the bindings and the system types are the test's fake system ---- */
#include "serve_host.h"
#else
/* ---- The Amiga build: the bindings are the real calls ------------------------------- */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <devices/ahi.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <proto/aar.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/timer.h>

/* Exec: memory, ports, I/O requests, messages, signals, libraries */
#define X_ALLOC_MEM(...) AllocMem(__VA_ARGS__)
#define X_FREE_MEM(...) FreeMem(__VA_ARGS__)
#define X_ALLOC_VEC(...) AllocVec(__VA_ARGS__)
#define X_FREE_VEC(...) FreeVec(__VA_ARGS__)
#define X_COPY_MEM(...) CopyMem(__VA_ARGS__)
#define X_CREATE_PORT(...) CreateMsgPort(__VA_ARGS__)
#define X_DELETE_PORT(...) DeleteMsgPort(__VA_ARGS__)
#define X_CREATE_IOREQ(...) CreateIORequest(__VA_ARGS__)
#define X_DELETE_IOREQ(...) DeleteIORequest(__VA_ARGS__)
#define X_OPEN_DEVICE(...) OpenDevice(__VA_ARGS__)
#define X_CLOSE_DEVICE(...) CloseDevice(__VA_ARGS__)
#define X_SEND_IO(...) SendIO(__VA_ARGS__)
#define X_ABORT_IO(...) AbortIO(__VA_ARGS__)
#define X_WAIT_IO(...) WaitIO(__VA_ARGS__)
#define X_CHECK_IO(...) CheckIO(__VA_ARGS__)
#define X_PUT_MSG(...) PutMsg(__VA_ARGS__)
#define X_GET_MSG(...) GetMsg(__VA_ARGS__)
#define X_REPLY_MSG(...) ReplyMsg(__VA_ARGS__)
#define X_WAIT_PORT(...) WaitPort(__VA_ARGS__)
#define X_WAIT(...) Wait(__VA_ARGS__)
#define X_SET_SIGNAL(...) SetSignal(__VA_ARGS__)
#define X_OPEN_LIB(...) OpenLibrary(__VA_ARGS__)
#define X_CLOSE_LIB(...) CloseLibrary(__VA_ARGS__)

/* dos.library */
#define X_LOCK(...) Lock(__VA_ARGS__)
#define X_UNLOCK(...) UnLock(__VA_ARGS__)
#define X_DUP_LOCK(...) DupLock(__VA_ARGS__)
#define X_PARENT_DIR(...) ParentDir(__VA_ARGS__)
#define X_SAME_LOCK(...) SameLock(__VA_ARGS__)
#define X_IO_ERR(...) IoErr(__VA_ARGS__)
#define X_SET_IO_ERR(...) SetIoErr(__VA_ARGS__)
#define X_OPEN(...) Open(__VA_ARGS__)
#define X_CLOSE(...) Close(__VA_ARGS__)
#define X_WRITE(...) Write(__VA_ARGS__)
#define X_OUTPUT(...) Output(__VA_ARGS__)
#define X_INPUT(...) Input(__VA_ARGS__)
#define X_IS_INTERACTIVE(...) IsInteractive(__VA_ARGS__)
#define X_ALLOC_DOS_OBJECT(...) AllocDosObject(__VA_ARGS__)
#define X_FREE_DOS_OBJECT(...) FreeDosObject(__VA_ARGS__)
#define X_GET_DEVICE_PROC(...) GetDeviceProc(__VA_ARGS__)
#define X_FREE_DEVICE_PROC(...) FreeDeviceProc(__VA_ARGS__)
#define X_CURRENT_DIR(...) CurrentDir(__VA_ARGS__)
#define X_NAME_FROM_LOCK(...) NameFromLock(__VA_ARGS__)

/* intuition.library and graphics.library */
#define X_OPEN_WINDOW(...) OpenWindowTags(__VA_ARGS__)
#define X_CLOSE_WINDOW(...) CloseWindow(__VA_ARGS__)
#define X_CHANGE_SCREEN_BUFFER(...) ChangeScreenBuffer(__VA_ARGS__)
#define X_ALLOC_SCREEN_BUFFER(...) AllocScreenBuffer(__VA_ARGS__)
#define X_FREE_SCREEN_BUFFER(...) FreeScreenBuffer(__VA_ARGS__)
#define X_LOCK_PUB_SCREEN(...) LockPubScreen(__VA_ARGS__)
#define X_UNLOCK_PUB_SCREEN(...) UnlockPubScreen(__VA_ARGS__)
#define X_SCREEN_TO_FRONT(...) ScreenToFront(__VA_ARGS__)
#define X_SCROLL_VPORT(...) ScrollVPort(__VA_ARGS__)
#define X_LOAD_RGB32(...) LoadRGB32(__VA_ARGS__)
#define X_WAIT_TOF(...) WaitTOF(__VA_ARGS__)
#define X_GET_DEFAULT_PUB_SCREEN(...) GetDefaultPubScreen(__VA_ARGS__)
#define X_EASY_REQUEST(...) EasyRequestArgs(__VA_ARGS__)

/* aar.library, through the program's AARBase */
#define X_AAR_STATUS(...) AARStatus(__VA_ARGS__)
#define X_AAR_STOP(...) AARStop(__VA_ARGS__)
#define X_AAR_SERVICE_NEXT(...) AARServiceNext(__VA_ARGS__)
#define X_AAR_SERVICE_REPLY(...) AARServiceReply(__VA_ARGS__)
#define X_AAR_POST_EVENT(...) AARPostEvent(__VA_ARGS__)
#define X_AAR_AUDIO_READ(...) AARAudioRead(__VA_ARGS__)
#define X_AAR_SHARED_TOKEN(...) AARSharedToken(__VA_ARGS__)
#define X_AAR_MESSAGE_SIGNAL(...) AARMessageSignal(__VA_ARGS__)
#define X_AAR_SET_LOG_SINK(...) AARSetLogSink(__VA_ARGS__)
#define X_AAR_QUERY(...) AARQuery(__VA_ARGS__)
#define X_AAR_GET_MESSAGE(...) AARGetMessage(__VA_ARGS__)

/* Calls that need a library base the program does not have: aar_serve opens
 * timer.device and Picasso96API.library itself and keeps the bases in the
 * instance. The first argument is the instance. */
#define X_GET_SYS_TIME(s, tv) \
    ({ struct Device *TimerBase = (s)->tm.base; GetSysTime(tv); })
#define X_P96_BEST_MODE_ID(s, ...) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96BestModeIDTags(__VA_ARGS__); })
#define X_P96_OPEN_SCREEN(s, ...) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96OpenScreenTags(__VA_ARGS__); })
#define X_P96_CLOSE_SCREEN(s, scr) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96CloseScreen(scr); })
#define X_P96_BITMAP_ATTR(s, bm, attr) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96GetBitMapAttr(bm, attr); })

/* The custom chip registers the joystick sampling reads (no state changes). */
#define X_JOYDAT(port) (*((port) ? (volatile UWORD *)0xDFF00CUL : (volatile UWORD *)0xDFF00AUL))
#define X_CIAA_PRA() (*(volatile UBYTE *)0xBFE001UL)
#endif /* AAR_SERVE_HOST */

/* ---- The instance ---------------------------------------------------------------------- */

/* Whether the caller's hook table has a hook: the table is size-first and may
 * be older than this header. */
#define AAR_SERVE_HOOK(s, field)                                              \
    ((s)->cfg.hooks != NULL &&                                                \
     (s)->cfg.hooks->size >= offsetof(aar_serve_hooks, field) +               \
                             sizeof((s)->cfg.hooks->field) &&                 \
     (s)->cfg.hooks->field != NULL)

#define AAR_SERVE_AHI_BUFFER_COUNT 2u
#define AAR_SERVE_AHI_BUFFER_BYTES 8192u

/* One walk over the candidates of a DOS name: GetDeviceProc candidates, or one
 * fixed handler and directory lock (a relative parent, or a candidate pinned
 * after a probe). A pinned resolver may still own the DevProc that its
 * directory lock belongs to, so it is released only when the operation ends. */
struct aar_serve_resolver {
    const char *name;
    struct DevProc *devproc;
    struct MsgPort *handler;
    BPTR directory;
    int fixed_used;
};

/* The object one open acts on. dir is the parent lock of a relative name, or
 * 0 for an absolute (ABS) name that GetDeviceProc resolves. Resolving an
 * absolute name restarts at the first directory of a multi-directory assign,
 * so the probe pins the resolver to the candidate that holds the file and
 * every later packet of the same open (update, truncate, NEWFILE fallback)
 * reaches that file, never a new one made in an earlier directory. */
struct aar_serve_target {
    const char *name;
    BPTR dir;
    struct aar_serve_resolver resolver;
};

/* A completion that found the completion queue full waits here. */
struct aar_serve_pending_reply {
    struct aar_serve_req req;
    struct aar_serve_done done;
};

/*
 * Each section belongs to one backend file and is touched by that file only,
 * except where a section says otherwise. A backend that needs more state adds
 * fields to its own section.
 */
struct aar_serve {
    /* ---- Loop: the handle, the configuration and the state of the run ------------------ */
    AARHandle *handle;                  /* the caller's; never opened or closed here */
    aar_serve_config cfg;               /* the caller's configuration with the defaults applied */
    const char *title;                  /* cfg.screen_title, or "aar-serve" */
    BPTR root;                          /* a duplicate of the ROOT lock; unlocked at destroy */
    int claim_lost;
    int stop_asked;
    int user_stop;
    int stop_event_sent;
    uint32_t stop_ms;
    uint32_t app_state;
    uint32_t grace_ms;                  /* AARInfo.stop_grace_ms at create; 0: AAR_SERVE_GRACE_MS */
    uint32_t dropped_messages;          /* app messages drained and dropped */
    int saw_fault;
    int ownership_lost;
    int queue_corrupt;
    int display_failed;
    int console_output_failed;
    int audio_open_failed;
    struct aar_serve_core core;         /* the OS-free handlers: files, console, display, audio */
    struct aar_serve_events events;
    struct aar_serve_pending_reply reply_backlog[AAR_SERVE_REPLY_BACKLOG];
    unsigned reply_n;

    /* ---- aar_serve_dos.c: the DOS backend and the packet engine ------------------------ */
    struct {
        struct aar_serve_dos_ops ops;   /* the table the file handlers call; ctx is the instance */
        struct aar_serve_target target;
    } dos;

    /* ---- aar_serve_con.c: the console ------------------------------------------------------ */
    struct {
        struct MsgPort *port;           /* replies of both packets */
        struct StandardPacket *packet;  /* the read */
        UBYTE *buffer;
        BPTR fh;
        int pending;
        int interactive;
        struct StandardPacket *wpacket; /* the write */
        UBYTE *wbuffer;
        int wpending;
        int winteractive;
        int wsync;                      /* a Write() on a handle with no handler port finished; */
        LONG wsync_result;              /* ... with this result, delivered by the next pass */
        BPTR in;                        /* cfg.console_in, or 0 */
        BPTR handle;                    /* cfg.console_out, or the console aar_serve opened */
        int owned;                      /* handle is aar_serve's to close */
        CONST_STRPTR spec;              /* cfg.console_spec */
        int open_failed;                /* the console could not be opened, said once */
    } con;

    /* ---- aar_serve_gfx.c: the P96 screen and the input window ------------------------- */
    struct {
        struct Library *p96;            /* Picasso96API.library, or NULL */
        struct Screen *screen;
        struct Window *input_win;       /* also read by aar_serve_input.c */
        struct ScreenBuffer *buf_front;
        struct ScreenBuffer *buf_back;
        int have_two;                   /* two screen buffers flipped by ChangeScreenBuffer */
        /* Panning fallback uses a triple-height bitmap when P96 permits it. The
         * spare third page lets the app start the next frame while the last pan
         * is pending; a two-page bitmap remains a compatible fallback. */
        int have_pan;
        int pan_count;
        char pub_name[128];
        struct MsgPort *safe_port;      /* SafeMessage replies of the flips */
        uint32_t flip_pending;
        uint32_t safe_pending;
    } gfx;

    /* ---- aar_serve_ahi.c: AHI audio -------------------------------------------------------- */
    struct {
        struct MsgPort *port;
        struct AHIRequest *request[AAR_SERVE_AHI_BUFFER_COUNT];
        int device_open;                /* OpenDevice's result, -1: not opened */
        uint32_t rate;
        uint32_t channels;
        uint32_t pending_bytes[AAR_SERVE_AHI_BUFFER_COUNT];
        uint8_t *buffer[AAR_SERVE_AHI_BUFFER_COUNT];
        uint8_t pending[AAR_SERVE_AHI_BUFFER_COUNT];
    } ahi;

    /* ---- aar_serve_input.c: the input window's events and the joystick ---------------- */
    struct {
        uint16_t mouse_buttons;
        ULONG joy_last[2];
        int joy_valid[2];
    } in;

    /* ---- The timer (loop, and the packet engine's wait) ------------------------------------- */
    struct {
        struct MsgPort *port;
        struct timerequest *io;
        struct Device *base;            /* timer.device's base for GetSysTime */
        int pending;
    } tm;
};

/* ---- Services the backends take from the loop (defined in aar_serve.c) ------------------- */

static int claim_live(struct aar_serve *s);
static LONG claim_note(struct aar_serve *s, LONG rc);
static int claim_check(struct aar_serve *s);
static uint32_t now_ms(struct aar_serve *s);
static int ctrl_c_set(void);
static void timer_arm(struct aar_serve *s);
static void timer_reap(struct aar_serve *s);
static void push_event(struct aar_serve *s, uint16_t type, uint16_t code,
                       int16_t dx, int16_t dy, uint16_t buttons);
/* Stop the app (the old request_stop). */
static void request_stop(struct aar_serve *s);
/* The loop wants the app stopped, for AAR_SERVE_WHY_*: the stop_wanted hook, or
 * request_stop when there is none. */
static void want_stop(struct aar_serve *s, ULONG why);
/* A launcher diagnostic line; the hook, or the console writer's queue. */
static void launcher_status(struct aar_serve *s, const char *msg);
/* The same, with the program's title in front ("title: text"). */
static void launcher_say(struct aar_serve *s, const char *text);
static void serve_requester(struct aar_serve *s, const char *msg);

#endif /* AAR_SERVE_PRIV_H */
