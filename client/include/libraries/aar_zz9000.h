/*
 * aar.library extensions of the ZZ9000: the platform words of AARInfo and the
 * extension table that
 *
 *     AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT)
 *
 * returns. Only the ZZ9000 build of the library provides them; a program that
 * uses them is not portable. This is the client side. The app side extension
 * is in <aar/ext_zz9000.h>.
 *
 * The base library stays platform neutral. What a launcher needs of the card
 * beyond it lives here: the list of all claims and the administrative release
 * of any of them (aar-run STATUS and RELEASE), the library's counters and a test
 * hook for the lease. The log sink is not here: AARSetLogSink is a call of the
 * base library on every platform.
 *
 * The table is size-first and append-only, like every structure of the library.
 * Test a field with AAR_CLIENT_EXT_ZZ9000_COVERS before you call it. The
 * functions use the C calling convention of the compiler that built the
 * program; call them directly, not through the library base.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LIBRARIES_AAR_ZZ9000_H
#define LIBRARIES_AAR_ZZ9000_H

#include <stddef.h>

#include <libraries/aar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Second argument of AARExt for AAR_PLATFORM_ZZ9000. */
#define AAR_EXT_ZZ9000_CLIENT 1u

/* ---- AARInfo.platform[] --------------------------------------------------- */

/* The meaning of the eight platform words when AARInfo.platform_id is
 * AAR_PLATFORM_ZZ9000: what the card reports at the moment of the call. */
#define AAR_ZZ9000_INFO_RECORDS_FREE   0u /* app records free */
#define AAR_ZZ9000_INFO_TABLES_FREE    1u /* translation tables free */
#define AAR_ZZ9000_INFO_TABLES_TOTAL   2u
#define AAR_ZZ9000_INFO_COMMIT_FREE    3u /* private memory not yet committed, bytes */
#define AAR_ZZ9000_INFO_COMMIT_TOTAL   4u
#define AAR_ZZ9000_INFO_HOSTHEAP_FREE  5u /* host-visible heap free, bytes */
#define AAR_ZZ9000_INFO_HOSTHEAP_LARGEST 6u /* largest free host-visible block, bytes */
#define AAR_ZZ9000_INFO_CAPS           7u /* AAR_ZZ9000_CAP_* */

#define AAR_ZZ9000_CAP_Z3      (1u << 0) /* a Zorro III board */
#define AAR_ZZ9000_CAP_DISPLAY (1u << 1)
#define AAR_ZZ9000_CAP_AUDIO   (1u << 2)
#define AAR_ZZ9000_CAP_GL      (1u << 3)
#define AAR_ZZ9000_CAP_CLUT8   (1u << 4)

/* ---- The extension table -------------------------------------------------- */

/* One claim in the card's list (64 bytes). */
typedef struct AARZZ9000List {
    ULONG size;         /*  0 sizeof; set by the caller */
    ULONG handle;       /*  4 the card's handle word; 0: no further claim */
    ULONG state;        /*  8 AAR_STATE_* */
    ULONG committed;    /* 12 private memory committed, bytes */
    ULONG index;        /* 16 pass index + 1 as the next cursor */
    ULONG cpu_ms;       /* 20 CPU time, user plus SVC, idle excluded */
    ULONG service_ms;   /* 24 time spent waiting for the client */
    ULONG block_reason; /* 28 what the app is parked on (0: running) */
    ULONG end_reason;   /* 32 AAR_END_* */
    ULONG flags;        /* 36 AAR_ZZ9000_LISTF_* */
    UBYTE name[24];     /* 40 NUL-terminated app name, at most 20 characters */
} AARZZ9000List;

#define AAR_ZZ9000_LISTF_OVERLOAD (1u << 0)

/* The library's own counters for one handle (68 bytes). */
typedef struct AARZZ9000Stats {
    ULONG size;            /*  0 sizeof; set by the caller */
    ULONG requests;        /*  4 service requests handed out */
    ULONG replies;         /*  8 completions posted */
    ULONG reply_busy;      /* 12 completions that found the CQ full */
    ULONG bad_buf;         /* 16 requests whose buffer did not map */
    ULONG bad_entries;     /* 20 SQ entries of an op the client does not take */
    ULONG msgs_taken;      /* 24 messages moved into the inbox */
    ULONG sq_blocked;      /* 28 pump passes stopped at a message the full inbox could not take */
    ULONG msgs_sent;       /* 32 */
    ULONG logs;            /* 36 log records delivered to the sink */
    ULONG logs_unclaimed;  /* 40 log records dropped, no sink */
    ULONG events_posted;   /* 44 */
    ULONG events_merged;   /* 48 mouse events folded into another */
    ULONG events_dropped;  /* 52 events lost to a full CQ */
    ULONG kicks;           /* 56 doorbells sent */
    ULONG kicks_coalesced; /* 60 doorbells folded into a pending one */
    ULONG maps;            /* 64 ranges mapped */
} AARZZ9000Stats;

typedef struct aar_client_ext_zz9000 {
    ULONG size;      /* sizeof of the table the library built */
    ULONG reserved;  /* 0 */
    /* The next live claim at or after `cursor` (0 to start). 1 with *info
     * filled, 0 when there is none, a negative AAR_E_* on failure. Works
     * without a handle: it lists every claim on the card, not only this
     * program's. */
    LONG (*list)(ULONG cursor, AARZZ9000List *info);
    /* Releases any claim by its card handle word (aar-run RELEASE, clearing a
     * quarantined claim). AAR_OK, or AAR_E_STALE for a handle that is not
     * live. It does not touch the library's own handles: use AARClose. */
    LONG (*release_handle)(ULONG handle);
    /* The counters of one of the caller's handles. */
    LONG (*stats)(AARHandle *handle, AARZZ9000Stats *out);
    /* Stops the library renewing the handle's lease for `ms` milliseconds.
     * A test hook for the lease; 0 resumes. AAR_E_NOSYS in a build without
     * the hook. */
    LONG (*lease_pause)(AARHandle *handle, ULONG ms);
} aar_client_ext_zz9000;

#define AAR_CLIENT_EXT_ZZ9000_COVERS(ext, field) \
    ((ext) != 0 && (ULONG)(ext)->size >= \
        (ULONG)(offsetof(aar_client_ext_zz9000, field) + sizeof((ext)->field)))

AAR_STATIC_ASSERT(sizeof(AARZZ9000List) == 64, "AARZZ9000List size");
AAR_STATIC_ASSERT(sizeof(AARZZ9000Stats) == 68, "AARZZ9000Stats size");

#ifdef __cplusplus
}
#endif

#endif /* LIBRARIES_AAR_ZZ9000_H */
