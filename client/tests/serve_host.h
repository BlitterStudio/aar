/*
 * The fake system under the host test of aar_serve (client/tests/serve_loop_test.c).
 *
 * client/serve/aar_serve.c and its backends reach Exec, dos.library, Intuition,
 * graphics.library, Picasso96, AHI, timer.device and aar.library only through
 * X_* bindings (aar_serve_priv.h). The test defines AAR_SERVE_HOST and puts this
 * header on the include path; the bindings below then drive a small simulated
 * machine on the host:
 *
 *   - one task with signal bits, message ports and a virtual clock (microseconds).
 *     Wait() runs the simulation forward to the next event (a timer, a DOS reply,
 *     an AHI buffer, a flip's SafeMessage, a scripted event of the test) and
 *     aborts the test when nothing could ever wake the task;
 *   - memory from a static pool below 4 GiB (a DOS packet carries 32-bit words),
 *     poisoned when it is freed, with every block counted;
 *   - DOS: a volume with files and directories behind a handler port that
 *     answers packets after a delay (or never), locks and file handles, a
 *     console handler, a handle with no handler port (NIL:), GetDeviceProc with
 *     assigns of several directories, IoErr. A packet's memory is checked to
 *     be alive when the handler replies, and every packet sent is counted;
 *   - Intuition, graphics and Picasso96: screens whose bitmaps are in the
 *     reference platform's arena (so AARSharedToken resolves them), screen
 *     buffers with one SafeMessage per successful change, a backdrop window with
 *     an IDCMP port, the palette, panning;
 *   - AHI and timer.device: I/O requests that complete after a time, with the
 *     rules of the real calls (no write after close, AbortIO and WaitIO before
 *     the memory goes);
 *   - aar.library: the calls go to the portable core (aar_core.c), which runs on
 *     the reference platform (refplat.c).
 *
 * Each binding checks the rule the real call has and aborts the test when the
 * code under test breaks it.
 *
 * Needs client/tests/stubs and a link without PIE (or -m32): the pool, the
 * arena and a DOS packet's words are below 4 GiB.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef SERVE_HOST_H
#define SERVE_HOST_H

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aar_core.h"
#include "../src/refplat.h"

#ifdef __linux__
#include <execinfo.h>
#define HX_BACKTRACE()                                                            \
    do {                                                                          \
        void *hx_frames[24];                                                      \
        backtrace_symbols_fd(hx_frames, backtrace(hx_frames, 24), 2);             \
    } while (0)
#else
#define HX_BACKTRACE() ((void)0)
#endif
#define HX_UNUSED __attribute__((unused))
#define HX_FAIL(msg)                                                              \
    do {                                                                          \
        fprintf(stderr, "fake system: %s (%s:%d)\n", msg, __FILE__, __LINE__);    \
        fflush(stderr);                                                           \
        HX_BACKTRACE();                                                           \
        abort();                                                                  \
    } while (0)

/* An APTR is a pointer here, as on the Amiga. */
#define APTR void *
/* The fake has its own messages: pointers where the stub's 68k layout has words. */
#define Message hxMessage
#define Node hxNode
#define timeval hxTimeval

/* ---- Constants ---------------------------------------------------------------- */

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1UL
#define MEMF_CLEAR 0x10000UL
#define SIGBREAKF_CTRL_C_UNUSED 0
#define NT_MESSAGE 5

#define ERROR_NO_FREE_STORE 103
#define ERROR_OBJECT_IN_USE 202
#define ERROR_OBJECT_EXISTS 203
#define ERROR_DIR_NOT_FOUND 204
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_OBJECT_TOO_LARGE 207
#define ERROR_ACTION_NOT_KNOWN 209
#define ERROR_OBJECT_WRONG_TYPE 212
#define ERROR_DEVICE_NOT_MOUNTED 218
#define ERROR_SEEK_ERROR 219
#define ERROR_DISK_FULL 221
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_BREAK 304

#define ACTION_LOCATE_OBJECT 8
#define ACTION_FREE_LOCK 15
#define ACTION_EXAMINE_OBJECT 23
#define ACTION_EXAMINE_NEXT 24
#define ACTION_FINDUPDATE 1004
#define ACTION_FINDINPUT 1005
#define ACTION_FINDOUTPUT 1006
#define ACTION_END 1007
#define ACTION_SEEK 1008
#define ACTION_SET_FILE_SIZE 1022
#define ACTION_READ 82
#define ACTION_WRITE 87

#define LOCK_SAME 0
#define LOCK_DIFFERENT (-1)
#define ACCESS_READ (-2)
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define MODE_READWRITE 1004
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT 0
#define OFFSET_END 1
#define DOS_FILEHANDLE 0
#define DOS_FIB 2

#define CMD_WRITE 3
#define TR_ADDREQUEST 9
#define TIMERNAME "timer.device"
#define UNIT_MICROHZ 0
#define AHINAME "ahi.device"
#define AHI_DEFAULT_UNIT 0
#define AHIST_S16S 3

#define IDCMP_MOUSEBUTTONS 0x00000008UL
#define IDCMP_MOUSEMOVE 0x00000010UL
#define IDCMP_CLOSEWINDOW 0x00000200UL
#define IDCMP_RAWKEY 0x00000400UL
#define IDCMP_DELTAMOVE 0x00100000UL
#define IECODE_UP_PREFIX 0x80
#define IEQUALIFIER_CONTROL 0x0008
#define SELECTDOWN 0x68
#define SELECTUP 0xE8
#define MENUDOWN 0x69
#define MENUUP 0xE9
#define MIDDLEDOWN 0x6A
#define MIDDLEUP 0xEA
#define WFLG_WINDOWACTIVE 0x2000
#define SB_SCREEN_BITMAP 1

#define WA_Left (TAG_USER + 100)
#define WA_Top (TAG_USER + 101)
#define WA_Width (TAG_USER + 102)
#define WA_Height (TAG_USER + 103)
#define WA_IDCMP (TAG_USER + 107)
#define WA_Borderless (TAG_USER + 109)
#define WA_Activate (TAG_USER + 110)
#define WA_CustomScreen (TAG_USER + 121)
#define WA_ReportMouse (TAG_USER + 126)
#define WA_Backdrop (TAG_USER + 130)
#define WA_RMBTrap (TAG_USER + 132)
#define WA_NoCareRefresh (TAG_USER + 135)

#define INVALID_ID ((ULONG)~0UL)
#define RGBFB_CLUT 0
#define RGBFB_R5G6B5 3
#define RGBFB_B8G8R8A8 11
#define RGBFF_CLUT (1UL << RGBFB_CLUT)
#define RGBFF_R5G6B5 (1UL << RGBFB_R5G6B5)
#define RGBFF_B8G8R8A8 (1UL << RGBFB_B8G8R8A8)
#define P96BMA_MEMORY 1
#define P96BMA_BYTESPERROW 2
#define P96BMA_RGBFORMAT 3
#define P96SA_Left (TAG_USER + 0x10000)
#define P96SA_Width (TAG_USER + 0x10001)
#define P96SA_Height (TAG_USER + 0x10002)
#define P96SA_Depth (TAG_USER + 0x10003)
#define P96SA_DisplayID (TAG_USER + 0x10004)
#define P96SA_Title (TAG_USER + 0x10005)
#define P96SA_Quiet (TAG_USER + 0x10006)
#define P96SA_ShowTitle (TAG_USER + 0x10007)
#define P96SA_AutoScroll (TAG_USER + 0x10008)
#define P96SA_RGBFormat (TAG_USER + 0x10009)
#define P96BIDTAG_NominalWidth (TAG_USER + 0x20000)
#define P96BIDTAG_NominalHeight (TAG_USER + 0x20001)
#define P96BIDTAG_Depth (TAG_USER + 0x20002)
#define P96BIDTAG_FormatsAllowed (TAG_USER + 0x20003)

/* ---- The system's types -------------------------------------------------------- */

struct Library {
    int hx_dummy;
};
struct Device {
    int hx_dummy;
};
struct GfxBase {
    int hx_dummy;
};
struct IntuitionBase {
    int hx_dummy;
};
struct DevProc;
struct timeval {
    ULONG tv_secs;
    ULONG tv_micro;
};

struct Node {
    char *ln_Name;
    UBYTE ln_Type;
};

struct MsgPort;
struct hxMessage {
    struct Node mn_Node;
    struct MsgPort *mn_ReplyPort;
    UWORD mn_Length;
};

#define HX_PORT_Q 64
#define HX_K_PLAIN 0
#define HX_K_VOLUME 1
#define HX_K_CONSOLE 2
#define HX_K_PIPE 3

struct MsgPort {
    struct Message *q[HX_PORT_Q];
    int n;
    int mp_SigBit;
    int kind;                    /* what answers packets put on it (HX_K_*) */
    int pooled;                  /* made by CreateMsgPort */
    unsigned long posts;
};

struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    void *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
    /* the simulation */
    int hx_pending;
    int hx_done;
};
struct IOStdReq {
    struct IORequest io;
    ULONG io_Actual;
    ULONG io_Length;
    void *io_Data;
    ULONG io_Offset;
};
/* AHIRequest and timerequest as the code under test reads them. */
struct timerequest {
    struct IORequest tr_node;
    struct timeval tr_time;
};
struct AHIRequest {
    struct {
        struct Message io_Message;
        struct Device *io_Device;
        void *io_Unit;
        UWORD io_Command;
        UBYTE io_Flags;
        BYTE io_Error;
        int hx_pending;
        int hx_done;
        ULONG io_Actual;
        ULONG io_Length;
        void *io_Data;
        ULONG io_Offset;
    } ahir_Std;
    UWORD ahir_Version;
    UWORD ahir_Type;
    ULONG ahir_Frequency;
    LONG ahir_Volume;
    LONG ahir_Position;
    struct AHIRequest *ahir_Link;
};

struct DosPacket {
    struct Message *dp_Link;
    struct MsgPort *dp_Port;
    LONG dp_Type;
    LONG dp_Res1;
    LONG dp_Res2;
    LONG dp_Arg1, dp_Arg2, dp_Arg3, dp_Arg4, dp_Arg5;
};
struct StandardPacket {
    struct Message sp_Msg;
    struct DosPacket sp_Pkt;
};
struct FileHandle {
    struct MsgPort *fh_Type;
    LONG fh_Arg1;
    int hx_interactive;
    int hx_owned;                /* Open()'s */
};
struct FileLock {
    struct MsgPort *fl_Task;
    LONG fl_Key;                 /* the node */
};
struct DateStamp {
    LONG ds_Days, ds_Minute, ds_Tick;
};
struct FileInfoBlock {
    LONG fib_DirEntryType;
    char fib_FileName[108];
    LONG fib_Size;
    struct DateStamp fib_Date;
    LONG hx_next;                /* EXAMINE_NEXT's position */
};
struct DevProc {
    struct MsgPort *dvp_Port;
    BPTR dvp_Lock;
    int hx_index;
};

struct BitMap {
    UBYTE *Planes[8];
    UWORD Rows;
    UWORD BytesPerRow;
    int hx_rgb;
};
struct RasInfo {
    WORD RyOffset;
};
struct ViewPort {
    struct RasInfo *RasInfo;
};
struct RastPort {
    struct BitMap *BitMap;
};
struct Screen {
    WORD Width, Height;
    struct RastPort RastPort;
    struct ViewPort ViewPort;
    struct RasInfo ras;
    struct BitMap bm;
};
struct DBufInfo {
    struct Message dbi_SafeMessage;
};
struct ScreenBuffer {
    struct BitMap *sb_BitMap;
    struct DBufInfo *sb_DBufInfo;
    struct BitMap own;
    int hx_safe_out;             /* a SafeMessage is still to come */
    int hx_changes;
};
struct Window {
    struct MsgPort *UserPort;
    UWORD Flags;
    WORD Width, Height;
    struct Screen *WScreen;
};
struct IntuiMessage {
    struct Message ExecMessage;
    ULONG Class;
    UWORD Code, Qualifier;
    WORD MouseX, MouseY;
};
struct EasyStruct {
    ULONG es_StructSize, es_Flags;
    UBYTE *es_Title, *es_TextFormat, *es_GadgetFormat;
};

/* BCPL pointers: an address shifted by two (everything is below 16 GiB). */
#define BADDR(b) ((void *)((uintptr_t)(uint32_t)(b) << 2))
#define MKBADDR(p) ((BPTR)((uintptr_t)(p) >> 2))

/* The libraries the program has: IntuitionBase and GfxBase are set non-NULL. */
static struct IntuitionBase hx_intuition_base;
static struct GfxBase hx_gfx_base;
HX_UNUSED static struct IntuitionBase *IntuitionBase = &hx_intuition_base;
HX_UNUSED static struct GfxBase *GfxBase = &hx_gfx_base;

/* ---- Memory ----------------------------------------------------------------------- */

#define HX_SMALL 2048u
#define HX_SMALLS 768u
#define HX_BIG 65536u
#define HX_BIGS 8u

static uint8_t hx_small[HX_SMALLS][HX_SMALL] __attribute__((aligned(16)));
static uint8_t hx_big[HX_BIGS][HX_BIG] __attribute__((aligned(16)));
static uint32_t hx_small_size[HX_SMALLS], hx_big_size[HX_BIGS];
static uint8_t hx_small_used[HX_SMALLS], hx_big_used[HX_BIGS];
HX_UNUSED static unsigned long hx_live_blocks;
HX_UNUSED static unsigned long hx_live_bytes;
HX_UNUSED static int hx_fail_alloc;        /* the next n allocations fail */

HX_UNUSED static void *hx_alloc(uint32_t bytes, int clear)
{
    uint32_t i;

    if (hx_fail_alloc > 0) {
        hx_fail_alloc--;
        return NULL;
    }
    if (bytes == 0)
        HX_FAIL("allocation of zero bytes");
    if (bytes <= HX_SMALL) {
        for (i = 0; i < HX_SMALLS; i++) {
            if (!hx_small_used[i]) {
                hx_small_used[i] = 1;
                hx_small_size[i] = bytes;
                hx_live_blocks++;
                hx_live_bytes += bytes;
                memset(hx_small[i], clear ? 0 : 0xCD, HX_SMALL);
                return hx_small[i];
            }
        }
        HX_FAIL("small pool exhausted");
    }
    if (bytes > HX_BIG)
        HX_FAIL("allocation larger than a pool slot");
    for (i = 0; i < HX_BIGS; i++) {
        if (!hx_big_used[i]) {
            hx_big_used[i] = 1;
            hx_big_size[i] = bytes;
            hx_live_blocks++;
            hx_live_bytes += bytes;
            memset(hx_big[i], clear ? 0 : 0xCD, HX_BIG);
            return hx_big[i];
        }
    }
    HX_FAIL("big pool exhausted");
    return NULL;
}

/* Whether p points into a block that is allocated now. */
HX_UNUSED static int hx_mem_live(const void *p)
{
    uint32_t i;
    const uint8_t *b = (const uint8_t *)p;

    for (i = 0; i < HX_SMALLS; i++)
        if (b >= hx_small[i] && b < hx_small[i] + hx_small_size[i])
            return hx_small_used[i];
    for (i = 0; i < HX_BIGS; i++)
        if (b >= hx_big[i] && b < hx_big[i] + hx_big_size[i])
            return hx_big_used[i];
    return 0;
}

/* Whether a block starts at p and is allocated now. */
HX_UNUSED static int hx_block_live(const void *p)
{
    uint32_t i;

    for (i = 0; i < HX_SMALLS; i++) {
        if ((const void *)hx_small[i] == p)
            return hx_small_used[i];
    }
    for (i = 0; i < HX_BIGS; i++) {
        if ((const void *)hx_big[i] == p)
            return hx_big_used[i];
    }
    return 0;
}

HX_UNUSED static void hx_free(const void *p)
{
    uint32_t i;

    for (i = 0; i < HX_SMALLS; i++) {
        if ((const void *)hx_small[i] == p) {
            if (!hx_small_used[i])
                HX_FAIL("double free");
            hx_small_used[i] = 0;
            hx_live_blocks--;
            hx_live_bytes -= hx_small_size[i];
            memset(hx_small[i], 0xA5, HX_SMALL);
            return;
        }
    }
    for (i = 0; i < HX_BIGS; i++) {
        if ((const void *)hx_big[i] == p) {
            if (!hx_big_used[i])
                HX_FAIL("double free");
            hx_big_used[i] = 0;
            hx_live_blocks--;
            hx_live_bytes -= hx_big_size[i];
            memset(hx_big[i], 0xA5, HX_BIG);
            return;
        }
    }
    HX_FAIL("free of a block the pool does not own");
}

HX_UNUSED static uint32_t hx_block_size(const void *p)
{
    uint32_t i;

    for (i = 0; i < HX_SMALLS; i++)
        if ((const void *)hx_small[i] == p)
            return hx_small_size[i];
    for (i = 0; i < HX_BIGS; i++)
        if ((const void *)hx_big[i] == p)
            return hx_big_size[i];
    HX_FAIL("size of a block the pool does not own");
    return 0;
}

/* ---- The simulation: time, events, signals --------------------------------------------- */

#define HX_EVS 512
struct hx_ev {
    int used;
    uint64_t due;
    unsigned long seq;
    void (*fn)(void *arg);
    void *arg;
};
static struct hx_ev hx_evs[HX_EVS];
HX_UNUSED static uint64_t hx_now;           /* microseconds */
static unsigned long hx_seq;
static uint32_t hx_sig;                     /* the task's pending signals */
static unsigned long hx_steps;
HX_UNUSED static unsigned long hx_waits;    /* Wait() calls */

HX_UNUSED static void hx_sched(uint64_t delay_us, void (*fn)(void *), void *arg)
{
    int i;

    for (i = 0; i < HX_EVS; i++) {
        if (!hx_evs[i].used) {
            hx_evs[i].used = 1;
            hx_evs[i].due = hx_now + delay_us;
            hx_evs[i].seq = ++hx_seq;
            hx_evs[i].fn = fn;
            hx_evs[i].arg = arg;
            return;
        }
    }
    HX_FAIL("too many events");
}

/* Forget the events of fn with this argument (a request that was aborted). */
HX_UNUSED static void hx_cancel(void (*fn)(void *), void *arg)
{
    int i;

    for (i = 0; i < HX_EVS; i++)
        if (hx_evs[i].used && hx_evs[i].fn == fn && hx_evs[i].arg == arg)
            hx_evs[i].used = 0;
}

/* Run the earliest event. 0: there is none. */
HX_UNUSED static int hx_advance(void)
{
    int i, best = -1;
    void (*fn)(void *);
    void *arg;

    if (++hx_steps > 400000UL)
        HX_FAIL("the simulation runs away");
    for (i = 0; i < HX_EVS; i++) {
        if (!hx_evs[i].used)
            continue;
        if (best < 0 || hx_evs[i].due < hx_evs[best].due ||
            (hx_evs[i].due == hx_evs[best].due && hx_evs[i].seq < hx_evs[best].seq))
            best = i;
    }
    if (best < 0)
        return 0;
    if (hx_evs[best].due > hx_now)
        hx_now = hx_evs[best].due;
    fn = hx_evs[best].fn;
    arg = hx_evs[best].arg;
    hx_evs[best].used = 0;
    fn(arg);
    return 1;
}

/* Run every event that is due by now + us, and move the clock there. */
HX_UNUSED static void hx_run_for(uint64_t us)
{
    uint64_t end = hx_now + us;

    for (;;) {
        int i, any = 0;

        for (i = 0; i < HX_EVS; i++)
            if (hx_evs[i].used && hx_evs[i].due <= end)
                any = 1;
        if (!any)
            break;
        hx_advance();
    }
    hx_now = end;
}

static int hx_next_sigbit = 16;

/* ---- Ports and messages ---------------------------------------------------------------- */

static void hx_dos_packet(struct MsgPort *port, struct StandardPacket *pkt);
HX_UNUSED static int hx_trace;                 /* print the packets (the test sets it from the environment) */

HX_UNUSED static struct MsgPort *hx_port_new(void)
{
    struct MsgPort *p = hx_alloc(sizeof(*p), 1);

    if (!p)
        return NULL;
    p->pooled = 1;
    if (hx_next_sigbit > 31)
        hx_next_sigbit = 16;
    p->mp_SigBit = hx_next_sigbit++;
    return p;
}

HX_UNUSED static void hx_port_delete(struct MsgPort *p)
{
    if (p->n != 0)
        HX_FAIL("DeleteMsgPort with a message still queued");
    hx_free(p);
}

HX_UNUSED static void hx_post(struct MsgPort *p, struct Message *m)
{
    int i;

    for (i = 0; i < p->n; i++)
        if (p->q[i] == m)
            HX_FAIL("PutMsg of a message that is already on the port");
    if (p->n >= HX_PORT_Q)
        HX_FAIL("port overflow");
    p->q[p->n++] = m;
    p->posts++;
    hx_sig |= 1UL << p->mp_SigBit;
}

HX_UNUSED static void hx_putmsg(struct MsgPort *p, struct Message *m)
{
    if (!p)
        HX_FAIL("PutMsg to a NULL port");
    if (p->kind != HX_K_PLAIN) {
        /* A packet for a handler: m is the sp_Msg of a StandardPacket. */
        hx_dos_packet(p, (struct StandardPacket *)m);
        return;
    }
    hx_post(p, m);
}

HX_UNUSED static struct Message *hx_getmsg(struct MsgPort *p)
{
    struct Message *m;
    int i;

    if (!p || p->n == 0)
        return NULL;
    m = p->q[0];
    for (i = 1; i < p->n; i++)
        p->q[i - 1] = p->q[i];
    p->n--;
    return m;
}

HX_UNUSED static uint32_t hx_wait(uint32_t mask)
{
    hx_waits++;
    for (;;) {
        uint32_t got = hx_sig & mask;

        if (got) {
            hx_sig &= ~got;
            return got;
        }
        if (!hx_advance())
            HX_FAIL("Wait with nothing that could ever wake the task");
    }
}

HX_UNUSED static void hx_waitport(struct MsgPort *p)
{
    while (p->n == 0)
        if (!hx_advance())
            HX_FAIL("WaitPort with nothing that could ever wake the task");
}

HX_UNUSED static uint32_t hx_setsignal(uint32_t new_bits, uint32_t mask)
{
    uint32_t old = hx_sig;

    hx_sig = (hx_sig & ~mask) | (new_bits & mask);
    return old;
}

/* ---- I/O requests: timer.device and AHI ------------------------------------------------ */

HX_UNUSED static int hx_ahi_fail_open;      /* OpenDevice(ahi.device) fails */
HX_UNUSED static int hx_ahi_open_count;     /* device opens not closed */
HX_UNUSED static int hx_timer_open_count;
HX_UNUSED static int hx_timer_fail_open;
HX_UNUSED static unsigned long hx_ahi_writes;        /* CMD_WRITE requests sent */
HX_UNUSED static unsigned long hx_ahi_bytes;         /* bytes of them */
HX_UNUSED static uint8_t hx_ahi_last[8192];          /* the last buffer sent */
HX_UNUSED static int hx_ahi_pending_now;
HX_UNUSED static int hx_ahi_max_pending;
HX_UNUSED static struct Device hx_timer_dev, hx_ahi_dev;
HX_UNUSED static unsigned long hx_timer_aborts;      /* timer requests aborted while still pending */
HX_UNUSED static uint64_t hx_ahi_end;                /* when the last queued AHI write finishes playing */
HX_UNUSED static struct IORequest *hx_ahi_tail;      /* the AHI write sent last */

static void hx_io_done(void *arg)
{
    struct IORequest *io = arg;

    io->hx_pending = 0;
    io->hx_done = 1;
    if (io->io_Command == CMD_WRITE && io->io_Device == &hx_ahi_dev)
        hx_ahi_pending_now--;
    hx_post(io->io_Message.mn_ReplyPort, &io->io_Message);
}

HX_UNUSED static struct IORequest *hx_create_ioreq(struct MsgPort *port, uint32_t size)
{
    struct IORequest *io;

    if (!port)
        HX_FAIL("CreateIORequest without a port");
    io = hx_alloc(size, 1);
    if (io)
        io->io_Message.mn_ReplyPort = port;
    return io;
}

HX_UNUSED static void hx_delete_ioreq(struct IORequest *io)
{
    if (io->hx_pending)
        HX_FAIL("DeleteIORequest of a request that is still pending");
    hx_free(io);
}

HX_UNUSED static BYTE hx_open_device(const char *name, ULONG unit, struct IORequest *io, ULONG flags)
{
    (void)unit;
    (void)flags;
    if (strcmp(name, TIMERNAME) == 0) {
        if (hx_timer_fail_open)
            return 1;
        hx_timer_open_count++;
        io->io_Device = &hx_timer_dev;
        return 0;
    }
    if (strcmp(name, AHINAME) == 0) {
        if (hx_ahi_fail_open)
            return 1;
        hx_ahi_open_count++;
        io->io_Device = &hx_ahi_dev;
        return 0;
    }
    return 1;
}

HX_UNUSED static void hx_close_device(struct IORequest *io)
{
    if (io->hx_pending)
        HX_FAIL("CloseDevice with a request still pending");
    if (io->io_Device == &hx_timer_dev)
        hx_timer_open_count--;
    else if (io->io_Device == &hx_ahi_dev)
        hx_ahi_open_count--;
    else
        HX_FAIL("CloseDevice of a device that is not open");
    io->io_Device = NULL;
}

HX_UNUSED static void hx_send_io(struct IORequest *io)
{
    if (io->hx_pending)
        HX_FAIL("SendIO of a request that is pending");
    io->hx_pending = 1;
    io->hx_done = 0;
    if (io->io_Device == &hx_timer_dev) {
        struct timerequest *tr = (struct timerequest *)io;

        if (io->io_Command != TR_ADDREQUEST)
            HX_FAIL("timer command");
        hx_sched((uint64_t)tr->tr_time.tv_secs * 1000000u + tr->tr_time.tv_micro, hx_io_done, io);
    } else if (io->io_Device == &hx_ahi_dev) {
        struct AHIRequest *ar = (struct AHIRequest *)io;
        uint64_t us;

        if (io->io_Command != CMD_WRITE)
            HX_FAIL("AHI command");
        if (!ar->ahir_Std.io_Data || ar->ahir_Std.io_Length == 0 || ar->ahir_Frequency == 0)
            HX_FAIL("AHI write without data");
        if (!hx_mem_live(ar->ahir_Std.io_Data))
            HX_FAIL("AHI write from memory that is not allocated");
        hx_ahi_writes++;
        hx_ahi_bytes += ar->ahir_Std.io_Length;
        memcpy(hx_ahi_last, ar->ahir_Std.io_Data,
               ar->ahir_Std.io_Length < sizeof(hx_ahi_last) ? ar->ahir_Std.io_Length : sizeof(hx_ahi_last));
        {
            /* ahi.device chains a write to the one before it, if that is still queued */
            struct IORequest *want = (hx_ahi_tail && hx_ahi_tail != io && hx_ahi_tail->hx_pending) ?
                                         hx_ahi_tail : NULL;

            if ((struct IORequest *)ar->ahir_Link != want)
                HX_FAIL("an AHI write is not chained to the tail of the queue");
            hx_ahi_tail = io;
        }
        hx_ahi_pending_now++;
        if (hx_ahi_pending_now > hx_ahi_max_pending)
            hx_ahi_max_pending = hx_ahi_pending_now;
        us = (uint64_t)ar->ahir_Std.io_Length / 4u * 1000000u / ar->ahir_Frequency;
        /* ahi.device plays the queued writes one after the other */
        if (hx_ahi_end < hx_now)
            hx_ahi_end = hx_now;
        hx_ahi_end += us;
        hx_sched(hx_ahi_end - hx_now, hx_io_done, io);
    } else {
        HX_FAIL("SendIO on a request with no device");
    }
}

HX_UNUSED static int hx_check_io(struct IORequest *io)
{
    return io->hx_done || !io->hx_pending;
}

HX_UNUSED static void hx_abort_io(struct IORequest *io)
{
    if (!io->hx_pending)
        return;
    if (io->io_Device == &hx_timer_dev)
        hx_timer_aborts++;
    hx_cancel(hx_io_done, io);
    io->io_Error = -2;
    hx_io_done(io);
}

HX_UNUSED static BYTE hx_wait_io(struct IORequest *io)
{
    int i;
    struct MsgPort *p = io->io_Message.mn_ReplyPort;

    while (io->hx_pending)
        if (!hx_advance())
            HX_FAIL("WaitIO with nothing that could ever complete it");
    for (i = 0; i < p->n; i++) {
        if (p->q[i] == &io->io_Message) {
            for (; i + 1 < p->n; i++)
                p->q[i] = p->q[i + 1];
            p->n--;
            break;
        }
    }
    io->hx_done = 0;
    return io->io_Error;
}

/* ---- DOS: the volume, the handlers, locks and handles ----------------------------------- */

#define HX_NODES 48
struct hx_node {
    int used;
    int parent;
    char name[24];
    int is_dir;
    int held;                    /* locks of it fail with OBJECT_IN_USE */
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    uint32_t mtime_days;
};
#define HX_FILE_CAP 65536u
HX_UNUSED static struct hx_node hx_nodes[HX_NODES];

#define HX_OPEN_FILES 32
struct hx_open {
    int used;
    int node;
    uint32_t pos;
};
static struct hx_open hx_opens[HX_OPEN_FILES];

/* The ports of the handlers, and the devices GetDeviceProc knows. */
static struct MsgPort hx_vol_port = {{0}, 0, 20, HX_K_VOLUME, 0, 0};
static struct MsgPort hx_vol2_port = {{0}, 0, 21, HX_K_VOLUME, 0, 0};
static struct MsgPort hx_con_port = {{0}, 0, 22, HX_K_CONSOLE, 0, 0};
static struct MsgPort hx_pipe_port = {{0}, 0, 23, HX_K_PIPE, 0, 0};

HX_UNUSED static unsigned long hx_packets;              /* packets sent to a handler */
HX_UNUSED static unsigned long hx_packets_by[8];        /* 0 locate 1 free 2 examine 3 open 4 end 5 read 6 write 7 other */
HX_UNUSED static uint64_t hx_vol_delay_us = 200;
HX_UNUSED static int hx_vol_never;                       /* the volume handler never answers */
HX_UNUSED static int hx_vol_full;                        /* writes fail with DISK_FULL */
HX_UNUSED static uint64_t hx_con_delay_us = 100;
HX_UNUSED static int hx_con_never_read;                  /* a console read is never answered */
HX_UNUSED static int hx_con_fail_after;                  /* console writes beyond this many bytes fail (0: none) */
HX_UNUSED static char hx_con_out[16384];                 /* what the console handler received */
HX_UNUSED static uint32_t hx_con_out_len;
HX_UNUSED static char hx_nil_out[16];                    /* writes that reached Write() of a NIL: handle */
HX_UNUSED static uint32_t hx_sync_bytes;                 /* bytes written through Write() */
HX_UNUSED static char hx_sync_out[16384];
HX_UNUSED static char hx_pipe_out[16384];                /* what the pipe handler took */
HX_UNUSED static uint32_t hx_pipe_len;
HX_UNUSED static uint32_t hx_pipe_ok_bytes = 1u << 30;   /* writes beyond this many bytes fail */
HX_UNUSED static struct FileHandle hx_pipe_fh_store;
HX_UNUSED static uint32_t hx_sync_len;
HX_UNUSED static char hx_in_data[1024];                  /* what the console handler gives to reads */
HX_UNUSED static uint32_t hx_in_len, hx_in_pos;
HX_UNUSED static uint32_t hx_con_reads;                  /* console read packets */
HX_UNUSED static uint8_t *hx_read_buf_seen;              /* where the last read's data went */

static LONG hx_ioerr;
HX_UNUSED static LONG hx_io_err(void) { return hx_ioerr; }
HX_UNUSED static LONG hx_set_io_err(LONG e)
{
    LONG old = hx_ioerr;

    hx_ioerr = e;
    return old;
}

HX_UNUSED static struct FileHandle hx_con_fh_store, hx_nil_fh_store;
HX_UNUSED static BPTR hx_output_bptr, hx_input_bptr;     /* Output() and Input() */
HX_UNUSED static BPTR hx_current_dir_bptr;               /* CurrentDir's */
HX_UNUSED static int hx_live_locks, hx_live_handles, hx_live_fibs, hx_live_devprocs;
HX_UNUSED static int hx_open_files_now;

HX_UNUSED static int hx_node_add(int parent, const char *name, int is_dir, const void *data, uint32_t size)
{
    int i;

    for (i = 1; i < HX_NODES; i++) {
        if (!hx_nodes[i].used) {
            memset(&hx_nodes[i], 0, sizeof(hx_nodes[i]));
            hx_nodes[i].used = 1;
            hx_nodes[i].parent = parent;
            strncpy(hx_nodes[i].name, name, sizeof(hx_nodes[i].name) - 1);
            hx_nodes[i].is_dir = is_dir;
            if (!is_dir) {
                hx_nodes[i].data = calloc(1, HX_FILE_CAP);
                hx_nodes[i].cap = HX_FILE_CAP;
                if (data && size) {
                    memcpy(hx_nodes[i].data, data, size);
                    hx_nodes[i].size = size;
                }
            }
            hx_nodes[i].mtime_days = 9000;
            return i;
        }
    }
    HX_FAIL("too many nodes");
    return 0;
}

HX_UNUSED static int hx_node_find(int dir, const char *name)
{
    int i;

    for (i = 1; i < HX_NODES; i++)
        if (hx_nodes[i].used && hx_nodes[i].parent == dir && strcmp(hx_nodes[i].name, name) == 0)
            return i;
    return 0;
}

static struct FileLock *hx_lock_new(struct MsgPort *port, int node)
{
    struct FileLock *l = hx_alloc(sizeof(*l), 1);

    if (!l)
        return NULL;
    l->fl_Task = port;
    l->fl_Key = node;
    hx_live_locks++;
    return l;
}

/* A BSTR: a length byte and the characters. */
static void hx_bstr_to_c(const uint8_t *b, char *out, size_t cap)
{
    size_t n = b[0];

    if (!hx_mem_live(b))
        HX_FAIL("a name packet argument is not live memory");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, b + 1, n);
    out[n] = 0;
}

/* The node a path names from dir: components split by '/', a leading '/' (or an
 * empty component) is the parent. -1: a directory on the way is missing, 0: the
 * last component is missing (dirs counts the matches). */
static int hx_walk(int dir, const char *path, int *err)
{
    char comp[32];
    const char *p = path;
    int cur = dir;

    *err = 0;
    if (!*p)
        return cur;
    while (*p) {
        size_t n = 0;

        if (*p == '/') {
            cur = hx_nodes[cur].parent;
            p++;
            continue;
        }
        while (p[n] && p[n] != '/')
            n++;
        if (n >= sizeof(comp))
            n = sizeof(comp) - 1;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        if (*p == '/')
            p++;
        if (!hx_nodes[cur].is_dir) {
            *err = ERROR_DIR_NOT_FOUND;
            return -1;
        }
        {
            int next = hx_node_find(cur, comp);

            if (!next) {
                *err = *p ? ERROR_DIR_NOT_FOUND : ERROR_OBJECT_NOT_FOUND;
                return *p ? -1 : 0;
            }
            cur = next;
        }
    }
    return cur;
}

static void hx_fib_fill(struct FileInfoBlock *fib, int node)
{
    struct hx_node *n = &hx_nodes[node];
    size_t len = strlen(n->name);

    memset(fib, 0, sizeof(*fib));
    fib->fib_DirEntryType = n->is_dir ? 2 : -3;
    fib->fib_FileName[0] = (char)len;
    memcpy(fib->fib_FileName + 1, n->name, len);
    fib->fib_Size = (LONG)n->size;
    fib->fib_Date.ds_Days = (LONG)n->mtime_days;
}

static void hx_vol_reply(struct StandardPacket *pkt, LONG res1, LONG res2)
{
    pkt->sp_Pkt.dp_Res1 = res1;
    pkt->sp_Pkt.dp_Res2 = res2;
    if (hx_trace)
        fprintf(stderr, "[%llu] reply res1 %ld res2 %ld\n", (unsigned long long)hx_now, (long)res1, (long)res2);
    hx_post(pkt->sp_Pkt.dp_Port, &pkt->sp_Msg);
}

static void hx_vol_run(void *arg)
{
    struct StandardPacket *pkt = arg;
    if (hx_trace)
        fprintf(stderr, "[%llu] handler runs type %ld\n", (unsigned long long)hx_now, (long)pkt->sp_Pkt.dp_Type);
    struct DosPacket *d = &pkt->sp_Pkt;
    struct FileLock *lock;
    struct hx_open *of;
    int node, err;
    char name[64];

    if (!hx_block_live(pkt))
        HX_FAIL("a handler replies to a packet that was freed (the packet's memory must live until the reply)");
    switch (d->dp_Type) {
    case ACTION_LOCATE_OBJECT: {
        struct FileLock *dir = BADDR(d->dp_Arg1);
        struct MsgPort *port = dir ? dir->fl_Task : &hx_vol_port;

        hx_bstr_to_c(BADDR(d->dp_Arg2), name, sizeof(name));
        node = hx_walk(dir ? (int)dir->fl_Key : 1, name, &err);
        if (node <= 0) {
            hx_vol_reply(pkt, 0, err ? err : ERROR_OBJECT_NOT_FOUND);
            return;
        }
        if (hx_nodes[node].held) {
            hx_vol_reply(pkt, 0, ERROR_OBJECT_IN_USE);
            return;
        }
        lock = hx_lock_new(port, node);
        hx_vol_reply(pkt, lock ? (LONG)MKBADDR(lock) : 0, lock ? 0 : ERROR_NO_FREE_STORE);
        return;
    }
    case ACTION_FREE_LOCK:
        lock = BADDR(d->dp_Arg1);
        if (lock) {
            hx_live_locks--;
            hx_free(lock);
        }
        hx_vol_reply(pkt, 1, 0);
        return;
    case ACTION_EXAMINE_OBJECT: {
        struct FileInfoBlock *fib = BADDR(d->dp_Arg2);

        lock = BADDR(d->dp_Arg1);
        hx_fib_fill(fib, (int)lock->fl_Key);
        fib->hx_next = 0;
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = BADDR(d->dp_Arg2);
        int i;

        lock = BADDR(d->dp_Arg1);
        for (i = fib->hx_next ? fib->hx_next : 1; i < HX_NODES; i++) {
            if (hx_nodes[i].used && hx_nodes[i].parent == (int)lock->fl_Key) {
                hx_fib_fill(fib, i);
                fib->hx_next = i + 1;
                hx_vol_reply(pkt, 1, 0);
                return;
            }
        }
        hx_vol_reply(pkt, 0, ERROR_NO_MORE_ENTRIES);
        return;
    }
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = BADDR(d->dp_Arg1);
        struct FileLock *dir = BADDR(d->dp_Arg2);
        int slot, parent;
        char *slash;

        hx_bstr_to_c(BADDR(d->dp_Arg3), name, sizeof(name));
        parent = dir ? (int)dir->fl_Key : 1;
        node = hx_walk(parent, name, &err);
        if (node < 0) {
            hx_vol_reply(pkt, 0, err);
            return;
        }
        if (node > 0 && hx_nodes[node].is_dir) {
            hx_vol_reply(pkt, 0, ERROR_OBJECT_WRONG_TYPE);
            return;
        }
        if (node == 0) {
            if (d->dp_Type == ACTION_FINDINPUT) {
                hx_vol_reply(pkt, 0, ERROR_OBJECT_NOT_FOUND);
                return;
            }
            slash = strrchr(name, '/');
            if (slash) {
                *slash = 0;
                parent = hx_walk(parent, name, &err);
                memmove(name, slash + 1, strlen(slash + 1) + 1);
                if (parent <= 0) {
                    hx_vol_reply(pkt, 0, ERROR_DIR_NOT_FOUND);
                    return;
                }
            }
            if (hx_vol_full) {
                hx_vol_reply(pkt, 0, ERROR_DISK_FULL);
                return;
            }
            node = hx_node_add(parent, name, 0, NULL, 0);
        } else if (d->dp_Type == ACTION_FINDOUTPUT) {
            hx_nodes[node].size = 0;
        }
        for (slot = 0; slot < HX_OPEN_FILES; slot++)
            if (!hx_opens[slot].used)
                break;
        if (slot == HX_OPEN_FILES)
            HX_FAIL("too many open files");
        hx_opens[slot].used = 1;
        hx_opens[slot].node = node;
        hx_opens[slot].pos = 0;
        hx_open_files_now++;
        fh->fh_Arg1 = slot + 1;
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    case ACTION_END:
        of = &hx_opens[d->dp_Arg1 - 1];
        if (!of->used)
            HX_FAIL("ACTION_END of a file that is not open");
        of->used = 0;
        hx_open_files_now--;
        hx_vol_reply(pkt, 1, 0);
        return;
    case ACTION_READ: {
        uint8_t *dst = (uint8_t *)(uintptr_t)(uint32_t)d->dp_Arg2;
        uint32_t want = (uint32_t)d->dp_Arg3, got;

        of = &hx_opens[d->dp_Arg1 - 1];
        if (!hx_mem_live(dst))
            HX_FAIL("a file read into memory that is not allocated");
        got = of->pos < hx_nodes[of->node].size ? hx_nodes[of->node].size - of->pos : 0;
        if (got > want)
            got = want;
        memcpy(dst, hx_nodes[of->node].data + of->pos, got);
        of->pos += got;
        hx_vol_reply(pkt, (LONG)got, 0);
        return;
    }
    case ACTION_WRITE: {
        const uint8_t *src = (const uint8_t *)(uintptr_t)(uint32_t)d->dp_Arg2;
        uint32_t n = (uint32_t)d->dp_Arg3;

        of = &hx_opens[d->dp_Arg1 - 1];
        if (hx_vol_full) {
            hx_vol_reply(pkt, -1, ERROR_DISK_FULL);
            return;
        }
        if (of->pos + n > HX_FILE_CAP)
            n = of->pos < HX_FILE_CAP ? HX_FILE_CAP - of->pos : 0;
        memcpy(hx_nodes[of->node].data + of->pos, src, n);
        of->pos += n;
        if (of->pos > hx_nodes[of->node].size)
            hx_nodes[of->node].size = of->pos;
        hx_vol_reply(pkt, (LONG)n, 0);
        return;
    }
    case ACTION_SEEK: {
        LONG off = d->dp_Arg2, mode = d->dp_Arg3, old, np;

        of = &hx_opens[d->dp_Arg1 - 1];
        old = (LONG)of->pos;
        np = mode == OFFSET_BEGINNING ? off : (mode == OFFSET_CURRENT ? old + off : (LONG)hx_nodes[of->node].size + off);
        if (np < 0 || np > (LONG)hx_nodes[of->node].size) {
            hx_vol_reply(pkt, -1, ERROR_SEEK_ERROR);
            return;
        }
        of->pos = (uint32_t)np;
        hx_vol_reply(pkt, old, 0);
        return;
    }
    case ACTION_SET_FILE_SIZE:
        of = &hx_opens[d->dp_Arg1 - 1];
        hx_nodes[of->node].size = (uint32_t)d->dp_Arg2;
        hx_vol_reply(pkt, d->dp_Arg2, 0);
        return;
    default:
        hx_vol_reply(pkt, 0, ERROR_ACTION_NOT_KNOWN);
        return;
    }
}

static void hx_con_run(void *arg)
{
    struct StandardPacket *pkt = arg;
    struct DosPacket *d = &pkt->sp_Pkt;

    if (!hx_block_live(pkt))
        HX_FAIL("the console handler replies to a packet that was freed");
    if (d->dp_Type == ACTION_WRITE) {
        const char *src = (const char *)(uintptr_t)(uint32_t)d->dp_Arg2;
        uint32_t n = (uint32_t)d->dp_Arg3;

        if (!hx_mem_live(src))
            HX_FAIL("a console write from memory that is not allocated");
        if (hx_con_fail_after && hx_con_out_len + n > (uint32_t)hx_con_fail_after) {
            hx_vol_reply(pkt, -1, ERROR_DISK_FULL);
            return;
        }
        if (hx_con_out_len + n > sizeof(hx_con_out))
            n = (uint32_t)(sizeof(hx_con_out) - hx_con_out_len);
        memcpy(hx_con_out + hx_con_out_len, src, n);
        hx_con_out_len += n;
        hx_vol_reply(pkt, (LONG)n, 0);
    } else if (d->dp_Type == ACTION_READ) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(uint32_t)d->dp_Arg2;
        uint32_t want = (uint32_t)d->dp_Arg3, got = hx_in_len - hx_in_pos;

        if (!hx_mem_live(dst))
            HX_FAIL("the console read lands in memory that is not allocated (the request buffer must not be used)");
        if (got > want)
            got = want;
        memcpy(dst, hx_in_data + hx_in_pos, got);
        hx_in_pos += got;
        hx_vol_reply(pkt, (LONG)got, 0);
    } else {
        hx_vol_reply(pkt, 0, ERROR_ACTION_NOT_KNOWN);
    }
}

/* A pipe: writes are taken until hx_pipe_ok_bytes, then they fail (a closed
 * pipe, a full volume); a read finds the end of input. */
static void hx_pipe_run(void *arg)
{
    struct StandardPacket *pkt = arg;
    struct DosPacket *d = &pkt->sp_Pkt;

    if (!hx_block_live(pkt))
        HX_FAIL("the pipe handler replies to a packet that was freed");
    if (d->dp_Type == ACTION_WRITE) {
        const char *src = (const char *)(uintptr_t)(uint32_t)d->dp_Arg2;
        uint32_t n = (uint32_t)d->dp_Arg3;

        if (!hx_mem_live(src))
            HX_FAIL("a pipe write from memory that is not allocated");
        if (hx_pipe_len + n > hx_pipe_ok_bytes) {
            hx_vol_reply(pkt, -1, ERROR_DISK_FULL);
            return;
        }
        if (hx_pipe_len + n > sizeof(hx_pipe_out))
            n = (uint32_t)(sizeof(hx_pipe_out) - hx_pipe_len);
        memcpy(hx_pipe_out + hx_pipe_len, src, n);
        hx_pipe_len += n;
        hx_vol_reply(pkt, (LONG)n, 0);
    } else if (d->dp_Type == ACTION_READ) {
        hx_vol_reply(pkt, 0, 0);
    } else {
        hx_vol_reply(pkt, 0, ERROR_ACTION_NOT_KNOWN);
    }
}

static void hx_dos_packet(struct MsgPort *port, struct StandardPacket *pkt)
{
    int k;

    if (!hx_block_live(pkt))
        HX_FAIL("a packet that is not live memory was sent");
    if (pkt->sp_Msg.mn_Node.ln_Name != (char *)&pkt->sp_Pkt || pkt->sp_Pkt.dp_Link != &pkt->sp_Msg || !pkt->sp_Pkt.dp_Port)
        HX_FAIL("a packet is not set up (name, link and reply port)");
    hx_packets++;
    if (hx_trace)
        fprintf(stderr, "[%llu] packet type %ld to port %d\n", (unsigned long long)hx_now, (long)pkt->sp_Pkt.dp_Type, port->kind);
    switch (pkt->sp_Pkt.dp_Type) {
    case ACTION_LOCATE_OBJECT: k = 0; break;
    case ACTION_FREE_LOCK: k = 1; break;
    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_NEXT: k = 2; break;
    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: k = 3; break;
    case ACTION_END: k = 4; break;
    case ACTION_READ: k = 5; break;
    case ACTION_WRITE: k = 6; break;
    default: k = 7; break;
    }
    hx_packets_by[k]++;
    if (port->kind == HX_K_VOLUME) {
        if (hx_vol_never)
            return;
        hx_sched(hx_vol_delay_us, hx_vol_run, pkt);
    } else if (port->kind == HX_K_PIPE) {
        hx_sched(hx_con_delay_us, hx_pipe_run, pkt);
    } else {
        if (pkt->sp_Pkt.dp_Type == ACTION_READ) {
            hx_con_reads++;
            hx_read_buf_seen = (uint8_t *)(uintptr_t)(uint32_t)pkt->sp_Pkt.dp_Arg2;
            if (hx_con_never_read)
                return;
        }
        hx_sched(hx_con_delay_us, hx_con_run, pkt);
    }
}

/* The devices GetDeviceProc knows: name, ports and directories of the candidates. */
struct hx_dev {
    const char *name;
    struct MsgPort *port[2];
    int node[2];
    int count;
};
HX_UNUSED static struct hx_dev hx_devs[4];
HX_UNUSED static int hx_ndevs;

HX_UNUSED static void hx_free_device_proc(struct DevProc *dp);

HX_UNUSED static struct DevProc *hx_get_device_proc(const char *name, struct DevProc *prev)
{
    struct DevProc *dp;
    const char *colon = strchr(name, ':');
    size_t n = colon ? (size_t)(colon - name) : 0;
    int i, next = 0;

    if (!colon)
        return NULL;
    for (i = 0; i < hx_ndevs; i++) {
        if (strlen(hx_devs[i].name) == n && strncmp(hx_devs[i].name, name, n) == 0)
            break;
    }
    if (i == hx_ndevs) {
        hx_set_io_err(ERROR_DEVICE_NOT_MOUNTED);
        return NULL;
    }
    if (prev) {
        next = prev->hx_index + 1;
        hx_free_device_proc(prev);
        if (next >= hx_devs[i].count) {
            hx_set_io_err(ERROR_OBJECT_NOT_FOUND);
            return NULL;
        }
    }
    dp = hx_alloc(sizeof(*dp), 1);
    if (!dp)
        return NULL;
    dp->hx_index = next;
    dp->dvp_Port = hx_devs[i].port[next];
    {
        struct FileLock *l = hx_lock_new(dp->dvp_Port, hx_devs[i].node[next]);

        dp->dvp_Lock = MKBADDR(l);
    }
    hx_live_devprocs++;
    return dp;
}

HX_UNUSED static void hx_free_device_proc(struct DevProc *dp)
{
    struct FileLock *l = BADDR(dp->dvp_Lock);

    hx_live_locks--;
    hx_free(l);
    hx_live_devprocs--;
    hx_free(dp);
}

HX_UNUSED static BPTR hx_dup_lock(BPTR b)
{
    struct FileLock *l = BADDR(b), *n;

    if (!l)
        return 0;
    n = hx_lock_new(l->fl_Task, (int)l->fl_Key);
    return MKBADDR(n);
}

HX_UNUSED static void hx_unlock(BPTR b)
{
    struct FileLock *l = BADDR(b);

    if (!l)
        return;
    hx_live_locks--;
    hx_free(l);
}

HX_UNUSED static BPTR hx_parent_dir(BPTR b)
{
    struct FileLock *l = BADDR(b);
    int p = hx_nodes[l->fl_Key].parent;

    if (l->fl_Key == 1 || p == 0)
        return 0;
    return MKBADDR(hx_lock_new(l->fl_Task, p));
}

HX_UNUSED static LONG hx_same_lock(BPTR a, BPTR b)
{
    struct FileLock *x = BADDR(a), *y = BADDR(b);

    return x->fl_Key == y->fl_Key && x->fl_Task == y->fl_Task ? LOCK_SAME : LOCK_DIFFERENT;
}

HX_UNUSED static void *hx_alloc_dos_object(ULONG type, void *tags)
{
    (void)tags;
    if (type == DOS_FILEHANDLE) {
        struct FileHandle *fh = hx_alloc(sizeof(*fh), 1);

        if (fh)
            hx_live_handles++;
        return fh;
    } else if (type == DOS_FIB) {
        struct FileInfoBlock *fib = hx_alloc(sizeof(*fib), 1);

        if (fib)
            hx_live_fibs++;
        return fib;
    }
    HX_FAIL("AllocDosObject of an unknown type");
    return NULL;
}

HX_UNUSED static void hx_free_dos_object(ULONG type, void *p)
{
    if (type == DOS_FILEHANDLE)
        hx_live_handles--;
    else if (type == DOS_FIB)
        hx_live_fibs--;
    else
        HX_FAIL("FreeDosObject of an unknown type");
    hx_free(p);
}

HX_UNUSED static BPTR hx_open(const char *name, LONG mode)
{
    (void)mode;
    if (strcmp(name, "CONSOLE:") == 0) {
        struct FileHandle *fh = hx_alloc(sizeof(*fh), 1);

        if (!fh)
            return 0;
        fh->fh_Type = &hx_con_port;
        fh->hx_owned = 1;
        hx_live_handles++;
        return MKBADDR(fh);
    }
    if (strncmp(name, "CON:", 4) == 0) {
        struct FileHandle *fh = hx_alloc(sizeof(*fh), 1);

        fh->fh_Type = &hx_con_port;
        fh->hx_owned = 1;
        fh->hx_interactive = 1;
        hx_live_handles++;
        return MKBADDR(fh);
    }
    hx_set_io_err(ERROR_OBJECT_NOT_FOUND);
    return 0;
}

HX_UNUSED static LONG hx_close(BPTR b)
{
    struct FileHandle *fh = BADDR(b);

    if (!fh->hx_owned)
        HX_FAIL("Close of a handle the program did not open");
    hx_live_handles--;
    hx_free(fh);
    return 1;
}

/* Write() on a handle: a handle with no handler port is NIL:, which takes it all. */
HX_UNUSED static LONG hx_write(BPTR b, const void *buf, LONG len)
{
    struct FileHandle *fh = BADDR(b);

    if (!fh)
        HX_FAIL("Write on no handle");
    hx_sync_bytes += (uint32_t)len;
    if (fh->fh_Type == NULL) {
        return len;
    }
    if (fh->fh_Type == &hx_pipe_port) {
        if (hx_pipe_len + (uint32_t)len <= sizeof(hx_pipe_out)) {
            memcpy(hx_pipe_out + hx_pipe_len, buf, (size_t)len);
            hx_pipe_len += (uint32_t)len;
        }
        return len;
    }
    if (hx_sync_len + (uint32_t)len <= sizeof(hx_sync_out)) {
        memcpy(hx_sync_out + hx_sync_len, buf, (size_t)len);
        hx_sync_len += (uint32_t)len;
    }
    return len;
}

HX_UNUSED static LONG hx_is_interactive(BPTR b)
{
    struct FileHandle *fh = BADDR(b);

    return fh->hx_interactive;
}

HX_UNUSED static BPTR hx_current_dir(BPTR b)
{
    BPTR old = hx_current_dir_bptr;

    hx_current_dir_bptr = b;
    return old;
}

HX_UNUSED static LONG hx_name_from_lock(BPTR b, char *buf, LONG size)
{
    struct FileLock *l = BADDR(b);
    char tmp[128];
    int n = (int)l->fl_Key, pos = (int)sizeof(tmp) - 1;

    tmp[pos] = 0;
    while (n > 1) {
        size_t len = strlen(hx_nodes[n].name);

        if ((int)len + 1 > pos)
            return 0;
        pos -= (int)len;
        memcpy(tmp + pos, hx_nodes[n].name, len);
        tmp[--pos] = '/';
        n = hx_nodes[n].parent;
    }
    if (snprintf(buf, (size_t)size, "RAM:%s", tmp + pos + (tmp[pos] == '/' ? 1 : 0)) >= size)
        return 0;
    return -1; /* dos.library DOSTRUE, not Exec TRUE */
}

/* ---- Intuition, graphics, Picasso96 --------------------------------------------------------- */

HX_UNUSED static struct Library hx_p96_lib;
HX_UNUSED static int hx_p96_present = 1;             /* Picasso96API.library opens */
HX_UNUSED static int hx_p96_no_mode;                 /* no mode for the request */
HX_UNUSED static ULONG hx_p96_max_height = 100000;   /* a screen taller than this does not open */
HX_UNUSED static ULONG hx_p96_clamp_height;          /* a taller screen opens, but only this tall (0: as asked) */
HX_UNUSED static int hx_p96_fail_buffers;            /* AllocScreenBuffer fails */
HX_UNUSED static int hx_p96_buffer_pitch_differs;    /* the second buffer has another pitch */
HX_UNUSED static ULONG hx_last_mode_asked, hx_last_mode_used;
HX_UNUSED static int hx_screens, hx_buffers_live, hx_windows;
HX_UNUSED static unsigned long hx_changes, hx_safe_msgs, hx_pans, hx_palettes;
HX_UNUSED static ULONG hx_last_palette[2 + 256 * 3];
HX_UNUSED static int hx_pub_front;                   /* the public screen was brought to front */
HX_UNUSED static int hx_pub_locks;
HX_UNUSED static uint64_t hx_vblank_us = 20000;
HX_UNUSED static struct Screen *hx_cur_screen;
HX_UNUSED static struct Window *hx_cur_window;
HX_UNUSED static int hx_requesters;
HX_UNUSED static char hx_requester_text[256];
static uint32_t hx_gfx_next;                         /* the bump pointer in the arena */
#define HX_GFX_SLOT (REFPLAT_CLAIMS - 1u)

/* Bitmap memory is in the arena (the last claim's area, which a test never
 * opens), so that AARSharedToken can name it. */
static uint8_t *hx_gfx_mem(uint32_t bytes)
{
    uint8_t *base = (uint8_t *)(uintptr_t)refplat_ptr(REFPLAT_TOKEN_BASE + HX_GFX_SLOT * REFPLAT_CLAIM_STRIDE);
    uint8_t *p;

    bytes = (bytes + 15u) & ~15u;
    if (hx_gfx_next + bytes > REFPLAT_CLAIM_STRIDE)
        HX_FAIL("the fake graphics memory is full");
    p = base + hx_gfx_next;
    hx_gfx_next += bytes;
    return p;
}

HX_UNUSED static struct Library *hx_open_lib(const char *name, ULONG version)
{
    (void)version;
    if (strcmp(name, "Picasso96API.library") == 0 && hx_p96_present)
        return &hx_p96_lib;
    return NULL;
}

HX_UNUSED static void hx_close_lib(struct Library *l)
{
    if (l != &hx_p96_lib)
        HX_FAIL("CloseLibrary of a library that is not open");
}

static uint32_t hx_bpp(int rgb)
{
    return rgb == RGBFB_B8G8R8A8 ? 4u : (rgb == RGBFB_CLUT ? 1u : 2u);
}

HX_UNUSED static ULONG hx_p96_best_mode(ULONG w, ULONG h, ULONG depth, ULONG formats)
{
    (void)w;
    (void)h;
    (void)depth;
    (void)formats;
    if (hx_p96_no_mode)
        return INVALID_ID;
    return 0x50001UL;
}

HX_UNUSED static ULONG hx_p96_best_mode_tags(ULONG first, ...)
{
    va_list ap;
    ULONG tag = first, w = 0, h = 0, d = 0, f = 0;

    va_start(ap, first);
    while (tag != TAG_DONE) {
        ULONG v = va_arg(ap, ULONG);

        if (tag == P96BIDTAG_NominalWidth) w = v;
        else if (tag == P96BIDTAG_NominalHeight) h = v;
        else if (tag == P96BIDTAG_Depth) d = v;
        else if (tag == P96BIDTAG_FormatsAllowed) f = v;
        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
    return hx_p96_best_mode(w, h, d, f);
}

HX_UNUSED static struct Screen *hx_p96_open_screen(ULONG first, ...)
{
    va_list ap;
    ULONG tag = first, mode = 0, w = 0, h = 0, rgb = 0;
    struct Screen *s;
    uint32_t pitch;

    va_start(ap, first);
    while (tag != TAG_DONE) {
        ULONG v = va_arg(ap, ULONG);

        if (tag == P96SA_DisplayID) mode = v;
        else if (tag == P96SA_Width) w = v;
        else if (tag == P96SA_Height) h = v;
        else if (tag == P96SA_RGBFormat) rgb = v;
        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
    hx_last_mode_asked = mode;
    if (h > hx_p96_max_height)
        return NULL;
    if (hx_p96_clamp_height && h > hx_p96_clamp_height)
        h = hx_p96_clamp_height;
    s = hx_alloc(sizeof(*s), 1);
    if (!s)
        return NULL;
    hx_last_mode_used = mode;
    s->Width = (WORD)w;
    s->Height = (WORD)h;
    pitch = (w * hx_bpp((int)rgb) + 15u) & ~15u;
    s->bm.Planes[0] = hx_gfx_mem(pitch * h);
    s->bm.Rows = (UWORD)h;
    s->bm.BytesPerRow = (UWORD)pitch;
    s->bm.hx_rgb = (int)rgb;
    s->RastPort.BitMap = &s->bm;
    s->ViewPort.RasInfo = &s->ras;
    hx_screens++;
    hx_cur_screen = s;
    return s;
}

HX_UNUSED static void hx_p96_close_screen(struct Screen *s)
{
    if (hx_buffers_live)
        HX_FAIL("a screen is closed while its screen buffers are not freed");
    if (hx_windows)
        HX_FAIL("a screen is closed while a window is open on it");
    hx_screens--;
    if (hx_cur_screen == s)
        hx_cur_screen = NULL;
    hx_free(s);
}

HX_UNUSED static ULONG hx_p96_bitmap_attr(struct BitMap *bm, ULONG attr)
{
    if (attr == P96BMA_MEMORY)
        return (ULONG)(uintptr_t)bm->Planes[0];
    if (attr == P96BMA_BYTESPERROW)
        return bm->BytesPerRow;
    if (attr == P96BMA_RGBFORMAT)
        return (ULONG)bm->hx_rgb;
    return 0;
}

HX_UNUSED static struct ScreenBuffer *hx_alloc_screen_buffer(struct Screen *s, void *bm, ULONG flags)
{
    struct ScreenBuffer *sb;

    (void)bm;
    if (hx_p96_fail_buffers)
        return NULL;
    sb = hx_alloc(sizeof(*sb), 1);
    if (!sb)
        return NULL;
    sb->sb_DBufInfo = hx_alloc(sizeof(*sb->sb_DBufInfo), 1);
    if (flags & SB_SCREEN_BITMAP) {
        sb->sb_BitMap = &s->bm;
    } else {
        sb->own = s->bm;
        sb->own.Planes[0] = hx_gfx_mem(s->bm.BytesPerRow * s->bm.Rows);
        if (hx_p96_buffer_pitch_differs)
            sb->own.BytesPerRow = (UWORD)(sb->own.BytesPerRow + 16);
        sb->sb_BitMap = &sb->own;
    }
    hx_buffers_live++;
    return sb;
}

HX_UNUSED static void hx_free_screen_buffer(struct Screen *s, struct ScreenBuffer *sb)
{
    (void)s;
    if (sb->hx_safe_out)
        HX_FAIL("a screen buffer is freed while its SafeMessage is still to come");
    hx_buffers_live--;
    hx_free(sb->sb_DBufInfo);
    hx_free(sb);
}

static void hx_safe_run(void *arg)
{
    struct ScreenBuffer *sb = arg;

    sb->hx_safe_out = 0;
    hx_safe_msgs++;
    if (!sb->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort)
        HX_FAIL("a flip's SafeMessage has no reply port");
    hx_post(sb->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort, &sb->sb_DBufInfo->dbi_SafeMessage);
}

HX_UNUSED static ULONG hx_change_screen_buffer(struct Screen *s, struct ScreenBuffer *sb)
{
    (void)s;
    if (sb->hx_safe_out)
        HX_FAIL("ChangeScreenBuffer with the same buffer while its SafeMessage is out");
    sb->hx_safe_out = 1;
    sb->hx_changes++;
    hx_changes++;
    hx_sched(hx_vblank_us, hx_safe_run, sb);
    return 1;
}

HX_UNUSED static void hx_scroll_vport(struct ViewPort *vp)
{
    (void)vp;
    hx_pans++;
}

HX_UNUSED static void hx_load_rgb32(struct ViewPort *vp, const ULONG *table)
{
    uint32_t count = table[0] >> 16, n;

    (void)vp;
    n = 1 + count * 3 + 1;
    if (n > sizeof(hx_last_palette) / sizeof(hx_last_palette[0]))
        HX_FAIL("palette too long");
    memcpy(hx_last_palette, table, n * sizeof(ULONG));
    hx_palettes++;
}

HX_UNUSED static void hx_wait_tof(void)
{
    hx_run_for(hx_vblank_us);
}

HX_UNUSED static struct Screen *hx_lock_pub_screen(const char *name)
{
    (void)name;
    hx_pub_locks++;
    return (struct Screen *)(uintptr_t)&hx_pub_locks;
}

HX_UNUSED static void hx_unlock_pub_screen(void *name, struct Screen *s)
{
    (void)name;
    (void)s;
    hx_pub_locks--;
}

HX_UNUSED static void hx_screen_to_front(struct Screen *s)
{
    (void)s;
    hx_pub_front++;
}

HX_UNUSED static void hx_get_default_pub_screen(char *name)
{
    strcpy(name, "Workbench");
}

HX_UNUSED static struct Window *hx_open_window(void *newwin, ULONG first, ...)
{
    va_list ap;
    ULONG tag = first;
    struct Window *w;
    struct Screen *scr = NULL;

    (void)newwin;
    va_start(ap, first);
    while (tag != TAG_DONE) {
        ULONG v = va_arg(ap, ULONG);

        if (tag == WA_CustomScreen)
            scr = (struct Screen *)(uintptr_t)v;
        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
    w = hx_alloc(sizeof(*w), 1);
    w->UserPort = hx_port_new();
    w->Flags = WFLG_WINDOWACTIVE;
    w->WScreen = scr;
    if (scr) {
        w->Width = scr->Width;
        w->Height = scr->Height;
    }
    hx_windows++;
    hx_cur_window = w;
    return w;
}

HX_UNUSED static void hx_close_window(struct Window *w)
{
    while (w->UserPort->n) {
        struct Message *m = hx_getmsg(w->UserPort);

        hx_free(m);
    }
    hx_port_delete(w->UserPort);
    hx_windows--;
    if (hx_cur_window == w)
        hx_cur_window = NULL;
    hx_free(w);
}

/* A message of the input window: the test's input. */
HX_UNUSED static void hx_window_post(ULONG cls, UWORD code, UWORD qual, WORD mx, WORD my)
{
    struct IntuiMessage *im = hx_alloc(sizeof(*im), 1);

    if (!hx_cur_window)
        HX_FAIL("an input message for a window that is not open");
    im->Class = cls;
    im->Code = code;
    im->Qualifier = qual;
    im->MouseX = mx;
    im->MouseY = my;
    hx_post(hx_cur_window->UserPort, &im->ExecMessage);
}

HX_UNUSED static void hx_reply_msg(struct Message *m)
{
    /* The window's messages are the test's: the reply gives them back. */
    hx_free(m);
}

HX_UNUSED static LONG hx_easy_request(void *win, struct EasyStruct *es, void *a, void *b)
{
    (void)win;
    (void)a;
    (void)b;
    hx_requesters++;
    snprintf(hx_requester_text, sizeof(hx_requester_text), "%s", es->es_TextFormat ? (const char *)es->es_TextFormat : "");
    return 1;
}

/* ---- Memory bindings ------------------------------------------------------------------------ */

HX_UNUSED static void *hx_alloc_mem(ULONG bytes, ULONG flags)
{
    return hx_alloc(bytes, (flags & MEMF_CLEAR) != 0);
}

HX_UNUSED static void hx_free_mem(void *p, ULONG bytes)
{
    if (hx_block_size(p) != bytes)
        HX_FAIL("FreeMem with another size than AllocMem");
    hx_free(p);
}

/* ---- The joystick registers and the clock ----------------------------------------------------- */

HX_UNUSED static UWORD hx_joydat[2];
HX_UNUSED static UBYTE hx_ciaa_pra = 0xFF;

/* ---- aar.library through the portable core ----------------------------------------------------- */

HX_UNUSED static aar_core *hx_aar;
HX_UNUSED static ULONG hx_token_add;       /* added to every token: a platform whose tokens lie elsewhere */

HX_UNUSED static LONG hx_aar_shared_token(AARHandle *h, const void *p, ULONG n, ULONG *t)
{
    LONG rc = aar_core_shared_token(hx_aar, h, p, n, t);

    if (rc == AAR_OK)
        *t += hx_token_add;
    return rc;
}

HX_UNUSED static LONG hx_aar_set_log_sink(AARHandle *h, void (*sink)(void *, const void *, ULONG), void *user)
{
    return aar_core_set_log_sink(hx_aar, h, (AARLogSink)(uintptr_t)sink, (ULONG)(uintptr_t)user);
}

/* ---- Back to a clean machine ---------------------------------------------------------------------- */

HX_UNUSED static void hx_reset(void)
{
    int i;

    for (i = 0; i < HX_NODES; i++)
        free(hx_nodes[i].data);
    memset(hx_nodes, 0, sizeof(hx_nodes));
    memset(hx_opens, 0, sizeof(hx_opens));
    memset(hx_evs, 0, sizeof(hx_evs));
    memset(hx_small_used, 0, sizeof(hx_small_used));
    memset(hx_big_used, 0, sizeof(hx_big_used));
    hx_live_blocks = hx_live_bytes = 0;
    hx_fail_alloc = 0;
    hx_now = 0;
    hx_seq = 0;
    hx_sig = 0;
    hx_steps = 0;
    hx_waits = 0;
    hx_timer_aborts = 0;
    hx_ahi_end = 0;
    hx_ahi_tail = NULL;
    hx_next_sigbit = 16;
    hx_ahi_fail_open = hx_ahi_open_count = hx_timer_open_count = hx_timer_fail_open = 0;
    hx_ahi_writes = hx_ahi_bytes = 0;
    hx_ahi_pending_now = hx_ahi_max_pending = 0;
    hx_packets = 0;
    memset(hx_packets_by, 0, sizeof(hx_packets_by));
    hx_vol_delay_us = 200;
    hx_vol_never = hx_vol_full = 0;
    hx_con_delay_us = 100;
    hx_con_never_read = hx_con_fail_after = 0;
    hx_con_out_len = hx_sync_bytes = hx_sync_len = hx_in_len = hx_in_pos = hx_con_reads = 0;
    hx_read_buf_seen = NULL;
    hx_ioerr = 0;
    hx_live_locks = hx_live_handles = hx_live_fibs = hx_live_devprocs = hx_open_files_now = 0;
    memset(&hx_vol_port, 0, sizeof(hx_vol_port));
    hx_vol_port.mp_SigBit = 20;
    hx_vol_port.kind = HX_K_VOLUME;
    memset(&hx_vol2_port, 0, sizeof(hx_vol2_port));
    hx_vol2_port.mp_SigBit = 21;
    hx_vol2_port.kind = HX_K_VOLUME;
    memset(&hx_con_port, 0, sizeof(hx_con_port));
    hx_con_port.mp_SigBit = 22;
    hx_con_port.kind = HX_K_CONSOLE;
    memset(&hx_pipe_port, 0, sizeof(hx_pipe_port));
    hx_pipe_port.mp_SigBit = 23;
    hx_pipe_port.kind = HX_K_PIPE;
    memset(&hx_pipe_fh_store, 0, sizeof(hx_pipe_fh_store));
    hx_pipe_fh_store.fh_Type = &hx_pipe_port;
    hx_pipe_len = 0;
    hx_pipe_ok_bytes = 1u << 30;
    hx_nodes[1].used = 1;
    hx_nodes[1].is_dir = 1;
    hx_nodes[1].parent = 0;
    memset(&hx_con_fh_store, 0, sizeof(hx_con_fh_store));
    memset(&hx_nil_fh_store, 0, sizeof(hx_nil_fh_store));
    hx_con_fh_store.fh_Type = &hx_con_port;
    hx_nil_fh_store.fh_Type = NULL;
    hx_output_bptr = MKBADDR(&hx_con_fh_store);
    hx_input_bptr = MKBADDR(&hx_con_fh_store);
    hx_current_dir_bptr = 0;
    hx_devs[0].name = "RAM";
    hx_devs[0].port[0] = &hx_vol_port;
    hx_devs[0].node[0] = 1;
    hx_devs[0].count = 1;
    hx_ndevs = 1;
    hx_p96_present = 1;
    hx_p96_no_mode = 0;
    hx_p96_max_height = 100000;
    hx_p96_clamp_height = 0;
    hx_p96_fail_buffers = hx_p96_buffer_pitch_differs = 0;
    hx_last_mode_asked = hx_last_mode_used = 0;
    hx_screens = hx_buffers_live = hx_windows = 0;
    hx_changes = hx_safe_msgs = hx_pans = hx_palettes = 0;
    hx_pub_front = hx_pub_locks = 0;
    hx_cur_screen = NULL;
    hx_cur_window = NULL;
    hx_requesters = 0;
    hx_requester_text[0] = 0;
    hx_gfx_next = 0;
    hx_vblank_us = 20000;
    hx_joydat[0] = hx_joydat[1] = 0;
    hx_ciaa_pra = 0xFF;
    hx_token_add = 0;
}

/* ---- The bindings ---------------------------------------------------------------------------------- */

#define X_ALLOC_MEM(...) hx_alloc_mem(__VA_ARGS__)
#define X_FREE_MEM(...) hx_free_mem(__VA_ARGS__)
#define X_ALLOC_VEC(bytes, flags) hx_alloc((bytes), ((flags) & MEMF_CLEAR) != 0)
#define X_FREE_VEC(p) hx_free(p)
#define X_COPY_MEM(src, dst, n) memcpy((dst), (src), (n))
#define X_CREATE_PORT() hx_port_new()
#define X_DELETE_PORT(p) hx_port_delete(p)
#define X_CREATE_IOREQ(port, size) hx_create_ioreq((port), (size))
#define X_DELETE_IOREQ(io) hx_delete_ioreq(io)
#define X_OPEN_DEVICE(...) hx_open_device(__VA_ARGS__)
#define X_CLOSE_DEVICE(io) hx_close_device(io)
#define X_SEND_IO(io) hx_send_io(io)
#define X_ABORT_IO(io) hx_abort_io(io)
#define X_WAIT_IO(io) hx_wait_io(io)
#define X_CHECK_IO(io) hx_check_io(io)
#define X_PUT_MSG(port, msg) hx_putmsg((port), (msg))
#define X_GET_MSG(port) hx_getmsg(port)
#define X_REPLY_MSG(msg) hx_reply_msg(msg)
#define X_WAIT_PORT(port) hx_waitport(port)
#define X_WAIT(mask) hx_wait(mask)
#define X_SET_SIGNAL(n, m) hx_setsignal((n), (m))
#define X_OPEN_LIB(name, ver) hx_open_lib((const char *)(name), (ver))
#define X_CLOSE_LIB(l) hx_close_lib(l)

#define X_UNLOCK(l) hx_unlock(l)
#define X_DUP_LOCK(l) hx_dup_lock(l)
#define X_PARENT_DIR(l) hx_parent_dir(l)
#define X_SAME_LOCK(a, b) hx_same_lock((a), (b))
#define X_IO_ERR() hx_io_err()
#define X_SET_IO_ERR(e) hx_set_io_err(e)
#define X_OPEN(name, mode) hx_open((const char *)(name), (mode))
#define X_CLOSE(fh) hx_close(fh)
#define X_WRITE(fh, buf, len) hx_write((fh), (buf), (len))
#define X_OUTPUT() hx_output_bptr
#define X_INPUT() hx_input_bptr
#define X_IS_INTERACTIVE(fh) hx_is_interactive(fh)
#define X_ALLOC_DOS_OBJECT(type, tags) hx_alloc_dos_object((type), (tags))
#define X_FREE_DOS_OBJECT(type, p) hx_free_dos_object((type), (p))
#define X_GET_DEVICE_PROC(name, prev) hx_get_device_proc((const char *)(name), (prev))
#define X_FREE_DEVICE_PROC(dp) hx_free_device_proc(dp)
#define X_CURRENT_DIR(l) hx_current_dir(l)
#define X_NAME_FROM_LOCK(l, buf, n) hx_name_from_lock((l), (char *)(buf), (n))

#define X_OPEN_WINDOW(...) hx_open_window(__VA_ARGS__)
#define X_CLOSE_WINDOW(w) hx_close_window(w)
#define X_CHANGE_SCREEN_BUFFER(s, sb) hx_change_screen_buffer((s), (sb))
#define X_ALLOC_SCREEN_BUFFER(s, bm, f) hx_alloc_screen_buffer((s), (bm), (f))
#define X_FREE_SCREEN_BUFFER(s, sb) hx_free_screen_buffer((s), (sb))
#define X_LOCK_PUB_SCREEN(name) hx_lock_pub_screen((const char *)(name))
#define X_UNLOCK_PUB_SCREEN(a, b) hx_unlock_pub_screen((a), (b))
#define X_SCREEN_TO_FRONT(s) hx_screen_to_front(s)
#define X_SCROLL_VPORT(vp) hx_scroll_vport(vp)
#define X_LOAD_RGB32(vp, t) hx_load_rgb32((vp), (t))
#define X_WAIT_TOF() hx_wait_tof()
#define X_GET_DEFAULT_PUB_SCREEN(n) hx_get_default_pub_screen((char *)(n))
#define X_EASY_REQUEST(w, es, a, b) hx_easy_request((w), (es), (a), (b))

#define X_AAR_STATUS(h, st) aar_core_status(hx_aar, (h), (st))
#define X_AAR_STOP(h) aar_core_stop(hx_aar, (h))
#define X_AAR_SERVICE_NEXT(h, r) aar_core_service_next(hx_aar, (h), (r))
#define X_AAR_SERVICE_REPLY(h, r) aar_core_service_reply(hx_aar, (h), (r))
#define X_AAR_POST_EVENT(h, e) aar_core_post_event(hx_aar, (h), (e))
#define X_AAR_AUDIO_READ(h, d, f, g) aar_core_audio_read(hx_aar, (h), (d), (f), (g))
#define X_AAR_SHARED_TOKEN(h, p, n, t) hx_aar_shared_token((h), (p), (n), (t))
#define X_AAR_MESSAGE_SIGNAL(h) aar_core_message_signal(hx_aar, (h))
#define X_AAR_SET_LOG_SINK(h, sink, user) hx_aar_set_log_sink((h), (sink), (user))
#define X_AAR_QUERY(info) aar_core_query(hx_aar, (info))
#define X_AAR_GET_MESSAGE(h, m) aar_core_get_message(hx_aar, (h), (m))

#define X_GET_SYS_TIME(s, tv)                                                                  \
    do {                                                                                       \
        (void)(s);                                                                             \
        (tv)->tv_secs = (ULONG)(hx_now / 1000000u);                                            \
        (tv)->tv_micro = (ULONG)(hx_now % 1000000u);                                           \
    } while (0)
#define X_P96_BEST_MODE_ID(s, ...) ((void)(s), hx_p96_best_mode_tags(__VA_ARGS__))
#define X_P96_OPEN_SCREEN(s, ...) ((void)(s), hx_p96_open_screen(__VA_ARGS__))
#define X_P96_CLOSE_SCREEN(s, scr) ((void)(s), hx_p96_close_screen(scr))
#define X_P96_BITMAP_ATTR(s, bm, attr) ((void)(s), hx_p96_bitmap_attr((bm), (attr)))
#define X_JOYDAT(port) hx_joydat[(port) ? 1 : 0]
#define X_CIAA_PRA() hx_ciaa_pra

#endif /* SERVE_HOST_H */
