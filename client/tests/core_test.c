/*
 * Host test of the portable core (client/src/aar_core.c) against the reference
 * platform (client/src/refplat.c). The test plays the app side through the
 * refplat_app_* functions and two or more "tasks" through the owner hook of
 * the core environment.
 *
 * Every check names the test it belongs to, so that the mutation run
 * (client/tests/mutants.sh) can require that a deliberately broken core makes
 * exactly that test fail.
 *
 * Build with -no-pie (or -m32): pointers into the reference arena travel
 * through 32-bit APTR fields.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aar_core.h"
#include "../src/refplat.h"

static unsigned long checks, failures;
static const char *cur_test = "";
static int notify_answer = AAR_CORE_NOTIFIED; /* what the notify callback answers */

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

/* ---- The environment ---------------------------------------------------------------- */

#define POOL_SLOT 8192u
#define POOL_SLOTS 64u
static uint8_t pool[POOL_SLOTS][POOL_SLOT] __attribute__((aligned(16)));
static uint8_t pool_used[POOL_SLOTS];
static unsigned long pool_live;
static unsigned long alloc_live_bytes; /* bytes the core has allocated and not given back */
static int fail_alloc;

static void *t_alloc(void *user, uint32_t bytes)
{
    uint32_t i;

    (void)user;
    if (fail_alloc > 0) {
        fail_alloc--;
        return NULL;
    }
    if (bytes > POOL_SLOT)
        return NULL;
    for (i = 0; i < POOL_SLOTS; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            pool_live++;
            alloc_live_bytes += bytes;
            memset(pool[i], 0, POOL_SLOT);
            return pool[i];
        }
    }
    return NULL;
}

static void t_release(void *user, void *p, uint32_t bytes)
{
    uint32_t i;

    (void)user;
    (void)bytes;
    for (i = 0; i < POOL_SLOTS; i++) {
        if ((void *)pool[i] == p) {
            pool_used[i] = 0;
            pool_live--;
            alloc_live_bytes -= bytes;
            memset(pool[i], 0xA5, POOL_SLOT); /* a use after free shows */
            return;
        }
    }
    printf("FAIL [%s] release of memory that is not the pool's\n", cur_test);
    failures++;
}

static struct { int pad; } tasks[4];
static int cur;                  /* the calling task */
static int lock_depth, irq_depth;
static int sharedlock_calls, exclock_calls;

static void t_self(void *user, aar_owner_stamp *s)
{
    (void)user;
    s->id = &tasks[cur];
    s->name = NULL;
    s->number = 0;
    s->kind = 1;
}

static void t_lock(void *user, int exclusive)
{
    (void)user;
    lock_depth++;
    if (exclusive)
        exclock_calls++;
    else
        sharedlock_calls++;
}

static void t_unlock(void *user)
{
    (void)user;
    lock_depth--;
}

static void t_irq_off(void *user)
{
    (void)user;
    irq_depth++;
}

static void t_irq_on(void *user)
{
    (void)user;
    irq_depth--;
}

static aar_core core;
static struct aar_core_env env;

/* What the platform saw when it released a claim: nothing may be held. */
static int rel_lock_depth, rel_irq_depth, rel_in_lease_list;
static uint32_t rel_lease_before, rel_lease_after;
static int rel_check_lease;

static void on_release(void *user, uint32_t handle)
{
    int slot = refplat_slot(handle);
    AARHandle *h;
    int i;

    (void)user;
    rel_lock_depth = lock_depth;
    rel_irq_depth = irq_depth;
    rel_in_lease_list = 0;
    for (h = core.handles; h; h = h->next) {
        if (h->claim.handle == handle && h->lease_slot != 0u)
            rel_in_lease_list = 1;
    }
    if (rel_check_lease && slot >= 0) {
        /* The interrupt keeps running while the platform releases. */
        rel_lease_before = aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER);
        core.disp_alive = core.ticks;
        for (i = 0; i < 20; i++)
            aar_core_vblank(&core);
        rel_lease_after = aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER);
    }
}

static void setup(const char *name)
{
    cur_test = name;
    printf("== %s\n", name); /* the mutation run names a crashing test by this line */
    fflush(stdout);
    refplat_reset();
    memset(pool_used, 0, sizeof(pool_used));
    pool_live = 0;
    alloc_live_bytes = 0;
    fail_alloc = 0;
    cur = 0;
    notify_answer = AAR_CORE_NOTIFIED;
    lock_depth = irq_depth = 0;
    rel_check_lease = 0;
    memset(&env, 0, sizeof(env));
    env.size = sizeof(env);
    env.alloc = t_alloc;
    env.release = t_release;
    env.self = t_self;
    env.lock = t_lock;
    env.unlock = t_unlock;
    env.irq_off = t_irq_off;
    env.irq_on = t_irq_on;
    env.ticks_per_sec = 50;
    refplat.on_release = on_release;
    CHECK(aar_core_init(&core, aar_plat_get(), &env) == AAR_OK);
    CHECK(aar_core_attach(&core) == AAR_OK);
    CHECK(((uint64_t)(uintptr_t)refplat_ptr(REFPLAT_TOKEN_BASE) >> 32) == 0); /* link with -no-pie */
}

static void finish(void)
{
    aar_core_shutdown(&core);
    aar_core_detach(&core);
    CHECK(refplat_live() == 0);
    CHECK(pool_live == 0);
    CHECK(alloc_live_bytes == 0);
    CHECK(lock_depth == 0 && irq_depth == 0);
}

static AARHandle *open1(uint32_t mask)
{
    AARHandle *h = NULL;
    ULONG reason = 99;
    int rc = aar_core_open(&core, mask, NULL, NULL, &h, &reason);

    CHECK(rc == AAR_OK);
    CHECK(reason == AAR_REASON_NONE);
    return h;
}

static int slot_of(AARHandle *h)
{
    return refplat_slot(h->claim.handle);
}

static uint32_t cb_word(AARHandle *h, uint32_t off)
{
    return aar_get_be32(refplat_cb(slot_of(h)) + off);
}

static void put_le(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void dispatch_once(void)
{
    aar_core_dispatch(&core, NULL, NULL);
}

/* The claims on the lease lists. */
static int lease_listed(void)
{
    uint32_t i;
    int n = 0;

    for (i = 0; i < AAR_CORE_LEASE_PERIOD_TICKS; i++)
        n += (int)core.lease_count[i];
    return n;
}

/* ---- Tests ------------------------------------------------------------------------------ */

static void test_open_close_many(void)
{
    AARHandle *hs[REFPLAT_CLAIMS], *a[3], *b[3], *x = (AARHandle *)(uintptr_t)1;
    ULONG reason = 0;
    unsigned i;
    int rc;

    setup("open_close_many");
    for (i = 0; i < REFPLAT_CLAIMS; i++) {
        hs[i] = open1(0x10);
        CHECK(hs[i] != NULL);
    }
    CHECK(refplat_live() == REFPLAT_CLAIMS);
    rc = aar_core_open(&core, 0x10, NULL, NULL, &x, &reason);
    CHECK(rc == AAR_E_NORESOURCE);
    CHECK(reason == AAR_REASON_TABLES);
    CHECK(x == NULL);
    for (i = 0; i < REFPLAT_CLAIMS; i++)
        CHECK(aar_core_close(&core, hs[i]) == AAR_OK);
    CHECK(refplat_live() == 0);
    CHECK(pool_live == 0);
    /* Open and close again and again: the slots come back. */
    for (i = 0; i < 40; i++) {
        AARHandle *h = open1(0x10);

        CHECK(h != NULL && aar_core_close(&core, h) == AAR_OK);
    }
    CHECK(refplat_live() == 0 && pool_live == 0);
    /* Two tasks, three handles each. */
    for (i = 0; i < 3; i++) {
        cur = 0;
        a[i] = open1(0x10);
        cur = 1;
        b[i] = open1(0x20);
        CHECK(a[i] && b[i] && a[i]->owner.id == &tasks[0] && b[i]->owner.id == &tasks[1]);
    }
    cur = 1;
    CHECK(aar_core_close_owner(&core) == 3);
    CHECK(refplat_live() == 3);
    cur = 0;
    CHECK(aar_core_check(&core, a[0]) == AAR_OK);
    CHECK(aar_core_close_owner(&core) == 3);
    CHECK(refplat_live() == 0);
    finish();
}

static void test_query(void)
{
    AARInfo info;
    unsigned char raw[sizeof(AARInfo) + 8];
    AARInfo *big = (AARInfo *)(void *)raw;

    setup("query");
    memset(&info, 0xEE, sizeof(info));
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_OK);
    CHECK(info.size == sizeof(info));
    CHECK(info.api_major == AAR_API_MAJOR && info.api_minor == AAR_API_MINOR);
    CHECK(info.platform_id == AAR_PLATFORM_ZZ9000);
    CHECK(info.profiles == (AAR_PROFILEF_A32 | AAR_PROFILEF_A64));
    CHECK(info.max_image_bytes == 0x01000000u && info.max_args == 16 && info.max_arg_bytes == 128);
    CHECK(info.queue_depth == 64 && info.msg_capacity == 63);
    CHECK(info.weight_max == 256 && info.shared_max_mib == 4095);
    CHECK(info.stall_limit_ms == 10000u); /* spec 3.4 minimum */
    CHECK(info.platform[AAR_ZZ9000_INFO_RECORDS_FREE] == REFPLAT_CLAIMS);
    CHECK(info.platform[AAR_ZZ9000_INFO_CAPS] == AAR_ZZ9000_CAP_Z3);
    CHECK(info.reserved0 == 0 && info.reserved1[0] == 0 && info.reserved1[3] == 0);
    {
        AARHandle *h = open1(1);

        aar_core_query(&core, &info);
        CHECK(info.platform[AAR_ZZ9000_INFO_RECORDS_FREE] == REFPLAT_CLAIMS - 1);
        CHECK(aar_core_close(&core, h) == AAR_OK);
    }
    /* A small size fills only that much. */
    memset(&info, 0xEE, sizeof(info));
    info.size = 16;
    CHECK(aar_core_query(&core, &info) == AAR_OK);
    CHECK(info.size == 16 && info.api_major == AAR_API_MAJOR && info.platform_id == AAR_PLATFORM_ZZ9000);
    CHECK(info.profiles == 0xEEEEEEEEu); /* beyond size 16: untouched */
    info.size = 3;
    CHECK(aar_core_query(&core, &info) == AAR_E_INVAL);
    CHECK(aar_core_query(&core, NULL) == AAR_E_INVAL);
    /* A larger caller structure: the library fills its own size and leaves the rest. */
    memset(raw, 0xEE, sizeof(raw));
    big->size = sizeof(AARInfo) + 8;
    CHECK(aar_core_query(&core, big) == AAR_OK);
    CHECK(big->size == sizeof(AARInfo) + 8 && raw[sizeof(AARInfo)] == 0xEE);
    /* No platform. */
    aar_core_detach(&core);
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_E_NOSYS);
    {
        AARHandle *h = (AARHandle *)(uintptr_t)1;

        CHECK(aar_core_open(&core, 1, NULL, NULL, &h, NULL) == AAR_E_NOSYS && h == NULL);
    }
    refplat.attach_rc = AAR_E_NOSYS;
    CHECK(aar_core_attach(&core) == AAR_E_NOSYS);
    CHECK(aar_core_query(&core, &info) == AAR_E_NOSYS);
    refplat.attach_rc = AAR_OK;
    CHECK(aar_core_attach(&core) == AAR_OK);
    CHECK(aar_core_query(&core, &info) == AAR_OK);
    finish();
}

static void test_owner_stale(void)
{
    AARHandle *h, *junk = (AARHandle *)(uintptr_t)0x1234;
    struct aar_core_tags t;
    volatile uint8_t *staging;
    uint32_t chunk, token;
    ULONG got = 0, tok = 0;
    AARStatusInfo st;
    AARMessage msg;
    AARServiceReq req;
    struct AARSharedMap map;
    aar_event ev;
    AARZZ9000Stats stats;
    char buf[4];
    const char *argv[1] = { "x" };
    int pass;

    setup("owner_stale");
    cur = 0;
    h = open1(0x10);
    memset(&st, 0, sizeof(st));
    memset(&msg, 0, sizeof(msg));
    memset(&req, 0, sizeof(req));
    memset(&map, 0, sizeof(map));
    memset(&ev, 0, sizeof(ev));
    memset(&stats, 0, sizeof(stats));
    st.size = sizeof(st);
    req.size = sizeof(req);
    map.size = sizeof(map);
    stats.size = sizeof(stats);
    aar_core_tags_init(&t);
    (void)token;
    (void)chunk;
    (void)staging;
    for (pass = 0; pass < 2; pass++) {
        /* pass 0: another task; pass 1: after the close. */
        int expect = pass == 0 ? AAR_E_OWNER : AAR_E_STALE;

        if (pass == 0) {
            cur = 1;
        } else {
            cur = 0;
            CHECK(aar_core_close(&core, h) == AAR_OK);
        }
        CHECK(aar_core_check(&core, h) == expect);
        CHECK(aar_core_close(&core, h) == expect);
        CHECK(aar_core_set_tags(&core, h, &t) == expect);
        CHECK(aar_core_load_begin(&core, h, 100, &staging, &chunk) == expect);
        CHECK(aar_core_load_chunk(&core, h, 0, 10) == expect);
        CHECK(aar_core_load_end(&core, h) == expect);
        CHECK(aar_core_load_memory(&core, h, buf, 4) == expect);
        CHECK(aar_core_start(&core, h, argv, 1) == expect);
        CHECK(aar_core_stop(&core, h) == expect);
        CHECK(aar_core_status(&core, h, &st) == expect);
        CHECK(aar_core_send_message(&core, h, &msg) == expect);
        CHECK(aar_core_get_message(&core, h, &msg) == expect);
        CHECK(aar_core_shared_map(&core, h, 0x100000, 16, 0, &map) == expect);
        CHECK(aar_core_shared_unmap(&core, h, &map) == expect);
        CHECK(aar_core_shared_publish(&core, h, &map, 0, 4) == expect);
        CHECK(aar_core_shared_acquire(&core, h, &map, 0, 4) == expect);
        CHECK(aar_core_shared_token(&core, h, buf, 4, &tok) == expect);
        CHECK(aar_core_service_next(&core, h, &req) == expect);
        CHECK(aar_core_service_reply(&core, h, &req) == expect);
        CHECK(aar_core_post_event(&core, h, &ev) == expect);
        CHECK(aar_core_audio_read(&core, h, buf, 1, &got) == expect);
        CHECK(aar_core_ext_stats(h, &stats) == expect);
        CHECK(aar_core_ext_lease_pause(h, 10) == expect);
        CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == expect);
        if (pass == 0) {
            /* AARMessageSignal is for any task. */
            CHECK(aar_core_message_signal(&core, h) == 0x10);
        } else {
            CHECK(aar_core_message_signal(&core, h) == 0);
        }
    }
    /* A pointer the library never made. */
    CHECK(aar_core_check(&core, junk) == AAR_E_STALE);
    CHECK(aar_core_close(&core, junk) == AAR_E_STALE);
    CHECK(aar_core_message_signal(&core, junk) == 0);
    /* The right task still works, and the owner's other handle was never touched. */
    cur = 0;
    h = open1(0x10);
    CHECK(aar_core_check(&core, h) == AAR_OK);
    finish();
}

static void test_tags(void)
{
    struct aar_core_tags t;
    AARHandle *h;
    volatile uint8_t *staging;
    uint32_t chunk;
    int slot;

    setup("tags");
    h = open1(1);
    slot = slot_of(h);
    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 256) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 0) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 257) == AAR_E_INVAL);
    CHECK(t.weight == 0); /* the bad pair left the set as it was */
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 4095) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 4096) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_SHARED_MIB, 4096) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_AUDIO_ROUTE, 2) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_CLIENT_CAPS, 8) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_BASE + 0, 1) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_BASE + 8, 1) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PROFILE_HZ, AAR_PROFILE_HZ_MAX) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PROFILE_HZ, AAR_PROFILE_HZ_MAX + 1) == AAR_E_INVAL);
    CHECK(t.profile_hz == AAR_PROFILE_HZ_MAX); /* the bad pair left the set as it was */
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PLATFORM_BASE(1) + 1, 1) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, TAG_USER + 5, 1) == AAR_E_INVAL);
    refplat.info.weight_max = 0;
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 1) == AAR_E_NOSYS);
    refplat.info.weight_max = 256;
    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 64) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_SHARED_MIB, 32) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 4) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_LEASE_EXTENDED, 0) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_AUDIO_ROUTE, AAR_AUDIO_ROUTE_AHI) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_CLIENT_CAPS, AAR_CCAP_KNOWN) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PROFILE_HZ, 250) == AAR_OK);
    CHECK(h->opts.lease_extended == 1); /* the default: the long lease */
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_OK);
    CHECK(h->opts.lease_extended == 0 && h->opts.audio_route == AAR_AUDIO_ROUTE_AHI);
    CHECK(h->opts.client_caps == AAR_CCAP_KNOWN);
    /* The limit words reach the control block at load begin. */
    CHECK(cb_word(h, AAR_CB_PRIVATE_MIB) == 0);
    CHECK(aar_core_load_begin(&core, h, 100, &staging, &chunk) == AAR_OK);
    CHECK(cb_word(h, AAR_CB_PRIVATE_MIB) == 64 && cb_word(h, AAR_CB_SHARED_MIB) == 32);
    CHECK(cb_word(h, AAR_CB_WEIGHT) == 4);
    CHECK(cb_word(h, AAR_CB_PROFILE_HZ) == 250);
    CHECK(refplat.claims[slot].opts.client_caps == AAR_CCAP_KNOWN);
    CHECK(refplat.claims[slot].opts.lease_extended == 0);
    CHECK(refplat.claims[slot].opts.audio_route == AAR_AUDIO_ROUTE_AHI);
    /* Once the load began, or the app started, tags are too late. */
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_BUSY);
    finish();
}

static void test_load_start(void)
{
    AARHandle *h;
    static uint8_t image[150000];
    uint32_t i, sum = 0;
    const char *argv[3] = { "prog", "one", "two" };
    const char *many[17];
    char longarg[200];
    AARStatusInfo st;
    int slot;

    setup("load_start");
    for (i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)(i * 7 + 3);
        sum += image[i];
    }
    h = open1(1);
    slot = slot_of(h);
    CHECK(aar_core_start(&core, h, argv, 3) == AAR_E_INVAL); /* nothing loaded */
    CHECK(aar_core_load_memory(&core, h, NULL, 10) == AAR_E_INVAL);
    CHECK(aar_core_load_memory(&core, h, image, 0) == AAR_E_INVAL);
    CHECK(aar_core_load_memory(&core, h, image, 0x01000001u) == AAR_E_INVAL);
    CHECK(refplat.claims[slot].chunks == 0);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(refplat.claims[slot].image_len == sizeof(image));
    CHECK(refplat.claims[slot].image_sum == sum);
    CHECK(refplat.claims[slot].chunks == 3); /* 64 KiB chunks */
    CHECK(refplat.claims[slot].image_first ==
          (((uint32_t)image[0] << 24) | ((uint32_t)image[1] << 16) | ((uint32_t)image[2] << 8) | image[3]));
    CHECK(aar_core_load_memory(&core, h, image, 100) == AAR_E_BUSY); /* loaded twice */
    /* Start: argument rules. */
    for (i = 0; i < 17; i++)
        many[i] = "a";
    memset(longarg, 'x', sizeof(longarg));
    longarg[199] = '\0';
    CHECK(aar_core_start(&core, h, many, 17) == AAR_E_INVAL);
    CHECK(aar_core_start(&core, h, NULL, 1) == AAR_E_INVAL);
    many[0] = longarg;
    CHECK(aar_core_start(&core, h, many, 1) == AAR_E_INVAL);
    longarg[127] = '\0'; /* 127 characters and the NUL fit */
    CHECK(aar_core_start(&core, h, many, 1) == AAR_OK);
    CHECK(refplat.claims[slot].argc == 1 && strlen(refplat.claims[slot].argv[0]) == 31);
    CHECK(aar_core_start(&core, h, argv, 3) == AAR_E_INVAL); /* started already */
    CHECK(aar_core_load_memory(&core, h, image, 100) == AAR_E_BUSY);
    {
        struct aar_core_tags t;

        aar_core_tags_init(&t);
        CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_BUSY);
    }
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.state == AAR_STATE_RUNNING);
    /* A refusal at load end carries its reason into the status. */
    {
        AARHandle *h2 = open1(1);

        refplat.load_end_rc = AAR_E_NORESOURCE;
        refplat.load_end_reason = AAR_REASON_HOSTHEAP;
        CHECK(aar_core_load_memory(&core, h2, image, 5000) == AAR_E_NORESOURCE);
        CHECK(aar_core_status(&core, h2, &st) == AAR_OK && st.reason == AAR_REASON_HOSTHEAP);
        refplat.load_end_rc = AAR_OK;
        CHECK(aar_core_close(&core, h2) == AAR_OK);
    }
    finish();
}

/*
 * The rule: AARStart needs a load that completed. A load the platform failed or
 * that was not finished leaves the handle where start is refused (AAR_E_INVAL)
 * and a new load is refused (AAR_E_BUSY); a load the platform refused to begin
 * leaves the handle as it was.
 */
static void expect_unstartable(AARHandle *h, const char *tag)
{
    static uint8_t image[5000];
    const char *argv[1] = { "prog" };
    AARStatusInfo st;
    struct aar_core_tags t;

    (void)tag;
    aar_core_tags_init(&t);
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_E_INVAL);
    CHECK(aar_core_load_chunk(&core, h, 0, 10) == AAR_E_INVAL);
    CHECK(aar_core_load_end(&core, h) == AAR_E_INVAL);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_BUSY);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_BUSY);
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.state != AAR_STATE_RUNNING);
}

static void test_load_failure(void)
{
    static uint8_t image[5000];
    const char *argv[1] = { "prog" };
    volatile uint8_t *staging;
    uint32_t chunk;
    AARStatusInfo st;
    AARHandle *h;

    setup("load_failure");
    /* 1. The platform fails the end of the load. */
    h = open1(1);
    refplat.load_end_rc = AAR_E_NORESOURCE;
    refplat.load_end_reason = AAR_REASON_HOSTHEAP;
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_NORESOURCE);
    refplat.load_end_rc = AAR_OK;
    expect_unstartable(h, "end");
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 2. The platform fails a chunk. */
    h = open1(1);
    refplat.load_chunk_rc = AAR_E_IO;
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_IO);
    refplat.load_chunk_rc = AAR_OK;
    expect_unstartable(h, "chunk");
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 3. A load that began and was cut short (Ctrl-C between chunks). */
    h = open1(1);
    CHECK(aar_core_load_begin(&core, h, sizeof(image), &staging, &chunk) == AAR_OK);
    CHECK(aar_core_load_chunk(&core, h, 0, 100) == AAR_OK);
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_E_INVAL);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_BUSY);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 4. A platform that begins with a staging area too small to use. */
    h = open1(1);
    refplat.chunk_max = 100;
    CHECK(aar_core_load_begin(&core, h, sizeof(image), &staging, &chunk) == AAR_E_IO);
    refplat.chunk_max = 0;
    expect_unstartable(h, "small chunk");
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 5. A refused begin leaves the handle as it was: a new load may be tried. */
    h = open1(1);
    refplat.load_begin_rc = AAR_E_NORESOURCE;
    refplat.load_begin_reason = AAR_REASON_HOSTHEAP;
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_NORESOURCE);
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.reason == AAR_REASON_HOSTHEAP);
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_E_INVAL);
    refplat.load_begin_rc = AAR_OK;
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_OK);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.state == AAR_STATE_RUNNING);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 6. The platform fails the start: no second start, the image is gone. */
    h = open1(1);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    refplat.start_rc = AAR_E_IO;
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_E_IO);
    refplat.start_rc = AAR_OK;
    expect_unstartable(h, "start");
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 7. A complete load takes no more chunks and no second end. */
    h = open1(1);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(aar_core_load_chunk(&core, h, 0, 10) == AAR_E_INVAL);
    CHECK(aar_core_load_end(&core, h) == AAR_E_INVAL);
    CHECK(aar_core_start(&core, h, argv, 1) == AAR_OK);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    finish();
}

static void test_status(void)
{
    AARHandle *h;
    AARStatusInfo st;
    struct { AARStatusInfo st; uint32_t guard[4]; } g;
    volatile uint8_t *cb;

    setup("status");
    h = open1(1);
    cb = refplat_cb(slot_of(h));
    aar_put_be32(cb + AAR_CB_STATE, AAR_STATE_RUNNING);
    aar_put_be32(cb + AAR_CB_END_REASON, AAR_END_EXIT);
    aar_put_be32(cb + AAR_CB_EXIT_CODE, (uint32_t)-5);
    aar_put_be32(cb + AAR_CB_FLAGS, AAR_CBF_DISPLAY | AAR_CBF_AUDIO);
    aar_put_be32(cb + AAR_CB_PROFILE, AAR_PROFILE_A64);
    aar_put_be32(cb + AAR_CB_FAULT_CLASS, 3);
    aar_put_be64(cb + AAR_CB_FAULT_PC_HI, 0x1122334455667788ull);
    aar_put_be64(cb + AAR_CB_FAULT_SP_HI, 0x0102030405060708ull);
    aar_put_be64(cb + AAR_CB_FAULT_LR_HI, 0xAAull);
    aar_put_be64(cb + AAR_CB_FAULT_ADDR_HI, 0xBBull);
    aar_put_be64(cb + AAR_CB_LOAD_BASE_HI, 0x80000000ull);
    aar_put_be32(cb + AAR_CB_LEASE_PERIOD_MS, 2000);
    aar_put_be32(cb + AAR_CB_MSG_CONSUMED, 9);
    aar_put_be64(cb + AAR_CB_PROGRESS_HI, 0x100000001ull);
    aar_put_be64(cb + AAR_CB_SHARED_USED_HI, 4096);
    aar_put_be64(cb + AAR_CB_PRIVATE_COMMITTED_HI, 0x2000000ull);
    aar_put_be32(cb + AAR_CB_SQ_FULL, 7);
    aar_put_be32(cb + AAR_CB_LOG_DROPPED, 8);
    aar_put_be32(cb + AAR_CB_EVENTS_DROPPED, 9);
    aar_put_be32(cb + AAR_CB_CQ_FULL, 10);
    aar_put_be64(cb + AAR_CB_STACK_SP_HI, 0x0000000200003000ull);
    aar_put_be32(cb + AAR_CB_STACK_BYTES, 128);
    aar_put_be32(cb + AAR_CB_PROFILE_GRANTED_HZ, 200);
    aar_put_be32(cb + AAR_CB_PROFILE_DROPPED, 11);
    aar_put_be32(cb + AAR_CB_PROFILE_HEAD, 6);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.size == sizeof(st));
    CHECK(st.state == AAR_STATE_RUNNING && st.end_reason == AAR_END_EXIT);
    CHECK(st.generation == aar_handle_generation(h->claim.handle));
    CHECK(st.exit_code == -5 && st.flags == (AAR_CBF_DISPLAY | AAR_CBF_AUDIO));
    CHECK(st.profile == AAR_PROFILE_A64 && st.fault_class == 3);
    CHECK(st.fault_pc.hi == 0x11223344u && st.fault_pc.lo == 0x55667788u);
    CHECK(st.fault_sp.hi == 0x01020304u && st.fault_sp.lo == 0x05060708u);
    CHECK(st.fault_lr.lo == 0xAA && st.fault_addr.lo == 0xBB && st.load_base.lo == 0x80000000u);
    CHECK(st.lease_period_ms == 2000 && st.msg_consumed == 9 && st.reserved == 0);
    CHECK(st.progress.hi == 1 && st.progress.lo == 1);
    CHECK(st.shared_used.lo == 4096 && st.private_committed.lo == 0x2000000u);
    CHECK(st.sq_full == 7 && st.log_dropped == 8 && st.events_dropped == 9 && st.cq_full == 10);
    CHECK(st.stack_sp.hi == 2 && st.stack_sp.lo == 0x3000 && st.stack_bytes == 128);
    CHECK(st.profile_hz == 200 && st.profile_dropped == 11);
    CHECK(st.profile_samples == 6);
    CHECK(st.reason == AAR_REASON_NONE);
    /* size honouring: only that many bytes change, the rest of what is filled is zero. */
    memset(&g, 0xEE, sizeof(g));
    g.st.size = 16;
    CHECK(aar_core_status(&core, h, &g.st) == AAR_OK);
    CHECK(g.st.size == 16 && g.st.state == AAR_STATE_RUNNING && g.st.end_reason == AAR_END_EXIT);
    CHECK(g.st.generation == aar_handle_generation(h->claim.handle));
    CHECK(g.st.exit_code == (LONG)0xEEEEEEEEu); /* byte 16 and later: untouched */
    g.st.size = 24;                              /* fills what fits */
    CHECK(aar_core_status(&core, h, &g.st) == AAR_OK);
    CHECK(g.st.exit_code == -5 && g.st.flags == (AAR_CBF_DISPLAY | AAR_CBF_AUDIO));
    CHECK(g.st.profile == 0xEEEEEEEEu);
    g.st.size = sizeof(AARStatusInfo) + 16;
    CHECK(aar_core_status(&core, h, &g.st) == AAR_OK);
    CHECK(g.guard[1] == 0xEEEEEEEEu); /* the caller's extra bytes are not ours */
    g.st.size = 2;
    CHECK(aar_core_status(&core, h, &g.st) == AAR_E_INVAL);
    CHECK(aar_core_status(&core, h, NULL) == AAR_E_INVAL);
    finish();
}

static void test_messages(void)
{
    AARHandle *h;
    AARMessage m, r;
    uint8_t entry[AAR_ENTRY_BYTES];
    uint8_t raw[AAR_MSG_BYTES];
    unsigned i;
    int slot, rc;
    uint32_t next_seen = 0, next_sent = 0;

    setup("messages");
    h = open1(1);
    slot = slot_of(h);
    /* To the app: 63 in flight, then no credit until it consumes. */
    for (i = 0; i < 63; i++) {
        memset(&m, (int)i, sizeof(m));
        CHECK(aar_core_send_message(&core, h, &m) == AAR_OK);
    }
    CHECK(aar_core_send_message(&core, h, &m) == AAR_E_BUSY);
    CHECK(h->stats.msgs_sent == 63);
    /* The app takes one off the CQ, but credit is the inbox count, not the queue. */
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    CHECK(aar_get_be16(entry + AAR_ENTRY_OP) == AAR_OP_MSG && entry[AAR_ENTRY_MSG] == 0);
    CHECK(aar_core_send_message(&core, h, &m) == AAR_E_BUSY);
    refplat_app_set_consumed(slot, 1);
    CHECK(aar_core_send_message(&core, h, &m) == AAR_OK);
    CHECK(aar_core_send_message(&core, h, &m) == AAR_E_BUSY);
    CHECK(aar_core_send_message(&core, h, NULL) == AAR_E_INVAL);
    /* From the app: no message is lost or reordered, even through a full inbox. */
    for (i = 0; i < 63; i++) {
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)next_sent;
        raw[1] = (uint8_t)(next_sent >> 8);
        CHECK(refplat_app_message(slot, raw) == AAR_OK);
        next_sent++;
    }
    CHECK(aar_core_get_message(&core, h, &r) == AAR_OK);
    CHECK(r.bytes[0] == (next_seen & 0xFF));
    next_seen++;
    /* 62 are in the inbox. Fill the SQ again past the inbox: 126 in all. */
    for (i = 0; i < 63; i++) {
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)next_sent;
        raw[1] = (uint8_t)(next_sent >> 8);
        rc = refplat_app_message(slot, raw);
        if (rc != AAR_OK)
            break;
        next_sent++;
    }
    CHECK(next_sent >= 120);
    while ((rc = aar_core_get_message(&core, h, &r)) == AAR_OK) {
        CHECK((r.bytes[0] | (r.bytes[1] << 8)) == (int)(next_seen & 0xFFFF));
        next_seen++;
        if (next_seen < next_sent && refplat_app_sq_used(slot) < 63) {
            /* keep the app posting while the client drains */
            memset(raw, 0, sizeof(raw));
            raw[0] = (uint8_t)next_sent;
            raw[1] = (uint8_t)(next_sent >> 8);
            if (next_sent < 140 && refplat_app_message(slot, raw) == AAR_OK)
                next_sent++;
        }
    }
    CHECK(rc == AAR_E_BUSY);
    CHECK(next_seen == next_sent); /* every message arrived once, in order */
    CHECK(h->stats.sq_blocked > 0);  /* the inbox did fill up and block the SQ */
    CHECK(h->stats.msgs_taken == next_seen);
    CHECK(aar_core_get_message(&core, h, NULL) == AAR_E_INVAL);
    finish();
}

static void fill_cq_with_keys(AARHandle *h, int n)
{
    aar_event ev;
    int i;

    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN;
    for (i = 0; i < n; i++) {
        ev.code = (uint16_t)i;
        CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    }
}

static void test_events(void)
{
    AARHandle *h;
    aar_event ev, got;
    uint8_t entry[AAR_ENTRY_BYTES];
    int slot, i;

    setup("events");
    h = open1(1);
    slot = slot_of(h);
    memset(&ev, 0, sizeof(ev));
    /* Mouse motion with room on the CQ is posted at once, one entry per event. */
    ev.type = AAR_EV_MOUSE;
    ev.dx = 3;
    ev.dy = -4;
    ev.time_ms = 10;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    aar_entry_get_event(entry, &got);
    CHECK(got.type == AAR_EV_MOUSE && got.dx == 3 && got.dy == -4 && got.time_ms == 10);
    CHECK(h->stats.events_posted == 1 && h->stats.events_merged == 0);
    /* A full CQ: motion merges into one pending event. */
    fill_cq_with_keys(h, 64);
    CHECK(refplat_app_cq_used(slot) == 64);
    ev.dx = 30000;
    ev.dy = 0;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    ev.dx = 30000;
    ev.dy = -5;
    ev.time_ms = 99;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    ev.dx = 1;
    ev.dy = 0;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->stats.events_merged == 2);
    CHECK(h->stats.events_dropped == 0);
    /* Room appears; the dispatcher pass flushes the pending event. */
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    dispatch_once();
    CHECK(refplat_app_cq_used(slot) == 64);
    for (i = 0; i < 64; i++) {
        CHECK(refplat_app_cq_pop(slot, entry) == 1);
        if (i < 63) {
            aar_entry_get_event(entry, &got);
            CHECK(got.type == AAR_EV_KEY_DOWN && got.code == i + 1);
        } else {
            aar_entry_get_event(entry, &got);
            CHECK(got.type == AAR_EV_MOUSE);
            CHECK(got.dx == 32767); /* saturated, not wrapped */
            CHECK(got.dy == -5 && got.time_ms == 99);
        }
    }
    CHECK(refplat_app_cq_pop(slot, entry) == 0);
    /* Drops are counted, in the stats and in the control block. */
    fill_cq_with_keys(h, 64);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_UP;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_E_BUSY);
    CHECK(h->stats.events_dropped == 1);
    CHECK(cb_word(h, AAR_CB_EVENTS_DROPPED) == 1 && cb_word(h, AAR_CB_CQ_FULL) == 1);
    /* A button change is not folded into a pending motion: with the CQ full it is dropped. */
    ev.type = AAR_EV_MOUSE;
    ev.buttons = 0;
    ev.dx = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK); /* pending */
    ev.buttons = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_E_BUSY);
    CHECK(h->stats.events_dropped == 2);
    CHECK(aar_core_post_event(&core, h, NULL) == AAR_E_INVAL);
    finish();
}

static void test_absolute_mouse_merge(void)
{
    AARHandle *h;
    aar_event ev, got;
    uint8_t entry[AAR_ENTRY_BYTES];
    int slot, i;

    setup("absolute mouse merge");
    h = open1(1);
    slot = slot_of(h);
    memset(&ev, 0, sizeof(ev));
    /* A full CQ: absolute positions replace the pending one, they are not summed. */
    fill_cq_with_keys(h, 64);
    ev.type = AAR_EV_MOUSE;
    ev.buttons = AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT;
    ev.dx = 300;
    ev.dy = 200;
    ev.time_ms = 5;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    ev.dx = 310;
    ev.dy = -7;
    ev.time_ms = 6;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    ev.dx = 20000;
    ev.dy = 20000;
    ev.time_ms = 7;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    ev.dx = 12;
    ev.dy = 34;
    ev.time_ms = 8;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->stats.events_merged == 3 && h->stats.events_dropped == 0);
    /* A button change is not folded into an absolute motion. */
    ev.code = 2;
    ev.buttons = AAR_MOUSE_ABSOLUTE;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_E_BUSY);
    CHECK(h->stats.events_dropped == 1);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    dispatch_once();
    got.type = 0;
    for (i = 0; i < 70 && got.type != AAR_EV_MOUSE; i++) {
        if (refplat_app_cq_pop(slot, entry) != 1)
            break;
        aar_entry_get_event(entry, &got);
    }
    CHECK(got.type == AAR_EV_MOUSE);
    CHECK(got.buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT));
    CHECK(got.dx == 12 && got.dy == 34 && got.time_ms == 8);
    finish();
}

static void test_kick(void)
{
    AARHandle *h;
    aar_event ev;
    int slot;
    uint32_t i;

    setup("kick");
    h = open1(1);
    slot = slot_of(h);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN;
    refplat.kick_busy = 2;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(refplat.claims[slot].kicks == 0 && h->kick_pending == AAR_PLAT_KICK_CQ);
    CHECK(h->stats.kicks_coalesced == 0);
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->stats.kicks_coalesced == 1 && refplat.claims[slot].kicks == 0);
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->stats.kicks_coalesced == 2);
    CHECK(refplat.claims[slot].kicks == 1 && h->kick_pending == 0 && h->stats.kicks == 1);
    CHECK(refplat.claims[slot].kick_bits == AAR_PLAT_KICK_CQ);
    /* A busy bell is retried by the dispatcher pass, with no call of the owner. */
    refplat.kick_busy = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->kick_pending != 0 && refplat.claims[slot].kicks == 1);
    dispatch_once();
    CHECK(h->kick_pending == 0 && refplat.claims[slot].kicks == 2);
    CHECK(refplat.polls >= 1); /* the platform's housekeeping runs each pass */
    /* While the owner is inside a call the dispatcher leaves its state alone. */
    refplat.kick_busy = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    h->busy = 1;
    dispatch_once();
    CHECK(h->kick_pending != 0 && refplat.claims[slot].kicks == 2);
    h->busy = 0;
    dispatch_once();
    CHECK(h->kick_pending == 0 && refplat.claims[slot].kicks == 3);
    /* The app freed room on a full SQ: a doorbell with the SQ bit follows. */
    for (i = 0; i < 64; i++) {
        uint32_t arg[6] = { 0 };

        CHECK(refplat_app_request(slot, AAR_OP_CONSOLE_WRITE, 0, 100 + i, arg, 0, 0, 0) == AAR_OK);
    }
    {
        AARServiceReq req;

        req.size = sizeof(req);
        refplat.claims[slot].kick_bits = 0;
        dispatch_once();
        dispatch_once();
        CHECK(h->in_work == 0); /* idle */
        CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 100);
        CHECK(h->kick_pending & AAR_PLAT_KICK_SQ);
        CHECK(h->in_work == 1); /* the bell is the pass's to send */
        CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 101);
        CHECK(refplat.claims[slot].kick_bits & AAR_PLAT_KICK_SQ);
    }
    finish();
}

static void test_service(void)
{
    AARHandle *h;
    AARServiceReq req, small;
    aar_completion c;
    uint8_t entry[AAR_ENTRY_BYTES];
    uint32_t arg[6] = { 11, 22, 33, 44, 55, 66 };
    uint32_t tok;
    volatile uint8_t *p;
    int slot;
    unsigned i;

    setup("service");
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x2000);
    p = refplat_ptr(tok);
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
    /* A request with a payload buffer: the library maps it and lends the pointer. */
    for (i = 0; i < 16; i++)
        p[0x100 + i] = (uint8_t)(0x40 + i);
    CHECK(refplat_app_request(slot, AAR_OP_FILE_READ, 0x5, 777, arg, tok, 0x100, 16) == AAR_OK);
    memset(&req, 0xEE, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK);
    CHECK(req.size == sizeof(req) && req.op == AAR_OP_FILE_READ && req.id == 777);
    CHECK(req.arg[0] == 11 && req.arg[5] == 66 && req.flags == 5);
    CHECK(req.buf == tok && req.buf_off == 0x100 && req.buf_len == 16);
    CHECK(req.ptr == (APTR)(uintptr_t)(p + 0x100));
    CHECK(req.result == 0 && req.out[0] == 0 && req.out[5] == 0 && req.reply_flags == 0);
    CHECK(req.reserved[0] == 0 && req.reserved[1] == 0);
    CHECK(((volatile uint8_t *)(uintptr_t)req.ptr)[3] == 0x43);
    /* The client writes the answer into the buffer: the app sees it. */
    ((volatile uint8_t *)(uintptr_t)req.ptr)[0] = 0x99;
    CHECK(p[0x100] == 0x99);
    CHECK(h->stats.requests == 1);
    /* Not yet answered: a reply for another id is refused, so is a second one. */
    {
        AARServiceReq other = req;

        other.id = 778;
        CHECK(aar_core_service_reply(&core, h, &other) == AAR_E_INVAL);
        other = req;
        other.op = AAR_OP_FILE_WRITE;
        CHECK(aar_core_service_reply(&core, h, &other) == AAR_E_INVAL);
    }
    req.result = AAR_OK;
    req.out[AAR_R_FILE_COUNT] = 16;
    req.reply_flags = AAR_CFLAG_FLIP_SUBMITTED;
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_E_INVAL); /* answered once only */
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    aar_entry_get_completion(entry, &c);
    CHECK(c.op == AAR_OP_FILE_READ && c.id == 777 && c.result == AAR_OK && c.out[0] == 16);
    CHECK(c.flags == AAR_CFLAG_FLIP_SUBMITTED);
    CHECK(h->stats.replies == 1);
    /* No buffer: ptr is NULL. A smaller structure gets what fits. */
    CHECK(refplat_app_request(slot, AAR_OP_CONSOLE_WRITE, 0, 5, arg, 0, 0, 0) == AAR_OK);
    memset(&small, 0xEE, sizeof(small));
    small.size = 16;
    CHECK(aar_core_service_next(&core, h, &small) == AAR_OK);
    CHECK(small.op == AAR_OP_CONSOLE_WRITE && small.id == 5 && small.arg[0] == 0xEEEEEEEEu);
    memset(&req, 0, sizeof(req));
    req.size = 16; /* a reply built with a small structure: the rest is zero */
    req.op = AAR_OP_CONSOLE_WRITE;
    req.id = 5;
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    aar_entry_get_completion(entry, &c);
    CHECK(c.result == 0 && c.out[0] == 0);
    small.size = 2;
    CHECK(aar_core_service_next(&core, h, &small) == AAR_E_INVAL);
    CHECK(aar_core_service_next(&core, h, NULL) == AAR_E_INVAL);
    CHECK(aar_core_service_reply(&core, h, NULL) == AAR_E_INVAL);
    /* A buffer the platform does not have: refused with AAR_E_INVAL, not returned. */
    CHECK(refplat_app_request(slot, AAR_OP_FILE_READ, 0, 9, arg, 0x10, 0, 8) == AAR_OK);
    CHECK(refplat_app_request(slot, AAR_OP_CONSOLE_WRITE, 0, 10, arg, 0, 0, 0) == AAR_OK);
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 10);
    CHECK(h->stats.bad_buf == 1);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    aar_entry_get_completion(entry, &c);
    CHECK(c.op == AAR_OP_FILE_READ && c.id == 9 && c.result == AAR_E_INVAL);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    /* A full CQ: the reply is BUSY, the request stays outstanding, and a retry works. */
    CHECK(refplat_app_request(slot, AAR_OP_CONSOLE_WRITE, 0, 20, arg, 0, 0, 0) == AAR_OK);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 20);
    fill_cq_with_keys(h, 64);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_E_BUSY);
    CHECK(h->stats.reply_busy == 1 && cb_word(h, AAR_CB_CQ_FULL) == 1);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    /* Requests behind the table's limit wait in the SQ. */
    for (i = 0; i < 70; i++) {
        int rc;

        while (refplat_app_cq_pop(slot, entry))
            ;
        rc = refplat_app_request(slot, AAR_OP_CONSOLE_WRITE, 0, 1000 + i, arg, 0, 0, 0);
        if (rc != AAR_OK)
            break;
        if (i < 64)
            CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 1000 + i);
    }
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY); /* 64 unanswered */
    req.id = 1010;
    req.op = AAR_OP_CONSOLE_WRITE;
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id >= 1064);
    finish();
}

static struct { unsigned calls; unsigned long user; unsigned char data[32]; ULONG len; } sink_seen;

static void sink(APTR user, const void *data, ULONG len)
{
    sink_seen.calls++;
    sink_seen.user = (unsigned long)(uintptr_t)user;
    sink_seen.len = len;
    memcpy(sink_seen.data, data, len < 32 ? len : 32);
}

static void test_logs_and_stats(void)
{
    AARHandle *h;
    AARServiceReq req;
    AARZZ9000Stats stats;
    volatile uint8_t *p;
    uint32_t tok;
    int slot;
    const aar_client_ext_zz9000 *ext;

    setup("logs_and_stats");
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0);
    p = refplat_ptr(tok);
    memcpy((void *)(uintptr_t)(p + 8), "hello log", 9);
    req.size = sizeof(req);
    CHECK(refplat_app_log(slot, tok, 8, 9) == AAR_OK);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
    CHECK(h->stats.logs_unclaimed == 1 && h->stats.logs == 0);
    CHECK(aar_core_set_log_sink(&core, h, sink, (APTR)(uintptr_t)0x77) == AAR_OK);
    CHECK(refplat_app_log(slot, tok, 8, 9) == AAR_OK);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
    CHECK(sink_seen.calls == 1 && sink_seen.len == 9 && sink_seen.user == 0x77);
    CHECK(memcmp(sink_seen.data, "hello log", 9) == 0);
    CHECK(h->stats.logs == 1);
    CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == AAR_OK);
    /* The extension table. */
    ext = (const aar_client_ext_zz9000 *)aar_core_ext(&core, AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL && AAR_CLIENT_EXT_ZZ9000_COVERS(ext, lease_pause));
    CHECK(ext != NULL && ext->size == sizeof(aar_client_ext_zz9000)); /* no log sink field */
    CHECK(aar_core_ext(&core, AAR_PLATFORM_ZZ9000, 99) == NULL);
    CHECK(aar_core_ext(&core, AAR_PLATFORM_NONE, AAR_EXT_ZZ9000_CLIENT) == NULL);
    if (ext) {
        AARZZ9000List list;

        memset(&stats, 0xEE, sizeof(stats));
        stats.size = sizeof(stats);
        CHECK(ext->stats(h, &stats) == AAR_OK);
        CHECK(stats.requests == 0 && stats.logs == 1 && stats.logs_unclaimed == 1);
        CHECK(stats.maps >= 3); /* control block, two rings, a buffer */
        stats.size = 8;
        CHECK(ext->stats(h, &stats) == AAR_OK && stats.size == 8);
        list.size = sizeof(list);
        CHECK(ext->list(0, &list) == 1 && list.handle == h->claim.handle && list.index == 0);
        CHECK(ext->list(list.index + 1u, &list) == 0);
        /* An administrative release is the card's: the handle's next call is stale. */
        CHECK(ext->release_handle(h->claim.handle) == AAR_OK);
        CHECK(ext->release_handle(h->claim.handle) == AAR_E_STALE);
        {
            aar_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = AAR_EV_KEY_DOWN;
            CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK); /* the queues are gone from the card, not yet known */
            CHECK(aar_core_stop(&core, h) == AAR_E_STALE);
            CHECK(aar_core_post_event(&core, h, &ev) == AAR_E_STALE);
            CHECK(aar_core_status(&core, h, (AARStatusInfo *)&stats) == AAR_E_INVAL || 1);
        }
    }
    finish();
}

static void make_ring(int slot, uint32_t off, uint32_t frames, uint32_t channels, uint32_t written)
{
    volatile uint8_t *r = refplat_ptr(refplat_buf_token(slot, off));
    uint32_t f, c;

    memset((void *)(uintptr_t)r, 0, 0x80 + frames * channels * 2);
    put_le(r + AAR_AUDIO_RING_RATE, 48000);
    put_le(r + AAR_AUDIO_RING_CHANNELS, channels);
    put_le(r + AAR_AUDIO_RING_FORMAT, AAR_AUDIO_FORMAT_S16LE);
    for (f = 0; f < written; f++)
        for (c = 0; c < channels; c++) {
            uint32_t v = (uint16_t)(int16_t)(f * 100 + c - 50);

            r[0x80 + ((f % frames) * channels + c) * 2] = (uint8_t)v;
            r[0x80 + ((f % frames) * channels + c) * 2 + 1] = (uint8_t)(v >> 8);
        }
    put_le(r + AAR_AUDIO_RING_WRITE_FRAME, written);
}

static void open_audio(AARHandle *h, int slot, uint32_t off, uint32_t frames, uint32_t ring_bytes,
                       uint32_t reply_frames, int expect_reply_rc, uint32_t *result)
{
    AARServiceReq req;
    uint32_t arg[6] = { 48000, 2, AAR_AUDIO_ROUTE_AHI, 0, 0, 0 };
    uint8_t entry[AAR_ENTRY_BYTES];
    aar_completion c;

    (void)frames;
    CHECK(refplat_app_request(slot, AAR_OP_AUDIO_OPEN, 0, 300, arg, refplat_buf_token(slot, off), 0,
                              ring_bytes) == AAR_OK);
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.op == AAR_OP_AUDIO_OPEN);
    req.result = AAR_OK;
    req.out[AAR_R_AUDIO_OPEN_RATE] = 48000;
    req.out[AAR_R_AUDIO_OPEN_CHANNELS] = 2;
    req.out[AAR_R_AUDIO_OPEN_RING] = req.buf;
    req.out[AAR_R_AUDIO_OPEN_RING_BYTES] = ring_bytes;
    req.out[AAR_R_AUDIO_OPEN_FRAMES] = reply_frames;
    CHECK(aar_core_service_reply(&core, h, &req) == expect_reply_rc);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    aar_entry_get_completion(entry, &c);
    *result = (uint32_t)c.result;
}

/*
 * The ring is attached when the open completion is delivered, not before: a
 * reply that finds the CQ full leaves the stream unattached.
 */
static void test_audio_reply_busy(void)
{
    AARHandle *h;
    AARServiceReq req;
    uint32_t arg[6] = { 48000, 2, AAR_AUDIO_ROUTE_AHI, 0, 0, 0 };
    uint8_t entry[AAR_ENTRY_BYTES];
    int16_t pcm[8];
    ULONG got;
    int slot;

    setup("audio_reply_busy");
    h = open1(1);
    slot = slot_of(h);
    make_ring(slot, 0x4000, 64, 2, 40);
    CHECK(refplat_app_request(slot, AAR_OP_AUDIO_OPEN, 0, 300, arg, refplat_buf_token(slot, 0x4000), 0,
                              0x180) == AAR_OK);
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.op == AAR_OP_AUDIO_OPEN);
    req.result = AAR_OK;
    req.out[AAR_R_AUDIO_OPEN_FRAMES] = 64;
    fill_cq_with_keys(h, 64);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_E_BUSY);
    CHECK(aar_core_audio_read(&core, h, pcm, 4, &got) == AAR_E_INVAL); /* not attached */
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    CHECK(aar_core_audio_read(&core, h, pcm, 4, &got) == AAR_OK && got == 4);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    finish();
}

static void test_audio(void)
{
    AARHandle *h;
    AARServiceReq req;
    int16_t pcm[64 * 2];
    ULONG got;
    int slot, i;
    uint32_t result;
    volatile uint8_t *ring;
    uint8_t entry[AAR_ENTRY_BYTES];

    setup("audio");
    h = open1(1);
    slot = slot_of(h);
    ring = refplat_ptr(refplat_buf_token(slot, 0x4000));
    CHECK(aar_core_audio_read(&core, h, pcm, 4, &got) == AAR_E_INVAL); /* no ring yet */
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_E_INVAL);
    make_ring(slot, 0x4000, 64, 2, 40);
    open_audio(h, slot, 0x4000, 64, 0x180, 64, AAR_OK, &result);
    CHECK((int)result == AAR_OK);
    /* The available query: frames readable now, nothing consumed, no underrun. */
    got = 99;
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_OK);
    CHECK(got == 40);
    CHECK(get_le(ring + AAR_AUDIO_RING_READ_FRAME) == 0 && get_le(ring + AAR_AUDIO_RING_UNDERRUNS) == 0);
    CHECK(aar_core_audio_read(&core, h, pcm, 0, &got) == AAR_OK && got == 40);
    CHECK(aar_core_audio_read(&core, h, pcm, 8, NULL) == AAR_E_INVAL);
    CHECK(aar_core_audio_read(&core, h, NULL, 8, &got) == AAR_E_INVAL);
    /* A real read: samples in 68k order, read_frame advances. */
    CHECK(aar_core_audio_read(&core, h, pcm, 16, &got) == AAR_OK && got == 16);
    for (i = 0; i < 16; i++) {
        CHECK(pcm[i * 2] == (int16_t)(i * 100 - 50));
        CHECK(pcm[i * 2 + 1] == (int16_t)(i * 100 - 49));
    }
    CHECK(get_le(ring + AAR_AUDIO_RING_READ_FRAME) == 16);
    CHECK(get_le(ring + AAR_AUDIO_RING_UNDERRUNS) == 0);
    got = 0;
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_OK && got == 24);
    /* Asking for more than there is: what there is, and one underrun. */
    CHECK(aar_core_audio_read(&core, h, pcm, 64, &got) == AAR_OK && got == 24);
    CHECK(get_le(ring + AAR_AUDIO_RING_READ_FRAME) == 40);
    CHECK(get_le(ring + AAR_AUDIO_RING_UNDERRUNS) == 1);
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_OK && got == 0);
    CHECK(get_le(ring + AAR_AUDIO_RING_UNDERRUNS) == 1);
    /* The ring wraps. */
    put_le(ring + AAR_AUDIO_RING_WRITE_FRAME, 40 + 60);
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_OK && got == 60);
    CHECK(aar_core_audio_read(&core, h, pcm, 60, &got) == AAR_OK && got == 60);
    /* A corrupt index (write behind read by more than the ring) is AAR_E_IO. */
    put_le(ring + AAR_AUDIO_RING_WRITE_FRAME, 5);
    CHECK(aar_core_audio_read(&core, h, pcm, 1, &got) == AAR_E_IO);
    CHECK(aar_core_audio_read(&core, h, NULL, 0, &got) == AAR_E_IO);
    /* AUDIO_CLOSE detaches. */
    req.size = sizeof(req);
    {
        uint32_t arg[6] = { 0 };

        CHECK(refplat_app_request(slot, AAR_OP_AUDIO_CLOSE, 0, 301, arg, 0, 0, 0) == AAR_OK);
    }
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.op == AAR_OP_AUDIO_CLOSE);
    req.result = AAR_OK;
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_OK);
    CHECK(refplat_app_cq_pop(slot, entry) == 1);
    CHECK(aar_core_audio_read(&core, h, pcm, 4, &got) == AAR_E_INVAL);
    /* A ring whose header is wrong: the completion becomes the error, nothing attaches. */
    make_ring(slot, 0x8000, 64, 2, 8);
    put_le(refplat_ptr(refplat_buf_token(slot, 0x8000)) + AAR_AUDIO_RING_FORMAT, 7);
    open_audio(h, slot, 0x8000, 64, 0x180, 64, AAR_E_IO, &result);
    CHECK((int)result == AAR_E_IO);
    CHECK(aar_core_audio_read(&core, h, pcm, 4, &got) == AAR_E_INVAL);
    /* A frame count that is not a power of two, or does not fit. */
    make_ring(slot, 0xC000, 64, 2, 8);
    open_audio(h, slot, 0xC000, 64, 0x180, 48, AAR_E_INVAL, &result);
    CHECK((int)result == AAR_E_INVAL);
    open_audio(h, slot, 0xC000, 64, 0x100, 64, AAR_E_IO, &result);
    CHECK((int)result == AAR_E_IO);
    finish();
}

static void test_shared_map(void)
{
    AARHandle *h, *h2;
    struct AARSharedMap map, map2;
    uint32_t tok;
    ULONG token = 0;
    int slot, maps_before;
    volatile uint8_t *p;

    setup("shared_map");
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x2000);
    p = refplat_ptr(tok);
    memset(&map, 0xEE, sizeof(map));
    map.size = sizeof(map);
    CHECK(aar_core_shared_map(&core, h, tok, 5000, 0, &map) == AAR_OK);
    CHECK(map.size == sizeof(map) && map.base == (APTR)(uintptr_t)p && map.bytes == 5000);
    CHECK(map.haddr == tok && map.flags == AAR_MAPF_UNCACHED && map.granule == 4096);
    CHECK(map.reserved[0] != 0 && map.reserved[1] == 0);
    maps_before = (int)refplat.maps;
    /* A second map of the same block is the cache's: the platform is not asked again. */
    map2.size = sizeof(map2);
    CHECK(aar_core_shared_map(&core, h, tok, 4000, 0, &map2) == AAR_OK);
    CHECK((int)refplat.maps == maps_before && map2.base == map.base);
    /* A bigger range extends the map. */
    CHECK(aar_core_shared_map(&core, h, tok, 0x9000, 0, &map2) == AAR_OK);
    CHECK((int)refplat.maps == maps_before + 1 && map2.bytes == 0x9000);
    /* Argument and range checks. */
    CHECK(aar_core_shared_map(&core, h, tok, 0, 0, &map2) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, tok, 16, 1, &map2) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, 0, 16, 0, &map2) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, 0x1000, 16, 0, &map2) == AAR_E_INVAL);        /* below the aperture */
    CHECK(aar_core_shared_map(&core, h, REFPLAT_TOKEN_BASE + REFPLAT_ARENA_BYTES, 16, 0, &map2) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, tok, 16, 0, NULL) == AAR_E_INVAL);
    refplat.fail_map_token = tok + 0x10000;
    CHECK(aar_core_shared_map(&core, h, tok + 0x10000, 16, 0, &map2) == AAR_E_NOMEM);
    /* Publish and acquire: nothing to do on an uncached map; a range check always. */
    CHECK(aar_core_shared_publish(&core, h, &map, 0, 5000) == AAR_OK);
    CHECK(aar_core_shared_acquire(&core, h, &map, 100, 100) == AAR_OK);
    CHECK(refplat.publishes == 0 && refplat.acquires == 0);
    CHECK(aar_core_shared_publish(&core, h, &map, 4000, 1001) == AAR_E_INVAL);
    CHECK(aar_core_shared_acquire(&core, h, &map, 5001, 0) == AAR_E_INVAL);
    CHECK(aar_core_shared_publish(&core, h, &map, 0xFFFFFFF0u, 0x20) == AAR_E_INVAL);
    CHECK(aar_core_shared_publish(&core, h, &map, 5000, 0) == AAR_OK);
    /* On a cached map the platform is asked, rounded to the granule. */
    refplat.map_mapf = 0;
    CHECK(aar_core_shared_map(&core, h, refplat_buf_token(slot, 0x6000), 20000, 0, &map2) == AAR_OK);
    CHECK(map2.flags == 0);
    CHECK(aar_core_shared_publish(&core, h, &map2, 5000, 100) == AAR_OK);
    CHECK(refplat.publishes == 1 && refplat.last_cache_len == 4096);
    CHECK(refplat.last_cache_ptr == (const volatile void *)(refplat_ptr(refplat_buf_token(slot, 0x6000)) + 4096));
    CHECK(aar_core_shared_acquire(&core, h, &map2, 4000, 200) == AAR_OK);
    CHECK(refplat.acquires == 1 && refplat.last_cache_len == 8192);
    refplat.map_mapf = AAR_MAPF_COHERENT;
    CHECK(aar_core_shared_map(&core, h, refplat_buf_token(slot, 0xA000), 100, 0, &map2) == AAR_OK);
    CHECK(map2.flags == AAR_MAPF_COHERENT);
    CHECK(aar_core_shared_publish(&core, h, &map2, 0, 100) == AAR_OK && refplat.publishes == 1);
    /* Unmap is bookkeeping: once; the cache keeps the block. */
    CHECK(aar_core_shared_unmap(&core, h, &map) == AAR_OK);
    CHECK(map.base == 0 && map.reserved[0] == 0);
    CHECK(aar_core_shared_unmap(&core, h, &map) == AAR_E_INVAL);
    CHECK(aar_core_shared_publish(&core, h, &map, 0, 4) == AAR_E_INVAL);
    CHECK((int)refplat.maps >= maps_before);
    /* A map of another claim is stale for this handle. */
    h2 = open1(1);
    CHECK(aar_core_shared_map(&core, h2, refplat_buf_token(slot_of(h2), 0x2000), 64, 0, &map2) == AAR_OK);
    CHECK(aar_core_shared_publish(&core, h, &map2, 0, 4) == AAR_E_STALE);
    CHECK(aar_core_shared_unmap(&core, h, &map2) == AAR_E_STALE);
    /* A small structure holds no cookie: it cannot be used for the rest. */
    {
        struct AARSharedMap tiny = { 8, 0, 0, 0, 0, 0, { 0, 0 } };

        CHECK(aar_core_shared_map(&core, h, tok, 64, 0, &tiny) == AAR_OK && tiny.size == 8);
        CHECK(aar_core_shared_publish(&core, h, &tiny, 0, 4) == AAR_E_INVAL);
    }
    /* The token of a pointer, and back. */
    CHECK(aar_core_shared_token(&core, h, (const void *)(p + 0x40), 16, &token) == AAR_OK && token == tok + 0x40);
    CHECK(aar_core_shared_token(&core, h, &token, 4, &token) == AAR_E_INVAL);
    CHECK(aar_core_shared_token(&core, h, NULL, 4, &token) == AAR_E_INVAL);
    CHECK(aar_core_shared_token(&core, h, (const void *)p, 0, &token) == AAR_E_INVAL);
    /* A revoked claim answers AAR_E_STALE. */
    refplat_revoke(slot);
    CHECK(aar_core_stop(&core, h) == AAR_E_STALE);
    CHECK(aar_core_shared_map(&core, h, tok, 64, 0, &map2) == AAR_E_STALE);
    CHECK(aar_core_shared_publish(&core, h, &map2, 0, 4) == AAR_E_STALE);
    finish();
}

static uint32_t lease_of(AARHandle *h)
{
    return aar_get_be32(refplat_cb(slot_of(h)) + AAR_CB_LEASE_COUNTER);
}

static void test_lease(void)
{
    AARHandle *hs[5];
    uint32_t before[5], c0;
    unsigned i, t, n, per_tick_max = 0, stores[5] = { 0 }, changed;

    setup("lease");
    for (i = 0; i < 5; i++)
        hs[i] = open1(1);
    for (i = 0; i < 5; i++)
        CHECK(lease_of(hs[i]) == 1);
    dispatch_once();
    /* One store per handle every 5 ticks, spread: never all in the same tick. */
    for (t = 0; t < 25; t++) {
        for (i = 0; i < 5; i++)
            before[i] = lease_of(hs[i]);
        aar_core_vblank(&core);
        n = 0;
        for (i = 0; i < 5; i++) {
            if (lease_of(hs[i]) != before[i]) {
                n++;
                stores[i]++;
            }
        }
        if (n > per_tick_max)
            per_tick_max = n;
    }
    CHECK(per_tick_max == 1);
    for (i = 0; i < 5; i++)
        CHECK(stores[i] == 5);
    /* The counter is never 0, also after it wraps. */
    hs[0]->lease = 0xFFFFFFFFu;
    for (t = 0; t < 5; t++)
        aar_core_vblank(&core);
    CHECK(lease_of(hs[0]) == 1);
    /* The gate: renewals go on while the dispatcher stamped within 250 ticks ... */
    dispatch_once();
    for (t = 0; t < 245; t++)
        aar_core_vblank(&core);
    c0 = lease_of(hs[1]);
    for (t = 0; t < 5; t++)
        aar_core_vblank(&core);
    CHECK(lease_of(hs[1]) != c0);
    /* ... and stop when the dispatcher is silent for longer. */
    for (i = 0; i < 5; i++)
        before[i] = lease_of(hs[i]);
    for (t = 0; t < 60; t++)
        aar_core_vblank(&core);
    changed = 0;
    for (i = 0; i < 5; i++)
        changed += lease_of(hs[i]) != before[i];
    CHECK(changed == 0);
    /* A pass of the dispatcher brings them back. */
    dispatch_once();
    for (t = 0; t < 5; t++)
        aar_core_vblank(&core);
    changed = 0;
    for (i = 0; i < 5; i++)
        changed += lease_of(hs[i]) != before[i];
    CHECK(changed == 5);
    /* A blocked owner (inside a long call, under Forbid) does not stop the renewal. */
    hs[2]->busy = 1;
    c0 = lease_of(hs[2]);
    for (t = 0; t < 100; t++) {
        dispatch_once();
        aar_core_vblank(&core);
    }
    CHECK(lease_of(hs[2]) != c0);
    hs[2]->busy = 0;
    /* The test hook: no renewal for a while, then again. */
    CHECK(aar_core_ext_lease_pause(hs[3], 1000) == AAR_OK);
    c0 = lease_of(hs[3]);
    for (t = 0; t < 45; t++) {
        dispatch_once();
        aar_core_vblank(&core);
    }
    CHECK(lease_of(hs[3]) == c0);
    CHECK(lease_of(hs[4]) != 0);
    for (t = 0; t < 15; t++) {
        dispatch_once();
        aar_core_vblank(&core);
    }
    CHECK(lease_of(hs[3]) != c0);
    CHECK(aar_core_ext_lease_pause(hs[3], 100000) == AAR_OK);
    c0 = lease_of(hs[3]);
    CHECK(aar_core_ext_lease_pause(hs[3], 0) == AAR_OK); /* 0 resumes */
    for (t = 0; t < 5; t++) {
        dispatch_once();
        aar_core_vblank(&core);
    }
    CHECK(lease_of(hs[3]) != c0);
    finish();
}

/* The order of design 3.3: out of the lease list, then the platform release. */
static void test_close_order(void)
{
    AARHandle *h, *h2;

    setup("close_order");
    h = open1(1);
    h2 = open1(1);
    dispatch_once();
    rel_check_lease = 1;
    rel_in_lease_list = 1;
    rel_lock_depth = rel_irq_depth = 7;
    CHECK(aar_core_close(&core, h) == AAR_OK);
    CHECK(rel_in_lease_list == 0); /* unlinked before the release */
    CHECK(rel_lock_depth == 0 && rel_irq_depth == 0); /* nothing held across the platform call */
    CHECK(rel_lease_after == rel_lease_before); /* the interrupt did not store into the claim */
    CHECK(aar_get_be32(refplat_cb(0) + AAR_CB_LEASE_COUNTER) == 0xAAAAAAAAu);
    CHECK(lease_listed() == 1 && h2->lease_slot != 0 && h2->lease_next == NULL && h2->lease_prev == NULL);
    /* The memory of a released claim is never written by the interrupt either. */
    {
        uint32_t w = aar_get_be32(refplat_cb(0) + AAR_CB_LEASE_COUNTER);
        int i;

        for (i = 0; i < 30; i++)
            aar_core_vblank(&core);
        CHECK(aar_get_be32(refplat_cb(0) + AAR_CB_LEASE_COUNTER) == w);
    }
    finish();
}

static int ostate[4];

static int owner_fn(void *user, const aar_owner_stamp *o)
{
    int i;

    (void)user;
    for (i = 0; i < 4; i++) {
        if (o->id == &tasks[i])
            return ostate[i];
    }
    return AAR_OWNER_ABSENT;
}

static int trace_has_stop_then_release(void)
{
    uint32_t i;
    int stop = -1, rel = -1;

    for (i = 0; i < refplat.trace_len; i++) {
        if ((refplat.trace[i] >> 8) == REFPLAT_T_STOP)
            stop = (int)i;
        if ((refplat.trace[i] >> 8) == REFPLAT_T_RELEASE)
            rel = (int)i;
    }
    return stop >= 0 && rel > stop;
}

static void test_reap(void)
{
    AARHandle *a1, *a2, *b1;
    AARStatusInfo st;
    static uint8_t image[64];
    const char *argv[1] = { "x" };

    setup("reap");
    memset(ostate, 0, sizeof(ostate));
    st.size = sizeof(st);
    cur = 0;
    a1 = open1(0x10);
    a2 = open1(0x10);
    cur = 1;
    b1 = open1(0x20);
    cur = 0;
    CHECK(aar_core_load_memory(&core, a1, image, sizeof(image)) == AAR_OK);
    CHECK(aar_core_start(&core, a1, argv, 1) == AAR_OK);
    dispatch_once();
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0);
    CHECK(refplat_live() == 3 && pool_live == 3 + 1); /* three handles and the handle table */
    /* The owner is replaced at its address (or renamed): the reap takes its handles. */
    ostate[0] = AAR_OWNER_CHANGED;
    rel_check_lease = 1;
    rel_in_lease_list = 1;
    rel_lock_depth = rel_irq_depth = 7;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 2);
    CHECK(rel_in_lease_list == 0 && rel_lock_depth == 0 && rel_irq_depth == 0);
    CHECK(rel_lease_after == rel_lease_before);
    CHECK(trace_has_stop_then_release()); /* a running app is stopped, then released */
    rel_check_lease = 0;
    CHECK(refplat_live() == 1);
    CHECK(a1->state == AAR_H_DEAD && a2->state == AAR_H_DEAD);
    CHECK(pool_live == 3 + 1); /* the tombstones stay */
    CHECK(a1->claim.handle == 0 && a1->cb == NULL);
    /* A tombstone answers AAR_E_STALE, to the old owner and to anyone else. */
    CHECK(aar_core_status(&core, a1, &st) == AAR_E_STALE);
    CHECK(aar_core_close(&core, a2) == AAR_E_STALE);
    cur = 2;
    CHECK(aar_core_status(&core, a1, &st) == AAR_E_STALE);
    CHECK(aar_core_message_signal(&core, a1) == 0);
    /* The other owner is untouched. */
    cur = 1;
    CHECK(aar_core_status(&core, b1, &st) == AAR_OK);
    /* The interrupt keeps renewing the live claim and none of the reaped ones. */
    {
        uint32_t before = lease_of(b1);
        int i;

        for (i = 0; i < 10; i++) {
            dispatch_once();
            aar_core_vblank(&core);
        }
        CHECK(lease_of(b1) != before);
    }
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0);
    CHECK(pool_live == 3 + 1);
    /* Once nothing is at the address any more, the next pass frees them. */
    ostate[0] = AAR_OWNER_ABSENT;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0);
    CHECK(pool_live == 1 + 1);
    cur = 1;
    CHECK(aar_core_close_owner(&core) == 1);
    finish();

    /* A task that vanishes: live handles are reaped and freed in one pass. */
    setup("reap");
    memset(ostate, 0, sizeof(ostate));
    cur = 0;
    a1 = open1(1);
    a2 = open1(1);
    ostate[0] = AAR_OWNER_ABSENT;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 2);
    CHECK(refplat_live() == 0 && pool_live == 0);
    (void)a1;
    (void)a2;
    /* A falsely declared owner (a renamed live task) frees its tombstones at its last close. */
    setup("reap");
    memset(ostate, 0, sizeof(ostate));
    cur = 0;
    a1 = open1(1);
    ostate[0] = AAR_OWNER_CHANGED;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 1);
    ostate[0] = AAR_OWNER_ALIVE;
    CHECK(aar_core_check(&core, a1) == AAR_E_STALE);
    CHECK(aar_core_close_owner(&core) == 1);
    CHECK(pool_live == 0);
    finish();
}

/* ---- AARSetLogSink --------------------------------------------------------------------------- */

#define LS_MAX 16
static struct { int which; unsigned long user; ULONG len; char text[4]; } ls_log[LS_MAX];
static unsigned ls_n;
enum { LS_PLAIN, LS_REMOVE, LS_REPLACE, LS_CALLBACK };
static int ls_mode;
static AARHandle *ls_h;
static int ls_nested_rc = 99, ls_nested_busy = -1, ls_event_rc = 99, ls_event_busy = -1;
static unsigned ls_off;

static void ls_note(int which, APTR user, const void *data, ULONG len)
{
    if (ls_n < LS_MAX) {
        ls_log[ls_n].which = which;
        ls_log[ls_n].user = (unsigned long)(uintptr_t)user;
        ls_log[ls_n].len = len;
        memset(ls_log[ls_n].text, 0, sizeof(ls_log[ls_n].text));
        memcpy(ls_log[ls_n].text, data, len < 3 ? len : 3);
    }
    ls_n++;
}

static void ls_sink_b(APTR user, const void *data, ULONG len)
{
    ls_note(2, user, data, len);
}

/* The first sink: records the call, and in some modes calls the library from inside. */
static void ls_sink_a(APTR user, const void *data, ULONG len)
{
    aar_event ev;

    ls_note(1, user, data, len);
    if (ls_mode == LS_REMOVE) {
        ls_nested_rc = aar_core_set_log_sink(&core, ls_h, NULL, 0);
        ls_nested_busy = ls_h->busy;
    } else if (ls_mode == LS_REPLACE) {
        ls_nested_rc = aar_core_set_log_sink(&core, ls_h, ls_sink_b, (APTR)(uintptr_t)0xB2);
        ls_nested_busy = ls_h->busy;
    } else if (ls_mode == LS_CALLBACK) {
        memset(&ev, 0, sizeof(ev));
        ev.type = AAR_EV_KEY_DOWN;
        ls_event_rc = aar_core_post_event(&core, ls_h, &ev);
        ls_event_busy = ls_h->busy;
    }
}

/* One two-byte record from the app, at a new offset of its buffer. */
static void ls_post(int slot, uint32_t tok, const char *text)
{
    volatile uint8_t *p = refplat_ptr(tok);

    memcpy((void *)(uintptr_t)(p + ls_off), text, 2);
    CHECK(refplat_app_log(slot, tok, ls_off, 2) == AAR_OK);
    ls_off += 16;
}

/* The log records are taken while the client asks for requests. */
static void ls_pump(AARHandle *h)
{
    AARServiceReq req;

    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
}

static int ls_is(unsigned i, int which, unsigned long user, const char *text)
{
    return i < LS_MAX && ls_log[i].which == which && ls_log[i].user == user &&
           ls_log[i].len == 2 && memcmp(ls_log[i].text, text, 2) == 0;
}

static void test_log_sink(void)
{
    AARHandle *h, *h2, *junk = (AARHandle *)(uintptr_t)0x1234;
    AARMessage msg;
    uint32_t tok, tok2;
    int slot, slot2;
    unsigned seen;

    setup("log_sink");
    ls_n = 0;
    ls_off = 0;
    ls_mode = LS_PLAIN;
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x1000);

    /* No sink: the records are swallowed, as before. */
    ls_post(slot, tok, "a0");
    ls_post(slot, tok, "a1");
    ls_pump(h);
    CHECK(ls_n == 0 && h->stats.logs == 0 && h->stats.logs_unclaimed == 2);

    /* Set: every record arrives once, in order, with the user pointer. */
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_a, (APTR)(uintptr_t)0xA1) == AAR_OK);
    ls_post(slot, tok, "b0");
    ls_post(slot, tok, "b1");
    ls_post(slot, tok, "b2");
    ls_pump(h);
    CHECK(ls_n == 3);
    CHECK(ls_is(0, 1, 0xA1, "b0") && ls_is(1, 1, 0xA1, "b1") && ls_is(2, 1, 0xA1, "b2"));
    CHECK(h->stats.logs == 3 && h->stats.logs_unclaimed == 2);
    ls_pump(h); /* nothing is delivered a second time */
    CHECK(aar_core_get_message(&core, h, &msg) == AAR_E_BUSY);
    CHECK(ls_n == 3);

    /* Replace: the next record goes to the new sink and its user, not the old. */
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_b, (APTR)(uintptr_t)0xB2) == AAR_OK);
    ls_post(slot, tok, "c0");
    CHECK(aar_core_get_message(&core, h, &msg) == AAR_E_BUSY); /* the other call that takes logs */
    CHECK(ls_n == 4 && ls_is(3, 2, 0xB2, "c0"));
    CHECK(h->stats.logs == 4);
    /* The same sink with another user is a replace too. */
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_b, (APTR)(uintptr_t)0xB3) == AAR_OK);
    ls_post(slot, tok, "c1");
    ls_pump(h);
    CHECK(ls_n == 5 && ls_is(4, 2, 0xB3, "c1"));

    /* Remove: no call reaches either sink; the records are swallowed again. */
    CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == AAR_OK);
    ls_post(slot, tok, "d0");
    ls_post(slot, tok, "d1");
    ls_pump(h);
    CHECK(ls_n == 5 && h->stats.logs == 5 && h->stats.logs_unclaimed == 4);
    CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == AAR_OK); /* removing twice is fine */

    /* Owner first, then live: another task changes nothing, and a stale handle
     * is reported as stale whatever the arguments. */
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_a, (APTR)(uintptr_t)0xA1) == AAR_OK);
    cur = 1;
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_b, (APTR)(uintptr_t)0xB4) == AAR_E_OWNER);
    CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == AAR_E_OWNER);
    CHECK(aar_core_set_log_sink(&core, junk, ls_sink_b, 0) == AAR_E_STALE);
    cur = 0;
    CHECK(aar_core_set_log_sink(&core, junk, NULL, 0) == AAR_E_STALE);
    ls_post(slot, tok, "e0");
    ls_pump(h);
    CHECK(ls_n == 6 && ls_is(5, 1, 0xA1, "e0")); /* still the owner's sink */
    refplat_revoke(slot);
    CHECK(aar_core_stop(&core, h) == AAR_E_STALE); /* the call that learns the claim is gone */
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_b, (APTR)(uintptr_t)0xB5) == AAR_E_STALE);
    CHECK(aar_core_set_log_sink(&core, h, NULL, 0) == AAR_E_STALE);
    CHECK(h->log_sink == ls_sink_a && h->log_user == (APTR)(uintptr_t)0xA1);
    CHECK(aar_core_get_message(&core, h, &msg) == AAR_E_STALE && ls_n == 6);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_b, 0) == AAR_E_STALE); /* closed */
    CHECK(ls_n == 6);

    /* A new handle does not inherit what the closed one had set. */
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x1000);
    ls_off = 0;
    ls_post(slot, tok, "f0");
    ls_pump(h);
    CHECK(ls_n == 6 && h->stats.logs == 0 && h->stats.logs_unclaimed == 1);

    /* The sink may remove itself: it gets that record and no later one. */
    h2 = open1(2);
    slot2 = slot_of(h2);
    tok2 = refplat_buf_token(slot2, 0x1000);
    ls_off = 0;
    ls_h = h2;
    ls_mode = LS_REMOVE;
    CHECK(aar_core_set_log_sink(&core, h2, ls_sink_a, (APTR)(uintptr_t)0xA2) == AAR_OK);
    ls_post(slot2, tok2, "g0");
    ls_post(slot2, tok2, "g1");
    ls_post(slot2, tok2, "g2");
    ls_pump(h2);
    CHECK(ls_n == 7 && ls_is(6, 1, 0xA2, "g0"));
    CHECK(ls_nested_rc == AAR_OK && h2->log_sink == NULL && h2->log_user == 0);
    CHECK(ls_nested_busy >= 1); /* still inside the call after the nested one */
    CHECK(h2->busy == 0);       /* and balanced after it */
    CHECK(h2->stats.logs == 1 && h2->stats.logs_unclaimed == 2);
    ls_post(slot2, tok2, "g3");
    ls_pump(h2);
    CHECK(ls_n == 7 && h2->stats.logs_unclaimed == 3);

    /* It may replace itself: the rest of the queue goes to the new sink. */
    ls_mode = LS_REPLACE;
    ls_nested_rc = 99;
    CHECK(aar_core_set_log_sink(&core, h2, ls_sink_a, (APTR)(uintptr_t)0xA3) == AAR_OK);
    ls_post(slot2, tok2, "h0");
    ls_post(slot2, tok2, "h1");
    ls_pump(h2);
    CHECK(ls_n == 9 && ls_is(7, 1, 0xA3, "h0") && ls_is(8, 2, 0xB2, "h1"));
    CHECK(ls_nested_rc == AAR_OK && ls_nested_busy >= 1 && h2->busy == 0);

    /* It may post an event to the app. */
    ls_mode = LS_CALLBACK;
    CHECK(aar_core_set_log_sink(&core, h2, ls_sink_a, (APTR)(uintptr_t)0xA4) == AAR_OK);
    ls_post(slot2, tok2, "i0");
    ls_pump(h2);
    CHECK(ls_n == 10 && ls_is(9, 1, 0xA4, "i0"));
    CHECK(ls_event_rc == AAR_OK && ls_event_busy >= 1 && h2->busy == 0);
    ls_mode = LS_PLAIN;

    /* Closing forgets the sink: a closed handle's records are never delivered, and
     * the handle that takes its place starts without one. */
    seen = ls_n;
    CHECK(aar_core_set_log_sink(&core, h2, ls_sink_a, (APTR)(uintptr_t)0xA5) == AAR_OK);
    CHECK(aar_core_close(&core, h2) == AAR_OK);
    h2 = open1(2);
    slot2 = slot_of(h2);
    tok2 = refplat_buf_token(slot2, 0x1000);
    ls_off = 0;
    ls_post(slot2, tok2, "j0");
    ls_pump(h2);
    CHECK(ls_n == seen && h2->log_sink == NULL && h2->log_user == 0);
    CHECK(aar_core_close(&core, h2) == AAR_OK);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    finish();

    /* A reaped handle (its owner is gone) keeps no pointer of the caller's. */
    setup("log_sink");
    memset(ostate, 0, sizeof(ostate));
    cur = 0;
    h = open1(1);
    cur = 1;
    h2 = open1(2);
    cur = 0;
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_a, (APTR)(uintptr_t)0xA6) == AAR_OK);
    cur = 1;
    CHECK(aar_core_set_log_sink(&core, h2, ls_sink_b, (APTR)(uintptr_t)0xB6) == AAR_OK);
    ostate[0] = AAR_OWNER_CHANGED;
    ostate[1] = AAR_OWNER_ALIVE;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 1);
    CHECK(h->state == AAR_H_DEAD && h->log_sink == NULL && h->log_user == 0);
    CHECK(h2->state == AAR_H_LIVE && h2->log_sink == ls_sink_b); /* the other owner is untouched */
    cur = 0;
    CHECK(aar_core_set_log_sink(&core, h, ls_sink_a, 0) == AAR_E_STALE); /* a tombstone */
    CHECK(h->log_sink == NULL);
    cur = 1;
    ostate[1] = AAR_OWNER_CHANGED;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 1);
    CHECK(h2->state == AAR_H_DEAD && h2->log_sink == NULL && h2->log_user == 0);
    finish();
}

static struct { AARHandle *h; uint32_t type; } rec[16];
static unsigned nrec;

static int notify_cb(void *user, AARHandle *h, uint32_t type)
{
    (void)user;
    if (nrec < 16) {
        rec[nrec].h = h;
        rec[nrec].type = type;
    }
    nrec++;
    return notify_answer;
}

static void scan(void)
{
    nrec = 0;
    aar_core_dispatch(&core, notify_cb, NULL);
}

static void ticks(unsigned n)
{
    while (n--)
        aar_core_vblank(&core);
}

static void test_notify(void)
{
    AARHandle *h1, *h2, *h3;
    static int port;
    ULONG reason;
    uint8_t raw[AAR_MSG_BYTES] = { 1 };
    uint32_t arg[6] = { 0 };
    AARMessage m;
    int s1, s2;

    setup("notify");
    h1 = open1(1);
    CHECK(aar_core_open(&core, 1, &port, NULL, &h2, &reason) == AAR_OK);
    CHECK(h1->armed == 0 && h2->armed == 1 && h2->notify == &port);
    s1 = slot_of(h1);
    s2 = slot_of(h2);
    scan();
    CHECK(nrec == 0);
    /* An unarmed handle costs nothing: work on it is not reported. */
    CHECK(refplat_app_request(s1, AAR_OP_CONSOLE_WRITE, 0, 1, arg, 0, 0, 0) == AAR_OK);
    scan();
    CHECK(nrec == 0);
    /* ... also when it is on the dispatcher's work list for a doorbell. */
    {
        aar_event key;

        memset(&key, 0, sizeof(key));
        key.type = AAR_EV_KEY_DOWN;
        refplat.kick_busy = 1;
        CHECK(aar_core_post_event(&core, h1, &key) == AAR_OK);
        CHECK(h1->in_work == 1 && h1->armed == 0);
        scan();
        CHECK(nrec == 0 && h1->kick_pending == 0); /* the bell went, nothing was reported */
    }
    CHECK(aar_core_message_signal(&core, h1) == 1 && h1->armed == 1);
    scan();
    CHECK(nrec == 1 && rec[0].h == h1 && rec[0].type == AAR_NOTIFY_REQUEST);
    /* Pending work is signalled again after 5 ticks, not before. */
    scan();
    CHECK(nrec == 0);
    ticks(4);
    scan();
    CHECK(nrec == 0);
    ticks(1);
    scan();
    CHECK(nrec == 1 && rec[0].type == AAR_NOTIFY_REQUEST);
    /* A message takes the type, and a message waiting in the inbox keeps it. */
    CHECK(refplat_app_message(s2, raw) == AAR_OK);
    scan();
    CHECK(nrec == 1 && rec[0].h == h2 && rec[0].type == AAR_NOTIFY_MESSAGE);
    ticks(5);
    scan();
    CHECK(nrec == 2); /* h1 (request still queued) and h2 (message) */
    CHECK(aar_core_get_message(&core, h2, &m) == AAR_OK && m.bytes[0] == 1);
    ticks(5);
    scan();
    CHECK(nrec == 1 && rec[0].h == h1); /* the message is drained: h2 is quiet */
    /* The end of the app is reported once, as complete or as a fault. */
    h3 = open1(1);
    CHECK(aar_core_message_signal(&core, h3) == 1);
    scan();
    CHECK(nrec <= 2); /* h1 may be due again; h3 has nothing to report */
    refplat_app_set_state(slot_of(h3), AAR_STATE_IDLE, AAR_END_EXIT);
    ticks(1);
    scan();
    {
        unsigned k, got = 0;

        for (k = 0; k < nrec && k < 16; k++)
            if (rec[k].h == h3) {
                got++;
                CHECK(rec[k].type == AAR_NOTIFY_COMPLETE);
            }
        CHECK(got == 1);
    }
    ticks(1);
    scan();
    {
        unsigned k;

        for (k = 0; k < nrec && k < 16; k++)
            CHECK(rec[k].h != h3);
    }
    /* Each kind of fault end. */
    {
        static const uint32_t faults[] = { AAR_END_USER_FAULT, AAR_END_STALL, AAR_END_STUCK_CALL,
                                           AAR_END_LEASE };
        static const uint32_t completes[] = { AAR_END_FORCED, AAR_END_PLATFORM_RESET };
        unsigned k, j;

        for (k = 0; k < 4; k++) {
            refplat_app_set_state(slot_of(h3), AAR_STATE_FAULTED, AAR_END_NONE);
            ticks(1);
            scan(); /* the end reason is back to none: re-armed */
            refplat_app_set_state(slot_of(h3), AAR_STATE_FAULTED, faults[k]);
            scan();
            for (j = 0; j < nrec && j < 16; j++)
                if (rec[j].h == h3)
                    CHECK(rec[j].type == AAR_NOTIFY_FAULT);
        }
        for (k = 0; k < 2; k++) {
            refplat_app_set_state(slot_of(h3), AAR_STATE_RUNNING, AAR_END_NONE);
            scan();
            refplat_app_set_state(slot_of(h3), AAR_STATE_IDLE, completes[k]);
            scan();
            for (j = 0; j < nrec && j < 16; j++)
                if (rec[j].h == h3)
                    CHECK(rec[j].type == AAR_NOTIFY_COMPLETE);
        }
    }
    /* The event: type, handle and the status as of now. */
    aar_core_event_fill(h2, AAR_NOTIFY_MESSAGE);
    CHECK(h2->event.type == AAR_NOTIFY_MESSAGE && h2->event.handle == (APTR)(uintptr_t)h2);
    CHECK(h2->event.status.size == sizeof(AARStatusInfo));
    CHECK(h2->event.status.state == AAR_STATE_CLAIMED);
    CHECK(h2->event.status.generation == aar_handle_generation(h2->claim.handle));
    finish();
}

static void test_revoke(void)
{
    AARHandle *h;
    AARMessage m;
    AARStatusInfo st;
    uint8_t raw[AAR_MSG_BYTES] = { 5 };
    int slot, i;
    uint32_t frozen;

    setup("revoke");
    h = open1(1);
    slot = slot_of(h);
    st.size = sizeof(st);
    for (i = 0; i < 3; i++) {
        raw[0] = (uint8_t)(10 + i);
        CHECK(refplat_app_message(slot, raw) == AAR_OK);
    }
    CHECK(aar_core_get_message(&core, h, &m) == AAR_OK && m.bytes[0] == 10); /* 2 wait in the inbox */
    dispatch_once();
    refplat_revoke(slot);
    CHECK(aar_core_stop(&core, h) == AAR_E_STALE); /* the platform's answer revokes the handle */
    CHECK(h->revoked == 1 && h->cb == NULL && lease_listed() == 0);
    frozen = aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER);
    for (i = 0; i < 30; i++)
        aar_core_vblank(&core);
    CHECK(aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER) == frozen); /* no store into a revoked claim */
    CHECK(aar_core_status(&core, h, &st) == AAR_E_STALE);
    CHECK(aar_core_load_begin(&core, h, 10, NULL, NULL) == AAR_E_STALE);
    /* A revoked claim answers AAR_E_STALE before anything else, including a
     * message that was already in the inbox. */
    CHECK(aar_core_get_message(&core, h, &m) == AAR_E_STALE);
    CHECK(aar_core_get_message(&core, h, NULL) == AAR_E_STALE);
    CHECK(aar_core_close(&core, h) == AAR_OK); /* the release is still owed to the platform */
    CHECK(refplat_live() == 0);
    finish();
}

static void test_open_failures(void)
{
    AARHandle *h = (AARHandle *)(uintptr_t)1;
    ULONG reason = 5;

    setup("open_failures");
    fail_alloc = 1;
    CHECK(aar_core_open(&core, 1, NULL, NULL, &h, &reason) == AAR_E_NOMEM);
    CHECK(h == NULL && reason == AAR_REASON_NONE && refplat_live() == 0);
    refplat.bad_magic = 1;
    CHECK(aar_core_open(&core, 1, NULL, NULL, &h, &reason) == AAR_E_IO);
    CHECK(refplat_live() == 0 && pool_live == 0); /* the claim was given back */
    refplat.bad_magic = 0;
    refplat.fail_map_token = REFPLAT_TOKEN_BASE;
    CHECK(aar_core_open(&core, 1, NULL, NULL, &h, &reason) == AAR_E_IO);
    CHECK(refplat_live() == 0 && pool_live == 0);
    refplat.fail_map_token = 0;
    refplat.max_claims = 0;
    refplat.refuse_reason = AAR_REASON_HOSTHEAP;
    CHECK(aar_core_open(&core, 1, NULL, NULL, &h, &reason) == AAR_E_NORESOURCE);
    CHECK(reason == AAR_REASON_HOSTHEAP && h == NULL);
    CHECK(aar_core_open(&core, 1, NULL, NULL, NULL, NULL) == AAR_E_INVAL);
    finish();
}

/*
 * What the core allocates it gives back: over many open and close pairs, with
 * every kind of use in between, the allocator is net zero in bytes (the card
 * test T3 measured AvailMem before and after 500 pairs).
 */
static void test_alloc_balance(void)
{
    static uint8_t image[64];
    const char *argv[1] = { "x" };
    static int port;
    struct AARSharedMap map;
    AARHandle *h;
    unsigned i;
    ULONG reason;
    unsigned long base;
    int slot;

    setup("alloc_balance");
    memset(ostate, 0, sizeof(ostate));
    base = alloc_live_bytes;
    map.size = sizeof(map);
    /* open, load, start, close: the library's own bytes come back. Platform
     * maps of a running app are not Amiga allocations (see the card note). */
    for (i = 0; i < 200; i++) {
        h = NULL;
        CHECK(aar_core_open(&core, 1, NULL, NULL, &h, &reason) == AAR_OK);
        CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
        CHECK(aar_core_start(&core, h, argv, 1) == AAR_OK);
        CHECK(aar_core_close(&core, h) == AAR_OK);
    }
    CHECK(alloc_live_bytes == base && pool_live == 0 && refplat_live() == 0);
    for (i = 0; i < 500; i++) {
        h = NULL;
        CHECK(aar_core_open(&core, 1, i % 5 == 4 ? &port : NULL, NULL, &h, &reason) == AAR_OK);
        slot = slot_of(h);
        switch (i % 5) {
        case 1:
            CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
            CHECK(aar_core_start(&core, h, argv, 1) == AAR_OK);
            break;
        case 2:
            CHECK(aar_core_message_signal(&core, h) == 1);
            dispatch_once();
            break;
        case 3:
            CHECK(aar_core_shared_map(&core, h, refplat_buf_token(slot, 0x2000), 100, 0, &map) == AAR_OK);
            break;
        default:
            break;
        }
        CHECK(aar_core_close(&core, h) == AAR_OK);
    }
    CHECK(alloc_live_bytes == base);
    CHECK(pool_live == 0 && refplat_live() == 0);
    /* The owner leaves without closing: the reap and the owner's last close give back too. */
    for (i = 0; i < 100; i++) {
        cur = 1;
        h = open1(1);
        CHECK(h != NULL);
        ostate[1] = AAR_OWNER_CHANGED;
        CHECK(aar_core_reap(&core, owner_fn, NULL) == 1);
        ostate[1] = AAR_OWNER_ABSENT;
        (void)aar_core_reap(&core, owner_fn, NULL);
        ostate[1] = AAR_OWNER_ALIVE;
        h = open1(1);
        CHECK(aar_core_close_owner(&core) == 1);
    }
    CHECK(alloc_live_bytes == base && pool_live == 0 && refplat_live() == 0);
    finish();
}

/*
 * Size and offset arithmetic. The rule: a sum or a round-up of values that the
 * client, the app or the platform supplies is checked against the room that is
 * left (a > MAX - b) before it is computed, so a huge length cannot wrap to a
 * small one and pass. Each case below is a value that wrapped before.
 */
static void test_size_arithmetic(void)
{
    AARHandle *h;
    struct AARSharedMap map;
    AARServiceReq req;
    uint8_t entry[AAR_ENTRY_BYTES];
    static const ULONG huge[] = { 0xFFFFFFFFu, 0xFFFFFFF0u, 0xFFFFF000u, 0xFFFFE001u };
    uint32_t arg[6] = { 0 };
    struct aar_plat plat = *aar_plat_get();
    struct aar_plat_ops ops = *plat.ops;
    aar_core other;
    uint32_t tok;
    unsigned i;
    ULONG reason = 0;
    AARHandle *g = NULL;
    int slot, rc;

    setup("size_arithmetic");
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x2000);
    /* The block is cached by an ordinary map first: a length that wraps to 0
     * would then be answered from the cache as if it fitted. */
    memset(&map, 0, sizeof(map));
    map.size = sizeof(map);
    CHECK(aar_core_shared_map(&core, h, tok, 0x1000, 0, &map) == AAR_OK && map.bytes == 0x1000);
    for (i = 0; i < sizeof(huge) / sizeof(huge[0]); i++) {
        memset(&map, 0, sizeof(map));
        map.size = sizeof(map);
        CHECK(aar_core_shared_map(&core, h, tok, huge[i], 0, &map) == AAR_E_INVAL);
        CHECK(map.bytes == 0 && map.base == 0); /* nothing advertised */
    }
    /* The same helper maps the buffer of a request: a request whose buffer
     * length wraps is refused to the app, not handed to the server. */
    CHECK(refplat_app_request(slot, AAR_OP_FILE_READ, 0, 7, arg, tok, 0x2000, 0xFFFFF000u) == AAR_OK);
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
    CHECK(h->stats.bad_buf == 1 && h->stats.requests == 0);
    CHECK(refplat_app_cq_pop(slot, entry) == 1 && aar_get_be16(entry + AAR_ENTRY_OP) == AAR_OP_FILE_READ);
    CHECK((int32_t)aar_get_be32(entry + AAR_ENTRY_RESULT) == AAR_E_INVAL);
    /* An offset and a length whose sum wraps. */
    CHECK(refplat_app_request(slot, AAR_OP_FILE_READ, 0, 8, arg, tok, 0xFFFFF000u, 0x2000) == AAR_OK);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY && h->stats.bad_buf == 2);
    /* Cache maintenance on a map object whose granule rounds past 4 GiB: the
     * range is clamped to the map, it does not wrap to nothing. */
    refplat.map_mapf = 0;
    CHECK(aar_core_close(&core, h) == AAR_OK);
    h = open1(1);
    tok = refplat_buf_token(slot_of(h), 0x2000);
    memset(&map, 0, sizeof(map));
    map.size = sizeof(map);
    CHECK(aar_core_shared_map(&core, h, tok, 0x1000, 0, &map) == AAR_OK);
    map.bytes = 0xFFFFF000u; /* the object is the client's to edit */
    map.granule = 0x80000000u;
    map.flags = 0;
    refplat.publishes = 0;
    refplat.last_cache_len = 99;
    CHECK(aar_core_shared_publish(&core, h, &map, 0, 0xFFFFE000u) == AAR_OK);
    CHECK(refplat.publishes == 1 && refplat.last_cache_len == 0xFFFFF000u);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* A platform whose per-claim state is so large that the handle's size
     * wraps is refused, not given a short block. */
    ops.claim_priv_bytes = 0xFFFFFFF8u;
    plat.ops = &ops;
    memset(&other, 0, sizeof(other));
    CHECK(aar_core_init(&other, &plat, &env) == AAR_OK);
    CHECK(aar_core_attach(&other) == AAR_OK);
    rc = aar_core_open(&other, 1, NULL, NULL, &g, &reason);
    CHECK(rc == AAR_E_NOMEM && g == NULL && refplat_live() == 0);
    if (rc == AAR_OK)
        (void)aar_core_close(&other, g);
    aar_core_detach(&other);
    aar_core_init(&core, aar_plat_get(), &env);
    finish();
}

/*
 * A notification counts as done only when L2 answers that it was delivered:
 * the end of the app stays pending until then (a busy port is not delivery),
 * and work is announced again at once when the owner could not be told, but
 * only after a few ticks when it was told and the port was merely busy.
 */
static unsigned count_for(AARHandle *h)
{
    unsigned k, n = 0;

    for (k = 0; k < nrec && k < 16; k++)
        n += rec[k].h == h;
    return n;
}

static void test_notify_answers(void)
{
    AARHandle *h, *w;
    uint8_t raw[AAR_MSG_BYTES] = { 3 };
    uint32_t arg[6] = { 0 };
    int s;

    setup("notify_answers");
    h = open1(1);
    CHECK(aar_core_message_signal(&core, h) == 1);
    s = slot_of(h);
    /* The end: pending through "not delivered" and "port busy", done on delivery. */
    refplat_app_set_state(s, AAR_STATE_IDLE, AAR_END_EXIT);
    notify_answer = 0;
    scan();
    CHECK(count_for(h) == 1 && rec[0].type == AAR_NOTIFY_COMPLETE && h->ended_notified == 0);
    scan();
    CHECK(count_for(h) == 1 && h->ended_notified == 0); /* at once, not after a few ticks */
    notify_answer = AAR_CORE_NOTIFY_BUSY;
    scan();
    CHECK(count_for(h) == 1 && h->ended_notified == 0);
    scan();
    CHECK(count_for(h) == 1 && h->ended_notified == 0); /* the end does not wait for the resignal */
    notify_answer = AAR_CORE_NOTIFIED;
    scan();
    CHECK(count_for(h) == 1 && h->ended_notified == 1);
    scan();
    CHECK(count_for(h) == 0); /* once */
    /* The app starts again and ends again: announced again. */
    refplat_app_set_state(s, AAR_STATE_RUNNING, AAR_END_NONE);
    scan();
    CHECK(h->ended_notified == 0);
    refplat_app_set_state(s, AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    notify_answer = 0;
    scan();
    CHECK(count_for(h) == 1 && rec[0].type == AAR_NOTIFY_FAULT && h->ended_notified == 0);
    notify_answer = AAR_CORE_NOTIFIED;
    scan();
    CHECK(count_for(h) == 1 && h->ended_notified == 1);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* A request waiting: not delivered, announced again on the next pass; told
     * with a busy port, announced again after the resignal ticks. */
    w = open1(1);
    CHECK(aar_core_message_signal(&core, w) == 1);
    s = slot_of(w);
    CHECK(refplat_app_request(s, AAR_OP_CONSOLE_WRITE, 0, 1, arg, 0, 0, 0) == AAR_OK);
    notify_answer = 0;
    scan();
    CHECK(count_for(w) == 1 && rec[0].type == AAR_NOTIFY_REQUEST);
    scan();
    CHECK(count_for(w) == 1);
    notify_answer = AAR_CORE_NOTIFY_BUSY;
    scan();
    CHECK(count_for(w) == 1);
    scan();
    CHECK(count_for(w) == 0);
    ticks(5);
    scan();
    CHECK(count_for(w) == 1);
    /* A message that arrives when the last one was announced is announced as
     * soon as the tail moves, not after the resignal ticks. */
    CHECK(aar_core_close(&core, w) == AAR_OK);
    w = open1(1);
    CHECK(aar_core_message_signal(&core, w) == 1);
    s = slot_of(w);
    notify_answer = AAR_CORE_NOTIFIED;
    CHECK(refplat_app_message(s, raw) == AAR_OK);
    scan();
    CHECK(count_for(w) == 1 && rec[0].type == AAR_NOTIFY_MESSAGE);
    scan();
    CHECK(count_for(w) == 0);
    CHECK(refplat_app_message(s, raw) == AAR_OK);
    scan();
    CHECK(count_for(w) == 1);
    CHECK(aar_core_close(&core, w) == AAR_OK);
    finish();
}

/*
 * The rule: a platform call that answers AAR_E_STALE for a handle revokes it at
 * once (the lease list, the maps, the queue views, the flag), and whatever
 * called the platform stops using the claim's pointers. Each case lets a
 * different call site meet the answer; the lease word of the claim must not be
 * written afterwards (on hardware it may belong to another claim now).
 */
static uint32_t lease_word(AARHandle *h)
{
    return aar_get_be32(refplat_cb(slot_of(h)) + AAR_CB_LEASE_COUNTER);
}

/* Passes the vertical blanks of a minute with a live dispatcher. */
static void run_lease(void)
{
    unsigned i;

    for (i = 0; i < 60; i++) {
        aar_core_dispatch(&core, NULL, NULL);
        aar_core_vblank(&core);
    }
}

static void expect_revoked(AARHandle *h)
{
    uint32_t frozen = lease_word(h);

    CHECK(h->revoked == 1 && h->cb == NULL && h->renew == 0 && h->poll_needed == 0);
    run_lease();
    CHECK(lease_word(h) == frozen);
    CHECK(aar_core_status(&core, h, &(AARStatusInfo){ .size = 16 }) == AAR_E_STALE);
}

static int sink_calls;

static void sink_fn(APTR user, const void *data, ULONG len)
{
    (void)user;
    (void)data;
    (void)len;
    sink_calls++;
}

static void test_stale_platform(void)
{
    AARHandle *h, *live;
    aar_event ev;
    AARServiceReq req;
    AARMessage m;
    uint32_t arg[6] = { 0 };
    uint32_t tok;
    uint32_t before;
    unsigned long polls;
    int slot, i;

    setup("stale_platform");
    live = open1(1);
    before = lease_word(live);
    run_lease();
    CHECK(lease_word(live) != before); /* the loop does renew a live claim */
    /* 1. kick, from a call of the owner (an event posted on a released claim). */
    h = open1(1);
    slot = slot_of(h);
    refplat_revoke(slot);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_JOY;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK); /* the event went; the bell was refused */
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 2. kick, from the dispatcher (a bell that was busy, then the claim went). */
    h = open1(1);
    slot = slot_of(h);
    refplat.kick_busy = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK && h->kick_pending != 0);
    refplat_revoke(slot);
    polls = refplat.polls;
    dispatch_once();
    CHECK(refplat.polls == polls); /* no poll: nothing is in flight, and not for the dead one */
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 2b. kick, from the owner, on a claim whose earlier bell is still polled. */
    h = open1(1);
    slot = slot_of(h);
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK && h->poll_needed == 1);
    refplat_revoke(slot);
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    expect_revoked(h); /* nothing is left to poll */
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 3. kick, at the start of a service call: it goes no further. */
    h = open1(1);
    slot = slot_of(h);
    refplat.kick_busy = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK && h->kick_pending != 0);
    refplat_revoke(slot);
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_STALE);
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 4. kick, after a message sent and after a reply. */
    h = open1(1);
    refplat_revoke(slot_of(h));
    memset(&m, 0, sizeof(m));
    CHECK(aar_core_send_message(&core, h, &m) == AAR_OK);
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 5. the stop of a handle whose event answered stale: no platform stop follows. */
    h = open1(1);
    refplat_revoke(slot_of(h));
    refplat.stale_calls = 0;
    CHECK(aar_core_stop(&core, h) == AAR_E_STALE);
    CHECK(refplat.stale_calls == 1); /* the bell found it; the stop did not go on to ask again */
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 6. map, for the buffer of a request: refused to the app? no: the claim is
     * gone, nothing is posted and nothing is read from its queues. */
    h = open1(1);
    slot = slot_of(h);
    tok = refplat_buf_token(slot, 0x4000);
    refplat.stale_map_token = tok;
    CHECK(refplat_app_request(slot, AAR_OP_FILE_READ, 0, 5, arg, tok, 0, 100) == AAR_OK);
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_STALE);
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 7. map, for a log record. */
    h = open1(1);
    slot = slot_of(h);
    h->log_sink = sink_fn;
    refplat.stale_map_token = tok;
    CHECK(refplat_app_log(slot, tok, 0, 8) == AAR_OK);
    CHECK(aar_core_get_message(&core, h, &m) == AAR_E_STALE && sink_calls == 0);
    expect_revoked(h);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* 8. map, for the ring of an audio stream. */
    h = open1(1);
    slot = slot_of(h);
    refplat.stale_map_token = 0;
    tok = refplat_buf_token(slot, 0x2000);
    CHECK(refplat_app_request(slot, AAR_OP_AUDIO_OPEN, 0, 9, arg, tok, 0, 0x1000) == AAR_OK);
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_OK);
    for (i = 0; i < (int)AAR_CORE_OUTSTANDING; i++)
        if (h->out[i].op == AAR_OP_AUDIO_OPEN)
            h->out[i].buf_len = 0x4000; /* more than is mapped: the platform is asked */
    refplat.stale_map_token = tok;
    req.result = AAR_OK;
    req.out[AAR_R_AUDIO_OPEN_FRAMES] = 64;
    CHECK(aar_core_service_reply(&core, h, &req) == AAR_E_STALE);
    expect_revoked(h);
    CHECK(refplat_app_cq_used(slot) == 0); /* nothing was posted on the dead claim */
    refplat.stale_map_token = 0;
    CHECK(aar_core_close(&core, h) == AAR_OK);
    CHECK(aar_core_close(&core, live) == AAR_OK);
    finish();
}

/*
 * The work list of the dispatcher: a handle is on it while it has something for
 * the pass (a doorbell to send or retire, a mouse event that waits, a scan to
 * make), and on nothing else.
 */
static int work_count(void)
{
    AARHandle *h;
    int n = 0;

    for (h = core.work_head; h && n < 64; h = h->work_next) {
        CHECK(h->in_work == 1);
        CHECK(h->work_next == NULL || h->work_next->work_prev == h);
        CHECK(h->work_prev != NULL || core.work_head == h);
        n++;
    }
    return n;
}

static void test_work_list(void)
{
    AARHandle *h, *a, *b;
    struct aar_plat_ops noop;
    aar_event ev;
    ULONG reason;
    int port, slot;

    setup("work_list");
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN;
    /* An idle handle is on no list; one with a notify port, or armed by
     * AARMessageSignal, is on it for good. */
    a = open1(1);
    CHECK(a->in_work == 0 && work_count() == 0);
    reason = 0;
    CHECK(aar_core_open(&core, 2, &port, NULL, &b, &reason) == AAR_OK);
    CHECK(b->armed == 1 && b->in_work == 1 && work_count() == 1);
    CHECK(aar_core_message_signal(&core, a) == 1u);
    CHECK(a->in_work == 1 && work_count() == 2);
    CHECK(aar_core_message_signal(&core, a) == 1u); /* once more: not twice */
    CHECK(work_count() == 2);
    dispatch_once();
    dispatch_once();
    CHECK(work_count() == 2); /* armed handles stay */
    CHECK(aar_core_close(&core, a) == AAR_OK);
    CHECK(work_count() == 1 && b->in_work == 1);
    CHECK(aar_core_close(&core, b) == AAR_OK);
    CHECK(core.work_head == NULL);
    /* A bell that was busy is the pass's; a bell sent is polled until the platform
     * has retired it; then the handle is off the list. */
    h = open1(1);
    slot = slot_of(h);
    refplat.kick_busy = 1;
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->kick_pending != 0 && h->in_work == 1 && work_count() == 1);
    h->busy = 1; /* the owner is inside a call: the pass leaves it, and keeps it */
    dispatch_once();
    CHECK(h->kick_pending != 0 && h->in_work == 1);
    h->busy = 0;
    refplat.poll_inflight = 1;
    dispatch_once();
    CHECK(h->kick_pending == 0 && refplat.claims[slot].kicks == 1);
    CHECK(h->poll_needed == 1 && h->in_work == 1); /* the platform still holds the request */
    dispatch_once();
    CHECK(h->poll_needed == 0 && h->in_work == 0 && core.work_head == NULL);
    /* A bell sent from the owner's call is polled at the next pass. */
    CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
    CHECK(h->poll_needed == 1 && h->in_work == 1);
    dispatch_once();
    CHECK(h->poll_needed == 0 && h->in_work == 0);
    /* A platform with no poll has nothing to retire. */
    noop = *refplat.plat.ops;
    noop.poll = NULL;
    {
        const struct aar_plat_ops *saved = refplat.plat.ops;

        refplat.plat.ops = &noop;
        CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
        CHECK(h->poll_needed == 0 && h->in_work == 0);
        refplat.kick_busy = 1;
        CHECK(aar_core_post_event(&core, h, &ev) == AAR_OK);
        CHECK(h->kick_pending != 0 && h->in_work == 1);
        dispatch_once();
        CHECK(h->kick_pending == 0 && h->poll_needed == 0 && h->in_work == 0);
        refplat.plat.ops = saved;
    }
    CHECK(aar_core_close(&core, h) == AAR_OK);
    /* Handles with work close in any order: the list stays whole. */
    {
        AARHandle *hs[5];
        int i;

        for (i = 0; i < 5; i++)
            hs[i] = open1(1);
        refplat.kick_busy = 5;
        for (i = 0; i < 5; i++)
            CHECK(aar_core_post_event(&core, hs[i], &ev) == AAR_OK);
        CHECK(work_count() == 5);
        CHECK(aar_core_close(&core, hs[2]) == AAR_OK); /* the middle */
        CHECK(work_count() == 4);
        CHECK(aar_core_close(&core, hs[4]) == AAR_OK); /* the head: the newest was put first */
        CHECK(work_count() == 3);
        CHECK(aar_core_close(&core, hs[0]) == AAR_OK); /* the tail */
        CHECK(work_count() == 2);
        dispatch_once();
        CHECK(work_count() == 0 && core.work_head == NULL);
        CHECK(aar_core_close(&core, hs[1]) == AAR_OK);
        CHECK(aar_core_close(&core, hs[3]) == AAR_OK);
    }
    /* A revoked claim leaves the list; a reaped handle too. */
    a = open1(1);
    cur = 1;
    b = open1(1);
    cur = 0;
    refplat.kick_busy = 2;
    CHECK(aar_core_post_event(&core, a, &ev) == AAR_OK);
    cur = 1;
    CHECK(aar_core_post_event(&core, b, &ev) == AAR_OK);
    cur = 0;
    CHECK(work_count() == 2);
    refplat_revoke(slot_of(a));
    memset(ostate, 0, sizeof(ostate));
    ostate[1] = AAR_OWNER_CHANGED;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 1); /* b's owner is gone */
    CHECK(b->state == AAR_H_DEAD);
    dispatch_once();
    CHECK(a->revoked == 1 && a->in_work == 0 && b->in_work == 0 && core.work_head == NULL);
    CHECK(aar_core_close(&core, a) == AAR_OK);
    ostate[1] = AAR_OWNER_ABSENT;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0);
    finish();
}

/* ---- The stack snapshot and the profiler ------------------------------------------------------- */

static uint64_t be64_at(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void test_stack(void)
{
    AARHandle *h;
    AARStatusInfo st;
    uint8_t data[AAR_CB_STACK_MAX], out[AAR_CB_STACK_MAX + 8];
    ULONG got = 77;
    unsigned i;
    int slot;

    setup("stack");
    h = open1(1);
    slot = slot_of(h);
    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 3 + 1);
    /* No snapshot: the block is zero until the platform writes one. */
    CHECK(aar_core_read_stack(&core, h, out, sizeof(out), &got) == AAR_E_NOENT);
    CHECK(got == 0);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.stack_bytes == 0 && st.stack_sp.hi == 0 && st.stack_sp.lo == 0);
    /* A snapshot of 64 bytes at an app address. */
    refplat_app_set_stack(slot, 0x00000001BFFFF000ull, data, 64);
    memset(out, 0xEE, sizeof(out));
    CHECK(aar_core_read_stack(&core, h, out, sizeof(out), &got) == AAR_OK);
    CHECK(got == 64 && memcmp(out, data, 64) == 0 && out[64] == 0xEE); /* as they lie, no swap */
    memset(out, 0xEE, sizeof(out));
    CHECK(aar_core_read_stack(&core, h, out, 10, &got) == AAR_OK); /* a smaller buffer: a prefix */
    CHECK(got == 10 && memcmp(out, data, 10) == 0 && out[10] == 0xEE);
    CHECK(aar_core_read_stack(&core, h, NULL, 0, &got) == AAR_OK && got == 64); /* the size */
    CHECK(aar_core_read_stack(&core, h, NULL, 4, &got) == AAR_E_INVAL && got == 0);
    CHECK(aar_core_read_stack(&core, h, out, 4, NULL) == AAR_E_INVAL);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.stack_bytes == 64 && st.stack_sp.hi == 1 && st.stack_sp.lo == 0xBFFFF000u);
    /* The largest snapshot. */
    refplat_app_set_stack(slot, 0x1000, data, AAR_CB_STACK_MAX);
    CHECK(aar_core_read_stack(&core, h, out, sizeof(out), &got) == AAR_OK);
    CHECK(got == AAR_CB_STACK_MAX && memcmp(out, data, AAR_CB_STACK_MAX) == 0);
    /* A size above the maximum is a corrupt block. */
    aar_put_be32(refplat_cb(slot) + AAR_CB_STACK_BYTES, AAR_CB_STACK_MAX + 4);
    CHECK(aar_core_read_stack(&core, h, out, sizeof(out), &got) == AAR_E_IO && got == 0);
    /* Owner and stale rules. */
    cur = 1;
    CHECK(aar_core_read_stack(&core, h, out, 4, &got) == AAR_E_OWNER);
    cur = 0;
    CHECK(aar_core_close(&core, h) == AAR_OK);
    CHECK(aar_core_read_stack(&core, h, out, 4, &got) == AAR_E_STALE);
    finish();
}

/* The platform writes more samples while the client copies: from inside the
 * cache maintenance call, which runs between the client's first read of the
 * head and its copy. */
static int hook_slot, hook_more;
static uint64_t hook_pc;

static void hook_push(void *user, const volatile void *ptr, uint32_t len)
{
    (void)user;
    (void)ptr;
    (void)len;
    while (hook_more > 0) {
        refplat_app_profile_push(hook_slot, hook_pc++);
        hook_more--;
    }
}

/* The platform has begun to store the next sample, into the slot of the oldest, and has
 * not published the head. */
static void hook_partial(void *user, const volatile void *ptr, uint32_t len)
{
    (void)user;
    (void)ptr;
    (void)len;
    refplat_app_profile_partial(hook_slot, 0xDEADBEEFu);
}

static AARHandle *profile_open(uint32_t hz)
{
    struct aar_core_tags t;
    AARHandle *h = open1(1);
    volatile uint8_t *staging;
    uint32_t chunk;

    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PROFILE_HZ, hz) == AAR_OK);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_OK);
    CHECK(aar_core_load_begin(&core, h, 100, &staging, &chunk) == AAR_OK);
    return h;
}

static void test_profile(void)
{
    AARHandle *h;
    AARStatusInfo st;
    uint8_t out[80 * 8];
    ULONG got = 77, lost = 77;
    uint64_t pc;
    unsigned i;
    int slot;

    /* A platform without a profiler: the tag is accepted, nothing is granted. */
    setup("profile_none");
    h = profile_open(100);
    slot = slot_of(h);
    CHECK(cb_word(h, AAR_CB_PROFILE_HZ) == 100);
    CHECK(cb_word(h, AAR_CB_PROFILE_GRANTED_HZ) == 0 && cb_word(h, AAR_CB_PROFILE_TOKEN) == 0);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_NOSYS);
    CHECK(got == 0 && lost == 0);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.profile_hz == 0 && st.profile_samples == 0);
    (void)slot;
    finish();

    /* A profiler is there but none was asked for. */
    setup("profile_off");
    refplat.profile_max_hz = 1000;
    h = profile_open(0);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_NOSYS);
    finish();

    /* A platform that grants a lower rate. */
    setup("profile_granted");
    refplat.profile_max_hz = 100;
    h = profile_open(500);
    CHECK(cb_word(h, AAR_CB_PROFILE_GRANTED_HZ) == 100);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.profile_hz == 100);
    finish();

    /* Samples in order, taken in pieces, big-endian 64-bit words. */
    setup("profile_read");
    refplat.profile_max_hz = 1000;
    h = profile_open(100);
    slot = slot_of(h);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_OK && got == 0 && lost == 0);
    for (i = 0; i < 10; i++)
        refplat_app_profile_push(slot, 0x0000000100001000ull + i * 4u);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.profile_hz == 100 && st.profile_samples == 10);
    CHECK(aar_core_read_profile(&core, h, NULL, 0, &got, &lost) == AAR_OK && got == 10); /* the count */
    CHECK(aar_core_read_profile(&core, h, out, 4, &got, &lost) == AAR_OK);
    CHECK(got == 4 && lost == 0);
    for (i = 0; i < 4; i++)
        CHECK(be64_at(out + i * 8) == 0x0000000100001000ull + i * 4u);
    CHECK(out[0] == 0 && out[3] == 1); /* the bytes of the ring, not converted */
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, NULL) == AAR_OK && got == 6);
    for (i = 0; i < 6; i++)
        CHECK(be64_at(out + i * 8) == 0x0000000100001000ull + (i + 4) * 4u);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 0 && lost == 0);
    CHECK(aar_core_read_profile(&core, h, NULL, 4, &got, &lost) == AAR_E_INVAL);
    CHECK(aar_core_read_profile(&core, h, out, 4, NULL, &lost) == AAR_E_INVAL);
    cur = 1;
    CHECK(aar_core_read_profile(&core, h, out, 4, &got, &lost) == AAR_E_OWNER);
    cur = 0;
    finish();

    /* The reader falls behind by more than the ring can give (63 of 64 samples are
     * trusted): the oldest are lost and counted. */
    setup("profile_lost");
    refplat.profile_max_hz = 1000;
    h = profile_open(100);
    slot = slot_of(h);
    for (pc = 0; pc < 210; pc++)
        refplat_app_profile_push(slot, pc);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK);
    CHECK(got == 63 && lost == 147);
    for (i = 0; i < 63; i++)
        CHECK(be64_at(out + i * 8) == 147 + i);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 0 && lost == 0);
    /* A reader that is 63 behind loses nothing. */
    for (pc = 210; pc < 273; pc++)
        refplat_app_profile_push(slot, pc);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 63 && lost == 0);
    CHECK(be64_at(out) == 210);
    /* A buffer smaller than what waits takes the oldest, the rest stays for the next call. */
    for (pc = 273; pc < 300; pc++)
        refplat_app_profile_push(slot, pc);
    CHECK(aar_core_read_profile(&core, h, out, 10, &got, &lost) == AAR_OK && got == 10 && lost == 0);
    CHECK(be64_at(out) == 273 && be64_at(out + 72) == 282);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 17 && lost == 0);
    CHECK(be64_at(out) == 283);
    finish();

    /* The platform overwrites the oldest samples while the client copies them. */
    setup("profile_torn");
    refplat.profile_max_hz = 1000;
    refplat.map_mapf = 0; /* a cacheable map: the client calls cache_acquire before it copies */
    h = profile_open(100);
    slot = slot_of(h);
    for (pc = 0; pc < 30; pc++)
        refplat_app_profile_push(slot, pc);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 30 && lost == 0);
    for (pc = 30; pc < 80; pc++)
        refplat_app_profile_push(slot, pc);
    refplat.on_acquire = hook_push;
    hook_slot = slot;
    hook_more = 20;
    hook_pc = 80;
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK);
    refplat.on_acquire = NULL;
    /* Head 100 at the end, the client started at 30: 70 samples, and only those from
     * 100 - 63 = 37 on are trusted. */
    CHECK(got == 43 && lost == 7);
    for (i = 0; i < 43; i++)
        CHECK(be64_at(out + i * 8) == 37 + i);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 20 && lost == 0);
    CHECK(be64_at(out) == 80);
    finish();

    /* A full ring behind: the platform has begun to store the next sample into the slot of
     * the oldest and has not published the head. The half-written slot is not returned. */
    setup("profile_partial");
    refplat.profile_max_hz = 1000;
    refplat.map_mapf = 0;
    h = profile_open(100);
    slot = slot_of(h);
    for (pc = 0; pc < 64; pc++)
        refplat_app_profile_push(slot, 0x1000 + pc);
    refplat.on_acquire = hook_partial;
    hook_slot = slot;
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK);
    refplat.on_acquire = NULL;
    CHECK(got == 63 && lost == 1);
    for (i = 0; i < 63; i++)
        CHECK(be64_at(out + i * 8) == 0x1000 + 1 + i);
    finish();

    /* The head is one 32-bit word: the count wraps at 2^32 and the reader follows. */
    setup("profile_wrap");
    refplat.profile_max_hz = 1000;
    h = profile_open(100);
    slot = slot_of(h);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_HEAD, 0xFFFFFFF0u);
    h->prof_next = 0xFFFFFFF0u;
    for (pc = 0; pc < 32; pc++)
        refplat_app_profile_push(slot, 0x5000 + pc);
    CHECK(cb_word(h, AAR_CB_PROFILE_HEAD) == 0x10u);
    memset(&st, 0xEE, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.profile_samples == 0x10u);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 32 && lost == 0);
    for (i = 0; i < 32; i++)
        CHECK(be64_at(out + i * 8) == 0x5000 + i);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 0 && lost == 0);
    /* More than a ring across the wrap: the loss is counted across it too. */
    for (pc = 0; pc < 100; pc++)
        refplat_app_profile_push(slot, 0x6000 + pc);
    CHECK(aar_core_read_profile(&core, h, out, 80, &got, &lost) == AAR_OK && got == 63 && lost == 37);
    CHECK(be64_at(out) == 0x6000 + 37);
    finish();

    /* A ring that breaks the rules, and a head that goes back, are a corrupt block. */
    setup("profile_corrupt");
    refplat.profile_max_hz = 1000;
    h = profile_open(100);
    slot = slot_of(h);
    refplat_app_profile_push(slot, 1);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_OK && got == 1);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_BYTES, 100);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_IO && got == 0);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_BYTES, 32);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_IO);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_BYTES, 512);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_OK && got == 0);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_HEAD, 0);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_IO);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_HEAD, 1);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_OK && got == 0);
    aar_put_be32(refplat_cb(slot) + AAR_CB_PROFILE_TOKEN, 0);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_NOSYS);
    CHECK(aar_core_close(&core, h) == AAR_OK);
    CHECK(aar_core_read_profile(&core, h, out, 8, &got, &lost) == AAR_E_STALE);
    finish();
}

int main(void)
{
    test_open_close_many();
    test_query();
    test_owner_stale();
    test_tags();
    test_load_start();
    test_load_failure();
    test_status();
    test_messages();
    test_events();
    test_absolute_mouse_merge();
    test_kick();
    test_service();
    test_logs_and_stats();
    test_audio();
    test_audio_reply_busy();
    test_shared_map();
    test_lease();
    test_close_order();
    test_reap();
    test_log_sink();
    test_notify();
    test_notify_answers();
    test_revoke();
    test_stale_platform();
    test_work_list();
    test_alloc_balance();
    test_open_failures();
    test_stack();
    test_profile();
    test_size_arithmetic();
    printf("core_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
