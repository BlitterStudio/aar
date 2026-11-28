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
#define ERROR_RENAME_ACROSS_DEVICES 215
#define ERROR_DIRECTORY_NOT_EMPTY 216
#define ERROR_DELETE_PROTECTED 222
#define ERROR_WRITE_PROTECTED 223

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
#define ACTION_DELETE_OBJECT 16
#define ACTION_RENAME_OBJECT 17
#define ACTION_SET_PROTECT 21
#define ACTION_CREATE_DIR 22
#define ACTION_INFO 26
#define ACTION_SET_COMMENT 28
#define ACTION_SET_DATE 34
#define ID_WRITE_PROTECTED 80
#define ID_VALIDATED 82
#define GVF_GLOBAL_ONLY 0x100UL
#define GVF_BINARY_VAR 0x400UL
#define CMD_READ 2
#define CMD_UPDATE 4

#define LOCK_SAME 0
#define LOCK_SAME_VOLUME 1
#define LOCK_DIFFERENT (-1)
#define DVPF_ASSIGN 2UL
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
#define IDCMP_NEWSIZE 0x00000002UL
#define IDCMP_ACTIVEWINDOW 0x00040000UL
#define IDCMP_INACTIVEWINDOW 0x00080000UL
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
#define WA_PubScreen (TAG_USER + 150)
#define WA_Title (TAG_USER + 151)
#define WA_InnerWidth (TAG_USER + 152)
#define WA_InnerHeight (TAG_USER + 153)
#define WA_DragBar (TAG_USER + 154)
#define WA_DepthGadget (TAG_USER + 155)
#define WA_CloseGadget (TAG_USER + 156)
#define WA_SizeGadget (TAG_USER + 157)
#define WA_SizeBRight (TAG_USER + 158)
#define WA_SizeBBottom (TAG_USER + 159)
#define WA_MinWidth (TAG_USER + 160)
#define WA_MinHeight (TAG_USER + 161)
#define WA_MaxWidth (TAG_USER + 162)
#define WA_MaxHeight (TAG_USER + 163)
#define WA_SmartRefresh (TAG_USER + 164)
#define WA_BusyPointer (TAG_USER + 165)
#define WA_PointerDelay (TAG_USER + 166)
#define WA_Pointer (TAG_USER + 167)

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
#define P96BMA_BYTESPERPIXEL 4
#define P96BMA_ISP96 5
#define P96IDA_ISP96 6
#define P96IDA_COMPATIBLEFORMATS 7
#define BMF_CLEAR 1UL
#define BMF_DISPLAYABLE 2UL
#define DTAG_DISP 0x80000000UL
#define DTAG_DIMS 0x80001000UL
#define DTAG_MNTR 0x80002000UL
#define IECLASS_RAWKEY 1
#define IEQUALIFIER_LCOMMAND 0x0040
#define IEQUALIFIER_RCOMMAND 0x0080
#define JP_TYPE_NOTAVAIL (0UL << 28)
#define JP_TYPE_GAMECTLR (1UL << 28)
#define JP_TYPE_MOUSE (2UL << 28)
#define JP_TYPE_JOYSTK (3UL << 28)
#define JPF_BUTTON_BLUE (1UL << 23)
#define JPF_BUTTON_RED (1UL << 22)
#define JPF_BUTTON_YELLOW (1UL << 21)
#define JPF_BUTTON_GREEN (1UL << 20)
#define JPF_BUTTON_FORWARD (1UL << 19)
#define JPF_BUTTON_REVERSE (1UL << 18)
#define JPF_BUTTON_PLAY (1UL << 17)
#define JPF_JOY_UP (1UL << 3)
#define JPF_JOY_DOWN (1UL << 2)
#define JPF_JOY_LEFT (1UL << 1)
#define JPF_JOY_RIGHT (1UL << 0)
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
struct Screen;
struct IntuitionBase {
    int hx_dummy;
    struct Screen *ActiveScreen;
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
    LONG fib_Protection;
    struct DateStamp fib_Date;
    char fib_Comment[80];
    LONG hx_next;                /* EXAMINE_NEXT's position */
};
struct InfoData {
    LONG id_NumSoftErrors;
    LONG id_UnitNumber;
    LONG id_DiskState;
    LONG id_NumBlocks;
    LONG id_NumBlocksUsed;
    LONG id_BytesPerBlock;
    LONG id_DiskType;
    BPTR id_VolumeNode;
    LONG id_InUse;
};
/* The fields of locale.library's Locale that aar_serve reads. */
struct Locale {
    STRPTR loc_LanguageName;
    ULONG loc_CountryCode;
    LONG loc_GMTOffset;
    STRPTR loc_DecimalPoint;
    STRPTR loc_GroupSeparator;
};
struct IOClipReq {
    struct Message io_Message;
    struct Device *io_Device;
    void *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
    /* the simulation: the same place as in struct IORequest */
    int hx_pending;
    int hx_done;
    ULONG io_Actual;
    ULONG io_Length;
    STRPTR io_Data;
    ULONG io_Offset;
    LONG io_ClipID;
};
struct ClipboardHandle {
    struct IOClipReq cbh_Req;
    int hx_unit;
};
struct DevProc {
    struct MsgPort *dvp_Port;
    BPTR dvp_Lock;
    ULONG dvp_Flags;
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
    UBYTE *Title, *DefaultTitle;
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
    struct RastPort *RPort;
    BYTE BorderLeft, BorderTop, BorderRight, BorderBottom;
    struct RastPort hx_rp;
    /* what the test asks about the window it was opened with */
    ULONG IDCMPFlags;
    int hx_close_gadget, hx_size_gadget, hx_custom_screen;
    struct Screen *hx_pub;
    char hx_title[256], hx_screen_title[256];
};
struct IntuiMessage {
    struct Message ExecMessage;
    ULONG Class;
    UWORD Code, Qualifier;
    WORD MouseX, MouseY;
    APTR IAddress;
};
struct InputEvent {
    struct InputEvent *ie_NextEvent;
    UBYTE ie_Class, ie_SubClass;
    UWORD ie_Code, ie_Qualifier;
    APTR ie_EventAddress;
};
struct Preferences {
    UWORD color17, color18, color19;
};
struct Rectangle {
    WORD MinX, MinY, MaxX, MaxY;
};
struct DimensionInfo {
    struct Rectangle Nominal;
};
struct DisplayInfo {
    UWORD NotAvailable;
};
struct MonitorInfo {
    UWORD TotalRows, TotalColorClocks;
};
typedef ULONG RGBFTYPE;
struct RenderInfo {
    APTR Memory;
    WORD BytesPerRow;
    WORD pad;
    RGBFTYPE RGBFormat;
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
#define HX_BIG (AAR_SERVE_CLIP_RAW_MAX + 8192u) /* FTXT stream and final I/O chunk */
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
            memset(hx_big[i], clear ? 0 : 0xCD, bytes);
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
            memset(hx_big[i], 0xA5, hx_big_size[i]);
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
HX_UNUSED static struct Device hx_timer_dev, hx_ahi_dev, hx_clip_dev;
static void hx_clip_send(struct IORequest *io);
static void hx_clip_abort(struct IORequest *io);
HX_UNUSED static unsigned long hx_timer_aborts;      /* timer requests aborted while still pending */
HX_UNUSED static uint64_t hx_vblank_us = 20000;     /* the vertical blank period */
HX_UNUSED static uint64_t hx_ahi_end;                /* when the last queued AHI write finishes playing */
HX_UNUSED static unsigned long hx_ahi_short;         /* writes shorter than hx_ahi_short_ms (each costs a 5 ms gap) */
HX_UNUSED static unsigned hx_ahi_short_ms = 40;      /* a write shorter than this leaves a gap */
HX_UNUSED static struct IORequest *hx_ahi_tail;      /* the AHI write sent last */

static void hx_io_done(void *arg)
{
    struct IORequest *io = arg;

    io->hx_pending = 0;
    io->hx_done = 1;
    if (io->io_Command == CMD_WRITE && io->io_Device == &hx_ahi_dev) {
        struct AHIRequest *ar = (struct AHIRequest *)io;

        hx_ahi_pending_now--;
        /* CMD_WRITE: "the other fields, except io_Device, io_Unit and io_Command,
         * are trashed" when the request finishes */
        ar->ahir_Std.io_Offset = 0x5a5a;
        ar->ahir_Link = (struct AHIRequest *)(uintptr_t)1;
    }
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
        if (ar->ahir_Std.io_Offset != 0)
            HX_FAIL("an AHI write with io_Offset not 0 (the device trashes it when a request finishes)");
        hx_ahi_pending_now++;
        /* The AHI developer guide describes double buffering with two requests;
         * on the card (8c7be36) ahi.device played the first of six chained
         * writes and nothing after it. More than two in flight is not played. */
        if (hx_ahi_pending_now > 2)
            HX_FAIL("more than two AHI writes in flight: ahi.device does not play them");
        if (hx_ahi_pending_now > hx_ahi_max_pending)
            hx_ahi_max_pending = hx_ahi_pending_now;
        us = (uint64_t)ar->ahir_Std.io_Length / 4u * 1000000u / ar->ahir_Frequency;
        /* A driver that leaves a gap at every request boundary when the request is
         * shorter than hx_ahi_short_ms: the gap is silence between two writes. */
        if (us < (uint64_t)hx_ahi_short_ms * 1000u) {
            hx_ahi_short++;
            hx_ahi_end += 5000u;
        }
        /* ahi.device plays the queued writes one after the other */
        if (hx_ahi_end < hx_now)
            hx_ahi_end = hx_now;
        hx_ahi_end += us;
        hx_sched(hx_ahi_end - hx_now, hx_io_done, io);
    } else if (io->io_Device == &hx_clip_dev) {
        hx_clip_send(io);
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
    if (io->io_Device == &hx_clip_dev) {
        hx_clip_abort(io);
        return;
    }
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
    int volume;                  /* independent of the handler port */
    char name[24];
    int is_dir;
    int held;                    /* locks of it fail with OBJECT_IN_USE */
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    uint32_t mtime_days;
    uint32_t mtime_min;
    uint32_t mtime_tick;
    uint32_t protect;            /* FIB_Protection */
    uint8_t comment[AAR_FILE_COMMENT_MAX + 1u]; /* native BSTR */
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
HX_UNUSED static int hx_vol_readonly;                    /* the volume is write-protected: changes fail with WRITE_PROTECTED */
HX_UNUSED static LONG hx_vol_blocks = 1000, hx_vol_blocks_used = 250;
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
            hx_nodes[i].volume = hx_nodes[parent].volume;
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
    fib->fib_Date.ds_Minute = (LONG)n->mtime_min;
    fib->fib_Date.ds_Tick = (LONG)n->mtime_tick;
    fib->fib_Protection = (LONG)n->protect;
    memcpy(fib->fib_Comment, n->comment, (size_t)n->comment[0] + 1u);
}

static void hx_vol_reply(struct StandardPacket *pkt, LONG res1, LONG res2)
{
    pkt->sp_Pkt.dp_Res1 = res1;
    pkt->sp_Pkt.dp_Res2 = res2;
    if (hx_trace)
        fprintf(stderr, "[%llu] reply res1 %ld res2 %ld\n", (unsigned long long)hx_now, (long)res1, (long)res2);
    hx_post(pkt->sp_Pkt.dp_Port, &pkt->sp_Msg);
}

/* The packets that name an object and change it (delete, create a directory,
 * set the date or the protection, rename), and Info of a volume. The handler of
 * a write-protected volume refuses every change. */
static void hx_vol_names(struct StandardPacket *pkt)
{
    struct DosPacket *d = &pkt->sp_Pkt;
    char name[64], name2[64];
    struct FileLock *dir;
    int node, err, parent, i;
    char *leaf, *slash;

    if (d->dp_Type == ACTION_INFO) {
        struct InfoData *info = BADDR(d->dp_Arg2);

        if (!hx_mem_live(info) || ((uintptr_t)info & 3u))
            HX_FAIL("an InfoData that is not live, longword aligned memory");
        memset(info, 0, sizeof(*info));
        info->id_NumBlocks = hx_vol_blocks;
        info->id_NumBlocksUsed = hx_vol_blocks_used;
        info->id_BytesPerBlock = 512;
        info->id_DiskState = hx_vol_readonly ? ID_WRITE_PROTECTED : ID_VALIDATED;
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    if (hx_vol_readonly) {
        hx_vol_reply(pkt, 0, ERROR_WRITE_PROTECTED);
        return;
    }
    if (d->dp_Type == ACTION_RENAME_OBJECT) {
        struct FileLock *fdir = BADDR(d->dp_Arg1), *tdir = BADDR(d->dp_Arg3);
        int from, to;

        hx_bstr_to_c(BADDR(d->dp_Arg2), name, sizeof(name));
        hx_bstr_to_c(BADDR(d->dp_Arg4), name2, sizeof(name2));
        if (fdir && tdir && fdir->fl_Task != tdir->fl_Task) {
            hx_vol_reply(pkt, 0, ERROR_RENAME_ACROSS_DEVICES);
            return;
        }
        from = hx_walk(fdir ? (int)fdir->fl_Key : 1, name, &err);
        if (from <= 0) {
            hx_vol_reply(pkt, 0, from < 0 ? err : ERROR_OBJECT_NOT_FOUND);
            return;
        }
        parent = tdir ? (int)tdir->fl_Key : 1;
        to = hx_walk(parent, name2, &err);
        if (to < 0) {
            hx_vol_reply(pkt, 0, err);
            return;
        }
        if (hx_nodes[from].volume != hx_nodes[to > 0 ? to : parent].volume) {
            hx_vol_reply(pkt, 0, ERROR_RENAME_ACROSS_DEVICES);
            return;
        }
        if (to > 0) {
            hx_vol_reply(pkt, 0, ERROR_OBJECT_EXISTS);
            return;
        }
        for (i = 0; i < HX_OPEN_FILES; i++)
            if (hx_opens[i].used && hx_opens[i].node == from) {
                hx_vol_reply(pkt, 0, ERROR_OBJECT_IN_USE);
                return;
            }
        leaf = name2;
        slash = strrchr(name2, '/');
        if (slash) {
            *slash = 0;
            parent = hx_walk(parent, name2, &err);
            leaf = slash + 1;
            if (parent <= 0) {
                hx_vol_reply(pkt, 0, ERROR_DIR_NOT_FOUND);
                return;
            }
        }
        hx_nodes[from].parent = parent;
        {
            size_t n = strlen(leaf);

            if (n >= sizeof(hx_nodes[from].name))
                n = sizeof(hx_nodes[from].name) - 1;
            memset(hx_nodes[from].name, 0, sizeof(hx_nodes[from].name));
            memcpy(hx_nodes[from].name, leaf, n);
        }
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    if (d->dp_Type == ACTION_SET_DATE || d->dp_Type == ACTION_SET_PROTECT ||
        d->dp_Type == ACTION_SET_COMMENT) {
        if (d->dp_Arg1 != 0)
            HX_FAIL("a metadata packet with a first argument");
        dir = BADDR(d->dp_Arg2);
        hx_bstr_to_c(BADDR(d->dp_Arg3), name, sizeof(name));
    } else {
        dir = BADDR(d->dp_Arg1);
        hx_bstr_to_c(BADDR(d->dp_Arg2), name, sizeof(name));
    }
    parent = dir ? (int)dir->fl_Key : 1;
    node = hx_walk(parent, name, &err);
    if (node < 0) {
        hx_vol_reply(pkt, 0, err);
        return;
    }
    if (d->dp_Type == ACTION_CREATE_DIR) {
        struct FileLock *lock;

        if (node > 0) {
            hx_vol_reply(pkt, 0, ERROR_OBJECT_EXISTS);
            return;
        }
        leaf = name;
        slash = strrchr(name, '/');
        if (slash) {
            *slash = 0;
            parent = hx_walk(parent, name, &err);
            leaf = slash + 1;
            if (parent <= 0) {
                hx_vol_reply(pkt, 0, ERROR_DIR_NOT_FOUND);
                return;
            }
        }
        node = hx_node_add(parent, leaf, 1, NULL, 0);
        lock = hx_lock_new(dir ? dir->fl_Task : &hx_vol_port, node);
        hx_vol_reply(pkt, lock ? (LONG)MKBADDR(lock) : 0, lock ? 0 : ERROR_NO_FREE_STORE);
        return;
    }
    if (node == 0) {
        hx_vol_reply(pkt, 0, ERROR_OBJECT_NOT_FOUND);
        return;
    }
    switch (d->dp_Type) {
    case ACTION_DELETE_OBJECT:
        if (hx_nodes[node].protect & 1u) {
            hx_vol_reply(pkt, 0, ERROR_DELETE_PROTECTED);
            return;
        }
        for (i = 0; i < HX_OPEN_FILES; i++)
            if (hx_opens[i].used && hx_opens[i].node == node) {
                hx_vol_reply(pkt, 0, ERROR_OBJECT_IN_USE);
                return;
            }
        for (i = 1; i < HX_NODES; i++)
            if (hx_nodes[i].used && hx_nodes[i].parent == node) {
                hx_vol_reply(pkt, 0, ERROR_DIRECTORY_NOT_EMPTY);
                return;
            }
        free(hx_nodes[node].data);
        memset(&hx_nodes[node], 0, sizeof(hx_nodes[node]));
        hx_vol_reply(pkt, 1, 0);
        return;
    case ACTION_SET_DATE: {
        struct DateStamp *ds = (struct DateStamp *)(uintptr_t)(uint32_t)d->dp_Arg4;

        if (!hx_mem_live(ds) || ((uintptr_t)ds & 3u))
            HX_FAIL("a DateStamp that is not live, longword aligned memory");
        hx_nodes[node].mtime_days = (uint32_t)ds->ds_Days;
        hx_nodes[node].mtime_min = (uint32_t)ds->ds_Minute;
        hx_nodes[node].mtime_tick = (uint32_t)ds->ds_Tick;
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    case ACTION_SET_PROTECT:
        hx_nodes[node].protect = (uint32_t)d->dp_Arg4;
        hx_vol_reply(pkt, 1, 0);
        return;
    case ACTION_SET_COMMENT: {
        const uint8_t *comment = BADDR(d->dp_Arg4);

        if (!hx_mem_live(comment) || ((uintptr_t)comment & 3u))
            HX_FAIL("a comment BSTR that is not live, longword aligned memory");
        if (comment[0] > AAR_FILE_COMMENT_MAX)
            HX_FAIL("a comment BSTR with more than 79 data bytes");
        memcpy(hx_nodes[node].comment, comment, (size_t)comment[0] + 1u);
        hx_vol_reply(pkt, 1, 0);
        return;
    }
    default:
        hx_vol_reply(pkt, 0, ERROR_ACTION_NOT_KNOWN);
        return;
    }
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
        if (!of->used)
            HX_FAIL("a pending read outlived its file handle");
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
        if (!of->used)
            HX_FAIL("a pending write outlived its file handle");
        if (!hx_mem_live(src))
            HX_FAIL("a file write from memory that is not allocated");
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
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_SET_PROTECT:
    case ACTION_CREATE_DIR:
    case ACTION_INFO:
    case ACTION_SET_COMMENT:
    case ACTION_SET_DATE:
        hx_vol_names(pkt);
        return;
    case ACTION_SET_FILE_SIZE:
        of = &hx_opens[d->dp_Arg1 - 1];
        {
            uint32_t cut = (uint32_t)d->dp_Arg2;
            uint32_t keep = cut;
            int k;

            of = &hx_opens[d->dp_Arg1 - 1];
            /* the handler cannot cut below the position of another handle on the file: it answers
             * the size it kept, not an error */
            for (k = 0; k < HX_OPEN_FILES; k++)
                if (hx_opens[k].used && hx_opens[k].node == of->node && &hx_opens[k] != of &&
                    hx_opens[k].pos > keep)
                    keep = hx_opens[k].pos;
            hx_nodes[of->node].size = keep;
            /* the position of this handle moves to the new end when it is past it */
            if (of->pos > keep)
                of->pos = keep;
            hx_vol_reply(pkt, (LONG)keep, 0);
        }
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
    dp->dvp_Flags = hx_devs[i].count > 1 ? DVPF_ASSIGN : 0;
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

    if (x->fl_Task != y->fl_Task ||
        hx_nodes[x->fl_Key].volume != hx_nodes[y->fl_Key].volume)
        return LOCK_DIFFERENT;
    return x->fl_Key == y->fl_Key ? LOCK_SAME : LOCK_SAME_VOLUME;
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
HX_UNUSED static struct Screen *hx_cur_screen;
HX_UNUSED static struct Window *hx_cur_window;
HX_UNUSED static int hx_requesters;
HX_UNUSED static char hx_requester_text[256];
HX_UNUSED static char hx_requester_format[16];
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

HX_UNUSED static struct Library hx_ll_lib, hx_keymap_lib;
HX_UNUSED static int hx_ll_present = 1, hx_keymap_present = 1; /* lowlevel.library, keymap.library open */
HX_UNUSED static int hx_ll_open, hx_keymap_open;               /* opens not closed */

/* locale.library, iffparse.library with clipboard.device, GetVar, DateStamp. */
HX_UNUSED static struct Library hx_locale_lib, hx_iffparse_lib;
HX_UNUSED static int hx_locale_present, hx_iffparse_present;
HX_UNUSED static int hx_host_libs_open;      /* opened and not closed */
HX_UNUSED static unsigned hx_host_lib_opens; /* OpenLibrary calls that found one */
HX_UNUSED static int hx_locale_open_now;     /* OpenLocale without CloseLocale */
HX_UNUSED static char hx_loc_language[64], hx_loc_decimal[8], hx_loc_group[8];
HX_UNUSED static ULONG hx_loc_country;
HX_UNUSED static LONG hx_loc_gmt_west;
HX_UNUSED static struct Locale hx_loc;
HX_UNUSED static struct DateStamp hx_clock;
HX_UNUSED static unsigned hx_getvar_calls;
HX_UNUSED static uint32_t hx_getvar_max_size; /* the largest buffer GetVar was given */
HX_UNUSED static uint32_t hx_getvar_grow_by;
HX_UNUSED static unsigned hx_getvar_grow_times;
HX_UNUSED static int hx_getvar_no_ioerr;
#define HX_ENVS 8
struct hx_env {
    char name[64];
    uint8_t *data;
    uint32_t len;
    int used;
};
HX_UNUSED static struct hx_env hx_envs[HX_ENVS];
#define HX_CLIP_UNITS 256
struct hx_clip {
    uint8_t *data;
    uint32_t len;
    uint8_t *pend;
    uint32_t pend_len;
};
HX_UNUSED static struct hx_clip hx_clips[HX_CLIP_UNITS];
HX_UNUSED static int hx_clip_handles;        /* OpenClipboard without CloseClipboard */
HX_UNUSED static int hx_clip_fail_open;
HX_UNUSED static int hx_clip_fail_io;
HX_UNUSED static unsigned hx_clip_updates;   /* CMD_UPDATE: clips replaced */
HX_UNUSED static int hx_clip_last_unit = -1; /* the unit OpenClipboard was last asked for */
HX_UNUSED static int hx_clip_hold;           /* a read or write waits until hx_clip_release: another program owns the unit */
HX_UNUSED static int hx_clip_abort_ignored;  /* the device does not grant AbortIO */
HX_UNUSED static unsigned long hx_clip_aborts; /* AbortIO calls on a clipboard request */
HX_UNUSED static struct IORequest *hx_clip_held; /* the request that waits */

HX_UNUSED static void hx_env_set(const char *name, const void *data, uint32_t len)
{
    int i;

    for (i = 0; i < HX_ENVS; i++)
        if (!hx_envs[i].used) {
            hx_envs[i].used = 1;
            strncpy(hx_envs[i].name, name, sizeof(hx_envs[i].name) - 1);
            hx_envs[i].data = malloc(len ? len : 1);
            memcpy(hx_envs[i].data, data, len);
            hx_envs[i].len = len;
            return;
        }
    HX_FAIL("too many environment variables");
}

HX_UNUSED static void hx_clip_set(unsigned unit, const void *data, uint32_t len)
{
    free(hx_clips[unit].data);
    hx_clips[unit].data = malloc(len ? len : 1);
    memcpy(hx_clips[unit].data, data, len);
    hx_clips[unit].len = len;
}

HX_UNUSED static struct Library *hx_open_lib(const char *name, ULONG version)
{
    if (strcmp(name, "Picasso96API.library") == 0 && hx_p96_present)
        return &hx_p96_lib;
    if (strcmp(name, "lowlevel.library") == 0 && hx_ll_present) {
        hx_ll_open++;
        return &hx_ll_lib;
    }
    if (strcmp(name, "keymap.library") == 0 && hx_keymap_present) {
        hx_keymap_open++;
        return &hx_keymap_lib;
    }
    if (strcmp(name, "cybergraphics.library") == 0)
        HX_FAIL("OpenLibrary of cybergraphics.library: ScalePixelArray takes no BGRA8888 format");
    if (strcmp(name, "locale.library") == 0 && hx_locale_present) {
        if (version > 38)
            HX_FAIL("locale.library opened with a version newer than OS 3.0 has");
        hx_host_libs_open++;
        hx_host_lib_opens++;
        return &hx_locale_lib;
    }
    if (strcmp(name, "iffparse.library") == 0 && hx_iffparse_present) {
        if (version > 39)
            HX_FAIL("iffparse.library opened with a version newer than OS 3.0 has");
        hx_host_libs_open++;
        hx_host_lib_opens++;
        return &hx_iffparse_lib;
    }
    return NULL;
}

HX_UNUSED static void hx_close_lib(struct Library *l)
{
    if (l == &hx_ll_lib) {
        hx_ll_open--;
        return;
    }
    if (l == &hx_keymap_lib) {
        hx_keymap_open--;
        return;
    }
    if (l == &hx_locale_lib || l == &hx_iffparse_lib) {
        hx_host_libs_open--;
        return;
    }
    if (l != &hx_p96_lib)
        HX_FAIL("CloseLibrary of a library that is not open");
}

HX_UNUSED static struct Locale *hx_open_locale(const char *name)
{
    if (name)
        HX_FAIL("OpenLocale of a named locale (the user's is NULL)");
    if (hx_host_libs_open <= 0 || !hx_locale_present)
        HX_FAIL("OpenLocale without locale.library open");
    hx_loc.loc_LanguageName = hx_loc_language;
    hx_loc.loc_CountryCode = hx_loc_country;
    hx_loc.loc_GMTOffset = hx_loc_gmt_west;
    hx_loc.loc_DecimalPoint = hx_loc_decimal;
    hx_loc.loc_GroupSeparator = hx_loc_group;
    hx_locale_open_now++;
    return &hx_loc;
}

HX_UNUSED static void hx_close_locale(struct Locale *l)
{
    if (l != &hx_loc || hx_locale_open_now <= 0)
        HX_FAIL("CloseLocale of a locale that is not open");
    hx_locale_open_now--;
}

/* GetVar with GVF_GLOBAL_ONLY | GVF_BINARY_VAR, as dos.library V37 and later
 * document it: the value is copied cut to size - 1 bytes and NUL terminated,
 * the return is the number of bytes copied, and IoErr() is the full size of the
 * variable. hx_getvar_no_ioerr: IoErr() is left 0 (a dos.library that does not
 * report the size). hx_getvar_grow_by, for the next hx_getvar_grow_times calls:
 * the variable is that much longer after the call has answered (another
 * program sets it). */
HX_UNUSED static LONG hx_get_var(const char *name, char *buf, LONG size, ULONG flags)
{
    int i;

    hx_getvar_calls++;
    if ((uint32_t)size > hx_getvar_max_size)
        hx_getvar_max_size = (uint32_t)size;
    if (flags != (GVF_GLOBAL_ONLY | GVF_BINARY_VAR))
        HX_FAIL("GetVar with flags other than GVF_GLOBAL_ONLY | GVF_BINARY_VAR");
    if (size <= 0 || !hx_mem_live(buf))
        HX_FAIL("GetVar into memory that is not allocated");
    for (i = 0; i < HX_ENVS; i++) {
        if (hx_envs[i].used && strcmp(hx_envs[i].name, name) == 0) {
            uint32_t n = hx_envs[i].len;
            uint32_t copied = n < (uint32_t)size ? n : (uint32_t)size - 1u;

            memcpy(buf, hx_envs[i].data, copied);
            buf[copied] = 0;
            hx_set_io_err(hx_getvar_no_ioerr ? 0 : (LONG)n);
            if (hx_getvar_grow_times) {
                uint8_t *grown = realloc(hx_envs[i].data, n + hx_getvar_grow_by);

                hx_getvar_grow_times--;
                memset(grown + n, 'g', hx_getvar_grow_by);
                hx_envs[i].data = grown;
                hx_envs[i].len = n + hx_getvar_grow_by;
            }
            return (LONG)copied;
        }
    }
    hx_set_io_err(ERROR_OBJECT_NOT_FOUND);
    return -1;
}

HX_UNUSED static struct ClipboardHandle *hx_open_clipboard(LONG unit)
{
    struct ClipboardHandle *h;

    hx_clip_last_unit = (int)unit;
    if (hx_host_libs_open <= 0 || !hx_iffparse_present)
        HX_FAIL("OpenClipboard without iffparse.library open");
    if (hx_clip_fail_open || unit < 0 || unit >= HX_CLIP_UNITS)
        return NULL;
    h = hx_alloc(sizeof(*h), 1);
    if (!h)
        return NULL;
    h->cbh_Req.io_Message.mn_ReplyPort = hx_port_new();
    if (!h->cbh_Req.io_Message.mn_ReplyPort) {
        hx_free(h);
        return NULL;
    }
    h->cbh_Req.io_Device = &hx_clip_dev;
    h->hx_unit = (int)unit;
    hx_clip_handles++;
    return h;
}

/* CloseClipboard is CloseDevice and the deletion of the reply port: neither is
 * allowed while the device still holds the request or its reply is unread. */
HX_UNUSED static void hx_close_clipboard(struct ClipboardHandle *h)
{
    if (h->cbh_Req.hx_pending)
        HX_FAIL("CloseClipboard with a clipboard request still pending");
    hx_port_delete(h->cbh_Req.io_Message.mn_ReplyPort);
    hx_clip_handles--;
    hx_free(h);
}

/* What the device does with the request of a clipboard handle when it gets to
 * it: CMD_READ reads at io_Offset, CMD_WRITE stores at io_Offset, CMD_UPDATE
 * makes the written data the clip. */
static void hx_clip_exec(struct IORequest *base)
{
    struct IOClipReq *io = (struct IOClipReq *)base;
    struct ClipboardHandle *h = (struct ClipboardHandle *)io;
    struct hx_clip *c = &hx_clips[h->hx_unit];

    io->io_Error = 0;
    io->io_Actual = 0;
    if (!hx_block_live(h))
        HX_FAIL("a clipboard request that is not live memory");
    if (hx_clip_fail_io) {
        io->io_Error = -1;
        return;
    }
    switch (io->io_Command) {
    case CMD_READ: {
        uint32_t avail = io->io_Offset < c->len ? c->len - io->io_Offset : 0;
        uint32_t n = avail < io->io_Length ? avail : io->io_Length;

        if (!hx_mem_live(io->io_Data))
            HX_FAIL("a clipboard read into memory that is not allocated");
        if (n)
            memcpy(io->io_Data, c->data + io->io_Offset, n);
        io->io_Actual = n;
        io->io_Offset += n;
        return;
    }
    case CMD_WRITE:
        if (!hx_mem_live(io->io_Data))
            HX_FAIL("a clipboard write from memory that is not allocated");
        if (io->io_Offset == 0) {
            free(c->pend);
            c->pend = NULL;
            c->pend_len = 0;
        }
        c->pend = realloc(c->pend, io->io_Offset + io->io_Length + 1);
        memcpy(c->pend + io->io_Offset, io->io_Data, io->io_Length);
        c->pend_len = io->io_Offset + io->io_Length;
        io->io_Actual = io->io_Length;
        io->io_Offset += io->io_Length;
        return;
    case CMD_UPDATE:
        free(c->data);
        c->data = c->pend;
        c->len = c->pend_len;
        c->pend = NULL;
        c->pend_len = 0;
        hx_clip_updates++;
        return;
    default:
        HX_FAIL("a clipboard command the code under test must not use");
    }
}

/* The device finishes a request: it is done, and the reply is on the port. */
static void hx_clip_finish(void *arg)
{
    struct IORequest *io = arg;

    if (hx_clip_held == io)
        hx_clip_held = NULL;
    hx_clip_exec(io);
    hx_io_done(io);
}

/* Another program ends its sequence: the request that waited gets its turn. */
HX_UNUSED static void hx_clip_release(void *arg)
{
    (void)arg;
    if (!hx_clip_held)
        HX_FAIL("a clipboard release with no request that waits");
    hx_clip_finish(hx_clip_held);
}

/* SendIO: a read or write is held while hx_clip_hold says another program owns
 * the unit; the rest are done in a moment. */
static void hx_clip_send(struct IORequest *io)
{
    if (hx_clip_hold && (io->io_Command == CMD_READ || io->io_Command == CMD_WRITE)) {
        if (hx_clip_held)
            HX_FAIL("two clipboard requests wait at once");
        hx_clip_held = io;
        return;
    }
    hx_sched(0, hx_clip_finish, io);
}

/* AbortIO: a device that grants it stops the request and replies with
 * IOERR_ABORTED, with no effect on the clip. One that does not (hx_clip_abort_ignored)
 * keeps the request until its turn comes. */
static void hx_clip_abort(struct IORequest *io)
{
    hx_clip_aborts++;
    if (hx_clip_abort_ignored)
        return;
    if (hx_clip_held == io)
        hx_clip_held = NULL;
    hx_cancel(hx_clip_finish, io);
    io->io_Error = -2;
    hx_io_done(io);
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

HX_UNUSED static struct Screen hx_pub_screen;        /* the default public screen (hx_lock_pub_screen) */
HX_UNUSED static int hx_pub_is_p96 = 1;              /* it is a Picasso96 screen */
HX_UNUSED static ULONG hx_p96_bitmap_attr(struct BitMap *bm, ULONG attr)
{
    if (attr == P96BMA_MEMORY)
        return (ULONG)(uintptr_t)bm->Planes[0];
    if (attr == P96BMA_BYTESPERROW)
        return bm->BytesPerRow;
    if (attr == P96BMA_RGBFORMAT)
        return (ULONG)bm->hx_rgb;
    if (attr == P96BMA_BYTESPERPIXEL)
        return hx_bpp(bm->hx_rgb);
    if (attr == P96BMA_ISP96)
        return bm == &hx_pub_screen.bm ? (ULONG)hx_pub_is_p96 : 1UL;
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

/* The drawing area of the last window or screen the test opened: p96WritePixelArray and
 * p96RectFill write here, at the coordinates the rast port has (the window's top-left is 0,0).
 * A pixel is hx_canvas_bpp bytes at x * bpp in its row. */
#define HX_CANVAS_W 256
#define HX_CANVAS_H 256
HX_UNUSED static uint8_t hx_canvas[HX_CANVAS_W * HX_CANVAS_H * 4];
HX_UNUSED static uint32_t hx_canvas_bpp = 4;
HX_UNUSED static unsigned long hx_pwa_calls, hx_rectfills;
HX_UNUSED static int hx_pwa_last_rgb = -1;
HX_UNUSED static const void *hx_pwa_last_mem;       /* RenderInfo.Memory of the last p96WritePixelArray */
HX_UNUSED static int hx_bitmaps_live, hx_pointers_live;
/* The library's ZZ9000 table (X_AAR_EXT): 0 the reference platform's, with real
 * stable surface allocations; 1 an older table that ends at lease_pause; 2 none. */
HX_UNUSED static int hx_surface_ext;
/* AllocMem answers memory in the arena, which AARSharedToken names: a platform
 * that shares Amiga RAM. The blocks live until FreeMem. */
HX_UNUSED static int hx_exec_mem_in_arena;
HX_UNUSED static int hx_arena_mem_live;
/* The public screen's bitmap is in the arena too (its scanout can be named). */
HX_UNUSED static int hx_pub_scanout;
HX_UNUSED static int hx_ptr_kind;                    /* 0 default, 1 busy, 2 an object */
HX_UNUSED static void *hx_ptr_obj;
HX_UNUSED static unsigned long hx_ptr_calls;
HX_UNUSED static WORD hx_pub_w = 640, hx_pub_h = 480;
HX_UNUSED static int hx_pub_rgb = RGBFB_B8G8R8A8;
HX_UNUSED static ULONG hx_pub_mode_id = 0x50002UL;
HX_UNUSED static int hx_intuition_version = 47;

HX_UNUSED static struct Screen *hx_lock_pub_screen(const char *name)
{
    struct Screen *s = &hx_pub_screen;

    (void)name;
    hx_pub_locks++;
    s->Width = hx_pub_w;
    s->Height = hx_pub_h;
    s->bm.hx_rgb = hx_pub_rgb;
    s->bm.Rows = (UWORD)hx_pub_h;
    s->bm.BytesPerRow = (UWORD)(hx_pub_w * (WORD)hx_bpp(hx_pub_rgb));
    if (hx_pub_scanout && !s->bm.Planes[0])
        s->bm.Planes[0] = hx_gfx_mem((uint32_t)s->bm.BytesPerRow * s->bm.Rows);
    s->RastPort.BitMap = &s->bm;
    s->ViewPort.RasInfo = &s->ras;
    return s;
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
    struct Screen *scr = NULL, *pub = NULL;
    ULONG idcmp = 0, iw = 0, ih = 0;
    int close_gadget = 0, size_gadget = 0, size_bottom = 0;
    const char *title = NULL;

    (void)newwin;
    va_start(ap, first);
    while (tag != TAG_DONE) {
        ULONG v = va_arg(ap, ULONG);

        if (tag == WA_CustomScreen)
            scr = (struct Screen *)(uintptr_t)v;
        else if (tag == WA_PubScreen)
            pub = (struct Screen *)(uintptr_t)v;
        else if (tag == WA_IDCMP)
            idcmp = v;
        else if (tag == WA_InnerWidth)
            iw = v;
        else if (tag == WA_InnerHeight)
            ih = v;
        else if (tag == WA_CloseGadget)
            close_gadget = v != 0;
        else if (tag == WA_SizeGadget)
            size_gadget = v != 0;
        else if (tag == WA_SizeBBottom)
            size_bottom = v != 0;
        else if (tag == WA_Title)
            title = (const char *)(uintptr_t)v;
        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
    w = hx_alloc(sizeof(*w), 1);
    w->UserPort = hx_port_new();
    w->Flags = WFLG_WINDOWACTIVE;
    w->RPort = &w->hx_rp;
    w->IDCMPFlags = idcmp;
    w->hx_close_gadget = close_gadget;
    w->hx_size_gadget = size_gadget;
    w->hx_custom_screen = scr != NULL;
    w->hx_pub = pub;
    if (title)
        snprintf(w->hx_title, sizeof(w->hx_title), "%s", title);
    if (pub) {
        /* a window with borders: 4 left and right, 11 on top, 4 or (a size gadget in it) 10 below */
        w->WScreen = pub;
        w->BorderLeft = 4;
        w->BorderRight = 4;
        w->BorderTop = 11;
        w->BorderBottom = (BYTE)(size_bottom ? 10 : 4);
        w->Width = (WORD)(iw + 8u);
        w->Height = (WORD)(ih + 11u + (size_bottom ? 10u : 4u));
    } else if (scr) {
        w->WScreen = scr;
        w->Width = scr->Width;
        w->Height = scr->Height;
    }
    w->hx_rp.BitMap = w->WScreen ? w->WScreen->RastPort.BitMap : NULL;
    memset(hx_canvas, 0xCD, sizeof(hx_canvas));
    hx_windows++;
    hx_cur_window = w;
    (void)size_gadget;
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
HX_UNUSED static APTR hx_post_iaddr;          /* IAddress of the next message posted */
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
    im->IAddress = hx_post_iaddr;
    hx_post(hx_cur_window->UserPort, &im->ExecMessage);
}

HX_UNUSED static void hx_reply_msg(struct Message *m)
{
    /* The window's messages are the test's: the reply gives them back. */
    hx_free(m);
}

/* ---- Surfaces, what a window shows, pointers, the display database, input libraries ------------- */

struct hx_ptr {
    struct BitMap *bm;
    LONG xo, yo;
};

HX_UNUSED static void hx_set_window_titles(struct Window *w, const char *wt, const char *st)
{
    if (wt != (const char *)~0UL)
        snprintf(w->hx_title, sizeof(w->hx_title), "%s", wt ? wt : "");
    if (st != (const char *)~0UL)
        snprintf(w->hx_screen_title, sizeof(w->hx_screen_title), "%s", st ? st : "");
}

HX_UNUSED static void hx_set_window_pointer(struct Window *w, ULONG first, ...)
{
    va_list ap;
    ULONG tag = first;

    (void)w;
    hx_ptr_calls++;
    hx_ptr_kind = 0;
    hx_ptr_obj = NULL;
    va_start(ap, first);
    while (tag != TAG_DONE) {
        ULONG v = va_arg(ap, ULONG);

        if (tag == WA_BusyPointer && v)
            hx_ptr_kind = 1;
        else if (tag == WA_Pointer) {
            hx_ptr_kind = 2;
            hx_ptr_obj = (void *)(uintptr_t)v;
        }
        tag = va_arg(ap, ULONG);
    }
    va_end(ap);
}

HX_UNUSED static void *hx_new_pointer(struct BitMap *bm, LONG xo, LONG yo)
{
    struct hx_ptr *p = hx_alloc(sizeof(*p), 1);

    if (!p)
        return NULL;
    p->bm = bm;
    p->xo = xo;
    p->yo = yo;
    hx_pointers_live++;
    return p;
}

HX_UNUSED static void hx_dispose_pointer(void *obj)
{
    if (obj == hx_ptr_obj)
        HX_FAIL("DisposeObject of the pointer a window still shows");
    hx_free(obj);
    hx_pointers_live--;
}

HX_UNUSED static ULONG hx_prefs_colors[3];
HX_UNUSED static void hx_get_prefs(struct Preferences *p, ULONG size)
{
    if (size < sizeof(*p))
        HX_FAIL("GetPrefs into too small a buffer");
    p->color17 = (UWORD)hx_prefs_colors[0];
    p->color18 = (UWORD)hx_prefs_colors[1];
    p->color19 = (UWORD)hx_prefs_colors[2];
}

/* graphics.library bitmaps: planar */
HX_UNUSED static struct BitMap *hx_alloc_bitmap(ULONG w, ULONG h, ULONG depth, ULONG flags,
                                                struct BitMap *friend)
{
    struct BitMap *bm;
    uint32_t bpr = ((w + 15u) / 16u) * 2u, i;

    (void)friend;
    if (depth == 0 || depth > 8 || h == 0 || w == 0)
        HX_FAIL("AllocBitMap with a bad size");
    bm = hx_alloc(sizeof(*bm), 1);
    if (!bm)
        return NULL;
    for (i = 0; i < depth; i++)
        bm->Planes[i] = hx_alloc(bpr * h, (flags & BMF_CLEAR) != 0);
    bm->Rows = (UWORD)h;
    bm->BytesPerRow = (UWORD)bpr;
    hx_bitmaps_live++;
    return bm;
}

HX_UNUSED static void hx_free_bitmap(struct BitMap *bm)
{
    int i;

    for (i = 0; i < 8; i++)
        if (bm->Planes[i])
            hx_free(bm->Planes[i]);
    hx_free(bm);
    hx_bitmaps_live--;
}


HX_UNUSED static void hx_p96_write_pixel_array(struct RenderInfo *ri, UWORD sx, UWORD sy,
                                               struct RastPort *rp, UWORD dx, UWORD dy,
                                               UWORD w, UWORD h)
{
    uint32_t bpp = hx_bpp((int)ri->RGBFormat), y;

    (void)rp;
    if (!ri->Memory)
        HX_FAIL("p96WritePixelArray from no memory");
    if ((uint32_t)dx + w > HX_CANVAS_W || (uint32_t)dy + h > HX_CANVAS_H)
        HX_FAIL("p96WritePixelArray outside the fake drawing area");
    hx_pwa_calls++;
    hx_pwa_last_rgb = (int)ri->RGBFormat;
    hx_pwa_last_mem = ri->Memory;
    hx_canvas_bpp = bpp;
    for (y = 0; y < h; y++)
        memcpy(hx_canvas + (uint32_t)(dy + y) * HX_CANVAS_W * 4u + (uint32_t)dx * bpp,
               (const uint8_t *)ri->Memory + (uint32_t)(sy + y) * (uint32_t)(UWORD)ri->BytesPerRow +
                   (uint32_t)sx * bpp,
               (uint32_t)w * bpp);
}

HX_UNUSED static void hx_p96_rect_fill(struct RastPort *rp, UWORD x0, UWORD y0, UWORD x1,
                                       UWORD y1, ULONG argb)
{
    uint32_t y, bpp = hx_canvas_bpp;

    (void)rp;
    (void)argb;
    if (x1 >= HX_CANVAS_W || y1 >= HX_CANVAS_H || x1 < x0 || y1 < y0)
        HX_FAIL("p96RectFill outside the fake drawing area");
    hx_rectfills++;
    hx_canvas_bpp = bpp;
    for (y = y0; y <= y1; y++)
        memset(hx_canvas + y * HX_CANVAS_W * 4u + (uint32_t)x0 * bpp, 0,
               (uint32_t)(x1 - x0 + 1u) * bpp);
}

/* the display database: the modes the test has */
struct hx_mode {
    ULONG id;
    int w, h;
    int is_p96;
    ULONG formats;               /* RGBFF_* */
    UWORD rows, clocks;
    UWORD unavailable;
};
HX_UNUSED static struct hx_mode hx_modes[16];
HX_UNUSED static int hx_nmodes;

HX_UNUSED static void hx_modes_default(void)
{
    static const struct hx_mode def[] = {
        /* Keep padded bitmaps inside the fake claim's graphics arena. */
        {0x50001UL, 128, 96, 1, RGBFF_R5G6B5 | RGBFF_CLUT, 525, 113, 0},
        {0x50002UL, 1280, 720, 1, RGBFF_R5G6B5 | RGBFF_B8G8R8A8 | RGBFF_CLUT, 750, 95, 0},
        {0x50003UL, 1920, 1080, 1, RGBFF_B8G8R8A8, 1125, 63, 0},
        {0x50004UL, 800, 600, 1, RGBFF_R5G6B5, 628, 90, 1},
        {0x50005UL, 640, 256, 0, RGBFF_CLUT, 312, 227, 0},
    };

    memcpy(hx_modes, def, sizeof(def));
    hx_nmodes = (int)(sizeof(def) / sizeof(def[0]));
}

static struct hx_mode *hx_mode_of(ULONG id)
{
    int i;

    for (i = 0; i < hx_nmodes; i++)
        if (hx_modes[i].id == id)
            return &hx_modes[i];
    return NULL;
}

HX_UNUSED static ULONG hx_next_display_info(ULONG id)
{
    int i = 0;

    if (id != INVALID_ID) {
        struct hx_mode *m = hx_mode_of(id);

        i = m ? (int)(m - hx_modes) + 1 : hx_nmodes;
    }
    return i < hx_nmodes ? hx_modes[i].id : INVALID_ID;
}

HX_UNUSED static ULONG hx_get_display_info(void *buf, ULONG size, ULONG tag, ULONG id)
{
    struct hx_mode *m = hx_mode_of(id);

    if (!m)
        return 0;
    memset(buf, 0, size);
    if (tag == DTAG_DISP) {
        ((struct DisplayInfo *)buf)->NotAvailable = m->unavailable;
    } else if (tag == DTAG_DIMS) {
        struct DimensionInfo *d = buf;

        d->Nominal.MaxX = (WORD)(m->w - 1);
        d->Nominal.MaxY = (WORD)(m->h - 1);
    } else if (tag == DTAG_MNTR) {
        struct MonitorInfo *mi = buf;

        mi->TotalRows = m->rows;
        mi->TotalColorClocks = m->clocks;
    } else {
        return 0;
    }
    return size;
}

HX_UNUSED static ULONG hx_p96_mode_attr(ULONG id, ULONG attr)
{
    struct hx_mode *m = hx_mode_of(id);

    if (!m)
        return 0;
    if (attr == P96IDA_ISP96)
        return (ULONG)m->is_p96;
    if (attr == P96IDA_COMPATIBLEFORMATS)
        return m->formats;
    return 0;
}

/* lowlevel.library's ReadJoyPort and keymap.library's MapRawKey */
HX_UNUSED static ULONG hx_jp[2];
HX_UNUSED static unsigned long hx_jp_reads, hx_keymap_calls;

HX_UNUSED static ULONG hx_read_joy_port(ULONG port)
{
    hx_jp_reads++;
    return hx_jp[port & 1];
}

/* A keymap of a few keys: the letter rows (lower case, upper with Shift), Space,
 * Return, Backspace, the cursor up key (a CSI sequence), a key with a letter above
 * 0x7F (raw 0x0D), and a dead key: the raw key 0x00 (the back quote) makes no
 * character, and the key 0x12 (E) after it makes an e with a grave accent. The
 * previous key of a dead key sequence is ie_EventAddress, as Intuition passes it. */
HX_UNUSED static LONG hx_map_raw_key(struct InputEvent *ie, UBYTE *buf, LONG len)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    unsigned raw = ie->ie_Code;
    int shift = (ie->ie_Qualifier & 3) != 0, n = 0;
    unsigned char out[4];

    hx_keymap_calls++;
    if (ie->ie_Class != IECLASS_RAWKEY || (raw & 0x80))
        HX_FAIL("MapRawKey of an event that is not a key press");
    if (raw == 0x12 && ie->ie_EventAddress) {
        out[n++] = 0xE8;
    } else if (raw >= 0x10 && raw <= 0x19) {
        out[n++] = (unsigned char)(shift ? row1[raw - 0x10] - 32 : row1[raw - 0x10]);
    } else if (raw >= 0x20 && raw <= 0x28) {
        out[n++] = (unsigned char)(shift ? row2[raw - 0x20] - 32 : row2[raw - 0x20]);
    } else if (raw >= 0x31 && raw <= 0x37) {
        out[n++] = (unsigned char)(shift ? row3[raw - 0x31] - 32 : row3[raw - 0x31]);
    } else if (raw == 0x40) {
        out[n++] = ' ';
    } else if (raw == 0x44) {
        out[n++] = 0x0D;
    } else if (raw == 0x41) {
        out[n++] = 0x08;
    } else if (raw == 0x4C) {
        out[n++] = 0x9B;
        out[n++] = 'A';
    } else if (raw == 0x0D) {
        out[n++] = 0xE9;
    }
    if (n > len)
        return -1;
    memcpy(buf, out, (size_t)n);
    return n;
}

HX_UNUSED static LONG hx_easy_request(void *win, struct EasyStruct *es, void *a, void *b)
{
    const char *format = es->es_TextFormat ? (const char *)es->es_TextFormat : "";

    (void)win;
    (void)a; /* the IDCMP pointer */
    hx_requesters++;
    snprintf(hx_requester_format, sizeof(hx_requester_format), "%s", format);
    /* Only a fixed "%s" is expanded, with its one argument; any other format is
     * recorded as it is, so a test sees a directive that would be interpreted. */
    if (strcmp(format, "%s") == 0 && b)
        snprintf(hx_requester_text, sizeof(hx_requester_text), "%s",
                 ((const char *const *)b)[0]);
    else
        snprintf(hx_requester_text, sizeof(hx_requester_text), "%s", format);
    return 1;
}

/* ---- Memory bindings ------------------------------------------------------------------------ */

/* Exec memory: from the pool, or, as a platform that shares Amiga RAM, from the
 * arena. A block in the arena is told apart by its address when it is freed. */
HX_UNUSED static int hx_in_arena_slot(const void *p)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)refplat_ptr(REFPLAT_TOKEN_BASE +
                                                                   HX_GFX_SLOT * REFPLAT_CLAIM_STRIDE);

    return (const uint8_t *)p >= base && (const uint8_t *)p < base + REFPLAT_CLAIM_STRIDE;
}

HX_UNUSED static void *hx_alloc_mem(ULONG bytes, ULONG flags)
{
    if (hx_exec_mem_in_arena && !hx_fail_alloc) {
        void *p = hx_gfx_mem(bytes);

        if (flags & MEMF_CLEAR)
            memset(p, 0, bytes);
        hx_arena_mem_live++;
        return p;
    }
    return hx_alloc(bytes, (flags & MEMF_CLEAR) != 0);
}

HX_UNUSED static void hx_free_mem(void *p, ULONG bytes)
{
    if (hx_in_arena_slot(p)) {
        if (!hx_arena_mem_live)
            HX_FAIL("FreeMem of arena memory that is not live");
        hx_arena_mem_live--;
        return;
    }
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
HX_UNUSED static int hx_token_fail;        /* the next n AARSharedToken calls answer AAR_E_INVAL */

HX_UNUSED static LONG hx_aar_shared_token(AARHandle *h, const void *p, ULONG n, ULONG *t)
{
    LONG rc;

    if (hx_token_fail > 0) {
        hx_token_fail--;
        return AAR_E_INVAL;
    }
    rc = aar_core_shared_token(hx_aar, h, p, n, t);

    if (rc == AAR_OK)
        *t += hx_token_add;
    return rc;
}

HX_UNUSED static LONG hx_aar_set_log_sink(AARHandle *h, void (*sink)(void *, const void *, ULONG), void *user)
{
    return aar_core_set_log_sink(hx_aar, h, (AARLogSink)(uintptr_t)sink, (ULONG)(uintptr_t)user);
}

/* AARExt: the reference platform's ZZ9000 table, an older one that ends at
 * lease_pause (the member count is tested by the caller), or none. */
HX_UNUSED static const aar_client_ext_zz9000 *hx_aar_ext(void)
{
    static aar_client_ext_zz9000 older;
    const aar_client_ext_zz9000 *ext;

    if (hx_surface_ext == 2)
        return NULL;
    ext = (const aar_client_ext_zz9000 *)aar_core_ext(hx_aar, AAR_PLATFORM_ZZ9000,
                                                      AAR_EXT_ZZ9000_CLIENT);
    if (ext && hx_surface_ext == 1) {
        older = *ext;
        older.size = (ULONG)offsetof(aar_client_ext_zz9000, surface_alloc);
        return &older;
    }
    return ext;
}

/* surface_alloc with a plain pointer: the stub header's APTR is a 32-bit word. */
HX_UNUSED static LONG hx_surface_alloc(const aar_client_ext_zz9000 *ext, ULONG bytes,
                                       ULONG *handle, void **mem)
{
    uint32_t word = 0;
    LONG rc = ext->surface_alloc(bytes, handle, &word);

    if (rc == AAR_OK)
        *mem = (void *)(uintptr_t)word;
    return rc;
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
    hx_ahi_short = 0;
    hx_ahi_tail = NULL;
    hx_next_sigbit = 16;
    hx_ahi_fail_open = hx_ahi_open_count = hx_timer_open_count = hx_timer_fail_open = 0;
    hx_ahi_writes = hx_ahi_bytes = 0;
    hx_ahi_pending_now = hx_ahi_max_pending = 0;
    hx_packets = 0;
    memset(hx_packets_by, 0, sizeof(hx_packets_by));
    hx_vol_delay_us = 200;
    hx_vol_never = hx_vol_full = 0;
    hx_vol_readonly = 0;
    hx_vol_blocks = 1000;
    hx_vol_blocks_used = 250;
    hx_locale_present = hx_iffparse_present = 1;
    hx_host_libs_open = 0;
    hx_host_lib_opens = 0;
    hx_locale_open_now = 0;
    strcpy(hx_loc_language, "english.language");
    strcpy(hx_loc_decimal, ".");
    strcpy(hx_loc_group, ",");
    hx_loc_country = 0x00004742UL; /* "GB" */
    hx_loc_gmt_west = 0;
    hx_clock.ds_Days = 0;
    hx_clock.ds_Minute = 0;
    hx_clock.ds_Tick = 0;
    hx_getvar_calls = 0;
    hx_getvar_max_size = 0;
    hx_getvar_grow_by = hx_getvar_grow_times = 0;
    hx_getvar_no_ioerr = 0;
    {
        int i;

        for (i = 0; i < HX_ENVS; i++) {
            free(hx_envs[i].data);
            memset(&hx_envs[i], 0, sizeof(hx_envs[i]));
        }
        for (i = 0; i < HX_CLIP_UNITS; i++) {
            free(hx_clips[i].data);
            free(hx_clips[i].pend);
            memset(&hx_clips[i], 0, sizeof(hx_clips[i]));
        }
    }
    hx_clip_handles = 0;
    hx_clip_fail_open = hx_clip_fail_io = 0;
    hx_clip_updates = 0;
    hx_clip_last_unit = -1;
    hx_clip_hold = hx_clip_abort_ignored = 0;
    hx_clip_aborts = 0;
    hx_clip_held = NULL;
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
    hx_requester_format[0] = 0;
    hx_gfx_next = 0;
    hx_vblank_us = 20000;
    hx_joydat[0] = hx_joydat[1] = 0;
    hx_ciaa_pra = 0xFF;
    hx_token_add = 0;
    hx_ll_present = hx_keymap_present = 1;
    hx_ll_open = hx_keymap_open = 0;
    hx_intuition_version = 47;
    hx_intuition_base.ActiveScreen = NULL;
    hx_pub_w = 640;
    hx_pub_h = 480;
    hx_pub_rgb = RGBFB_B8G8R8A8;
    hx_pub_is_p96 = 1;
    hx_pub_mode_id = 0x50002UL;
    memset(&hx_pub_screen, 0, sizeof(hx_pub_screen));
    hx_surface_ext = 0;
    hx_exec_mem_in_arena = 0;
    hx_arena_mem_live = 0;
    hx_pub_scanout = 0;
    hx_token_fail = 0;
    hx_bitmaps_live = hx_pointers_live = 0;
    hx_pwa_calls = hx_rectfills = 0;
    hx_pwa_last_rgb = -1;
    hx_pwa_last_mem = NULL;
    hx_ptr_kind = 0;
    hx_ptr_obj = NULL;
    hx_ptr_calls = 0;
    hx_post_iaddr = NULL;
    hx_jp[0] = hx_jp[1] = JP_TYPE_NOTAVAIL;
    hx_jp_reads = 0;
    hx_keymap_calls = 0;
    hx_prefs_colors[0] = 0x0000;
    hx_prefs_colors[1] = 0x0FFF;
    hx_prefs_colors[2] = 0x0F80;
    memset(hx_canvas, 0, sizeof(hx_canvas));
    hx_canvas_bpp = 4;
    hx_modes_default();
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
#define X_DATE_STAMP(ds) (*(ds) = hx_clock)
#define X_GET_VAR(name, buf, size, flags) hx_get_var((const char *)(name), (char *)(buf), (size), (flags))
#define X_OPEN_LOCALE(s, name) ((void)(s), hx_open_locale((const char *)(name)))
#define X_CLOSE_LOCALE(s, loc) ((void)(s), hx_close_locale(loc))
#define X_OPEN_CLIPBOARD(s, unit) ((void)(s), hx_open_clipboard(unit))
#define X_CLOSE_CLIPBOARD(s, cb) ((void)(s), hx_close_clipboard(cb))

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
#define X_AAR_EXT() hx_aar_ext()
#define X_AAR_SURFACE_ALLOC(ext, bytes, handle, mem) hx_surface_alloc((ext), (bytes), (handle), (mem))
#define X_AAR_SURFACE_FREE(ext, handle) ((ext)->surface_free(handle))

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
#define X_SET_WINDOW_TITLES(win, wt, st) hx_set_window_titles((win), (wt), (st))
#define X_SET_WINDOW_POINTER(...) hx_set_window_pointer(__VA_ARGS__)
#define X_NEW_POINTER(bm, xo, yo) hx_new_pointer((bm), (xo), (yo))
#define X_DISPOSE_POINTER(obj) hx_dispose_pointer(obj)
#define X_GET_PREFS(prefs, size) hx_get_prefs((prefs), (size))
#define X_ALLOC_BITMAP(w, h, depth, flags, friend) hx_alloc_bitmap((w), (h), (depth), (flags), (friend))
#define X_FREE_BITMAP(bm) hx_free_bitmap(bm)
#define X_GET_VP_MODE_ID(vp) ((void)(vp), hx_pub_mode_id)
#define X_NEXT_DISPLAY_INFO(id) hx_next_display_info(id)
#define X_GET_DISPLAY_INFO(buf, size, tag, id) hx_get_display_info((buf), (size), (tag), (id))
#define X_INTUITION_VERSION() ((ULONG)hx_intuition_version)
#define X_P96_WRITE_PIXEL_ARRAY(s, ri, sx, sy, rp, dx, dy, w, h) \
    ((void)(s), hx_p96_write_pixel_array((ri), (sx), (sy), (rp), (dx), (dy), (w), (h)))
#define X_P96_RECT_FILL(s, rp, x0, y0, x1, y1, argb) \
    ((void)(s), hx_p96_rect_fill((rp), (x0), (y0), (x1), (y1), (argb)))
#define X_P96_MODE_ATTR(s, id, attr) ((void)(s), hx_p96_mode_attr((id), (attr)))
#define X_READ_JOY_PORT(s, port) ((void)(s), hx_read_joy_port(port))
#define X_MAP_RAW_KEY(s, ie, buf, len) ((void)(s), hx_map_raw_key((ie), (UBYTE *)(buf), (len)))

#endif /* SERVE_HOST_H */
