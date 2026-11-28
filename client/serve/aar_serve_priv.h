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
#include <libraries/aar_zz9000.h>

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
#include <graphics/displayinfo.h>
#include <intuition/intuitionbase.h>
#include <intuition/pointerclass.h>
#include <libraries/lowlevel.h>
#include <proto/keymap.h>
#include <proto/lowlevel.h>
#include <proto/aar.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/timer.h>
#include <devices/clipboard.h>
#include <libraries/iffparse.h>
#include <libraries/locale.h>
#include <proto/iffparse.h>
#include <proto/locale.h>

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

/* Time, locale, environment and clipboard (aar_serve_host.c). The calls of
 * libraries the program does not have take the instance first, like the
 * timer's. */
#define X_DATE_STAMP(...) DateStamp(__VA_ARGS__)
#define X_GET_VAR(...) GetVar(__VA_ARGS__)
#define X_OPEN_LOCALE(s, name) \
    ({ struct Library *LocaleBase = (s)->host.locale; OpenLocale(name); })
#define X_CLOSE_LOCALE(s, loc) \
    ({ struct Library *LocaleBase = (s)->host.locale; CloseLocale(loc); })
#define X_OPEN_CLIPBOARD(s, unit) \
    ({ struct Library *IFFParseBase = (s)->host.iffparse; OpenClipboard(unit); })
#define X_CLOSE_CLIPBOARD(s, cb) \
    ({ struct Library *IFFParseBase = (s)->host.iffparse; CloseClipboard(cb); })

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
/* The ZZ9000 extension table of the library, or NULL on another platform
 * (callers test AAR_CLIENT_EXT_ZZ9000_COVERS), and its surface calls. The
 * memory comes back as a plain pointer. */
#define X_AAR_EXT() \
    ((const aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT))
#define X_AAR_SURFACE_ALLOC(ext, bytes, handle, mem) \
    ((ext)->surface_alloc((bytes), (handle), (APTR *)(mem)))
#define X_AAR_SURFACE_FREE(ext, handle) ((ext)->surface_free(handle))
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

/* Display, pointer, title, modes, text input and CD32 pads (API minor 6). */
#define X_SET_WINDOW_TITLES(win, wt, st) SetWindowTitles(win, wt, st)
#define X_SET_WINDOW_POINTER(...) SetWindowPointer(__VA_ARGS__)
#define X_NEW_POINTER(bm, xo, yo) \
    NewObject(NULL, (CONST_STRPTR)"pointerclass", POINTERA_BitMap, (ULONG)(bm), \
              POINTERA_XOffset, (ULONG)(xo), POINTERA_YOffset, (ULONG)(yo), TAG_DONE)
#define X_DISPOSE_POINTER(obj) DisposeObject(obj)
#define X_GET_PREFS(prefs, size) GetPrefs(prefs, size)
#define X_ALLOC_BITMAP(w, h, depth, flags, friend) AllocBitMap(w, h, depth, flags, friend)
#define X_FREE_BITMAP(bm) FreeBitMap(bm)
#define X_GET_VP_MODE_ID(vp) GetVPModeID(vp)
#define X_NEXT_DISPLAY_INFO(id) NextDisplayInfo(id)
#define X_GET_DISPLAY_INFO(buf, size, tag, id) \
    GetDisplayInfoData(NULL, (UBYTE *)(buf), size, tag, id)
#define X_INTUITION_VERSION() ((ULONG)IntuitionBase->LibNode.lib_Version)
#define X_P96_WRITE_PIXEL_ARRAY(s, ri, sx, sy, rp, dx, dy, w, h) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96WritePixelArray(ri, sx, sy, rp, dx, dy, w, h); })
#define X_P96_RECT_FILL(s, rp, x0, y0, x1, y1, argb) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96RectFill(rp, x0, y0, x1, y1, argb); })
#define X_P96_MODE_ATTR(s, id, attr) \
    ({ struct Library *P96Base = (s)->gfx.p96; p96GetModeIDAttr(id, attr); })
#define X_READ_JOY_PORT(s, port) \
    ({ struct Library *LowLevelBase = (s)->in.ll; ReadJoyPort(port); })
#define X_MAP_RAW_KEY(s, ie, buf, len) \
    ({ struct Library *KeymapBase = (s)->in.keymap; MapRawKey(ie, buf, len, NULL); })

/* The custom chip registers the joystick sampling reads (no state changes). */
#define X_JOYDAT(port) (*((port) ? (volatile UWORD *)0xDFF00CUL : (volatile UWORD *)0xDFF00AUL))
#define X_CIAA_PRA() (*(volatile UBYTE *)0xBFE001UL)
#endif /* AAR_SERVE_HOST */

/* Intuition V51 wheel message ABI. The m68k SDK predates these names,
 * but an m68k client can receive this message on AmigaOS 4. */
#define AAR_IDCMP_EXTENDEDMOUSE 0x08000000UL
#define AAR_IMSGCODE_INTUIWHEELDATA 0x8000u
#define AAR_INTUIWHEELDATA_VERSION 1u

/* ---- The instance ---------------------------------------------------------------------- */

/* Whether the caller's hook table has a hook: the table is size-first and may
 * be older than this header. */
#define AAR_SERVE_HOOK(s, field)                                              \
    ((s)->cfg.hooks != NULL &&                                                \
     (s)->cfg.hooks->size >= offsetof(aar_serve_hooks, field) +               \
                             sizeof((s)->cfg.hooks->field) &&                 \
     (s)->cfg.hooks->field != NULL)

#define AAR_SERVE_AHI_BUFFER_COUNT 2u   /* requests in flight: the guide's double buffering */
#define AAR_SERVE_AHI_BUFFER_BYTES 24576u /* one request: 3/50 s up to 96 kHz stereo */
#define AAR_SERVE_AHI_RESERVE_BYTES 65536u /* the reserve between the ring and the requests */

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

struct aar_serve_file_packet {
    struct StandardPacket packet;
    uint8_t buffer[AAR_SERVE_IO_CHUNK];
};

struct aar_serve_file_transfer {
    struct MsgPort *port;
    struct aar_serve_file_packet *storage;
    struct aar_serve_req req;
    BPTR fh;                     /* nonzero while a DOS packet owns storage */
    uint32_t count;              /* bytes already transferred */
    uint32_t chunk;              /* bytes requested in the packet now out */
};

struct aar_serve_requester;

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
    uint32_t runtime_dropped_logs;      /* AARStatusInfo.log_dropped at the last full status read */
    int saw_fault;
    int in_step;                        /* aar_serve_step is running: it arms the heartbeat itself */
    int closing;                        /* aar_serve_destroy is running: no timer is armed any more */
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
        struct aar_serve_file_transfer file;
    } dos;

    struct aar_serve_requester *requester; /* Native ASL dialog in flight. */

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
        /* A display in a window, or one that is scaled, has a surface of its own
         * (a bitmap on the board) that each flip shows. */
        struct Window *win;             /* the display window of a window display */
        struct Screen *pub;             /* the locked public screen it is on */
        void *surf_mem;                 /* its pixels: a stable allocation, never moved or locked */
        ULONG surf_handle;              /* the platform's handle of it (0: Amiga memory from X_ALLOC_MEM) */
        ULONG surf_bytes;               /* its size */
        struct RenderInfo surf_ri;      /* what p96WritePixelArray reads: surf_mem, its pitch and format */
        ULONG surf_rgb;                 /* its RGBFB_* */
        uint32_t surf_w, surf_h, surf_bpp;
        int scaled;                     /* flips show the surface scaled */
        uint32_t caps;                  /* the AAR_DISPCAP_* granted at the open */
        uint32_t fit_x, fit_y, fit_w, fit_h; /* where a scaled surface is drawn, in the inner area */
        uint32_t shown_w, shown_h;      /* the inner size that rectangle was made for (0: clear again) */
        uint8_t *strip;                 /* scaled rows on their way to p96WritePixelArray */
        uint32_t strip_bytes;
        uint32_t scr_w, scr_h;          /* size of the screen the surface is on */
        ULONG mode_id;                  /* the DisplayID shown (0 unknown) */
        char title_buf[256];            /* the title of the window or screen, ISO-8859-1 */
        APTR pointer_obj;               /* the custom pointer set, or NULL */
        struct BitMap *pointer_bm;
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
        volatile uint8_t *ring;         /* attached AUDIO_OPEN payload, until audio close */
        uint32_t ring_underruns;        /* last count observed in this ring */
        uint32_t underruns;             /* accumulated across AHI streams */
        uint8_t *reserve;               /* the FIFO between the ring and the requests */
        uint32_t reserve_frames;        /* its capacity */
        uint32_t ring_frames;           /* the app's ring: no request or prebuffer is larger */
        uint32_t rsv_head;              /* first frame in the reserve */
        uint32_t rsv_fill;              /* frames in the reserve */
        uint32_t last_data_ms;          /* when the ring last gave frames */
        int started;                    /* prebuffered: whole requests are sent */
        int dry;                        /* AHI ran dry since it was started (counted once) */
        int draining;                   /* the app closed the audio: the tail plays out */
        int tail;                       /* a last, short request was sent: the end of a sound */
        uint32_t requests;              /* requests sent */
        uint32_t min_frames;            /* the shortest of them */
        uint32_t total_frames;          /* frames in all of them */
        uint32_t gaps;                  /* refills that found AHI already idle with data waiting */
        unsigned last;                  /* slot of the request submitted last: the next one chains to it */
        unsigned passes;                /* feeder passes, to sample the ring's underrun word now and then */
    } ahi;

    /* ---- aar_serve_input.c: the input window's events and the joystick ---------------- */
    struct {
        uint16_t mouse_buttons;
        ULONG joy_last[2];
        int joy_valid[2];
        struct Library *ll;             /* lowlevel.library, when cfg.cd32_ports asks for it */
        struct Library *keymap;         /* keymap.library, opened with a TEXT_INPUT display */
        uint32_t cd32_ms[2];            /* when ReadJoyPort last read each port */
    } in;

    /* ---- The timer (loop, and the packet engine's wait) ------------------------------------- */
    struct {
        struct MsgPort *port;
        struct timerequest *io;
        struct Device *base;            /* timer.device's base for GetSysTime */
        int pending;
    } tm;

    /* ---- aar_serve_host.c: locale, environment and clipboard ---------------------------- */
    struct {
        struct Library *locale;         /* locale.library, opened at the first use, or NULL */
        struct Library *iffparse;       /* iffparse.library, likewise: it opens clipboard.device */
        int locale_tried;
        int iffparse_tried;
    } host;
};

/* ---- Services the backends take from the loop (defined in aar_serve.c) ------------------- */

static int claim_live(struct aar_serve *s);
static LONG claim_note(struct aar_serve *s, LONG rc);
static int claim_check(struct aar_serve *s);
static uint32_t now_ms(struct aar_serve *s);
static int ctrl_c_set(struct aar_serve *s);
#define AAR_SERVE_WATCHDOG_US 250000UL /* the longest the loop sleeps without a heartbeat (poll_us 0) */
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
