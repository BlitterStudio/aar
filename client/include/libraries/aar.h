/*
 * aar.library: the Amiga client library of the AAR standard (spec section 9).
 *
 * This header holds the types, constants and tag ids that a client of
 * aar.library uses. The calls are in clib/aar_protos.h and, through the usual
 * proto/aar.h, in the pragma and inline headers generated from
 * client/fd/aar_lib.fd. docs/aar-client-api.md describes every call.
 *
 * It reuses the constants of the app side and does not repeat them: AAR_OK and
 * the AAR_E_* error codes, AAR_REASON_*, AAR_STATE_*, AAR_END_*, AAR_FAULT_*,
 * AAR_GROUP_*, AAR_PROFILE_*, AAR_PLATFORM_*, AAR_INFOF_* and the version
 * macros come from <aar/app.h>. The AAR_OP_* codes, the AAR_A_* and AAR_R_*
 * argument and result word indexes, AAR_CBF_* and AAR_AUDIO_ROUTE_* come from
 * <aar/transport.h>. Both are included here.
 *
 * Layout rules (spec 1.3): every structure uses the AmigaOS m68k layout,
 * big-endian, with LONG, ULONG, APTR, and (only inside struct Message and
 * struct aar_event) the 16-bit types, at 2-byte alignment. Each structure that
 * a call fills or reads starts with `size`, holds the sizeof the caller was
 * built with, and only ever grows at the end. A call reads or writes at most
 * min(size, sizeof the library's structure) bytes. Where it fills a structure
 * it zeroes the rest of what it fills. Reserved words are zero. Host builds
 * (the tests) get ULONG and APTR as 32-bit integers from the stub headers in
 * client/tests/stubs, so every offset is the same as on the 68k.
 *
 * Use ULONG and LONG for the words you pass by address (reason, got, token).
 * On the 68k they are `unsigned long` and `long`, which are not the types
 * uint32_t and int32_t name.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBRARIES_AAR_H
#define LIBRARIES_AAR_H

#include <exec/types.h>
#include <exec/ports.h>
#include <utility/tagitem.h>

#include <aar/app.h>
#include <aar/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Library name and version -------------------------------------------- */

#define AAR_LIB_NAME "aar.library"

/* OpenLibrary(AAR_LIB_NAME, AAR_LIB_VERSION). The library version is the API
 * major (spec 9.1), 0 until the API reaches 1.0; the revision is the
 * implementation's own. At 1.0 this macro and nothing else changes. */
#define AAR_LIB_VERSION AAR_API_MAJOR

/* ---- Profiles as masks ---------------------------------------------------- */

/* AAR_PROFILE_A32 and AAR_PROFILE_A64 are values; AARInfo.profiles is a mask
 * of these. */
#define AAR_PROFILEF_A32 (1u << (AAR_PROFILE_A32 - 1u))
#define AAR_PROFILEF_A64 (1u << (AAR_PROFILE_A64 - 1u))

/* ---- Structures ----------------------------------------------------------- */

/* An open app instance. Opaque. A handle belongs to the task that called
 * AAROpen; use it from that task only (spec 9.3). */
struct AARHandle;
typedef struct AARHandle AARHandle;

typedef struct AARU64 {
    ULONG hi, lo;
} AARU64;

/*
 * AARQuery: what the platform offers (128 bytes). Static limits come from the
 * platform's constants; the live counts are in `platform`. Fields are
 * 0 when the platform has no such limit or does not report it, as noted.
 */
typedef struct AARInfo {
    ULONG size;               /*   0 sizeof; set by the caller */
    ULONG api_major;          /*   4 AAR_API_MAJOR the platform implements */
    ULONG api_minor;          /*   8 AAR_API_MINOR */
    ULONG platform_id;        /*  12 AAR_PLATFORM_* */
    ULONG profiles;           /*  16 mask of AAR_PROFILEF_* */
    ULONG groups;             /*  20 AAR_GROUP_* the platform provides */
    ULONG flags;              /*  24 AAR_INFOF_* */
    ULONG max_apps;           /*  28 apps at once; 0: limited by resources (ZZ9000) */
    ULONG max_image_bytes;    /*  32 largest image AARLoad* takes */
    ULONG max_args;           /*  36 most strings AARStart takes */
    ULONG max_arg_bytes;      /*  40 most bytes of one string, NUL included */
    ULONG shared_granule;     /*  44 publish and acquire granularity, bytes */
    ULONG msg_capacity;       /*  48 messages in flight per direction */
    ULONG stall_limit_ms;     /*  52 watchdog stall limit */
    ULONG stop_grace_ms;      /*  56 time an app gets to honour a stop request */
    ULONG shared_default_mib; /*  60 default shared-buffer limit, MiB */
    ULONG shared_max_mib;     /*  64 largest AAR_TAG_SHARED_MIB the platform grants */
    ULONG weight_max;         /*  68 largest AAR_TAG_WEIGHT; 0: no weighting */
    ULONG queue_depth;        /*  72 entries per queue of a new claim */
    ULONG reserved0;          /*  76 0 */
    ULONG platform[8];        /*  80 platform words; platform_id says what they mean */
    ULONG reserved1[4];       /* 112 0 */
} AARInfo;

/*
 * AARProbeFile and AARProbeMemory: the AAR ELF note of an image (48 bytes).
 * The probe reports what the file declares; it does not judge it against the
 * platform. A launcher compares `profile` with AARInfo.profiles and the group
 * masks with AARInfo.groups itself.
 */
typedef struct AARImageInfo {
    ULONG size;               /*  0 sizeof; set by the caller */
    ULONG profile;            /*  4 AAR_PROFILE_* */
    ULONG api_major;          /*  8 major the app was built against */
    ULONG api_minor_required; /* 12 lowest minor with every call the app needs */
    ULONG stack_bytes;        /* 16 as in the note: 0 is the 1 MiB default */
    ULONG min_heap_mib;       /* 20 private heap committed at load, MiB */
    ULONG groups_required;    /* 24 AAR_GROUP_* bits */
    ULONG groups_optional;    /* 28 AAR_GROUP_* bits, a hint */
    ULONG note_version;       /* 32 AAR_NOTE_VERSION of the note */
    ULONG image_bytes;        /* 36 bytes of the image (file or memory length) */
    ULONG reserved[2];        /* 40 0 */
} AARImageInfo;

/*
 * AARStatus: a snapshot of one app. Fields from `state` to `end_reason` come
 * first so that a poll with size = 16 reads three control block words.
 */
typedef struct AARStatusInfo {
    ULONG  size;              /*   0 sizeof; set by the caller */
    ULONG  state;             /*   4 AAR_STATE_* */
    ULONG  generation;        /*   8 the handle's generation (no read) */
    ULONG  end_reason;        /*  12 AAR_END_* */
    LONG   exit_code;         /*  16 */
    ULONG  flags;             /*  20 AAR_CBF_* */
    ULONG  profile;           /*  24 AAR_PROFILE_* of the loaded image */
    ULONG  fault_class;       /*  28 AAR_FAULT_* */
    ULONG  fault_syndrome;    /*  32 */
    ULONG  fault_pstate;      /*  36 */
    AARU64 fault_pc;          /*  40 */
    AARU64 fault_sp;          /*  48 */
    AARU64 fault_lr;          /*  56 */
    AARU64 fault_addr;        /*  64 */
    AARU64 load_base;         /*  72 */
    ULONG  reason;            /*  80 last AAR_REASON_* of a refused open, load or start */
    ULONG  lease_period_ms;   /*  84 */
    ULONG  msg_consumed;      /*  88 client-to-app messages the app has taken */
    ULONG  reserved;          /*  92 0 */
    AARU64 progress;          /*  96 load progress, bytes */
    AARU64 shared_used;       /* 104 */
    AARU64 private_committed; /* 112 */
    ULONG  sq_full;           /* 120 times the app side found the SQ full */
    ULONG  log_dropped;       /* 124 log records dropped */
    ULONG  events_dropped;    /* 128 events the client dropped (CQ full) */
    ULONG  cq_full;           /* 132 times the client found the CQ full */
} AARStatusInfo;

/* Types of AAREvent.type. */
#define AAR_NOTIFY_MESSAGE  1u /* a message from the app is waiting */
#define AAR_NOTIFY_COMPLETE 2u /* the app ended (exit, stop, forced) */
#define AAR_NOTIFY_FAULT    3u /* the app ended in a fault or a watchdog end */
#define AAR_NOTIFY_REQUEST  4u /* a service request is queued */

/*
 * What the library posts to the notify port given to AAROpen (164 bytes). The
 * message is never replied: ReplyMsg is not needed and mn_ReplyPort is NULL.
 * `handle` names the app, so one port can serve several handles. Take the
 * event with GetMsg; the library posts the next only after that. Events
 * coalesce (spec 9.4): the type is MESSAGE when a message is waiting, else
 * COMPLETE or FAULT when the app has ended, else REQUEST. `handle` is a
 * pointer, kept as APTR to hold the 4-byte layout on every build; cast it to
 * AARHandle *.
 */
typedef struct AAREvent {
    struct Message message;   /*  0 20 bytes */
    ULONG type;               /* 20 AAR_NOTIFY_* */
    APTR  handle;             /* 24 the AARHandle * */
    AARStatusInfo status;     /* 28 the status when the event was posted */
} AAREvent;

/* One message in either direction: AAR_MSG_BYTES opaque bytes. The library
 * never swaps them (spec 1.3), so the two sides agree on their order. */
typedef struct AARMessage {
    UBYTE bytes[AAR_MSG_BYTES];
} AARMessage;

/* AARSharedMap.flags as the library reports them. The `flags` argument of the
 * call is reserved: pass 0, any other bit is AAR_E_INVAL. */
#define AAR_MAPF_UNCACHED 1u /* the map is cache-inhibited */
#define AAR_MAPF_COHERENT 2u /* the platform is coherent end to end */

/*
 * A mapped shared buffer (32 bytes). It has no typedef: its name is also the
 * name of the call that fills it, and C keeps both in the same namespace.
 * Write `struct AARSharedMap`. reserved[0] belongs to the library.
 */
struct AARSharedMap {
    ULONG size;               /*  0 sizeof; set by the caller */
    APTR  base;               /*  4 68k pointer to the first byte */
    ULONG bytes;              /*  8 */
    ULONG haddr;              /* 12 the token the app gave */
    ULONG flags;              /* 16 AAR_MAPF_* */
    ULONG granule;            /* 20 publish and acquire granularity */
    ULONG reserved[2];        /* 24 */
};

/*
 * One service request from the app, and the reply the client builds in the
 * same structure (96 bytes; spec 9.6). Which words a request uses is given by
 * the AAR_A_* indexes for `arg` and the AAR_R_* indexes for `out`, per op, in
 * <aar/transport.h>.
 */
typedef struct AARServiceReq {
    ULONG size;               /*  0 sizeof; set by the caller */
    ULONG op;                 /*  4 AAR_OP_*, in */
    ULONG id;                 /*  8 echoed in the completion, in */
    LONG  result;             /* 12 AAR_OK or AAR_E_*, out */
    ULONG arg[6];             /* 16 in */
    ULONG out[6];             /* 40 out */
    ULONG buf;                /* 64 token of the payload buffer, 0 for none, in */
    ULONG buf_off;            /* 68 byte offset inside that buffer, in */
    ULONG buf_len;            /* 72 payload bytes, in */
    ULONG flags;              /* 76 the entry's flags word, in (0 when the op has none) */
    ULONG reply_flags;        /* 80 out: AAR_DISPCAP_* for DISPLAY_OPEN, AAR_CFLAG_* */
    APTR  ptr;                /* 84 in: the library-mapped buf + buf_off, NULL if buf is 0 */
    ULONG reserved[2];        /* 88 0 */
} AARServiceReq;

/* AARServiceReq.reply_flags for a DISPLAY_FLIP completion: the client has
 * submitted the pan or buffer change and relies on the platform's vertical
 * blank to show it. Without it the flip is complete when the completion
 * arrives. (The ZZ9000 value; other platforms ignore the flag.) */
#define AAR_CFLAG_FLIP_SUBMITTED (1u << 0)

/* ---- Tags of AARSetTagList ------------------------------------------------ */

/* 256 ids from AAR_TAG_BASE. A platform's own tags start at
 * AAR_TAG_PLATFORM_BASE(platform_id). Tags are applied before the load: once
 * a load has begun or the app has started they return AAR_E_BUSY. The tags
 * the launcher alone uses (root directory, absolute paths, console, screen
 * mode) are not tags: they belong to the serving code (aar_serve). */
#define AAR_TAG_BASE (TAG_USER + 0x00415200UL)
#define AAR_TAG_PLATFORM_BASE(platform_id) (AAR_TAG_BASE + 0x100UL * (platform_id))

#define AAR_TAG_PRIVATE_MIB    (AAR_TAG_BASE + 1UL) /* ULONG MiB cap on private memory, 0: the default */
#define AAR_TAG_SHARED_MIB     (AAR_TAG_BASE + 2UL) /* ULONG MiB shared limit, 0: the default, above 4095 is AAR_E_INVAL */
#define AAR_TAG_WEIGHT         (AAR_TAG_BASE + 3UL) /* ULONG CPU share weight 0..AARInfo.weight_max, 0: the default 1 */
#define AAR_TAG_LEASE_EXTENDED (AAR_TAG_BASE + 4UL) /* BOOL, default TRUE: the long lease */
#define AAR_TAG_AUDIO_ROUTE    (AAR_TAG_BASE + 5UL) /* ULONG AAR_AUDIO_ROUTE_*: AHI says the client serves audio */
#define AAR_TAG_CLIENT_CAPS    (AAR_TAG_BASE + 6UL) /* ULONG mask of AAR_CCAP_* */

/* What the client can serve, for AAR_TAG_CLIENT_CAPS. A platform that cannot
 * use a bit ignores it. They are the display capabilities that the serving
 * code decides and the platform must know before it starts the app. */
#define AAR_CCAP_DISPLAY_CLUT8   (1u << 0) /* 8-bit CLUT screens */
#define AAR_CCAP_DISPLAY_PAN_ACK (1u << 1) /* acknowledges pan flips (AAR_CFLAG_FLIP_SUBMITTED) */
#define AAR_CCAP_DISPLAY_LATEST  (1u << 2) /* accepts platform-owned flips (AAR_DISPLAY_LATEST) */
#define AAR_CCAP_KNOWN           0x7u

/* ---- Platform extensions -------------------------------------------------- */

/* The second argument of AARExt for the ZZ9000 is AAR_EXT_ZZ9000_CLIENT in
 * <libraries/aar_zz9000.h>. */

/* ---- Layout checks (spec 1.3, the 68k layout) ----------------------------- */

AAR_STATIC_ASSERT(sizeof(AARInfo) == 128, "AARInfo size");
AAR_STATIC_ASSERT(offsetof(AARInfo, platform) == 80, "AARInfo.platform");
AAR_STATIC_ASSERT(sizeof(AARImageInfo) == 48, "AARImageInfo size");
AAR_STATIC_ASSERT(sizeof(AARStatusInfo) == 136, "AARStatusInfo size");
AAR_STATIC_ASSERT(offsetof(AARStatusInfo, progress) == 96, "AARStatusInfo.progress");
AAR_STATIC_ASSERT(sizeof(AAREvent) == 164, "AAREvent size");
AAR_STATIC_ASSERT(offsetof(AAREvent, status) == 28, "AAREvent.status");
AAR_STATIC_ASSERT(sizeof(AARMessage) == 32, "AARMessage size");
AAR_STATIC_ASSERT(sizeof(struct AARSharedMap) == 32, "AARSharedMap size");
AAR_STATIC_ASSERT(sizeof(AARServiceReq) == 96, "AARServiceReq size");
AAR_STATIC_ASSERT(offsetof(AARServiceReq, ptr) == 84, "AARServiceReq.ptr");

#ifdef __cplusplus
}
#endif

#endif /* LIBRARIES_AAR_H */
