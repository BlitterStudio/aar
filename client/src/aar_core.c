/*
 * The portable core (L1) of aar.library. See aar_core.h.
 *
 * The queue pump, the inbox, the events, the doorbell, the audio read and the
 * map cache come from the client transport module of the ZZ9000 SDK, with the
 * board calls replaced by the operations of aar_plat.h. The handle table, the
 * owner and stale checks, the lease list, tags, status, AARInfo and the
 * dispatcher passes are new.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "aar_core.h"

#define PTR(a) ((void *)(uintptr_t)(a))
#define APTR_OF(p) ((APTR)(uintptr_t)(p))

#define ROUND8(n) (((n) + 7u) & ~7u)

/* AARExt functions take a handle and no core: they use this one. */
static aar_core *g_ext_core;

/* ---- Small helpers --------------------------------------------------------------- */

static const struct aar_plat_ops *ops_of(const AARHandle *h)
{
    return h->core->plat->ops;
}

static struct aar_plat *plat_of(const AARHandle *h)
{
    return h->core->plat;
}

static void env_lock(aar_core *c, int exclusive)
{
    if (c->env.lock)
        c->env.lock(c->env.user, exclusive);
}

static void env_unlock(aar_core *c)
{
    if (c->env.unlock)
        c->env.unlock(c->env.user);
}

static void irq_off(aar_core *c)
{
    if (c->env.irq_off)
        c->env.irq_off(c->env.user);
}

static void irq_on(aar_core *c)
{
    if (c->env.irq_on)
        c->env.irq_on(c->env.user);
}

/*
 * The size rule of this file: a sum or a round-up of a value that the client,
 * the app or the platform supplies is checked against the room that is left
 * (a > MAX - b) before it is computed, never after, so a huge length cannot
 * wrap to a small one and pass. The helpers answer 0 when the result does not
 * fit in 32 bits.
 */

/* [off, off + len) rounded up to the map step, in *need. */
static int map_need(uint32_t off, uint32_t len, uint32_t *need)
{
    uint32_t end;

    if (len > UINT32_MAX - off)
        return 0;
    end = off + len;
    if (end > UINT32_MAX - (AAR_CORE_MAP_STEP - 1u))
        return 0;
    *need = (end + (AAR_CORE_MAP_STEP - 1u)) & ~(AAR_CORE_MAP_STEP - 1u);
    return 1;
}

/* `v` rounded up to a multiple of the power of two `g`, in *out. */
static int align_up(uint32_t v, uint32_t g, uint32_t *out)
{
    if (v > UINT32_MAX - (g - 1u))
        return 0;
    *out = (v + (g - 1u)) & ~(g - 1u);
    return 1;
}

static uint32_t str_len(const char *s)
{
    uint32_t n = 0u;

    while (s[n] != '\0')
        n++;
    return n;
}

/* The audio ring's index words are little-endian and the app reads or writes
 * each with one 32-bit access while this side does the opposite. This side
 * therefore uses one aligned long too: byte accesses would show the app a
 * half-written read_frame. */
static uint32_t swap32(uint32_t v)
{
    return (v << 24) | ((v & 0x0000ff00u) << 8) | ((v >> 8) & 0x0000ff00u) |
           (v >> 24);
}

static uint32_t le32(const volatile uint8_t *p)
{
    uint32_t v = *(const volatile uint32_t *)(const volatile void *)p;

    return AAR_NATIVE_BIG_ENDIAN ? swap32(v) : v;
}

static void put_le32(volatile uint8_t *p, uint32_t v)
{
    *(volatile uint32_t *)(volatile void *)p =
        AAR_NATIVE_BIG_ENDIAN ? swap32(v) : v;
}

/* Copies into memory behind the platform's mapping: longs when both ends are
 * aligned, else bytes. */
static void copy_to_dev(volatile uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0u;

    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
        for (; i + 4u <= n; i += 4u)
            *(volatile uint32_t *)(volatile void *)(dst + i) =
                *(const uint32_t *)(const void *)(src + i);
    }
    for (; i < n; i++)
        dst[i] = src[i];
}

static void cb_add(AARHandle *h, uint32_t off, uint32_t n)
{
    volatile uint8_t *cb = h->cb;

    if (cb)
        aar_put_be32(cb + off, aar_get_be32(cb + off) + n);
}

static int claim_live(const AARHandle *h)
{
    return h->cb != NULL && !h->revoked ? AAR_OK : AAR_E_STALE;
}

/* ---- The lease list ------------------------------------------------------------------- */

static void lease_unlink(AARHandle *h)
{
    aar_core *c = h->core;
    AARHandle *p;

    irq_off(c);
    if (c->lease_head == h) {
        c->lease_head = h->lease_next;
    } else {
        for (p = c->lease_head; p; p = p->lease_next) {
            if (p->lease_next == h) {
                p->lease_next = h->lease_next;
                break;
            }
        }
    }
    h->lease_next = NULL;
    irq_on(c);
}

static void lease_store(AARHandle *h, volatile uint8_t *cb)
{
    uint32_t v = h->lease + 1u;

    if (v == 0u)
        v = 1u;
    h->lease = v;
    aar_put_be32(cb + AAR_CB_LEASE_COUNTER, v);
}

static void lease_link(AARHandle *h)
{
    aar_core *c = h->core;
    volatile uint8_t *cb = h->cb;

    h->countdown = (uint8_t)(1u + c->opens % AAR_CORE_LEASE_PERIOD_TICKS);
    c->opens++;
    if (cb)
        lease_store(h, cb);
    h->renew = 1u;
    irq_off(c);
    h->lease_next = c->lease_head;
    c->lease_head = h;
    irq_on(c);
}

void aar_core_vblank(aar_core *c)
{
    AARHandle *h;
    uint32_t t = c->ticks + 1u;

    c->ticks = t;
    if ((uint32_t)(t - c->disp_alive) > AAR_CORE_STALL_TICKS)
        return;
    for (h = c->lease_head; h; h = h->lease_next) {
        volatile uint8_t *cb = h->cb;

        if (!h->renew || !cb)
            continue;
        if (h->countdown > 1u) {
            h->countdown--;
            continue;
        }
        h->countdown = AAR_CORE_LEASE_PERIOD_TICKS;
        if (h->pause_until != 0u && (int32_t)(t - h->pause_until) < 0)
            continue;
        lease_store(h, cb);
    }
}

/* ---- The registry --------------------------------------------------------------------- */

/* Caller holds irq_off. */
static AARHandle *registry_find(aar_core *c, const AARHandle *h)
{
    AARHandle *p;

    for (p = c->handles; p; p = p->next) {
        if (p == h)
            return p;
    }
    return NULL;
}

static void registry_remove(aar_core *c, AARHandle *h)
{
    AARHandle *p;

    env_lock(c, 1);
    irq_off(c);
    if (c->handles == h) {
        c->handles = h->next;
    } else {
        for (p = c->handles; p; p = p->next) {
            if (p->next == h) {
                p->next = h->next;
                break;
            }
        }
    }
    h->next = NULL;
    irq_on(c);
    env_unlock(c);
}

static void handle_free(AARHandle *h)
{
    aar_core *c = h->core;
    uint32_t bytes = h->alloc_bytes;

    if (c->env.retire)
        c->env.retire(c->env.user, h);
    c->env.release(c->env.user, h, bytes);
}

/*
 * The calling task's right to use `h`: AAR_OK with h->busy set (the caller
 * clears it with leave), AAR_E_STALE for a pointer that is not a live handle,
 * AAR_E_OWNER for another task's handle. The pointer is only compared until it
 * has been found in the registry.
 */
static int enter(aar_core *c, AARHandle *h)
{
    aar_owner_stamp me;
    int rc;

    c->env.self(c->env.user, &me);
    irq_off(c);
    if (!registry_find(c, h) || h->state != AAR_H_LIVE) {
        rc = AAR_E_STALE;
    } else if (h->owner.id != me.id) {
        rc = AAR_E_OWNER;
    } else {
        while (h->bg) {
            irq_on(c);
            if (c->env.yield)
                c->env.yield(c->env.user);
            irq_off(c);
        }
        h->busy = 1u;
        rc = AAR_OK;
    }
    irq_on(c);
    return rc;
}

static void leave(AARHandle *h)
{
    h->busy = 0u;
}

/* Owner and registry first (enter), then a revoked or detached claim. A call
 * uses this before it looks at arguments or does any other work, so a stale
 * handle is never reported as a bad argument or a missing file. AARClose uses
 * enter alone: it still has to release the claim. */
static int require_live(aar_core *c, AARHandle *h)
{
    int rc = enter(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = claim_live(h);
    if (rc != AAR_OK)
        leave(h);
    return rc;
}

/* ---- Revocation and release ------------------------------------------------------------ */

/* Drops every pointer of the claim. The inbox stays readable. */
static void drop_pointers(AARHandle *h)
{
    h->cb = NULL;
    h->load = NULL;
    h->audio = NULL;
    h->audio_frames = 0u;
    h->audio_channels = 0u;
    h->load_state = AAR_LOAD_NONE;
    memset(h->maps, 0, sizeof(h->maps));
    h->map_next = 0u;
    memset(h->out, 0, sizeof(h->out));
    h->mouse_pending = 0u;
    h->kick_pending = 0u;
    h->sq_blocked = 0u;
    memset(&h->sq, 0, sizeof(h->sq));
    memset(&h->cq, 0, sizeof(h->cq));
}

/* The platform revoked the claim: nothing of it may be touched again, and the
 * interrupt must stop storing into it. The platform's release is still owed. */
static void revoke(AARHandle *h)
{
    h->renew = 0u;
    lease_unlink(h);
    drop_pointers(h);
    h->revoked = 1u;
}

/*
 * The stale rule: a platform call that answers AAR_E_STALE for a handle
 * revokes it at once (revoke above: the lease list, the maps, the queue views),
 * and whatever made the call stops using the claim's pointers. Every call of
 * the platform that takes the claim or maps its memory goes through plat_rc
 * (map, load_begin, load_chunk, load_end, start, stop, kick); calls that return
 * nothing (poll, cache_publish, cache_acquire) cannot report it, and token and
 * ext take no claim. A call site that can revoke is followed by claim_live
 * before anything else touches h->cb, h->sq, h->cq or a map.
 */
static int plat_rc(AARHandle *h, int rc)
{
    if (rc == AAR_E_STALE)
        revoke(h);
    return rc;
}

/*
 * The strict order of design 3.3: out of the lease list first, then the stop
 * and the release, then the pointers, so the interrupt never stores into
 * memory the platform has freed. Idempotent.
 */
static void teardown(AARHandle *h)
{
    const struct aar_plat_ops *o = ops_of(h);

    h->renew = 0u;
    lease_unlink(h);
    if (h->claim.handle != 0u) {
        if (h->started && !h->revoked && o->stop)
            (void)o->stop(plat_of(h), &h->claim);
        (void)o->release(plat_of(h), &h->claim);
        h->claim.handle = 0u;
    }
    drop_pointers(h);
    h->revoked = 1u;
}

/* ---- The map cache ---------------------------------------------------------------------- */

/*
 * The pointer of [token + off, token + off + len), mapped through the platform
 * once per block and extended when a later request needs more of it. The cache
 * never shrinks: a cache-inhibited page stays so.
 */
static int map_range(AARHandle *h, uint32_t token, uint32_t off, uint32_t len,
                     volatile uint8_t **out, uint32_t *mapf)
{
    const struct aar_plat_ops *o = ops_of(h);
    uint32_t need, i, slot, flags = 0u;
    volatile uint8_t *p = NULL;
    int rc;

    if (token == 0u || !o->map)
        return token == 0u ? AAR_E_INVAL : AAR_E_NOSYS;
    if (len == 0u)
        len = 1u;
    if (!map_need(off, len, &need))
        return AAR_E_INVAL;
    for (i = 0u; i < AAR_CORE_MAPS; i++) {
        if (h->maps[i].ptr && h->maps[i].token == token) {
            if (h->maps[i].bytes >= need) {
                *out = h->maps[i].ptr + off;
                if (mapf)
                    *mapf = h->maps[i].mapf;
                return AAR_OK;
            }
            slot = i;
            goto map_it;
        }
    }
    for (slot = 0u; slot < AAR_CORE_MAPS && h->maps[slot].ptr; slot++)
        ;
    if (slot == AAR_CORE_MAPS) {
        slot = h->map_next;
        h->map_next = (h->map_next + 1u) % AAR_CORE_MAPS;
    }
map_it:
    rc = o->map(plat_of(h), token, need, &p, &flags);
    if (rc != AAR_OK)
        return plat_rc(h, rc);
    if (!p)
        return AAR_E_IO;
    h->maps[slot].token = token;
    h->maps[slot].bytes = need;
    h->maps[slot].mapf = flags;
    h->maps[slot].ptr = p;
    h->stats.maps++;
    *out = p + off;
    if (mapf)
        *mapf = flags;
    return AAR_OK;
}

static int bind_queue(AARHandle *h, uint32_t which)
{
    uint32_t desc = AAR_CB_Q_DESC(which);
    uint32_t token = aar_get_be32(h->cb + desc + AAR_QD_TOKEN);
    uint32_t off = aar_get_be32(h->cb + desc + AAR_QD_OFFSET);
    uint32_t depth = aar_get_be32(h->cb + desc + AAR_QD_DEPTH);
    volatile uint8_t *ring;

    if (!aar_queue_depth_valid(depth))
        return AAR_E_IO;
    if (map_range(h, token, off, depth * AAR_ENTRY_BYTES, &ring, NULL) != AAR_OK)
        return AAR_E_IO;
    return aar_cb_queue_bind(which == AAR_QUEUE_SQ ? &h->sq : &h->cq,
                             (volatile void *)h->cb, (volatile void *)ring,
                             which);
}

/* ---- Setup and teardown of the core ------------------------------------------------------- */

int aar_core_init(aar_core *c, struct aar_plat *plat, const struct aar_core_env *env)
{
    if (!c || !plat || !plat->ops || !plat->info || !env || !env->alloc ||
        !env->release || !env->self)
        return AAR_E_INVAL;
    memset(c, 0, sizeof(*c));
    c->plat = plat;
    c->env = *env;
    if (c->env.ticks_per_sec == 0u)
        c->env.ticks_per_sec = 50u;
    c->attach_rc = AAR_E_NOSYS;
    g_ext_core = c;
    return AAR_OK;
}

int aar_core_attach(aar_core *c)
{
    const struct aar_plat_ops *o = c->plat->ops;

    c->attach_rc = o->attach ? o->attach(c->plat) : AAR_OK;
    return c->attach_rc;
}

void aar_core_detach(aar_core *c)
{
    const struct aar_plat_ops *o = c->plat->ops;

    if (c->attach_rc == AAR_OK && o->detach)
        o->detach(c->plat);
    c->attach_rc = AAR_E_NOSYS;
}

int aar_core_check(aar_core *c, AARHandle *h)
{
    int rc = require_live(c, h);

    if (rc == AAR_OK)
        leave(h);
    return rc;
}

/* ---- Query ------------------------------------------------------------------------------- */

int aar_core_query(aar_core *c, AARInfo *info)
{
    const struct aar_plat_info *pi = c->plat->info;
    const struct aar_plat_ops *o = c->plat->ops;
    struct aar_plat_query q;
    AARInfo local;
    uint32_t n, i;
    int rc;

    if (!info || info->size < 4u)
        return AAR_E_INVAL;
    if (c->attach_rc != AAR_OK)
        return c->attach_rc;
    memset(&q, 0, sizeof(q));
    if (o->query) {
        rc = o->query(c->plat, &q);
        if (rc != AAR_OK)
            return rc;
    }
    memset(&local, 0, sizeof(local));
    local.size = info->size;
    local.api_major = AAR_API_MAJOR;
    local.api_minor = AAR_API_MINOR;
    local.platform_id = pi->platform_id;
    local.profiles = pi->profiles;
    local.groups = pi->groups | q.groups;
    local.flags = pi->flags;
    local.max_apps = pi->max_apps;
    local.max_image_bytes = pi->max_image_bytes;
    local.max_args = pi->max_args;
    local.max_arg_bytes = pi->max_arg_bytes;
    local.shared_granule = pi->shared_granule;
    local.msg_capacity = q.queue_depth ? AAR_MSG_CAPACITY_FOR(q.queue_depth) : 0u;
    local.stall_limit_ms = pi->stall_limit_ms;
    local.stop_grace_ms = pi->stop_grace_ms;
    local.shared_default_mib = pi->shared_default_mib;
    local.shared_max_mib = pi->shared_max_mib;
    local.weight_max = pi->weight_max;
    local.queue_depth = q.queue_depth;
    for (i = 0u; i < 8u; i++)
        local.platform[i] = q.platform[i];
    n = info->size < sizeof(local) ? info->size : (uint32_t)sizeof(local);
    memcpy(info, &local, n);
    return AAR_OK;
}

/* ---- Open and close ----------------------------------------------------------------------- */

int aar_core_open(aar_core *c, uint32_t signal_mask, void *notify, void *opener,
                  AARHandle **out, ULONG *reason)
{
    const struct aar_plat_ops *o = c->plat->ops;
    AARHandle *h;
    uint32_t why = AAR_REASON_NONE, token, bytes;
    int rc;

    if (reason)
        *reason = AAR_REASON_NONE;
    if (!out)
        return AAR_E_INVAL;
    *out = NULL;
    if (c->attach_rc != AAR_OK)
        return c->attach_rc;
    /* The handle and the platform's claim state are one block: their sizes
     * must add up. */
    if (!align_up(o->claim_priv_bytes, 8u, &bytes) ||
        bytes > UINT32_MAX - ROUND8((uint32_t)sizeof(*h)))
        return AAR_E_NOMEM;
    bytes += ROUND8((uint32_t)sizeof(*h));
    h = (AARHandle *)c->env.alloc(c->env.user, bytes);
    if (!h)
        return AAR_E_NOMEM;
    memset(h, 0, sizeof(*h));
    h->core = c;
    h->alloc_bytes = bytes;
    h->state = AAR_H_CLOSING;
    c->env.self(c->env.user, &h->owner);
    h->signal_mask = signal_mask;
    h->notify = notify;
    h->opener = opener;
    h->armed = notify != NULL;
    h->opts.lease_extended = 1u;
    h->stats.size = (ULONG)sizeof(h->stats);
    h->claim.priv = o->claim_priv_bytes ?
        (void *)((uint8_t *)h + ROUND8((uint32_t)sizeof(*h))) : NULL;

    rc = o->claim(c->plat, &h->claim, &why);
    if (rc != AAR_OK) {
        if (reason && rc == AAR_E_NORESOURCE)
            *reason = why;
        handle_free(h);
        return rc;
    }
    rc = AAR_E_IO;
    token = h->claim.cb_token;
    if (h->claim.handle == 0u || token == 0u)
        goto undo;
    {
        volatile uint8_t *cb;
        int mrc = map_range(h, token, 0u, AAR_CB_BYTES, &cb, NULL);

        if (mrc != AAR_OK) {
            rc = mrc == AAR_E_NOSYS ? AAR_E_NOSYS : AAR_E_IO;
            goto undo;
        }
        h->cb = cb;
    }
    if (aar_cb_check(h->cb) != AAR_OK ||
        aar_get_be32(h->cb + AAR_CB_HANDLE) != h->claim.handle)
        goto undo;
    if (bind_queue(h, AAR_QUEUE_SQ) != AAR_OK ||
        bind_queue(h, AAR_QUEUE_CQ) != AAR_OK)
        goto undo;
    aar_put_be32(h->cb + AAR_CB_CLIENT_PRESENT, 1u);

    env_lock(c, 1);
    irq_off(c);
    h->next = c->handles;
    c->handles = h;
    h->state = AAR_H_LIVE;
    irq_on(c);
    env_unlock(c);
    lease_link(h);
    *out = h;
    return AAR_OK;

undo:
    /* The claim exists on the platform; give it back. */
    (void)o->release(c->plat, &h->claim);
    handle_free(h);
    return rc;
}

/* Takes a live handle for closing: out of reach of every other call. */
static int take_for_close(aar_core *c, AARHandle *h)
{
    int got = 0;

    env_lock(c, 1);
    irq_off(c);
    if (h->state == AAR_H_LIVE) {
        h->state = AAR_H_CLOSING;
        got = 1;
    }
    irq_on(c);
    env_unlock(c);
    return got;
}

/* The close sequence, for a handle that is CLOSING: teardown, then forget it. */
static void finish_close(aar_core *c, AARHandle *h)
{
    teardown(h);
    registry_remove(c, h);
    handle_free(h);
}

int aar_core_close(aar_core *c, AARHandle *h)
{
    int rc = enter(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!take_for_close(c, h))
        return AAR_E_STALE;
    finish_close(c, h);
    return AAR_OK;
}

uint32_t aar_core_close_owner(aar_core *c)
{
    aar_owner_stamp me;
    uint32_t n = 0u;

    c->env.self(c->env.user, &me);
    for (;;) {
        AARHandle *h, *found = NULL;
        int live = 0;

        env_lock(c, 0);
        for (h = c->handles; h; h = h->next) {
            if (h->owner.id == me.id &&
                (h->state == AAR_H_LIVE || h->state == AAR_H_DEAD)) {
                found = h;
                live = h->state == AAR_H_LIVE;
                break;
            }
        }
        env_unlock(c);
        if (!found)
            break;
        if (live) {
            if (!take_for_close(c, found))
                continue;
            finish_close(c, found);
        } else {
            registry_remove(c, found);
            handle_free(found);
        }
        n++;
    }
    return n;
}

void aar_core_shutdown(aar_core *c)
{
    for (;;) {
        AARHandle *h, *found = NULL;

        env_lock(c, 0);
        for (h = c->handles; h; h = h->next) {
            if (h->state != AAR_H_CLOSING) {
                found = h;
                break;
            }
        }
        env_unlock(c);
        if (!found)
            break;
        if (found->state == AAR_H_LIVE && take_for_close(c, found)) {
            finish_close(c, found);
        } else if (found->state == AAR_H_DEAD) {
            registry_remove(c, found);
            handle_free(found);
        }
    }
}

/* ---- Tags --------------------------------------------------------------------------------- */

void aar_core_tags_init(struct aar_core_tags *t)
{
    memset(t, 0, sizeof(*t));
}

#define TAG_LIMIT_MIB 4095u

int aar_core_tags_add(aar_core *c, struct aar_core_tags *t, uint32_t id, uint32_t data)
{
    const struct aar_plat_info *pi = c->plat->info;
    uint32_t n, limit;

    if (id < AAR_TAG_BASE || id >= AAR_TAG_BASE + 0x100u)
        return AAR_E_INVAL;
    n = id - AAR_TAG_BASE;
    switch (n) {
    case 1u:
        if (data > TAG_LIMIT_MIB)
            return AAR_E_INVAL;
        t->private_mib = data;
        break;
    case 2u:
        limit = pi->shared_max_mib != 0u && pi->shared_max_mib < TAG_LIMIT_MIB ?
                pi->shared_max_mib : TAG_LIMIT_MIB;
        if (data > limit)
            return AAR_E_INVAL;
        t->shared_mib = data;
        break;
    case 3u:
        if (pi->weight_max == 0u)
            return AAR_E_NOSYS;
        if (data > pi->weight_max)
            return AAR_E_INVAL;
        t->weight = data;
        break;
    case 4u:
        t->lease_extended = data ? 1u : 0u;
        break;
    case 5u:
        if (data != AAR_AUDIO_ROUTE_PLATFORM && data != AAR_AUDIO_ROUTE_AHI)
            return AAR_E_INVAL;
        t->audio_route = data;
        break;
    case 6u:
        if (data & ~(uint32_t)AAR_CCAP_KNOWN)
            return AAR_E_INVAL;
        t->caps = data;
        break;
    default:
        return AAR_E_INVAL;
    }
    t->set |= 1u << n;
    return AAR_OK;
}

static int set_tags_locked(AARHandle *h, const struct aar_core_tags *t)
{
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (h->load_state != AAR_LOAD_NONE || h->started)
        return AAR_E_BUSY;
    if (t->set & (1u << 1))
        h->private_mib = t->private_mib;
    if (t->set & (1u << 2))
        h->shared_mib = t->shared_mib;
    if (t->set & (1u << 3))
        h->weight = t->weight;
    if (t->set & (1u << 4))
        h->opts.lease_extended = t->lease_extended;
    if (t->set & (1u << 5))
        h->opts.audio_route = t->audio_route;
    if (t->set & (1u << 6))
        h->opts.client_caps = t->caps;
    return AAR_OK;
}

int aar_core_set_tags(aar_core *c, AARHandle *h, const struct aar_core_tags *t)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = t ? set_tags_locked(h, t) : AAR_E_INVAL;
    leave(h);
    return rc;
}

/* ---- Load, start, stop ------------------------------------------------------------------------- */

static int load_begin(AARHandle *h, uint32_t image_bytes, volatile uint8_t **staging,
                      uint32_t *chunk_max)
{
    const struct aar_plat_info *pi = h->core->plat->info;
    uint32_t why = AAR_REASON_NONE, chunk = 0u;
    volatile uint8_t *area = NULL;
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (h->load_state != AAR_LOAD_NONE || h->started)
        return AAR_E_BUSY;
    if (image_bytes == 0u || (pi->max_image_bytes != 0u && image_bytes > pi->max_image_bytes))
        return AAR_E_INVAL;
    aar_put_be32(h->cb + AAR_CB_PRIVATE_MIB, h->private_mib);
    aar_put_be32(h->cb + AAR_CB_SHARED_MIB, h->shared_mib);
    aar_put_be32(h->cb + AAR_CB_WEIGHT, h->weight);
    h->reason = AAR_REASON_NONE;
    rc = ops_of(h)->load_begin(plat_of(h), &h->claim, &h->opts, &area, &chunk, &why);
    if (rc != AAR_OK) {
        if (rc == AAR_E_NORESOURCE)
            h->reason = why;
        return plat_rc(h, rc);
    }
    if (!area || chunk < 4096u) {
        /* The platform has begun: this handle cannot load again. */
        h->load_state = AAR_LOAD_FAILED;
        return AAR_E_IO;
    }
    h->load = area;
    h->chunk_max = chunk;
    h->image_bytes = image_bytes;
    h->load_state = AAR_LOAD_LOADING;
    if (staging)
        *staging = area;
    if (chunk_max)
        *chunk_max = chunk;
    return AAR_OK;
}

int aar_core_load_begin(aar_core *c, AARHandle *h, uint32_t image_bytes,
                        volatile uint8_t **staging, uint32_t *chunk_max)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = load_begin(h, image_bytes, staging, chunk_max);
    leave(h);
    return rc;
}

static int load_chunk(AARHandle *h, uint32_t file_off, uint32_t len)
{
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (h->load_state != AAR_LOAD_LOADING || h->started)
        return AAR_E_INVAL;
    if (len == 0u || len > h->chunk_max || file_off > h->image_bytes ||
        len > h->image_bytes - file_off)
        return AAR_E_INVAL;
    rc = plat_rc(h, ops_of(h)->load_chunk(plat_of(h), &h->claim, file_off, len));
    if (rc != AAR_OK && h->load_state == AAR_LOAD_LOADING) /* not after a revoke */
        h->load_state = AAR_LOAD_FAILED;
    return rc;
}

int aar_core_load_chunk(aar_core *c, AARHandle *h, uint32_t file_off, uint32_t len)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = load_chunk(h, file_off, len);
    leave(h);
    return rc;
}

static int load_end(AARHandle *h)
{
    uint32_t why = AAR_REASON_NONE;
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (h->load_state != AAR_LOAD_LOADING || h->started)
        return AAR_E_INVAL;
    rc = ops_of(h)->load_end(plat_of(h), &h->claim, &why);
    if (rc == AAR_E_NORESOURCE)
        h->reason = why;
    rc = plat_rc(h, rc);
    if (h->load_state == AAR_LOAD_LOADING) /* not after a revoke */
        h->load_state = rc == AAR_OK ? AAR_LOAD_LOADED : AAR_LOAD_FAILED;
    return rc;
}

int aar_core_load_end(aar_core *c, AARHandle *h)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = load_end(h);
    leave(h);
    return rc;
}

int aar_core_load_memory(aar_core *c, AARHandle *h, const void *data, ULONG length)
{
    const uint8_t *src = (const uint8_t *)data;
    volatile uint8_t *staging = NULL;
    uint32_t chunk_max = 0u, off = 0u, n;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!data || length == 0u) {
        leave(h);
        return AAR_E_INVAL;
    }
    rc = load_begin(h, length, &staging, &chunk_max);
    while (rc == AAR_OK && off < length) {
        n = length - off < chunk_max ? length - off : chunk_max;
        copy_to_dev(staging, src + off, n);
        rc = load_chunk(h, off, n);
        off += n;
    }
    if (rc == AAR_OK)
        rc = load_end(h);
    leave(h);
    return rc;
}

int aar_core_start(aar_core *c, AARHandle *h, const char *const *argv, ULONG argc)
{
    const struct aar_plat_info *pi = c->plat->info;
    uint32_t i, n;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = claim_live(h);
    if (rc == AAR_OK && (h->load_state != AAR_LOAD_LOADED || h->started))
        rc = AAR_E_INVAL;
    if (rc == AAR_OK && ((pi->max_args != 0u && argc > pi->max_args) ||
                         (argc != 0u && !argv)))
        rc = AAR_E_INVAL;
    for (i = 0u; rc == AAR_OK && i < argc; i++) {
        if (!argv[i]) {
            rc = AAR_E_INVAL;
            break;
        }
        n = str_len(argv[i]);
        if (pi->max_arg_bytes != 0u && n + 1u > pi->max_arg_bytes)
            rc = AAR_E_INVAL;
    }
    if (rc == AAR_OK) {
        rc = ops_of(h)->start(plat_of(h), &h->claim, argv, argc);
        if (rc == AAR_OK) {
            /* The platform frees its staging area once it has the arguments. */
            h->started = 1u;
            h->load_state = AAR_LOAD_NONE;
            h->load = NULL;
        } else {
            rc = plat_rc(h, rc);
            /* The platform may have gone part of the way: no second start. */
            if (h->load_state == AAR_LOAD_LOADED)
                h->load_state = AAR_LOAD_FAILED;
        }
    }
    leave(h);
    return rc;
}

/* ---- Doorbell and events --------------------------------------------------------------------- */

static void kick_send(AARHandle *h)
{
    const struct aar_plat_ops *o = ops_of(h);
    int rc;

    if (!h->kick_pending || !o->kick || claim_live(h) != AAR_OK)
        return;
    rc = o->kick(plat_of(h), &h->claim, h->kick_pending);
    if (rc == AAR_OK) {
        h->kick_pending = 0u;
        h->stats.kicks++;
    } else {
        /* BUSY: the previous bell is still on its way; the bits stay for the
         * next pass. The kick is a hint, a lost one costs latency only. STALE
         * revokes the claim. */
        (void)plat_rc(h, rc);
    }
}

static void kick(AARHandle *h, uint32_t bits)
{
    if (claim_live(h) != AAR_OK || !bits)
        return;
    if (h->kick_pending)
        h->stats.kicks_coalesced++;
    h->kick_pending |= bits;
    kick_send(h);
}

static void note_event_drop(AARHandle *h)
{
    h->stats.events_dropped++;
    cb_add(h, AAR_CB_EVENTS_DROPPED, 1u);
    cb_add(h, AAR_CB_CQ_FULL, 1u);
}

/* Posts the pending mouse event: AAR_OK, AAR_E_BUSY (CQ full, still pending),
 * AAR_E_IO. */
static int mouse_post(AARHandle *h)
{
    const struct aar_plat_ops *o = ops_of(h);
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (!h->mouse_pending)
        return AAR_OK;
    rc = aar_queue_post_event(&h->cq, &h->mouse, o->barrier_acquire,
                              o->barrier_release);
    if (rc == AAR_OK) {
        h->mouse_pending = 0u;
        h->stats.events_posted++;
        kick(h, AAR_PLAT_KICK_CQ);
    }
    return rc;
}

static int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static int post_event(AARHandle *h, const aar_event *ev)
{
    const struct aar_plat_ops *o = ops_of(h);
    int rc = claim_live(h);

    if (rc != AAR_OK)
        return rc;
    if (!ev)
        return AAR_E_INVAL;
    if (ev->type == AAR_EV_MOUSE) {
        if (h->mouse_pending &&
            (h->mouse.buttons != ev->buttons || h->mouse.code != ev->code)) {
            /* A click or a button change is not folded away. */
            rc = mouse_post(h);
            if (rc == AAR_E_BUSY) {
                note_event_drop(h);
                return AAR_E_BUSY;
            }
            if (rc != AAR_OK)
                return rc;
        }
        if (h->mouse_pending) {
            h->mouse.dx = sat16((int32_t)h->mouse.dx + ev->dx);
            h->mouse.dy = sat16((int32_t)h->mouse.dy + ev->dy);
            h->mouse.time_ms = ev->time_ms;
            h->stats.events_merged++;
        } else {
            h->mouse = *ev;
            h->mouse.reserved = 0u;
            h->mouse_pending = 1u;
        }
        rc = mouse_post(h);
        return rc == AAR_E_BUSY ? AAR_OK : rc;
    }
    /* Keep the order of motion, keys and clicks. */
    rc = mouse_post(h);
    if (rc == AAR_E_BUSY) {
        note_event_drop(h);
        return AAR_E_BUSY;
    }
    if (rc != AAR_OK)
        return rc;
    rc = claim_live(h); /* the bell of the mouse event may have found the claim gone */
    if (rc != AAR_OK)
        return rc;
    rc = aar_queue_post_event(&h->cq, ev, o->barrier_acquire, o->barrier_release);
    if (rc == AAR_OK) {
        h->stats.events_posted++;
        kick(h, AAR_PLAT_KICK_CQ);
        return AAR_OK;
    }
    if (rc == AAR_E_BUSY)
        note_event_drop(h);
    return rc;
}

int aar_core_post_event(aar_core *c, AARHandle *h, const aar_event *event)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = post_event(h, event);
    leave(h);
    return rc;
}

int aar_core_stop(aar_core *c, AARHandle *h)
{
    aar_event ev;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = claim_live(h);
    if (rc == AAR_OK) {
        aar_put_be32(h->cb + AAR_CB_STOP_REQUEST, 1u);
        memset(&ev, 0, sizeof(ev));
        ev.type = AAR_EV_STOP;
        (void)post_event(h, &ev);
        /* The event's bell may have found the claim gone: no stop then. */
        rc = claim_live(h);
        if (rc == AAR_OK)
            rc = plat_rc(h, ops_of(h)->stop(plat_of(h), &h->claim));
    }
    leave(h);
    return rc;
}

/* ---- Status ------------------------------------------------------------------------------------- */

#define FITS(n, field) \
    ((n) >= offsetof(AARStatusInfo, field) + sizeof(((AARStatusInfo *)0)->field))

static uint32_t get_hi(volatile uint8_t *cb, uint32_t off)
{
    return aar_get_be32(cb + off);
}

static void get_u64(volatile uint8_t *cb, uint32_t off, AARU64 *v)
{
    v->hi = get_hi(cb, off);
    v->lo = aar_get_be32(cb + off + 4u);
}

/* Reads only the words that fit in `n` bytes, so a poll of three words costs
 * three reads. */
static void status_read(AARHandle *h, AARStatusInfo *st, uint32_t size)
{
    AARStatusInfo local;
    volatile uint8_t *cb = h->cb;
    uint32_t n = size < sizeof(local) ? size : (uint32_t)sizeof(local);

    memset(&local, 0, sizeof(local));
    local.size = size;
    if (FITS(n, state))
        local.state = aar_get_be32(cb + AAR_CB_STATE);
    if (FITS(n, generation))
        local.generation = aar_handle_generation(h->claim.handle);
    if (FITS(n, end_reason))
        local.end_reason = aar_get_be32(cb + AAR_CB_END_REASON);
    if (FITS(n, exit_code))
        local.exit_code = (LONG)aar_get_be32(cb + AAR_CB_EXIT_CODE);
    if (FITS(n, flags))
        local.flags = aar_get_be32(cb + AAR_CB_FLAGS);
    if (FITS(n, profile))
        local.profile = aar_get_be32(cb + AAR_CB_PROFILE);
    if (FITS(n, fault_class))
        local.fault_class = aar_get_be32(cb + AAR_CB_FAULT_CLASS);
    if (FITS(n, fault_syndrome))
        local.fault_syndrome = aar_get_be32(cb + AAR_CB_FAULT_SYNDROME);
    if (FITS(n, fault_pstate))
        local.fault_pstate = aar_get_be32(cb + AAR_CB_FAULT_PSTATE);
    if (FITS(n, fault_pc))
        get_u64(cb, AAR_CB_FAULT_PC_HI, &local.fault_pc);
    if (FITS(n, fault_sp))
        get_u64(cb, AAR_CB_FAULT_SP_HI, &local.fault_sp);
    if (FITS(n, fault_lr))
        get_u64(cb, AAR_CB_FAULT_LR_HI, &local.fault_lr);
    if (FITS(n, fault_addr))
        get_u64(cb, AAR_CB_FAULT_ADDR_HI, &local.fault_addr);
    if (FITS(n, load_base))
        get_u64(cb, AAR_CB_LOAD_BASE_HI, &local.load_base);
    if (FITS(n, reason))
        local.reason = h->reason;
    if (FITS(n, lease_period_ms))
        local.lease_period_ms = aar_get_be32(cb + AAR_CB_LEASE_PERIOD_MS);
    if (FITS(n, msg_consumed))
        local.msg_consumed = aar_get_be32(cb + AAR_CB_MSG_CONSUMED);
    if (FITS(n, progress))
        get_u64(cb, AAR_CB_PROGRESS_HI, &local.progress);
    if (FITS(n, shared_used))
        get_u64(cb, AAR_CB_SHARED_USED_HI, &local.shared_used);
    if (FITS(n, private_committed))
        get_u64(cb, AAR_CB_PRIVATE_COMMITTED_HI, &local.private_committed);
    if (FITS(n, sq_full))
        local.sq_full = aar_get_be32(cb + AAR_CB_SQ_FULL);
    if (FITS(n, log_dropped))
        local.log_dropped = aar_get_be32(cb + AAR_CB_LOG_DROPPED);
    if (FITS(n, events_dropped))
        local.events_dropped = aar_get_be32(cb + AAR_CB_EVENTS_DROPPED);
    if (FITS(n, cq_full))
        local.cq_full = aar_get_be32(cb + AAR_CB_CQ_FULL);
    memcpy(st, &local, n);
    st->size = size;
}

int aar_core_status(aar_core *c, AARHandle *h, AARStatusInfo *status)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!status || status->size < 4u)
        rc = AAR_E_INVAL;
    else
        rc = claim_live(h);
    if (rc == AAR_OK)
        status_read(h, status, status->size);
    leave(h);
    return rc;
}

/* ---- Messages and the SQ ------------------------------------------------------------------------- */

static void sq_pop(AARHandle *h)
{
    uint32_t used;

    /* A full SQ means the app may be parked waiting for room. */
    if (aar_queue_used(&h->sq, &used) == AAR_OK && used == h->sq.depth)
        h->kick_pending |= AAR_PLAT_KICK_SQ;
    aar_queue_pop(&h->sq, ops_of(h)->barrier_release);
}

static void take_log(AARHandle *h, const aar_request *r)
{
    volatile uint8_t *p;
    uint32_t n = r->buf_len;

    if (n > AAR_LOG_MAX)
        n = AAR_LOG_MAX;
    if (!h->log_sink || n == 0u) {
        h->stats.logs_unclaimed++;
        return;
    }
    if (map_range(h, r->buf, r->buf_off, n, &p, NULL) != AAR_OK) {
        h->stats.bad_buf++;
        return;
    }
    /* The record is valid until the entry is popped, which is after this call. */
    h->stats.logs++;
    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);
}

static aar_outstanding *out_free(AARHandle *h)
{
    uint32_t i;

    for (i = 0u; i < AAR_CORE_OUTSTANDING; i++) {
        if (h->out[i].op == 0u)
            return &h->out[i];
    }
    return NULL;
}

/*
 * Moves messages and logs off the front of the SQ. With `req` it stops at the
 * first request and returns it (1); without, it stops there and leaves it. 0
 * when the SQ is empty, the next entry is a request that stays, the next entry
 * is a message the full inbox cannot take, or no request slot is free. A
 * message the inbox cannot take is not popped: it stays at the SQ head, so the
 * app, which reads the head index, sees the queue fill and gets AAR_E_BUSY from
 * its own post instead of losing messages. Everything behind it waits.
 * AAR_E_IO on corrupt indices.
 */
static int sq_pump(AARHandle *h, AARServiceReq *req)
{
    const struct aar_plat_ops *o = ops_of(h);

    h->sq_blocked = 0u;
    for (;;) {
        const volatile uint8_t *slot;
        aar_request r;
        aar_outstanding *out;
        int rc;
        uint16_t op;

        /* A call below may have revoked the claim (the stale rule). */
        if (claim_live(h) != AAR_OK)
            return AAR_E_STALE;
        rc = aar_queue_peek(&h->sq, o->barrier_acquire, &slot);
        if (rc <= 0)
            return rc;
        op = aar_get_be16(slot + AAR_ENTRY_OP);
        if (op == AAR_OP_MSG) {
            if (h->inbox_tail - h->inbox_head >= AAR_CORE_INBOX) {
                h->sq_blocked = 1u;
                h->stats.sq_blocked++;
                return 0;
            }
            aar_entry_get_msg(slot, h->inbox[h->inbox_tail % AAR_CORE_INBOX]);
            h->inbox_tail++;
            h->stats.msgs_taken++;
            sq_pop(h);
            continue;
        }
        aar_entry_get_request(slot, &r);
        if (op == AAR_OP_LOG) {
            take_log(h, &r);
            if (claim_live(h) != AAR_OK) /* its map found the claim gone */
                return AAR_E_STALE;
            sq_pop(h);
            continue;
        }
        if (op == AAR_OP_NONE || op == AAR_OP_EVENT) {
            /* Not valid on the SQ. */
            h->stats.bad_entries++;
            sq_pop(h);
            continue;
        }
        if (!req)
            return 0;
        out = out_free(h);
        if (!out)
            return 0;
        memset(req, 0, sizeof(*req));
        req->size = (ULONG)sizeof(*req);
        req->op = r.op;
        req->id = r.id;
        memcpy(req->arg, r.arg, sizeof(req->arg));
        req->buf = r.buf;
        req->buf_off = r.buf_off;
        req->buf_len = r.buf_len;
        req->flags = r.flags;
        if (r.buf != 0u) {
            volatile uint8_t *p;

            if (map_range(h, r.buf, r.buf_off, r.buf_len, &p, NULL) != AAR_OK) {
                aar_completion comp;

                if (claim_live(h) != AAR_OK) /* the claim is gone: nobody to refuse it to */
                    return AAR_E_STALE;

                /* The platform named memory it does not have: refuse it, so
                 * the app is not left waiting. With no room on the CQ leave
                 * the entry for the next pass. */
                memset(&comp, 0, sizeof(comp));
                comp.op = r.op;
                comp.id = r.id;
                comp.result = AAR_E_INVAL;
                if (aar_queue_post_completion(&h->cq, &comp, o->barrier_acquire,
                                              o->barrier_release) != AAR_OK)
                    return 0;
                h->stats.bad_buf++;
                sq_pop(h);
                kick_send(h); /* last: it may revoke the claim */
                continue;
            }
            req->ptr = APTR_OF(p);
        }
        out->op = r.op;
        out->id = r.id;
        out->buf = r.buf;
        out->buf_len = r.buf_len;
        sq_pop(h);
        h->stats.requests++;
        return 1;
    }
}

int aar_core_service_next(aar_core *c, AARHandle *h, AARServiceReq *req)
{
    AARServiceReq local;
    uint32_t n;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!req || req->size < 4u) {
        leave(h);
        return AAR_E_INVAL;
    }
    rc = claim_live(h);
    if (rc == AAR_OK) {
        /* A pending doorbell and a mouse event the CQ had no room for. */
        (void)mouse_post(h);
        kick_send(h);
        rc = sq_pump(h, &local);
        if (rc == 1) {
            n = req->size < sizeof(local) ? req->size : (uint32_t)sizeof(local);
            local.size = req->size;
            memcpy(req, &local, n);
            req->size = local.size;
            rc = AAR_OK;
        } else if (rc == 0) {
            rc = AAR_E_BUSY;
        }
    }
    leave(h);
    return rc;
}

static int audio_attach(AARHandle *h, uint32_t token, uint32_t bytes, uint32_t frames)
{
    volatile uint8_t *ring;
    uint32_t channels;

    if (frames == 0u || (frames & (frames - 1u)) != 0u || bytes < AAR_AUDIO_RING_HDR_BYTES)
        return AAR_E_INVAL;
    if (map_range(h, token, 0u, bytes, &ring, NULL) != AAR_OK)
        return AAR_E_IO;
    channels = le32(ring + AAR_AUDIO_RING_CHANNELS);
    if (le32(ring + AAR_AUDIO_RING_FORMAT) != AAR_AUDIO_FORMAT_S16LE || channels == 0u ||
        channels > 8u || (bytes - AAR_AUDIO_RING_HDR_BYTES) / (channels * 2u) < frames)
        return AAR_E_IO;
    h->audio = ring;
    h->audio_frames = frames;
    h->audio_channels = channels;
    return AAR_OK;
}

static void audio_detach(AARHandle *h)
{
    h->audio = NULL;
    h->audio_frames = 0u;
    h->audio_channels = 0u;
}

int aar_core_service_reply(aar_core *c, AARHandle *h, const AARServiceReq *req)
{
    AARServiceReq local;
    aar_completion comp;
    aar_outstanding *out = NULL;
    uint32_t i, n;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!req || req->size < 4u) {
        leave(h);
        return AAR_E_INVAL;
    }
    rc = claim_live(h);
    if (rc != AAR_OK) {
        leave(h);
        return rc;
    }
    /* Fields the caller's structure does not reach are zero. */
    memset(&local, 0, sizeof(local));
    n = req->size < sizeof(local) ? req->size : (uint32_t)sizeof(local);
    memcpy(&local, req, n);
    for (i = 0u; i < AAR_CORE_OUTSTANDING; i++) {
        if (h->out[i].op != 0u && h->out[i].op == local.op && h->out[i].id == local.id) {
            out = &h->out[i];
            break;
        }
    }
    if (!out) {
        leave(h);
        return AAR_E_INVAL;
    }
    if (local.op == AAR_OP_AUDIO_OPEN && local.result == AAR_OK) {
        /* The ring is attached before the app is told it is open. A ring that
         * does not check out turns the completion into a refusal. */
        rc = audio_attach(h, out->buf, out->buf_len, local.out[AAR_R_AUDIO_OPEN_FRAMES]);
        if (claim_live(h) != AAR_OK) { /* its map found the claim gone */
            leave(h);
            return AAR_E_STALE;
        }
        if (rc != AAR_OK) {
            local.result = rc;
            memset(local.out, 0, sizeof(local.out));
        }
    } else {
        rc = AAR_OK;
    }
    comp.op = (uint16_t)local.op;
    comp.flags = (uint16_t)local.reply_flags;
    comp.id = local.id;
    comp.result = local.result;
    for (i = 0u; i < AAR_ENTRY_OUTS; i++)
        comp.out[i] = local.out[i];
    {
        int prc = aar_queue_post_completion(&h->cq, &comp, ops_of(h)->barrier_acquire,
                                            ops_of(h)->barrier_release);

        if (prc != AAR_OK) {
            /* Not delivered: the ring is attached only with the completion that
             * tells the app it is open. The retry attaches it again. */
            if (local.op == AAR_OP_AUDIO_OPEN)
                audio_detach(h);
            if (prc == AAR_E_BUSY) {
                h->stats.reply_busy++;
                cb_add(h, AAR_CB_CQ_FULL, 1u);
            }
            leave(h);
            return prc;
        }
    }
    out->op = 0u;
    h->stats.replies++;
    if (local.op == AAR_OP_AUDIO_CLOSE)
        audio_detach(h);
    kick(h, AAR_PLAT_KICK_CQ);
    leave(h);
    return rc;
}

int aar_core_send_message(aar_core *c, AARHandle *h, const AARMessage *message)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!message) {
        leave(h);
        return AAR_E_INVAL;
    }
    rc = claim_live(h);
    if (rc == AAR_OK) {
        rc = aar_cb_post_msg(&h->cq, h->cb, &h->msg_posted, message->bytes,
                             ops_of(h)->barrier_acquire, ops_of(h)->barrier_release);
        if (rc == AAR_OK) {
            h->stats.msgs_sent++;
            /* An app parked in a message wait is woken by the platform's
             * service pass: ring for it. */
            kick(h, AAR_PLAT_KICK_CQ);
        }
    }
    leave(h);
    return rc;
}

int aar_core_get_message(aar_core *c, AARHandle *h, AARMessage *message)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!message) {
        leave(h);
        return AAR_E_INVAL;
    }
    if (claim_live(h) == AAR_OK) {
        rc = sq_pump(h, NULL);
        if (rc < 0 && h->inbox_tail == h->inbox_head) {
            leave(h);
            return rc;
        }
    }
    if (h->inbox_tail == h->inbox_head) {
        leave(h);
        return claim_live(h) == AAR_OK ? AAR_E_BUSY : AAR_E_STALE;
    }
    memcpy(message->bytes, h->inbox[h->inbox_head % AAR_CORE_INBOX], AAR_MSG_BYTES);
    h->inbox_head++;
    leave(h);
    return AAR_OK;
}

ULONG aar_core_message_signal(aar_core *c, AARHandle *h)
{
    ULONG mask = 0u;

    irq_off(c);
    if (registry_find(c, h) && h->state == AAR_H_LIVE) {
        h->armed = 1u;
        mask = h->signal_mask;
    }
    irq_on(c);
    return mask;
}

/* ---- Shared maps ----------------------------------------------------------------------------------- */

int aar_core_shared_map(aar_core *c, AARHandle *h, ULONG haddr, ULONG bytes,
                        ULONG flags, struct AARSharedMap *map)
{
    struct AARSharedMap local;
    volatile uint8_t *p;
    uint32_t mapf = 0u, n;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!map || map->size < 4u || bytes == 0u || flags != 0u || haddr == 0u) {
        leave(h);
        return AAR_E_INVAL;
    }
    rc = claim_live(h);
    if (rc == AAR_OK)
        rc = map_range(h, haddr, 0u, bytes, &p, &mapf);
    if (rc == AAR_OK) {
        memset(&local, 0, sizeof(local));
        local.size = map->size;
        local.base = APTR_OF(p);
        local.bytes = bytes;
        local.haddr = haddr;
        local.flags = mapf;
        local.granule = c->plat->info->shared_granule;
        local.reserved[0] = h->claim.handle;
        n = map->size < sizeof(local) ? map->size : (uint32_t)sizeof(local);
        memcpy(map, &local, n);
        map->size = local.size;
    }
    leave(h);
    return rc;
}

/* A map object the library filled for this claim: it carries the claim's handle
 * word, so one from another claim or another generation is refused. */
static int map_check(const AARHandle *h, const struct AARSharedMap *m, uint32_t off,
                     uint32_t len)
{
    if (!m || m->size < offsetof(struct AARSharedMap, reserved) + 4u)
        return AAR_E_INVAL;
    if (m->reserved[0] == 0u || m->base == 0)
        return AAR_E_INVAL;
    if (m->reserved[0] != h->claim.handle)
        return AAR_E_STALE;
    if (off > m->bytes || len > m->bytes - off)
        return AAR_E_INVAL;
    return AAR_OK;
}

int aar_core_shared_unmap(aar_core *c, AARHandle *h, struct AARSharedMap *map)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = claim_live(h);
    if (rc == AAR_OK)
        rc = map_check(h, map, 0u, 0u);
    if (rc == AAR_OK) {
        /* Bookkeeping only: the pages stay cache-inhibited and the handle's
         * cache keeps the block. */
        map->base = 0;
        map->reserved[0] = 0u;
    }
    leave(h);
    return rc;
}

static int shared_cache(AARHandle *h, struct AARSharedMap *map, ULONG offset, ULONG length,
                        int publish)
{
    const struct aar_plat_ops *o = ops_of(h);
    uint32_t start, end, g;
    int rc = claim_live(h);

    if (rc == AAR_OK)
        rc = map_check(h, map, offset, length);
    if (rc != AAR_OK || length == 0u)
        return rc;
    if (map->flags & (AAR_MAPF_UNCACHED | AAR_MAPF_COHERENT))
        return AAR_OK;
    start = offset;
    end = offset + length; /* map_check: length <= bytes - offset */
    g = map->granule;
    if (g != 0u && (g & (g - 1u)) == 0u) {
        start &= ~(g - 1u);
        /* A rounded end past the map, or past 4 GiB, is the end of the map. */
        if (!align_up(end, g, &end) || end > map->bytes)
            end = map->bytes;
    }
    if (publish) {
        if (o->cache_publish)
            o->cache_publish(plat_of(h), (const volatile uint8_t *)PTR(map->base) + start, end - start);
    } else if (o->cache_acquire) {
        o->cache_acquire(plat_of(h), (const volatile uint8_t *)PTR(map->base) + start, end - start);
    }
    return AAR_OK;
}

int aar_core_shared_publish(aar_core *c, AARHandle *h, struct AARSharedMap *map,
                            ULONG offset, ULONG length)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = shared_cache(h, map, offset, length, 1);
    leave(h);
    return rc;
}

int aar_core_shared_acquire(aar_core *c, AARHandle *h, struct AARSharedMap *map,
                            ULONG offset, ULONG length)
{
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    rc = shared_cache(h, map, offset, length, 0);
    leave(h);
    return rc;
}

int aar_core_shared_token(aar_core *c, AARHandle *h, const void *ptr, ULONG bytes,
                          ULONG *token)
{
    uint32_t t = 0u;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (!ptr || bytes == 0u || !token) {
        leave(h);
        return AAR_E_INVAL;
    }
    if (!ops_of(h)->token)
        rc = AAR_E_NOSYS;
    else
        rc = ops_of(h)->token(plat_of(h), (const volatile void *)ptr, bytes, &t);
    if (rc == AAR_OK)
        *token = t;
    leave(h);
    return rc;
}

/* ---- Audio ------------------------------------------------------------------------------------------- */

int aar_core_audio_read(aar_core *c, AARHandle *h, void *dst, ULONG frames, ULONG *got)
{
    volatile uint8_t *ring;
    int16_t *out = (int16_t *)dst;
    uint32_t write, read, avail, n, f, ch, at;
    int rc = require_live(c, h);

    if (rc != AAR_OK)
        return rc;
    if (got)
        *got = 0u;
    if (claim_live(h) != AAR_OK || !h->audio || !got || (frames != 0u && !dst)) {
        leave(h);
        return AAR_E_INVAL;
    }
    ring = h->audio;
    write = le32(ring + AAR_AUDIO_RING_WRITE_FRAME);
    read = le32(ring + AAR_AUDIO_RING_READ_FRAME);
    avail = write - read;
    if (avail > h->audio_frames) {
        leave(h);
        return AAR_E_IO;
    }
    if (frames == 0u) {
        /* The available query: how many frames can be read now. Nothing is
         * consumed and no underrun is counted. */
        *got = avail;
        leave(h);
        return AAR_OK;
    }
    n = avail < frames ? avail : frames;
    for (f = 0u; f < n; f++) {
        at = AAR_AUDIO_RING_HDR_BYTES +
             ((read + f) & (h->audio_frames - 1u)) * h->audio_channels * 2u;
        for (ch = 0u; ch < h->audio_channels; ch++)
            out[f * h->audio_channels + ch] = (int16_t)(
                (uint16_t)ring[at + ch * 2u] | ((uint16_t)ring[at + ch * 2u + 1u] << 8));
    }
    put_le32(ring + AAR_AUDIO_RING_READ_FRAME, read + n);
    if (n < frames)
        put_le32(ring + AAR_AUDIO_RING_UNDERRUNS,
                 le32(ring + AAR_AUDIO_RING_UNDERRUNS) + 1u);
    *got = n;
    leave(h);
    return AAR_OK;
}

/* ---- Extensions ---------------------------------------------------------------------------------------- */

const void *aar_core_ext(aar_core *c, ULONG platform_id, ULONG ext_id)
{
    const struct aar_plat_ops *o = c->plat->ops;

    if (platform_id != c->plat->info->platform_id || !o->ext)
        return NULL;
    return o->ext(c->plat, ext_id);
}

LONG aar_core_ext_stats(AARHandle *h, AARZZ9000Stats *out)
{
    aar_core *c = g_ext_core;
    uint32_t n;
    int rc;

    if (!c)
        return AAR_E_NOSYS;
    rc = require_live(c, h);
    if (rc != AAR_OK)
        return rc;
    if (!out || out->size < 4u) {
        leave(h);
        return AAR_E_INVAL;
    }
    n = out->size < sizeof(h->stats) ? out->size : (uint32_t)sizeof(h->stats);
    {
        AARZZ9000Stats local = h->stats;

        local.size = out->size;
        memcpy(out, &local, n);
        out->size = local.size;
    }
    leave(h);
    return AAR_OK;
}

LONG aar_core_ext_lease_pause(AARHandle *h, ULONG ms)
{
    aar_core *c = g_ext_core;
    uint32_t t;
    int rc;

    if (!c)
        return AAR_E_NOSYS;
    rc = require_live(c, h);
    if (rc != AAR_OK)
        return rc;
    if (ms == 0u) {
        h->pause_until = 0u;
    } else {
        t = c->ticks + (ms * c->env.ticks_per_sec + 999u) / 1000u + 1u;
        h->pause_until = t ? t : 1u;
    }
    leave(h);
    return AAR_OK;
}

LONG aar_core_ext_set_log_sink(AARHandle *h, AARZZ9000LogSink sink, APTR user)
{
    aar_core *c = g_ext_core;
    int rc;

    if (!c)
        return AAR_E_NOSYS;
    rc = require_live(c, h);
    if (rc != AAR_OK)
        return rc;
    h->log_sink = sink;
    h->log_user = user;
    leave(h);
    return AAR_OK;
}

/* ---- The dispatcher ------------------------------------------------------------------------------------- */

/*
 * What the owner should hear about, or 0. Reads the control block and the head
 * entry of the SQ; writes only the dispatcher's own scan state. Work that is
 * still pending is signalled again every few ticks, so a lost wake cannot
 * stick; an end is reported once.
 */
static uint32_t notify_scan(AARHandle *h, uint32_t ticks, uint32_t *tail_seen, int *carries_end)
{
    volatile uint8_t *cb = h->cb;
    uint32_t head, tail, end_reason, used;
    int pending, inbox, ended, fire = 0, msg;

    *carries_end = 0;
    if (!h->armed || !cb || h->revoked)
        return 0u;
    tail = aar_get_be32(cb + AAR_CB_SQ_TAIL);
    head = aar_get_be32(cb + AAR_CB_SQ_HEAD);
    end_reason = aar_get_be32(cb + AAR_CB_END_REASON);
    ended = end_reason != AAR_END_NONE;
    if (!ended)
        h->ended_notified = 0u;
    inbox = h->inbox_tail != h->inbox_head;
    pending = tail != head || inbox;
    if (pending && (tail != h->last_tail ||
                    (uint32_t)(ticks - h->last_signal_tick) >= AAR_CORE_NOTIFY_RESIGNAL_TICKS))
        fire = 1;
    *tail_seen = tail;
    if (!pending)
        h->last_tail = tail; /* nothing to tell: just follow the queue */
    if (ended && !h->ended_notified) {
        fire = 1;
        *carries_end = 1;
    }
    if (!fire)
        return 0u;
    used = tail - head;
    msg = inbox;
    if (!msg && used != 0u && used <= h->sq.depth && h->sq.ring)
        msg = aar_get_be16(h->sq.ring + (size_t)(head & (h->sq.depth - 1u)) * AAR_ENTRY_BYTES +
                           AAR_ENTRY_OP) == AAR_OP_MSG;
    if (msg)
        return AAR_NOTIFY_MESSAGE;
    if (ended)
        return (end_reason == AAR_END_USER_FAULT || end_reason == AAR_END_STALL ||
                end_reason == AAR_END_STUCK_CALL || end_reason == AAR_END_LEASE) ?
               AAR_NOTIFY_FAULT : AAR_NOTIFY_COMPLETE;
    return AAR_NOTIFY_REQUEST;
}

/* The owner is not inside a call: the dispatcher may use the handle's
 * doorbell and event state. */
static int bg_take(aar_core *c, AARHandle *h)
{
    int got = 0;

    irq_off(c);
    if (h->state == AAR_H_LIVE && !h->busy) {
        h->bg = 1u;
        got = 1;
    }
    irq_on(c);
    return got;
}

void aar_core_dispatch(aar_core *c, aar_core_notify_fn notify, void *user)
{
    AARHandle *h;
    uint32_t t = c->ticks;

    c->disp_alive = t;
    env_lock(c, 0);
    for (h = c->handles; h; h = h->next) {
        uint32_t type, tail = 0u;
        int carries_end, answer;

        if (h->state != AAR_H_LIVE)
            continue;
        if (bg_take(c, h)) {
            if (claim_live(h) == AAR_OK)
                kick_send(h);
            if (claim_live(h) == AAR_OK) /* the bell may have found the claim gone */
                (void)mouse_post(h);
            if (claim_live(h) == AAR_OK && c->plat->ops->poll)
                c->plat->ops->poll(c->plat, &h->claim);
            h->bg = 0u;
        }
        type = notify_scan(h, t, &tail, &carries_end);
        if (!type)
            continue;
        answer = notify ? notify(user, h, type) : AAR_CORE_NOTIFIED;
        if (answer == AAR_CORE_NOTIFIED || answer == AAR_CORE_NOTIFY_BUSY) {
            /* Work is announced again after a few ticks, and a new entry
             * after the tail moved. */
            h->last_signal_tick = t;
            h->last_tail = tail;
        }
        /* The end is announced only by an event that carries it. */
        if (carries_end && answer == AAR_CORE_NOTIFIED)
            h->ended_notified = 1u;
    }
    env_unlock(c);
}

void aar_core_event_fill(AARHandle *h, uint32_t type)
{
    h->event.type = type;
    h->event.handle = APTR_OF(h);
    memset(&h->event.status, 0, sizeof(h->event.status));
    if (h->cb)
        status_read(h, &h->event.status, (uint32_t)sizeof(h->event.status));
    else
        h->event.status.size = (ULONG)sizeof(h->event.status);
}

uint32_t aar_core_reap(aar_core *c, aar_core_owner_fn owner_state, void *user)
{
    uint32_t n = 0u;

    for (;;) {
        AARHandle *h, *victim = NULL;

        env_lock(c, 0);
        for (h = c->handles; h; h = h->next) {
            /* A handle left closing by an owner that died in AARClose is taken
             * like any other handle of a dead owner: teardown is idempotent. */
            if ((h->state == AAR_H_LIVE || h->state == AAR_H_CLOSING) &&
                owner_state(user, &h->owner) != AAR_OWNER_ALIVE) {
                victim = h;
                break;
            }
        }
        env_unlock(c);
        if (!victim)
            break;
        if (victim->state == AAR_H_LIVE && !take_for_close(c, victim))
            continue;
        teardown(victim);
        victim->state = AAR_H_DEAD;
        n++;
    }
    for (;;) {
        AARHandle *h, *gone = NULL;

        env_lock(c, 0);
        for (h = c->handles; h; h = h->next) {
            if (h->state == AAR_H_DEAD && owner_state(user, &h->owner) == AAR_OWNER_ABSENT) {
                gone = h;
                break;
            }
        }
        env_unlock(c);
        if (!gone)
            break;
        registry_remove(c, gone);
        handle_free(gone);
    }
    return n;
}
