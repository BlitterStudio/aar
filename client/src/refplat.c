/*
 * The reference platform of the host tests. See refplat.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <libraries/aar.h>
#include <libraries/aar_zz9000.h>

#include "aar_core.h"
#include "refplat.h"

struct refplat_state refplat;

static uint8_t arena[REFPLAT_ARENA_BYTES] __attribute__((aligned(8)));

static const struct aar_plat_info info_default = {
    sizeof(struct aar_plat_info),
    AAR_PLATFORM_ZZ9000,
    AAR_PROFILEF_A32 | AAR_PROFILEF_A64,
    AAR_GROUPS_DEFINED,
    0u,                 /* flags */
    0u,                 /* max_apps: limited by resources */
    0x01000000u,        /* max_image_bytes */
    16u,                /* max_args */
    128u,               /* max_arg_bytes */
    4096u,              /* shared_granule */
    10000u,             /* stall_limit_ms: spec 3.4 minimum */
    1000u,              /* stop_grace_ms */
    16u,                /* shared_default_mib */
    4095u,              /* shared_max_mib */
    256u                /* weight_max */
};

/* ---- Helpers ------------------------------------------------------------------ */

static uint32_t claim_base(uint32_t slot)
{
    return slot * REFPLAT_CLAIM_STRIDE;
}

static uint32_t token_of_base(uint32_t base)
{
    return REFPLAT_TOKEN_BASE + base;
}

/* The slot a claim record names, or -1 when the claim is gone. */
static int slot_of(const struct aar_plat_claim *c)
{
    uint32_t slot;

    if (!c->priv)
        return -1;
    slot = *(const uint32_t *)c->priv;
    if (slot >= REFPLAT_CLAIMS || !refplat.claims[slot].used ||
        refplat.claims[slot].handle != c->handle)
        return -1;
    return (int)slot;
}

static void trace(uint32_t what, uint32_t slot)
{
    if (refplat.trace_len < sizeof(refplat.trace) / sizeof(refplat.trace[0]))
        refplat.trace[refplat.trace_len++] = (what << 8) | slot;
}

static void event_add(uint32_t word)
{
    aar_put_be32(refplat.ev_block + word * 4u, aar_get_be32(refplat.ev_block + word * 4u) + 1u);
}

void refplat_events_post(uint32_t record)
{
    if (record / 32u < REFPLAT_EVENT_GROUPS)
        event_add(1u + record / 32u);
    event_add(0u);
}

void refplat_events_set(uint32_t word, uint32_t value)
{
    aar_put_be32(refplat.ev_block + word * 4u, value);
}

static uint32_t record_of(const struct refplat_claim *rc)
{
    return aar_handle_index(rc->handle) + refplat.events_record_bias;
}

/* The card made something visible to the client of this claim. */
static void event_post_claim(const struct refplat_claim *rc)
{
    if (refplat.events_on)
        refplat_events_post(record_of(rc));
}

static void put_state(struct refplat_claim *rc, uint32_t state, uint32_t end_reason)
{
    aar_put_be32(rc->cb + AAR_CB_STATE, state);
    aar_put_be32(rc->cb + AAR_CB_END_REASON, end_reason);
    if (end_reason != AAR_END_NONE)
        event_post_claim(rc);
}

static void release_slot(uint32_t slot)
{
    struct refplat_claim *rc = &refplat.claims[slot];
    uint32_t base = claim_base(slot);

    trace(REFPLAT_T_RELEASE, slot);
    if (refplat.on_release)
        refplat.on_release(refplat.on_release_user, rc->handle);
    /* The memory goes back to the pool: poison it, so a store that still
     * lands in it shows. */
    memset(arena + base, 0xAA, REFPLAT_CLAIM_STRIDE);
    rc->releases++;
    rc->used = 0u;
    rc->revoked = 0u;
    rc->handle = 0u;
    rc->generation = aar_handle_next_gen(rc->generation);
}

/* ---- The operations ------------------------------------------------------------- */

static int op_attach(struct aar_plat *p)
{
    (void)p;
    refplat.attaches++;
    return refplat.attach_rc;
}

static void op_detach(struct aar_plat *p)
{
    (void)p;
    refplat.detaches++;
}

static uint32_t live_count(void)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < REFPLAT_CLAIMS; i++)
        n += refplat.claims[i].used;
    return n;
}

uint32_t refplat_live(void)
{
    return live_count();
}

static int op_query(struct aar_plat *p, struct aar_plat_query *q)
{
    (void)p;
    q->groups = 0u;
    q->queue_depth = REFPLAT_DEPTH;
    q->platform[AAR_ZZ9000_INFO_RECORDS_FREE] = refplat.max_claims - live_count();
    q->platform[AAR_ZZ9000_INFO_TABLES_FREE] = 3871u;
    q->platform[AAR_ZZ9000_INFO_TABLES_TOTAL] = 3871u;
    q->platform[AAR_ZZ9000_INFO_COMMIT_FREE] = 462422016u;
    q->platform[AAR_ZZ9000_INFO_COMMIT_TOTAL] = 462422016u;
    q->platform[AAR_ZZ9000_INFO_HOSTHEAP_FREE] = 75100416u;
    q->platform[AAR_ZZ9000_INFO_HOSTHEAP_LARGEST] = 74379520u;
    q->platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_Z3;
    return AAR_OK;
}

static int op_claim(struct aar_plat *p, struct aar_plat_claim *c, uint32_t *reason)
{
    uint32_t slot, base;
    struct refplat_claim *rc;

    (void)p;
    for (slot = 0u; slot < refplat.max_claims && refplat.claims[slot].used; slot++)
        ;
    if (slot >= refplat.max_claims) {
        *reason = refplat.refuse_reason ? refplat.refuse_reason : AAR_REASON_TABLES;
        return AAR_E_NORESOURCE;
    }
    rc = &refplat.claims[slot];
    base = claim_base(slot);
    memset(arena + base, 0, REFPLAT_CLAIM_STRIDE);
    if (rc->generation == 0u)
        rc->generation = 1u;
    rc->used = 1u;
    rc->revoked = 0u;
    rc->handle = aar_handle_make(slot + 1u, rc->generation);
    rc->token = token_of_base(base);
    rc->load_token = token_of_base(base + REFPLAT_PAIR_STRIDE);
    rc->buf_token = token_of_base(base + REFPLAT_PAIR_STRIDE + REFPLAT_LOAD_STRIDE);
    rc->cb = arena + base;
    rc->load = arena + base + REFPLAT_PAIR_STRIDE;
    rc->image_len = rc->image_sum = rc->image_first = rc->chunks = 0u;
    rc->argc = 0u;
    rc->kicks = rc->kick_bits = 0u;
    rc->ev_asked = 0u;
    rc->ev_seen = 0u;
    (void)aar_cb_init(rc->cb, rc->handle, AAR_PROFILE_A32, REFPLAT_DEPTH,
                      rc->token, 0x1000u, rc->token, 0x2000u);
    aar_put_be32(rc->cb + AAR_CB_LEASE_PERIOD_MS, 2000u);
    put_state(rc, AAR_STATE_CLAIMED, AAR_END_NONE);
    if (refplat.bad_magic)
        aar_put_be32(rc->cb + AAR_CB_MAGIC, 0u);
    *(uint32_t *)c->priv = slot;
    c->handle = rc->handle;
    c->cb_token = rc->token;
    trace(REFPLAT_T_CLAIM, slot);
    return AAR_OK;
}

static int op_release(struct aar_plat *p, struct aar_plat_claim *c)
{
    int slot = slot_of(c);

    (void)p;
    if (slot >= 0)
        release_slot((uint32_t)slot);
    c->handle = 0u;
    return AAR_OK;
}

static int op_load_begin(struct aar_plat *p, struct aar_plat_claim *c,
                         const struct aar_plat_load_opts *opts,
                         volatile uint8_t **staging, uint32_t *chunk_max,
                         uint32_t *reason)
{
    int slot = slot_of(c);
    struct refplat_claim *rc;

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked)
        return AAR_E_STALE;
    if (refplat.load_begin_rc != AAR_OK) {
        *reason = refplat.load_begin_reason;
        return refplat.load_begin_rc;
    }
    rc = &refplat.claims[slot];
    rc->opts = *opts;
    rc->image_len = rc->image_sum = rc->image_first = rc->chunks = 0u;
    put_state(rc, AAR_STATE_LOADING, AAR_END_NONE);
    *staging = rc->load;
    *chunk_max = refplat.chunk_max ? refplat.chunk_max : REFPLAT_CHUNK_MAX;
    return AAR_OK;
}

static int op_load_chunk(struct aar_plat *p, struct aar_plat_claim *c,
                         uint32_t file_off, uint32_t len)
{
    int slot = slot_of(c);
    struct refplat_claim *rc;
    uint32_t i;

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked)
        return AAR_E_STALE;
    if (refplat.load_chunk_rc != AAR_OK)
        return refplat.load_chunk_rc;
    rc = &refplat.claims[slot];
    for (i = 0u; i < len; i++)
        rc->image_sum += rc->load[i];
    if (file_off == 0u && len >= 4u)
        rc->image_first = aar_get_be32(rc->load);
    if (file_off + len > rc->image_len)
        rc->image_len = file_off + len;
    rc->chunks++;
    return AAR_OK;
}

static int op_load_end(struct aar_plat *p, struct aar_plat_claim *c, uint32_t *reason)
{
    int slot = slot_of(c);

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked)
        return AAR_E_STALE;
    trace(REFPLAT_T_LOAD_END, (uint32_t)slot);
    if (refplat.load_end_rc != AAR_OK) {
        *reason = refplat.load_end_reason;
        return refplat.load_end_rc;
    }
    return AAR_OK;
}

static int op_start(struct aar_plat *p, struct aar_plat_claim *c,
                    const char *const *argv, uint32_t argc)
{
    int slot = slot_of(c);
    struct refplat_claim *rc;
    uint32_t i;

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked)
        return AAR_E_STALE;
    if (refplat.start_rc != AAR_OK)
        return refplat.start_rc;
    rc = &refplat.claims[slot];
    rc->argc = argc;
    for (i = 0u; i < argc && i < 4u; i++) {
        uint32_t k;

        for (k = 0u; k + 1u < sizeof(rc->argv[i]) && argv[i][k] != '\0'; k++)
            rc->argv[i][k] = argv[i][k];
        rc->argv[i][k] = '\0';
    }
    put_state(rc, AAR_STATE_RUNNING, AAR_END_NONE);
    trace(REFPLAT_T_START, (uint32_t)slot);
    return AAR_OK;
}

static int op_stop(struct aar_plat *p, struct aar_plat_claim *c)
{
    int slot = slot_of(c);

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked) {
        refplat.stale_calls++;
        return AAR_E_STALE;
    }
    trace(REFPLAT_T_STOP, (uint32_t)slot);
    if (aar_get_be32(refplat.claims[slot].cb + AAR_CB_STATE) == AAR_STATE_RUNNING)
        put_state(&refplat.claims[slot], AAR_STATE_IDLE, AAR_END_FORCED);
    return AAR_OK;
}

static int op_map(struct aar_plat *p, uint32_t token, uint32_t bytes,
                  volatile uint8_t **ptr, uint32_t *mapf)
{
    (void)p;
    refplat.maps++;
    if (token == refplat.fail_map_token && token != 0u)
        return AAR_E_NOMEM;
    if (token == refplat.stale_map_token && token != 0u)
        return AAR_E_STALE;
    if (token < REFPLAT_TOKEN_BASE || bytes == 0u ||
        token - REFPLAT_TOKEN_BASE > REFPLAT_ARENA_BYTES ||
        bytes > REFPLAT_ARENA_BYTES - (token - REFPLAT_TOKEN_BASE))
        return AAR_E_INVAL;
    trace(REFPLAT_T_MAP, 0u);
    *ptr = arena + (token - REFPLAT_TOKEN_BASE);
    *mapf = refplat.map_mapf;
    return AAR_OK;
}

static int op_token(struct aar_plat *p, const volatile void *ptr, uint32_t bytes,
                    uint32_t *token)
{
    const volatile uint8_t *b = (const volatile uint8_t *)ptr;
    uintptr_t off;

    (void)p;
    if (b < arena || b >= arena + REFPLAT_ARENA_BYTES)
        return AAR_E_INVAL;
    off = (uintptr_t)(b - arena);
    if (bytes > REFPLAT_ARENA_BYTES - off)
        return AAR_E_INVAL;
    *token = REFPLAT_TOKEN_BASE + (uint32_t)off;
    return AAR_OK;
}

static int op_kick(struct aar_plat *p, struct aar_plat_claim *c, uint32_t bits)
{
    int slot = slot_of(c);

    (void)p;
    if (slot < 0 || refplat.claims[slot].revoked) {
        refplat.stale_calls++;
        return AAR_E_STALE;
    }
    if (refplat.kick_busy) {
        refplat.kick_busy--;
        return AAR_E_BUSY;
    }
    refplat.claims[slot].kicks++;
    refplat.claims[slot].kick_bits |= bits;
    trace(REFPLAT_T_KICK, (uint32_t)slot);
    return AAR_OK;
}

static int op_poll(struct aar_plat *p, struct aar_plat_claim *c)
{
    (void)p;
    (void)c;
    refplat.polls++;
    if (refplat.poll_inflight) {
        refplat.poll_inflight--;
        return 1;
    }
    return 0;
}

/* events_sample: the global count is one read; the group counts are read only when it changed. */
static int op_events_sample(struct aar_plat *p)
{
    uint32_t g, i;

    (void)p;
    if (!refplat.events_on)
        return AAR_PLAT_EVENTS_NONE;
    refplat.event_samples++;
    refplat.event_reads++;
    g = aar_get_be32(refplat.ev_block);
    if (refplat.ev_have_sample && g == refplat.ev_sample[0])
        return refplat.events_always_news ? AAR_PLAT_EVENTS_NEWS : AAR_PLAT_EVENTS_QUIET;
    refplat.ev_sample[0] = g;
    for (i = 1u; i < REFPLAT_EVENT_WORDS; i++)
        refplat.ev_sample[i] = aar_get_be32(refplat.ev_block + i * 4u);
    refplat.event_reads += REFPLAT_EVENT_WORDS - 1u;
    refplat.ev_have_sample = 1u;
    return AAR_PLAT_EVENTS_NEWS;
}

/* claim_news: from the sampled counts, no access to the block. */
static int op_claim_news(struct aar_plat *p, struct aar_plat_claim *c)
{
    int slot = slot_of(c);
    struct refplat_claim *rc;
    uint32_t group, now;

    (void)p;
    refplat.event_news_calls++;
    if (slot < 0 || !refplat.ev_have_sample)
        return 1;
    rc = &refplat.claims[slot];
    group = record_of(rc) / 32u;
    now = group < REFPLAT_EVENT_GROUPS ? refplat.ev_sample[1u + group] : refplat.ev_sample[0];
    if (rc->ev_asked && rc->ev_seen == now)
        return 0;
    rc->ev_asked = 1u;
    rc->ev_seen = now;
    return 1;
}

static void op_cache_publish(struct aar_plat *p, const volatile void *ptr, uint32_t len)
{
    (void)p;
    refplat.publishes++;
    refplat.last_cache_ptr = ptr;
    refplat.last_cache_len = len;
}

static void op_cache_acquire(struct aar_plat *p, const volatile void *ptr, uint32_t len)
{
    (void)p;
    refplat.acquires++;
    refplat.last_cache_ptr = ptr;
    refplat.last_cache_len = len;
}

/* ---- The extension table ------------------------------------------------------------ */

static LONG ext_list(ULONG cursor, AARZZ9000List *out)
{
    uint32_t i;

    if (!out || out->size < sizeof(*out))
        return AAR_E_INVAL;
    for (i = cursor; i < REFPLAT_CLAIMS; i++) {
        struct refplat_claim *rc = &refplat.claims[i];

        if (!rc->used)
            continue;
        memset(out, 0, sizeof(*out));
        out->size = sizeof(*out);
        out->handle = rc->handle;
        out->state = aar_get_be32(rc->cb + AAR_CB_STATE);
        out->index = i;
        out->end_reason = aar_get_be32(rc->cb + AAR_CB_END_REASON);
        memcpy(out->name, "refplat", 8);
        return 1;
    }
    return 0;
}

static LONG ext_release_handle(ULONG handle)
{
    int slot = refplat_slot(handle);

    if (slot < 0)
        return AAR_E_STALE;
    release_slot((uint32_t)slot);
    return AAR_OK;
}

static const aar_client_ext_zz9000 ext_table = {
    sizeof(aar_client_ext_zz9000),
    0u,
    ext_list,
    ext_release_handle,
    aar_core_ext_stats,
    aar_core_ext_lease_pause
};

static const void *op_ext(struct aar_plat *p, uint32_t ext_id)
{
    (void)p;
    return ext_id == AAR_EXT_ZZ9000_CLIENT ? &ext_table : NULL;
}

static const struct aar_plat_ops ops = {
    sizeof(struct aar_plat_ops),
    sizeof(uint32_t),
    op_attach,
    op_detach,
    op_query,
    op_claim,
    op_release,
    op_load_begin,
    op_load_chunk,
    op_load_end,
    op_start,
    op_stop,
    op_map,
    op_token,
    op_kick,
    op_poll,
    op_ext,
    NULL,
    NULL,
    op_cache_publish,
    op_cache_acquire,
    op_events_sample,
    op_claim_news
};

struct aar_plat *aar_plat_get(void)
{
    return &refplat.plat;
}

void refplat_reset(void)
{
    memset(&refplat, 0, sizeof(refplat));
    memset(arena, 0, sizeof(arena));
    refplat.plat.version = AAR_PLAT_VERSION;
    refplat.info = info_default;
    refplat.plat.info = &refplat.info;
    refplat.plat.ops = &ops;
    refplat.max_claims = REFPLAT_CLAIMS;
    refplat.attach_rc = AAR_OK;
    refplat.map_mapf = AAR_MAPF_UNCACHED;
    refplat.load_end_rc = AAR_OK;
}

/* ---- The app side ----------------------------------------------------------------- */

int refplat_slot(uint32_t handle)
{
    uint32_t slot = aar_handle_index(handle);

    if (slot == 0u || slot > REFPLAT_CLAIMS)
        return -1;
    slot--;
    if (!refplat.claims[slot].used || refplat.claims[slot].handle != handle)
        return -1;
    return (int)slot;
}

volatile uint8_t *refplat_cb(int slot)
{
    return refplat.claims[slot].cb;
}

uint32_t refplat_buf_token(int slot, uint32_t off)
{
    return refplat.claims[slot].buf_token + off;
}

volatile uint8_t *refplat_ptr(uint32_t token)
{
    return arena + (token - REFPLAT_TOKEN_BASE);
}

static void sq_view(int slot, aar_queue *q)
{
    volatile uint8_t *cb = refplat.claims[slot].cb;

    memset(q, 0, sizeof(*q));

    (void)aar_cb_queue_bind(q, (volatile void *)cb, (volatile void *)(cb + 0x1000u),
                            AAR_QUEUE_SQ);
}

static void cq_view(int slot, aar_queue *q)
{
    volatile uint8_t *cb = refplat.claims[slot].cb;

    memset(q, 0, sizeof(*q));

    (void)aar_cb_queue_bind(q, (volatile void *)cb, (volatile void *)(cb + 0x2000u),
                            AAR_QUEUE_CQ);
}

int refplat_app_request(int slot, uint32_t op, uint32_t flags, uint32_t id,
                        const uint32_t arg[6], uint32_t buf, uint32_t buf_off,
                        uint32_t buf_len)
{
    aar_queue sq;
    aar_request r;
    uint32_t i;
    int rc;

    sq_view(slot, &sq);
    memset(&r, 0, sizeof(r));
    r.op = (uint16_t)op;
    r.flags = (uint16_t)flags;
    r.id = id;
    for (i = 0u; arg && i < AAR_ENTRY_ARGS; i++)
        r.arg[i] = arg[i];
    r.buf = buf;
    r.buf_off = buf_off;
    r.buf_len = buf_len;
    rc = aar_queue_post_request(&sq, &r, NULL, NULL);
    if (rc == AAR_OK)
        event_post_claim(&refplat.claims[slot]);
    return rc;
}

int refplat_app_message(int slot, const uint8_t msg[AAR_MSG_BYTES])
{
    aar_queue sq;
    int rc;

    sq_view(slot, &sq);
    rc = aar_queue_post_msg(&sq, msg, NULL, NULL);
    if (rc == AAR_OK)
        event_post_claim(&refplat.claims[slot]);
    return rc;
}

int refplat_app_log(int slot, uint32_t buf, uint32_t off, uint32_t len)
{
    aar_queue sq;
    volatile uint8_t *s;
    int rc;

    sq_view(slot, &sq);
    rc = aar_queue_prepare(&sq, 0u, NULL, &s);
    if (rc != AAR_OK)
        return rc;
    aar_entry_clear(s);
    aar_entry_put_header(s, AAR_OP_LOG, 0u, 0u, 0);
    aar_put_be32(s + AAR_ENTRY_BUF, buf);
    aar_put_be32(s + AAR_ENTRY_BUF_OFF, off);
    aar_put_be32(s + AAR_ENTRY_BUF_LEN, len);
    aar_queue_commit(&sq, NULL);
    event_post_claim(&refplat.claims[slot]);
    return AAR_OK;
}

int refplat_app_cq_pop(int slot, uint8_t entry[AAR_ENTRY_BYTES])
{
    aar_queue cq;
    const volatile uint8_t *s;
    uint32_t i;
    int rc;

    cq_view(slot, &cq);
    rc = aar_queue_peek(&cq, NULL, &s);
    if (rc != 1)
        return 0;
    for (i = 0u; i < AAR_ENTRY_BYTES; i++)
        entry[i] = s[i];
    aar_queue_pop(&cq, NULL);
    return 1;
}

uint32_t refplat_app_cq_used(int slot)
{
    aar_queue cq;
    uint32_t used = 0u;

    cq_view(slot, &cq);
    (void)aar_queue_used(&cq, &used);
    return used;
}

uint32_t refplat_app_sq_used(int slot)
{
    aar_queue sq;
    uint32_t used = 0u;

    sq_view(slot, &sq);
    (void)aar_queue_used(&sq, &used);
    return used;
}

void refplat_app_set_state(int slot, uint32_t state, uint32_t end_reason)
{
    put_state(&refplat.claims[slot], state, end_reason);
}

void refplat_app_set_consumed(int slot, uint32_t consumed)
{
    aar_put_be32(refplat.claims[slot].cb + AAR_CB_MSG_CONSUMED, consumed);
}

void refplat_revoke(int slot)
{
    refplat.claims[slot].revoked = 1u;
}
