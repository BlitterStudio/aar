/*
 * The platform interface of the aar.library build: what a platform (L3)
 * supplies to the portable library core (L1) and to the AmigaOS shell (L2).
 * It is private to the library build, not part of the public API: clients see
 * <libraries/aar.h> and the vectors. Spec 10.7 names the same operations.
 *
 * Layers:
 *   L1 core     handles, owner checks, queue pump, inbox, events, messages,
 *               status, tags, probe, shared map cache. No OS calls.
 *   L2 shell    the Exec library, vertical-blank lease server, dispatcher,
 *               signals, notify port.
 *   L3 platform this table. The ZZ9000 transport (mailbox, board, mmu.library)
 *               is one; the reference platform of the tests is another.
 *
 * The control block and the queue pair are not hidden behind this table. L1
 * reads and writes them with <aar/transport.h> directly, because that header
 * is the contract between the two sides and every platform has the same
 * layout. The platform supplies what differs: admission and release of a
 * claim, the control calls, the load path, turning tokens into pointers, and
 * the doorbell.
 *
 * Conventions:
 *   - Operations return AAR_OK or a negative AAR_E_* (<aar/app.h>), as the
 *     public calls do. A platform that cannot do an operation at all leaves
 *     its pointer NULL where the text says so, else returns AAR_E_NOSYS.
 *   - A stale claim (the card revoked it) is AAR_E_STALE. L1 then drops every
 *     pointer of the claim and does not call the platform again for it except
 *     `release`.
 *   - Types are the fixed-width ones of <stdint.h>, so a platform written
 *     against the wire headers needs no conversion. L1 converts at the public
 *     boundary where ULONG and uint32_t differ.
 *   - Memory that the other side writes is `volatile`.
 *   - Operations are called from task context only, never from an interrupt.
 *     The lease interrupt of L2 does not call the platform at all: it stores
 *     one word in the control block.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_PLAT_H
#define AAR_PLAT_H

#include <stddef.h>
#include <stdint.h>

#include <aar/app.h>
#include <aar/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped when this interface changes incompatibly. */
#define AAR_PLAT_VERSION 1u

struct aar_plat;

/* ---- Constants of a platform ---------------------------------------------- */

/*
 * What the platform declares at build time. L1 copies these into AARInfo and
 * takes msg_capacity from the live queue depth (AAR_MSG_CAPACITY_FOR) unless
 * the platform has none. 0 means "no limit" or "not reported", as in AARInfo.
 * A build-time check ties a platform's constants to its own wire header.
 */
struct aar_plat_info {
    uint32_t size;               /* sizeof */
    uint32_t platform_id;        /* AAR_PLATFORM_* */
    uint32_t profiles;           /* AAR_PROFILEF_* mask (libraries/aar.h) */
    uint32_t groups;             /* AAR_GROUP_* always present; query may add */
    uint32_t flags;              /* AAR_INFOF_* */
    uint32_t max_apps;
    uint32_t max_image_bytes;
    uint32_t max_args;
    uint32_t max_arg_bytes;      /* NUL included */
    uint32_t shared_granule;
    uint32_t stall_limit_ms;
    uint32_t stop_grace_ms;
    uint32_t shared_default_mib;
    uint32_t shared_max_mib;     /* largest AAR_TAG_SHARED_MIB the platform grants */
    uint32_t weight_max;         /* 0: no weighting (AAR_TAG_WEIGHT is AAR_E_NOSYS) */
};

/* What the platform reports at the moment of the call (AARQuery). */
struct aar_plat_query {
    uint32_t groups;             /* AAR_GROUP_* available now (ORed with info.groups) */
    uint32_t queue_depth;        /* entries per queue of a new claim */
    uint32_t platform[8];        /* AARInfo.platform, as platform_id defines it */
};

/* ---- A claim ------------------------------------------------------------------ */

/*
 * One claimed app instance, as the platform sees it. L1 embeds it in its
 * handle record. `claim` fills the first two fields; `priv` is private to the
 * platform.
 */
struct aar_plat_claim {
    uint32_t handle;             /* the platform's handle word; 0 once released */
    uint32_t cb_token;           /* token of the control block (AAR_CB_BYTES) */
    void *priv;                  /* aar_plat_ops.claim_priv_bytes of storage, zeroed
                                  * by L1 before `claim`, kept until `release` */
};

/* Options of a load, from AARSetTagList. */
struct aar_plat_load_opts {
    uint32_t client_caps;        /* AAR_CCAP_* (libraries/aar.h) */
    uint32_t lease_extended;     /* AAR_TAG_LEASE_EXTENDED, 0 or 1 */
    uint32_t audio_route;        /* AAR_AUDIO_ROUTE_*: the AHI route sets the client's AHI capability */
};

/* Doorbell bits of `kick`. */
#define AAR_PLAT_KICK_CQ (1u << 0) /* completions or events were posted on the CQ */
#define AAR_PLAT_KICK_SQ (1u << 1) /* entries were freed on the SQ */

/* Answers of `events_sample`. */
#define AAR_PLAT_EVENTS_NONE  0 /* the platform has no event block */
#define AAR_PLAT_EVENTS_NEWS  1 /* it has one, and the global count changed since the previous sample (or this is the first) */
#define AAR_PLAT_EVENTS_QUIET 2 /* it has one, and nothing changed since the previous sample */

/* ---- The operations --------------------------------------------------------------- */

struct aar_plat_ops {
    uint32_t size;               /* sizeof */
    uint32_t claim_priv_bytes;   /* storage per claim for the platform's own state */

    /*
     * Resources the platform holds for the life of the library (ZZ9000: the
     * zz9k.library base and the board). L2 calls `attach` from the dispatcher
     * before any other operation and `detach` after the last. `attach`
     * returns AAR_E_NOSYS when the hardware or its driver is missing or too
     * old; the library then answers AARQuery and AAROpen with that error.
     * `detach` may be NULL.
     */
    int (*attach)(struct aar_plat *p);
    void (*detach)(struct aar_plat *p);

    /* AARQuery. Needs no claim and no mmu.library. */
    int (*query)(struct aar_plat *p, struct aar_plat_query *q);

    /*
     * Admission: claims an app instance and returns its handle word and the
     * token of its control block in `c`. L1 maps the control block with `map`,
     * checks AAR_CB_MAGIC and the handle, binds both queues from their
     * descriptors, and calls `release` if any of that fails. AAR_E_NORESOURCE
     * with *reason an AAR_REASON_*; AAR_E_NOSYS when the platform cannot map
     * (ZZ9000: mmu.library is missing). `reason` is never NULL.
     */
    int (*claim)(struct aar_plat *p, struct aar_plat_claim *c, uint32_t *reason);

    /*
     * Stops the app if it still runs, frees its control block, queues and
     * service buffers, and invalidates the tokens of the claim. Idempotent.
     * L1 has already unlinked the lease and dropped its own pointers; the
     * platform drops its own (load buffer) first.
     */
    int (*release)(struct aar_plat *p, struct aar_plat_claim *c);

    /*
     * Load, in three steps. `load_begin` allocates the platform's staging area
     * and returns a pointer to it (*staging) and the most bytes one chunk may
     * carry (*chunk_max, at least 4 KiB). `load_chunk` says that `len` bytes in
     * the staging area belong at `file_off` of the image. `load_end` makes the
     * platform check and map the image; it may take seconds. On AAR_E_NORESOURCE
     * *reason is an AAR_REASON_*. L1 enforces AARInfo.max_image_bytes and the
     * once-only order, and writes the limits (AAR_CB_PRIVATE_MIB, _SHARED_MIB,
     * _WEIGHT) before `load_begin`.
     */
    int (*load_begin)(struct aar_plat *p, struct aar_plat_claim *c,
                      const struct aar_plat_load_opts *opts,
                      volatile uint8_t **staging, uint32_t *chunk_max,
                      uint32_t *reason);
    int (*load_chunk)(struct aar_plat *p, struct aar_plat_claim *c,
                      uint32_t file_off, uint32_t len);
    int (*load_end)(struct aar_plat *p, struct aar_plat_claim *c,
                    uint32_t *reason);

    /*
     * Starts the loaded image with `argc` strings (L1 has checked the count
     * and the length of each against the platform's info). The platform
     * hands them to the app and frees its staging area.
     */
    int (*start)(struct aar_plat *p, struct aar_plat_claim *c,
                 const char *const *argv, uint32_t argc);

    /*
     * The control call of AARStop: the platform stops the app now. L1 has
     * already written AAR_CB_STOP_REQUEST and posted AAR_EV_STOP.
     */
    int (*stop)(struct aar_plat *p, struct aar_plat_claim *c);

    /*
     * Shared memory. `map` turns [token, token + bytes) into a pointer on this
     * side and sets the cache mode; it is called once per range and L1 caches
     * the result per handle. *mapf is AAR_MAPF_UNCACHED or AAR_MAPF_COHERENT
     * (libraries/aar.h). AAR_E_INVAL when the range is outside the shared
     * aperture, AAR_E_NOMEM, AAR_E_NOSYS when it cannot map. `token` is the
     * inverse (AARSharedToken): AAR_E_INVAL for a pointer outside the
     * aperture. A platform with no token to pointer difference (the identity)
     * implements both trivially.
     */
    int (*map)(struct aar_plat *p, uint32_t token, uint32_t bytes,
               volatile uint8_t **ptr, uint32_t *mapf);
    int (*token)(struct aar_plat *p, const volatile void *ptr, uint32_t bytes,
                 uint32_t *token);

    /*
     * The doorbell toward the app: tells the other side that the queues have
     * news. A hint, never needed for correctness. AAR_OK when sent, AAR_E_BUSY
     * when one is still in flight (L1 keeps the bits pending and tries again
     * from `poll` and the next call), another AAR_E_* when it cannot. May be
     * NULL when the platform polls.
     */
    int (*kick)(struct aar_plat *p, struct aar_plat_claim *c, uint32_t bits);

    /*
     * Housekeeping for one claim: retire a completed doorbell request. The
     * dispatcher calls it at each vertical blank for a claim whose doorbell
     * went out (a successful `kick`) and has not been seen to finish, and for
     * no other claim. It returns 1 while a doorbell request is still in flight,
     * so that the dispatcher keeps polling, and 0 when there is nothing left to
     * retire, after which the claim costs the dispatcher nothing until the next
     * `kick`. May be NULL: a platform without it has nothing to retire.
     */
    int (*poll)(struct aar_plat *p, struct aar_plat_claim *c);

    /*
     * AARExt: the platform's extension table for `ext_id`, or NULL. L1 has
     * already checked the platform id. May be NULL.
     */
    const void *(*ext)(struct aar_plat *p, uint32_t ext_id);

    /*
     * Barriers of the aar_queue_* functions (<aar/transport.h>) and the 68k
     * data cache maintenance of AARSharedPublish and AARSharedAcquire. Any of
     * them may be NULL: no barrier, no maintenance.
     */
    aar_barrier_fn barrier_acquire;
    aar_barrier_fn barrier_release;
    void (*cache_publish)(struct aar_plat *p, const volatile void *ptr, uint32_t len);
    void (*cache_acquire)(struct aar_plat *p, const volatile void *ptr, uint32_t len);

    /*
     * Event counts (spec 10.6, optional; both NULL: the platform has none and
     * the dispatcher scans every armed handle at every pass). A platform that
     * publishes monotonically increasing counts of what the other side made
     * visible lets L1 read one word per pass while nothing happened. The
     * counts are a hint: the queues stay authoritative.
     *
     * `events_sample` runs once per dispatcher pass. It reads the global
     * count; only if that changed since the previous sample does it read the
     * group counts. It returns AAR_PLAT_EVENTS_NONE, _NEWS or _QUIET. A
     * platform that cannot tell NEWS from QUIET may answer NEWS at every pass:
     * L1 then asks `claim_news` for each armed handle, which costs no card
     * access.
     *
     * `claim_news` makes no card access. It answers 1 when the claim's group
     * count (the global count, for a claim that has no group) differs from the
     * value this claim saw at its previous `claim_news` that answered 1, or when
     * the claim has never been asked, and then records the current value as
     * seen. It answers 0 otherwise. Wrap-around of a 32-bit count is a change.
     */
    int (*events_sample)(struct aar_plat *p);
    int (*claim_news)(struct aar_plat *p, struct aar_plat_claim *c);
};

/* The platform object. L3 defines one. */
struct aar_plat {
    uint32_t version;            /* AAR_PLAT_VERSION */
    uint32_t reserved;
    const struct aar_plat_info *info;
    const struct aar_plat_ops *ops;
    void *user;                  /* the platform's own state */
};

/*
 * The one platform of this build, defined by the platform's object file
 * (aar_zz9000.c on the ZZ9000, refplat.c in the host tests).
 */
struct aar_plat *aar_plat_get(void);

#ifdef __cplusplus
}
#endif

#endif /* AAR_PLAT_H */
