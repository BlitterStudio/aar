/*
 * Host test of what aar.library costs with many handles (client/src/aar_core.c
 * against the reference platform with 240 claims).
 *
 * The rules, counted in steps and visits that the core keeps (aar_core's stat_*
 * counters), never in time:
 *   - a call that takes a handle finds it in a number of steps that does not
 *     grow with the number of handles, and so do AAROpen and AARClose;
 *   - a dispatcher pass looks only at the handles that have work: an idle handle
 *     that is not armed costs it nothing, and a handle that has work still gets
 *     it done (a doorbell that was busy, a poll that is still in flight, a mouse
 *     event that waits for room, an armed handle's scan);
 *   - on a platform with event counts, an idle armed handle costs the pass
 *     nothing either: one read of the platform's global count per pass while
 *     nothing was posted, and a scan (three control block reads) only for a
 *     handle in a group that changed or that has work pending;
 *   - the vertical-blank interrupt looks at a fifth of the claims per tick, and
 *     every claim is renewed once per period, with up to 230 handles.
 *
 * Build with -no-pie (or -m32), and with -DREFPLAT_CLAIMS=240u and smaller
 * stride definitions for refplat.c, see run_host.sh: this test loads no image.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aar_core.h"
#include "../src/refplat.h"

#define MAXH 232u

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

/* ---- The environment -------------------------------------------------------------- */

#define POOL_SLOT 8192u
#define POOL_SLOTS 260u
static uint8_t pool[POOL_SLOTS][POOL_SLOT] __attribute__((aligned(16)));
static uint8_t pool_used[POOL_SLOTS];
static unsigned long pool_live;

static void *t_alloc(void *user, uint32_t bytes)
{
    uint32_t i;

    (void)user;
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
            memset(pool[i], 0xA5, POOL_SLOT);
            return;
        }
    }
    printf("FAIL [%s] release of memory that is not the pool's\n", cur_test);
    failures++;
}

static struct { int pad; } tasks[2];
static int cur;

static void t_self(void *user, aar_owner_stamp *s)
{
    (void)user;
    s->id = &tasks[cur];
    s->name = NULL;
    s->number = 0;
    s->kind = 1;
}

static int irq_depth;

static unsigned long irq_calls; /* the Disable() calls */

static void t_irq_off(void *user)
{
    (void)user;
    irq_depth++;
    irq_calls++;
}

static void t_irq_on(void *user)
{
    (void)user;
    irq_depth--;
}

static aar_core core;
static struct aar_core_env env;

static void setup(const char *name)
{
    cur_test = name;
    printf("== %s\n", name);
    fflush(stdout);
    refplat_reset();
    memset(pool_used, 0, sizeof(pool_used));
    pool_live = 0;
    irq_depth = 0;
    memset(&env, 0, sizeof(env));
    env.size = sizeof(env);
    env.alloc = t_alloc;
    env.release = t_release;
    env.self = t_self;
    env.irq_off = t_irq_off;
    env.irq_on = t_irq_on;
    env.ticks_per_sec = 50;
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
    CHECK(irq_depth == 0);
}

static AARHandle *open_one(uint32_t mask)
{
    AARHandle *h = NULL;
    ULONG reason = 99;
    int rc = aar_core_open(&core, mask, NULL, NULL, &h, &reason);

    CHECK(rc == AAR_OK);
    return h;
}

static void open_n(AARHandle **hs, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        hs[i] = open_one(1);
}

static void close_n(AARHandle **hs, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (hs[i])
            CHECK(aar_core_close(&core, hs[i]) == AAR_OK);
}

static void dispatch_once(void)
{
    aar_core_dispatch(&core, NULL, NULL);
}

/* The visits of one pass. */
static uint32_t pass_visits(void)
{
    uint32_t before = core.stat_pass_visits;

    dispatch_once();
    return core.stat_pass_visits - before;
}

static int slot_of(AARHandle *h)
{
    return refplat_slot(h->claim.handle);
}

static const unsigned sizes[] = { 1, 10, 50, 100, 230 };
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))

/* ---- Tests ------------------------------------------------------------------------ */

/* The steps a handle call takes to find its handle, at every size. */
static void test_validation(void)
{
    static AARHandle *hs[MAXH];
    unsigned s, i;

    for (s = 0; s < NSIZES; s++) {
        unsigned n = sizes[s];
        uint32_t most = 0u, before, steps;
        AARHandle *gone;

        setup("validation");
        open_n(hs, n);
        /* Every handle, the oldest and the newest included, is found in a few
         * steps: no handle costs more than 32, however many there are. */
        for (i = 0; i < n; i++) {
            before = core.stat_lookup_steps;
            CHECK(aar_core_check(&core, hs[i]) == AAR_OK);
            steps = core.stat_lookup_steps - before;
            if (steps > most)
                most = steps;
        }
        CHECK(most <= 32u);
        /* The oldest handle, repeatedly: the same as any other. */
        before = core.stat_lookup_steps;
        for (i = 0; i < 100; i++)
            CHECK(aar_core_check(&core, hs[0]) == AAR_OK);
        CHECK(core.stat_lookup_steps - before <= 100u * 32u);
        /* A pointer that is not a handle is refused without being touched. */
        before = core.stat_lookup_steps;
        CHECK(aar_core_check(&core, (AARHandle *)(uintptr_t)0x1000u) == AAR_E_STALE);
        CHECK(aar_core_check(&core, NULL) == AAR_E_STALE);
        CHECK(core.stat_lookup_steps - before <= 64u);
        /* The oldest handle closes in a few steps, and then is stale. */
        gone = hs[0];
        before = core.stat_lookup_steps;
        CHECK(aar_core_close(&core, gone) == AAR_OK);
        CHECK(core.stat_lookup_steps - before <= 64u);
        CHECK(aar_core_check(&core, gone) == AAR_E_STALE);
        hs[0] = NULL;
        /* Opening one more costs no lookup at all. */
        before = core.stat_lookup_steps;
        hs[0] = open_one(1);
        CHECK(core.stat_lookup_steps - before <= 64u);
        close_n(hs, n);
        finish();
    }
}

/* The table grows with the handles and keeps every pointer right, through any
 * order of opens and closes. */
static void test_table(void)
{
    static AARHandle *hs[MAXH];
    static AARHandle *old[MAXH];
    unsigned i, n = 230u, live = 0;

    setup("table");
    open_n(hs, n);
    for (i = 0; i < n; i++)
        CHECK(aar_core_check(&core, hs[i]) == AAR_OK);
    /* Every third handle closes, from the middle outwards. */
    for (i = n / 2; i < n; i += 3) {
        old[i] = hs[i];
        CHECK(aar_core_close(&core, hs[i]) == AAR_OK);
        hs[i] = NULL;
    }
    for (i = n / 2; i-- > 0;) {
        if (i % 3 == 0) {
            old[i] = hs[i];
            CHECK(aar_core_close(&core, hs[i]) == AAR_OK);
            hs[i] = NULL;
        }
    }
    for (i = 0; i < n; i++) {
        if (hs[i]) {
            CHECK(aar_core_check(&core, hs[i]) == AAR_OK);
            live++;
        } else {
            CHECK(aar_core_check(&core, old[i]) == AAR_E_STALE);
        }
    }
    CHECK(core.htab_count == live);
    /* Reopened handles are found, whatever addresses they get. */
    for (i = 0; i < n; i++) {
        if (!hs[i]) {
            hs[i] = open_one(1);
            if (hs[i])
                CHECK(aar_core_check(&core, hs[i]) == AAR_OK);
        }
    }
    for (i = 0; i < n; i++)
        CHECK(hs[i] && aar_core_check(&core, hs[i]) == AAR_OK);
    CHECK(core.htab_count == n);
    close_n(hs, n);
    CHECK(core.htab == NULL && core.htab_count == 0u); /* an idle library holds no table */
    finish();
}

/* What a dispatcher pass looks at. */
static void test_pass(void)
{
    static AARHandle *hs[MAXH];
    unsigned s, i;
    aar_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN;
    for (s = 0; s < NSIZES; s++) {
        unsigned n = sizes[s];

        setup("pass");
        open_n(hs, n);
        /* Idle handles, none armed: the pass looks at none of them, at any size. */
        CHECK(pass_visits() == 0u);
        CHECK(pass_visits() == 0u);
        close_n(hs, n);
        finish();
    }

    /* A doorbell that was busy: the pass retries it, and then has no more to do. */
    setup("pass");
    open_n(hs, 230u);
    refplat.kick_busy = 3;
    for (i = 0; i < 3; i++)
        CHECK(aar_core_post_event(&core, hs[100 + i], &ev) == AAR_OK);
    for (i = 0; i < 3; i++)
        CHECK(hs[100 + i]->kick_pending != 0u && hs[100 + i]->in_work == 1u);
    CHECK(refplat.claims[slot_of(hs[100])].kicks == 0);
    CHECK(pass_visits() == 3u); /* just those three */
    for (i = 0; i < 3; i++)
        CHECK(hs[100 + i]->kick_pending == 0u && refplat.claims[slot_of(hs[100 + i])].kicks == 1);
    CHECK(pass_visits() == 0u); /* sent, retired, off the list */
    CHECK(core.work_head == NULL);
    close_n(hs, 230u);
    finish();

    /* A doorbell the platform still holds is polled until it says it is done. */
    setup("pass");
    open_n(hs, 50u);
    refplat.poll_inflight = 2;
    CHECK(aar_core_post_event(&core, hs[7], &ev) == AAR_OK); /* the bell goes out at once */
    CHECK(hs[7]->poll_needed == 1u && refplat.claims[slot_of(hs[7])].kicks == 1);
    CHECK(pass_visits() == 1u && hs[7]->poll_needed == 1u); /* in flight */
    CHECK(pass_visits() == 1u && hs[7]->poll_needed == 1u); /* still */
    CHECK(pass_visits() == 1u && hs[7]->poll_needed == 0u); /* done */
    CHECK(pass_visits() == 0u);
    CHECK(refplat.polls == 3);
    close_n(hs, 50u);
    finish();

    /* A mouse event that waits for room in a full CQ is posted when there is room. */
    setup("pass");
    open_n(hs, 20u);
    {
        aar_event key = ev;
        aar_event mouse;
        uint8_t entry[AAR_ENTRY_BYTES];
        int slot = slot_of(hs[3]);

        for (i = 0; i < 64; i++) {
            key.code = (uint16_t)i;
            CHECK(aar_core_post_event(&core, hs[3], &key) == AAR_OK);
        }
        memset(&mouse, 0, sizeof(mouse));
        mouse.type = AAR_EV_MOUSE;
        mouse.dx = 5;
        while (pass_visits() != 0u) /* the bells of the keys are done: the handle is idle */
            ;
        CHECK(hs[3]->in_work == 0);
        CHECK(aar_core_post_event(&core, hs[3], &mouse) == AAR_OK); /* kept: the CQ is full */
        CHECK(hs[3]->mouse_pending == 1u && hs[3]->in_work == 1u);
        CHECK(pass_visits() == 1u && hs[3]->mouse_pending == 1u); /* no room yet */
        CHECK(refplat_app_cq_pop(slot, entry) == 1);
        CHECK(pass_visits() == 1u && hs[3]->mouse_pending == 0u); /* posted */
        CHECK(pass_visits() <= 1u); /* its bell may need a poll */
        CHECK(pass_visits() == 0u);
    }
    close_n(hs, 20u);
    finish();

    /* An armed handle is scanned at every pass, as long as it is armed and live;
     * the others in the library still cost nothing. */
    setup("pass");
    open_n(hs, 230u);
    for (i = 0; i < 5; i++)
        CHECK(aar_core_message_signal(&core, hs[10 + i * 40]) == 1u);
    {
        unsigned long before = irq_calls;

        CHECK(pass_visits() == 5u);
        CHECK(pass_visits() == 5u);
        CHECK(irq_calls == before); /* a scan with nothing to send takes no Disable() */
    }
    CHECK(aar_core_close(&core, hs[10]) == AAR_OK);
    hs[10] = NULL;
    CHECK(pass_visits() == 4u);
    CHECK(aar_core_message_signal(&core, hs[10 + 40]) == 1u); /* already armed: not twice on the list */
    CHECK(pass_visits() == 4u);
    close_n(hs, 230u);
    CHECK(pass_visits() == 0u);
    finish();

    /* A handle with work that is closed, reaped or revoked leaves the list. */
    setup("pass");
    open_n(hs, 6u);
    refplat.kick_busy = 4;
    for (i = 0; i < 4; i++)
        CHECK(aar_core_post_event(&core, hs[i], &ev) == AAR_OK);
    CHECK(aar_core_close(&core, hs[0]) == AAR_OK); /* on the list while it closes */
    hs[0] = NULL;
    refplat_revoke(slot_of(hs[1]));
    CHECK(pass_visits() == 3u); /* the revoked claim answers the bell with STALE and leaves */
    CHECK(hs[1]->revoked == 1u && hs[1]->in_work == 0u);
    CHECK(pass_visits() == 0u);
    CHECK(core.work_head == NULL);
    close_n(hs, 6u);
    finish();
}

/* ---- Event counts (spec 10.6) ------------------------------------------------------ */

static unsigned notified;
static AARHandle *noted;
static uint32_t noted_type;
static int n_answer = AAR_CORE_NOTIFIED;

static int ncb(void *user, AARHandle *h, uint32_t type)
{
    (void)user;
    notified++;
    noted = h;
    noted_type = type;
    return n_answer;
}

/* One pass; the scans it made (notify_scan runs, three control block reads each). */
static uint32_t pass_scans(void)
{
    uint32_t before = core.stat_pass_scans;

    notified = 0u;
    aar_core_dispatch(&core, ncb, NULL);
    return core.stat_pass_scans - before;
}

static void arm_n(AARHandle **hs, unsigned n)
{
    unsigned i;

    open_n(hs, n);
    for (i = 0; i < n; i++)
        CHECK(aar_core_message_signal(&core, hs[i]) == 1u);
}

/* A pass that finds nothing: its cost in card reads and platform questions. */
static void check_quiet_pass(void)
{
    uint32_t reads = refplat.event_reads, asked = refplat.event_news_calls;
    uint32_t visits = core.stat_pass_visits;

    CHECK(pass_scans() == 0u && notified == 0u);
    CHECK(refplat.event_reads - reads == 1u);          /* the global count */
    CHECK(refplat.event_news_calls == asked);          /* nobody was asked */
    CHECK(core.stat_pass_visits == visits);            /* nobody was visited */
    CHECK(core.work_head == NULL);
}

static void test_events(void)
{
    static AARHandle *hs[MAXH];
    unsigned i;
    uint32_t reads, asked, k;
    uint8_t raw[AAR_MSG_BYTES] = { 7 };
    AARMessage m;
    static int port;
    AARHandle *hp = NULL;
    ULONG reason;

    n_answer = AAR_CORE_NOTIFIED;

    /* An idle armed handle is not scanned; a post in its group scans it once, a
     * post in another group does not; the card is read once per quiet pass. */
    setup("events");
    refplat.events_on = 1u;
    arm_n(hs, 100u);
    reads = refplat.event_reads;
    CHECK(pass_scans() == 100u);                       /* armed at open or by signal: one scan */
    CHECK(refplat.event_reads - reads == 16u);         /* the first sample: global and 15 groups */
    CHECK(core.work_head == NULL);
    for (i = 0; i < 3; i++)
        check_quiet_pass();
    /* hs[70] is record 71, group 2: records 64..95, 32 handles. Its end is posted. */
    reads = refplat.event_reads;
    refplat_app_set_state(slot_of(hs[70]), AAR_STATE_IDLE, AAR_END_EXIT);
    CHECK(pass_scans() == 32u);
    CHECK(refplat.event_reads - reads == 16u);         /* the global changed: and the 15 groups */
    CHECK(notified == 1u && noted == hs[70] && noted_type == AAR_NOTIFY_COMPLETE);
    check_quiet_pass();                                /* announced once; nothing left */
    /* A post for a group that no handle has: the global changed, nobody is scanned. */
    reads = refplat.event_reads;
    asked = refplat.event_news_calls;
    refplat_events_post(300u);
    CHECK(pass_scans() == 0u && notified == 0u);
    CHECK(refplat.event_reads - reads == 16u);
    CHECK(refplat.event_news_calls - asked == 100u);   /* every armed handle was asked, none read */
    check_quiet_pass();
    /* Records 96..100 are group 3: five handles; the others are not scanned. */
    refplat_events_post(98u);
    CHECK(pass_scans() == 5u);
    check_quiet_pass();
    /* A post in a group again (the handle's own last scan was long ago). */
    refplat_events_post(40u);
    CHECK(pass_scans() == 32u);
    check_quiet_pass();
    close_n(hs, 100u);
    finish();

    /* A record the block has no group for (480 and up): any change of the global
     * count scans it. */
    setup("events");
    refplat.events_on = 1u;
    refplat.events_record_bias = 480u;
    arm_n(hs, 10u);
    CHECK(pass_scans() == 10u);
    check_quiet_pass();
    refplat_events_post(5000u);                        /* a group beyond the block: only the global */
    CHECK(pass_scans() == 10u);
    check_quiet_pass();
    refplat_events_post(0u);                           /* another group: still a change of the global */
    CHECK(pass_scans() == 10u);
    check_quiet_pass();
    close_n(hs, 10u);
    finish();

    /* A count that wraps is a change. Groups 0 and 1 hold records 1..31 and 32..40. */
    setup("events");
    refplat.events_on = 1u;
    arm_n(hs, 40u);
    CHECK(pass_scans() == 40u);
    refplat_events_set(0u, 0xffffffffu);
    refplat_events_set(2u, 0xffffffffu);               /* group 1 */
    CHECK(pass_scans() == 9u);
    check_quiet_pass();
    refplat_events_post(40u);                          /* group 1 and the global wrap to 0 */
    CHECK(aar_get_be32(refplat.ev_block) == 0u && aar_get_be32(refplat.ev_block + 8u) == 0u);
    CHECK(pass_scans() == 9u);
    check_quiet_pass();
    close_n(hs, 40u);
    finish();

    /* Work that is pending keeps the handle scanned at every pass, and it is
     * signalled again after 5 ticks, as without events. */
    setup("events");
    refplat.events_on = 1u;
    arm_n(hs, 70u);
    CHECK(pass_scans() == 70u);
    refplat_app_message(slot_of(hs[65]), raw);         /* record 66, group 2: records 64..70 */
    CHECK(pass_scans() == 7u);
    CHECK(notified == 1u && noted == hs[65] && noted_type == AAR_NOTIFY_MESSAGE);
    reads = refplat.event_reads;
    asked = refplat.event_news_calls;
    for (i = 0; i < 4; i++) {
        CHECK(pass_scans() == 1u && notified == 0u);   /* only it, every pass; no new signal yet */
    }
    CHECK(refplat.event_reads - reads == 4u);          /* one read per pass, as ever */
    for (k = 0; k < 5u; k++)
        aar_core_vblank(&core);
    CHECK(pass_scans() == 1u && notified == 1u && noted == hs[65]); /* signalled again */
    CHECK(aar_core_get_message(&core, hs[65], &m) == AAR_OK && m.bytes[0] == 7);
    CHECK(pass_scans() == 1u && notified == 0u);       /* it finds the queue drained */
    check_quiet_pass();                                /* and is idle again */
    /* An end that was not delivered is scanned until it is, then once. */
    n_answer = 0;
    refplat_app_set_state(slot_of(hs[65]), AAR_STATE_IDLE, AAR_END_EXIT);
    CHECK(pass_scans() == 7u && notified == 1u && noted_type == AAR_NOTIFY_COMPLETE);
    CHECK(pass_scans() == 1u && notified == 1u);       /* again: the owner was not reached */
    CHECK(pass_scans() == 1u && notified == 1u);
    n_answer = AAR_CORE_NOTIFIED;
    CHECK(pass_scans() == 1u && notified == 1u && noted_type == AAR_NOTIFY_COMPLETE);
    check_quiet_pass();                                /* announced: done, no repeat */
    close_n(hs, 70u);
    finish();

    /* Arming, signalling and opening with a port each cost one scan. */
    setup("events");
    refplat.events_on = 1u;
    hs[0] = open_one(1);
    CHECK(pass_scans() == 0u);                         /* not armed: no scan */
    check_quiet_pass();
    CHECK(aar_core_message_signal(&core, hs[0]) == 1u);
    CHECK(pass_scans() == 1u);
    check_quiet_pass();
    CHECK(aar_core_message_signal(&core, hs[0]) == 1u); /* again: it is looked at once more */
    CHECK(pass_scans() == 1u);
    check_quiet_pass();
    CHECK(aar_core_open(&core, 1, &port, NULL, &hp, &reason) == AAR_OK);
    CHECK(pass_scans() == 1u);
    check_quiet_pass();
    CHECK(aar_core_close(&core, hp) == AAR_OK);
    CHECK(aar_core_close(&core, hs[0]) == AAR_OK);
    finish();

    /* A platform that cannot tell NEWS from QUIET answers NEWS at every pass: the
     * handles are asked, not scanned, and the card is still read once. */
    setup("events");
    refplat.events_on = 1u;
    refplat.events_always_news = 1u;
    arm_n(hs, 30u);
    CHECK(pass_scans() == 30u);
    reads = refplat.event_reads;
    asked = refplat.event_news_calls;
    CHECK(pass_scans() == 0u && pass_scans() == 0u);
    CHECK(refplat.event_news_calls - asked == 60u);    /* 30 handles, 2 passes */
    CHECK(refplat.event_reads - reads == 2u);
    refplat_events_post(35u);                          /* nobody has record 35 */
    CHECK(pass_scans() == 0u);
    refplat_events_post(3u);                           /* group 0: records 1..30 */
    CHECK(pass_scans() == 30u);
    close_n(hs, 30u);
    finish();

    /* No events: the platform answers none, or has one operation only, or none:
     * every armed handle is scanned at every pass, as before. */
    for (i = 0; i < 3; i++) {
        static struct aar_plat_ops partial;

        setup("events");
        refplat.events_on = i != 0u;
        if (i != 0u) {
            partial = *refplat.plat.ops;
            if (i == 1u)
                partial.events_sample = NULL;
            else
                partial.claim_news = NULL;
            refplat.plat.ops = &partial;
        }
        arm_n(hs, 30u);
        for (k = 0; k < 3u; k++) {
            CHECK(pass_scans() == 30u);
            CHECK(notified == 0u);
        }
        CHECK(refplat.event_reads == 0u && refplat.event_news_calls == 0u);
        refplat_app_set_state(slot_of(hs[5]), AAR_STATE_IDLE, AAR_END_EXIT);
        CHECK(pass_scans() == 30u && notified == 1u && noted == hs[5]);
        CHECK(pass_scans() == 30u && notified == 0u);
        close_n(hs, 30u);
        finish();
    }

    /* A platform that stops having events: the handles that had left the list
     * come back. */
    setup("events");
    refplat.events_on = 1u;
    arm_n(hs, 30u);
    CHECK(pass_scans() == 30u);
    check_quiet_pass();
    refplat.events_on = 0u;
    CHECK(pass_scans() == 30u);
    CHECK(pass_scans() == 30u);
    close_n(hs, 30u);
    finish();

    /* A platform that is not attached is not asked. */
    setup("events");
    refplat.events_on = 1u;
    core.attach_rc = AAR_E_NOSYS;
    CHECK(pass_scans() == 0u);
    CHECK(refplat.event_samples == 0u && refplat.event_reads == 0u);
    core.attach_rc = AAR_OK;
    finish();
}

/* The interrupt renews every claim once per period and looks at a fifth of them
 * per tick. */
static void test_lease(void)
{
    static AARHandle *hs[MAXH];
    static uint32_t last_word[MAXH], last_tick[MAXH];
    unsigned s, i, t;

    for (s = 0; s < NSIZES; s++) {
        unsigned n = sizes[s];
        uint32_t worst_gap = 0u, per_tick_max = 0u, v0;

        setup("lease");
        open_n(hs, n);
        for (i = 0; i < n; i++) {
            last_word[i] = aar_get_be32(refplat_cb(slot_of(hs[i])) + AAR_CB_LEASE_COUNTER);
            last_tick[i] = 0u;
        }
        v0 = core.stat_lease_visits;
        for (t = 1; t <= 200u; t++) {
            uint32_t before = core.stat_lease_visits;

            core.disp_alive = core.ticks;
            aar_core_vblank(&core);
            if (core.stat_lease_visits - before > per_tick_max)
                per_tick_max = core.stat_lease_visits - before;
            for (i = 0; i < n; i++) {
                uint32_t w = aar_get_be32(refplat_cb(slot_of(hs[i])) + AAR_CB_LEASE_COUNTER);

                if (w != last_word[i]) {
                    if (t - last_tick[i] > worst_gap)
                        worst_gap = t - last_tick[i];
                    last_word[i] = w;
                    last_tick[i] = t;
                }
            }
        }
        /* Every claim, once per period, and never later (the first renewal after
         * the open counts from the open). */
        CHECK(worst_gap <= AAR_CORE_LEASE_PERIOD_TICKS);
        for (i = 0; i < n; i++)
            CHECK(200u - last_tick[i] < AAR_CORE_LEASE_PERIOD_TICKS);
        /* A tick looks at its share: a fifth of the claims, rounded up, and every
         * claim once per period. */
        CHECK(per_tick_max <= (n + AAR_CORE_LEASE_PERIOD_TICKS - 1u) / AAR_CORE_LEASE_PERIOD_TICKS + 1u);
        CHECK(core.stat_lease_visits - v0 == n * (200u / AAR_CORE_LEASE_PERIOD_TICKS));
        close_n(hs, n);
        finish();
    }
}

/* How often the dead-owner pass asks the system about an owner. */
static int owner_answer, owner_calls;

static int owner_fn(void *user, const aar_owner_stamp *o)
{
    (void)user;
    (void)o;
    owner_calls++;
    return owner_answer;
}

/* The pass asks about each owner once, not once per handle. */
static void test_reap(void)
{
    static AARHandle *hs[MAXH];
    unsigned n = 230u;

    setup("reap");
    open_n(hs, n);
    owner_answer = AAR_OWNER_ALIVE;
    owner_calls = 0;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0u);
    CHECK(owner_calls <= 1);
    owner_answer = AAR_OWNER_CHANGED; /* the owner is gone: every handle is reaped */
    owner_calls = 0;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == n);
    CHECK(owner_calls <= 2); /* the live handles, then the tombstones */
    owner_answer = AAR_OWNER_ABSENT;
    owner_calls = 0;
    CHECK(aar_core_reap(&core, owner_fn, NULL) == 0u);
    CHECK(owner_calls <= 1);
    CHECK(refplat_live() == 0 && core.handles == NULL && core.htab == NULL);
    finish();
}

int main(void)
{
    test_validation();
    test_reap();
    test_table();
    test_pass();
    test_events();
    test_lease();
    printf("scale_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
