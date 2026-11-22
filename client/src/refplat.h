/*
 * The reference platform: an in-process implementation of aar_plat.h on the
 * reference transport model of <aar/transport.h>, for the host tests of the
 * library core and shell. Claims, control blocks, queue pairs, the load
 * buffer and the service buffers are plain memory in one static arena that
 * stands for the board's shared memory; a token is an offset into it. It is
 * a platform of the ZZ9000 kind (AAR_PLATFORM_ZZ9000), so the extension table
 * of aar_zz9000.h works.
 *
 * The test plays the app side with the refplat_app_* functions: it posts
 * requests, messages and logs on the SQ and takes completions, messages and
 * events off the CQ, using the queue functions of the transport header in the
 * opposite roles. `refplat` exposes the state for assertions.
 *
 * Pointers into the arena are handed out through 32-bit APTR fields, so a
 * host test links it at an address below 4 GiB (-no-pie) or builds with
 * -m32.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_REFPLAT_H
#define AAR_REFPLAT_H

#include <stdint.h>

#include "aar_plat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The claims and the two areas behind the control block may be changed by a build
 * (-D) that never loads an image or serves a buffer: the scale test has 240 claims. */
#ifndef REFPLAT_CLAIMS
#define REFPLAT_CLAIMS      8u
#endif
#define REFPLAT_DEPTH       AAR_QUEUE_DEPTH_DEFAULT
#define REFPLAT_PAIR_STRIDE 0x4000u  /* control block and both rings, padded to 16 KiB */
#ifndef REFPLAT_LOAD_STRIDE
#define REFPLAT_LOAD_STRIDE 0x12000u /* a 64 KiB staging area and the argument area */
#endif
#define REFPLAT_CHUNK_MAX   0x10000u
#ifndef REFPLAT_BUF_STRIDE
#define REFPLAT_BUF_STRIDE  0x10000u /* the service buffers of one claim */
#endif
#define REFPLAT_CLAIM_STRIDE (REFPLAT_PAIR_STRIDE + REFPLAT_LOAD_STRIDE + REFPLAT_BUF_STRIDE)
#define REFPLAT_TOKEN_BASE  0x00100000u
#define REFPLAT_ARENA_BYTES (REFPLAT_CLAIMS * REFPLAT_CLAIM_STRIDE)

/* Entries of the operation trace (refplat.trace). */
#define REFPLAT_T_CLAIM    1u
#define REFPLAT_T_RELEASE  2u
#define REFPLAT_T_STOP     3u
#define REFPLAT_T_START    4u
#define REFPLAT_T_KICK     5u
#define REFPLAT_T_MAP      6u
#define REFPLAT_T_LOAD_END 7u

struct refplat_claim {
    uint8_t used;
    uint8_t revoked;              /* every operation on the claim answers AAR_E_STALE */
    uint32_t handle;              /* the handle word */
    uint32_t generation;          /* of the next claim of this slot */
    volatile uint8_t *cb;         /* control block */
    volatile uint8_t *load;       /* staging area */
    uint32_t token;               /* of the pair block */
    uint32_t load_token, buf_token;
    uint32_t image_len, image_sum, image_first; /* what load_chunk received */
    uint32_t chunks;
    uint32_t argc;
    char argv[4][32];
    uint32_t kicks, kick_bits;    /* doorbells received and the bits ORed */
    uint32_t releases;
    struct aar_plat_load_opts opts; /* what load_begin received */
};

struct refplat_state {
    struct aar_plat plat;
    struct refplat_claim claims[REFPLAT_CLAIMS];
    /* knobs */
    uint32_t max_claims;          /* fewer than REFPLAT_CLAIMS to force a refusal */
    uint32_t refuse_reason;       /* AAR_REASON_* of the refusal */
    int attach_rc;                /* what `attach` answers (default AAR_OK) */
    uint32_t kick_busy;           /* the next n kicks answer AAR_E_BUSY */
    uint32_t poll_inflight;       /* the next n polls answer 1: a doorbell is still in flight */
    uint32_t map_mapf;            /* AAR_MAPF_* the map reports (default AAR_MAPF_UNCACHED) */
    int load_begin_rc;            /* what load_begin answers (default AAR_OK) */
    uint32_t load_begin_reason;   /* the reason with AAR_E_NORESOURCE */
    int load_chunk_rc;            /* what load_chunk answers (default AAR_OK) */
    int start_rc;                 /* what start answers (default AAR_OK) */
    int load_end_rc;              /* what load_end answers (default AAR_OK) */
    uint32_t load_end_reason;     /* the reason with AAR_E_NORESOURCE */
    uint32_t bad_magic;           /* a claim whose control block has no magic */
    uint32_t chunk_max;           /* the most a chunk may carry (0: REFPLAT_CHUNK_MAX); more than that overruns the staging area, so a test that raises it expects the caller to cap */
    struct aar_plat_info info;    /* the constants; a test may change them */
    uint32_t fail_map_token;      /* a map of this token answers AAR_E_NOMEM (0: none) */
    uint32_t stale_map_token;     /* a map of this token answers AAR_E_STALE (0: none) */
    /* observations */
    uint32_t attaches, detaches;
    uint32_t polls;
    uint32_t stale_calls;         /* kicks and stops that reached a claim the platform had revoked */
    uint32_t maps;                /* calls of the map operation */
    uint32_t publishes, acquires; /* cache maintenance calls */
    uint32_t last_cache_len;
    const volatile void *last_cache_ptr;
    uint32_t trace[64], trace_len;
    /* A hook run inside `release`, before the control block is poisoned: the
     * test uses it to look at what the library did first. */
    void (*on_release)(void *user, uint32_t handle);
    void *on_release_user;
};

extern struct refplat_state refplat;

/* Back to a clean platform: no claims, default knobs, arena zeroed. */
void refplat_reset(void);

/* ---- The app side ------------------------------------------------------------- */

/* The slot of a handle word, or -1. */
int refplat_slot(uint32_t handle);
volatile uint8_t *refplat_cb(int slot);

/* Token of byte `off` of the claim's service buffer area (the runtime's shared
 * buffers: payloads, the audio ring); the pointer behind a token. */
uint32_t refplat_buf_token(int slot, uint32_t off);
volatile uint8_t *refplat_ptr(uint32_t token);

/* SQ producer: a request, a message, a log record. AAR_OK or AAR_E_BUSY. */
int refplat_app_request(int slot, uint32_t op, uint32_t flags, uint32_t id,
                        const uint32_t arg[6], uint32_t buf, uint32_t buf_off,
                        uint32_t buf_len);
int refplat_app_message(int slot, const uint8_t msg[AAR_MSG_BYTES]);
int refplat_app_log(int slot, uint32_t buf, uint32_t off, uint32_t len);

/* CQ consumer: copies the oldest entry (64 bytes) and pops it. 1, or 0 when
 * the CQ is empty. */
int refplat_app_cq_pop(int slot, uint8_t entry[AAR_ENTRY_BYTES]);
uint32_t refplat_app_cq_used(int slot);
uint32_t refplat_app_sq_used(int slot);

/* The platform's own writes to the control block: state, end reason, the
 * counter of messages the app has taken from its inbox. */
void refplat_app_set_state(int slot, uint32_t state, uint32_t end_reason);
void refplat_app_set_consumed(int slot, uint32_t consumed);

/* Makes the card revoke a claim: every operation on it answers AAR_E_STALE
 * until it is released. The memory stays, like a quarantined shell. */
void refplat_revoke(int slot);

/* The number of live claims. */
uint32_t refplat_live(void);

#ifdef __cplusplus
}
#endif

#endif /* AAR_REFPLAT_H */
