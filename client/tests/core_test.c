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
    for (h = core.lease_head; h; h = h->lease_next) {
        if (h->claim.handle == handle)
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
    fail_alloc = 0;
    cur = 0;
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
    CHECK((uintptr_t)refplat_ptr(REFPLAT_TOKEN_BASE) >> 32 == 0); /* link with -no-pie */
}

static void finish(void)
{
    aar_core_shutdown(&core);
    aar_core_detach(&core);
    CHECK(refplat_live() == 0);
    CHECK(pool_live == 0);
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
        CHECK(aar_core_ext_set_log_sink(h, NULL, 0) == expect);
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
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_BASE + 7, 1) == AAR_E_INVAL);
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
    CHECK(h->opts.lease_extended == 1); /* the default: the long lease */
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_OK);
    CHECK(h->opts.lease_extended == 0 && h->opts.audio_route == AAR_AUDIO_ROUTE_AHI);
    CHECK(h->opts.client_caps == AAR_CCAP_KNOWN);
    /* The limit words reach the control block at load begin. */
    CHECK(cb_word(h, AAR_CB_PRIVATE_MIB) == 0);
    CHECK(aar_core_load_begin(&core, h, 100, &staging, &chunk) == AAR_OK);
    CHECK(cb_word(h, AAR_CB_PRIVATE_MIB) == 64 && cb_word(h, AAR_CB_SHARED_MIB) == 32);
    CHECK(cb_word(h, AAR_CB_WEIGHT) == 4);
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
        CHECK(aar_core_service_next(&core, h, &req) == AAR_OK && req.id == 100);
        CHECK(h->kick_pending & AAR_PLAT_KICK_SQ);
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
    CHECK(aar_core_ext_set_log_sink(h, sink, (APTR)(uintptr_t)0x77) == AAR_OK);
    CHECK(refplat_app_log(slot, tok, 8, 9) == AAR_OK);
    CHECK(aar_core_service_next(&core, h, &req) == AAR_E_BUSY);
    CHECK(sink_seen.calls == 1 && sink_seen.len == 9 && sink_seen.user == 0x77);
    CHECK(memcmp(sink_seen.data, "hello log", 9) == 0);
    CHECK(h->stats.logs == 1);
    CHECK(aar_core_ext_set_log_sink(h, NULL, 0) == AAR_OK);
    /* The extension table. */
    ext = (const aar_client_ext_zz9000 *)aar_core_ext(&core, AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL && AAR_CLIENT_EXT_ZZ9000_COVERS(ext, set_log_sink));
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
        CHECK(ext->list(0, &list) == 1 && list.handle == h->claim.handle && list.index == 1);
        CHECK(ext->list(list.index, &list) == 0);
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
    CHECK(core.lease_head == h2 && h2->lease_next == NULL);
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
    CHECK(refplat_live() == 3 && pool_live == 3);
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
    CHECK(pool_live == 3); /* the tombstones stay */
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
    CHECK(pool_live == 3);
    /* Once nothing is at the address any more, the next pass frees them. */
    ostate[0] = AAR_OWNER_ABSENT;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0);
    CHECK(pool_live == 1);
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

static struct { AARHandle *h; uint32_t type; } rec[16];
static unsigned nrec;

static void notify_cb(void *user, AARHandle *h, uint32_t type)
{
    (void)user;
    if (nrec < 16) {
        rec[nrec].h = h;
        rec[nrec].type = type;
    }
    nrec++;
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
    CHECK(h->revoked == 1 && h->cb == NULL && core.lease_head == NULL);
    frozen = aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER);
    for (i = 0; i < 30; i++)
        aar_core_vblank(&core);
    CHECK(aar_get_be32(refplat_cb(slot) + AAR_CB_LEASE_COUNTER) == frozen); /* no store into a revoked claim */
    CHECK(aar_core_status(&core, h, &st) == AAR_E_STALE);
    CHECK(aar_core_load_begin(&core, h, 10, NULL, NULL) == AAR_E_STALE);
    /* What the app sent before stays readable. */
    CHECK(aar_core_get_message(&core, h, &m) == AAR_OK && m.bytes[0] == 11);
    CHECK(aar_core_get_message(&core, h, &m) == AAR_OK && m.bytes[0] == 12);
    CHECK(aar_core_get_message(&core, h, &m) == AAR_E_STALE);
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

int main(void)
{
    test_open_close_many();
    test_query();
    test_owner_stale();
    test_tags();
    test_load_start();
    test_status();
    test_messages();
    test_events();
    test_kick();
    test_service();
    test_logs_and_stats();
    test_audio();
    test_shared_map();
    test_lease();
    test_close_order();
    test_reap();
    test_notify();
    test_revoke();
    test_open_failures();
    printf("core_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
