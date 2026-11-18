/*
 * Test of the reference transport model in aar/transport.h: handle helpers,
 * big-endian entry layout, queue full and empty, message credits, wraparound of the
 * free-running indices past 2^32, message reserve, unsolicited completion
 * entries, barrier ordering, and the control block helpers. Pure host code,
 * single thread; the barrier hooks are test probes.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude \
 *       tests/transport_model_test.c -o transport_model_test
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar/transport.h"

static unsigned long checks;
static unsigned long failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

#define CHECK_EQ(actual, expected)                                       \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(expected);                            \
        checks++;                                                        \
        if (a_ != e_) {                                                  \
            failures++;                                                  \
            printf("FAIL %s:%d: %s is %lld, expected %lld\n", __FILE__,  \
                   __LINE__, #actual, a_, e_);                           \
        }                                                                \
    } while (0)

/* Shared memory for one pair at depth 64, accessed as bytes, halves and words
 * through the volatile accessors. The union keeps every view legal. */
typedef union {
    uint8_t b[0x3000];
    uint16_t h[0x3000 / 2];
    uint32_t w[0x3000 / 4];
} pair_mem;

static _Alignas(4096) pair_mem mem;

static void clear_mem(void)
{
    memset(&mem, 0, sizeof(mem));
}

static uint8_t *ring_sq(void) { return mem.b + AAR_PAIR_SQ_OFFSET; }
static uint8_t *ring_cq(void) { return mem.b + AAR_PAIR_CQ_OFFSET; }

static void bind_pair(aar_queue *sq, aar_queue *cq)
{
    CHECK_EQ(aar_cb_init(mem.b, aar_handle_make(3, 7), AAR_PROFILE_A32, 64,
                         0x1000, AAR_PAIR_SQ_OFFSET, 0x1000,
                         AAR_PAIR_CQ_OFFSET), AAR_OK);
    CHECK_EQ(aar_cb_queue_bind(sq, mem.b, ring_sq(), AAR_QUEUE_SQ), AAR_OK);
    CHECK_EQ(aar_cb_queue_bind(cq, mem.b, ring_cq(), AAR_QUEUE_CQ), AAR_OK);
}

/* ---- Barrier probes -------------------------------------------------------------- */

static unsigned acquire_calls;
static unsigned release_calls;

static void count_acquire(void) { acquire_calls++; }
static void count_release(void) { release_calls++; }

/* Release probe for a post: when it runs, the entry must already be written
 * and the tail must not have moved yet. */
static const aar_queue *probe_queue;
static uint32_t probe_tail_before;
static uint32_t probe_head_before;
static unsigned probe_order_ok;

static void release_before_tail(void)
{
    uint32_t tail = aar_get_be32(probe_queue->tail);
    const volatile uint8_t *slot =
        probe_queue->ring + (size_t)(tail & (probe_queue->depth - 1u)) * 64u;

    if (tail == probe_tail_before && aar_get_be16(slot + AAR_ENTRY_OP) != 0u)
        probe_order_ok++;
}

static void release_before_head(void)
{
    if (aar_get_be32(probe_queue->head) == probe_head_before)
        probe_order_ok++;
}

/* ---- Tests ----------------------------------------------------------------------------- */

static void test_handles(void)
{
    uint32_t h = aar_handle_make(0x1234, 0xABCD);
    uint32_t gen;
    unsigned seen_zero = 0;
    unsigned n;

    CHECK_EQ(h, 0xABCD1234u);
    CHECK_EQ(aar_handle_index(h), 0x1234);
    CHECK_EQ(aar_handle_generation(h), 0xABCD);
    CHECK_EQ(aar_handle_make(0x1FFFF, 0x1FFFF), 0xFFFFFFFFu);

    CHECK_EQ(aar_handle_next_gen(1), 2);
    CHECK_EQ(aar_handle_next_gen(0xFFFE), 0xFFFF);
    CHECK_EQ(aar_handle_next_gen(0xFFFF), 1);
    CHECK_EQ(aar_handle_next_gen(0), 1);

    /* A full cycle never issues generation 0 and revisits 1 after 0xFFFF. */
    gen = 1;
    for (n = 0; n < 0x20000u; n++) {
        gen = aar_handle_next_gen(gen);
        if (gen == 0 || gen > 0xFFFFu)
            seen_zero++;
    }
    CHECK_EQ(seen_zero, 0);
    gen = 1;
    for (n = 0; n < 0xFFFFu; n++)
        gen = aar_handle_next_gen(gen);
    CHECK_EQ(gen, 1);

    /* Stale and invalid handles. */
    CHECK_EQ(aar_handle_check(h, 0xABCD), AAR_OK);
    CHECK_EQ(aar_handle_check(h, 0xABCE), AAR_E_STALE);
    CHECK_EQ(aar_handle_check(h, 0), AAR_E_STALE); /* index is free now */
    CHECK_EQ(aar_handle_check(0, 0), AAR_E_INVAL);
    CHECK_EQ(aar_handle_check(0, 5), AAR_E_INVAL);
    CHECK_EQ(aar_handle_check(0x00000007u, 0), AAR_E_INVAL); /* gen 0 never issued */
    CHECK_EQ(aar_handle_check(aar_handle_make(0, 1), 1), AAR_OK); /* index 0 is fine */
    /* A reused index: the old handle goes stale when the generation moves. */
    {
        uint32_t old_handle = aar_handle_make(9, 0xFFFF);
        uint32_t live = aar_handle_next_gen(0xFFFF);

        CHECK_EQ(aar_handle_check(old_handle, 0xFFFF), AAR_OK);
        CHECK_EQ(aar_handle_check(old_handle, live), AAR_E_STALE);
        CHECK_EQ(aar_handle_check(aar_handle_make(9, live), live), AAR_OK);
    }
}

static void test_be_helpers(void)
{
    uint8_t buf[16];

    memset(buf, 0xEE, sizeof(buf));
    aar_put_be16(buf, 0x1234);
    CHECK(buf[0] == 0x12 && buf[1] == 0x34);
    CHECK_EQ(aar_get_be16(buf), 0x1234);
    aar_put_be32(buf + 4, 0xA1B2C3D4u);
    CHECK(buf[4] == 0xA1 && buf[5] == 0xB2 && buf[6] == 0xC3 && buf[7] == 0xD4);
    CHECK_EQ((uint32_t)aar_get_be32(buf + 4), 0xA1B2C3D4u);
    aar_put_be64(buf + 8, 0x0102030405060708ull);
    CHECK(buf[8] == 1 && buf[9] == 2 && buf[10] == 3 && buf[11] == 4 &&
          buf[12] == 5 && buf[13] == 6 && buf[14] == 7 && buf[15] == 8);
    CHECK(aar_get_be64(buf + 8) == 0x0102030405060708ull);
}

static void test_entry_layout(void)
{
    uint8_t *e = ring_sq();
    aar_request rq;
    aar_request rq2;
    aar_completion c;
    aar_completion c2;
    aar_event ev;
    aar_event ev2;
    uint8_t msg[AAR_MSG_BYTES];
    uint8_t msg2[AAR_MSG_BYTES];
    unsigned i;

    clear_mem();
    memset(&rq, 0, sizeof(rq));
    rq.op = AAR_OP_FILE_READ;
    rq.flags = 0xA5A5;
    rq.id = 0x01020304u;
    for (i = 0; i < AAR_ENTRY_ARGS; i++)
        rq.arg[i] = 0x10000000u * (i + 1) + i;
    rq.buf = 0xDEADBEEFu;
    rq.buf_off = 0x00001000u;
    rq.buf_len = 0x00004000u;

    /* Pre-dirty the slot to prove a put clears reserved bytes. */
    memset(e, 0xFF, AAR_ENTRY_BYTES);
    aar_entry_put_request(e, &rq);
    CHECK(e[0] == 0x00 && e[1] == AAR_OP_FILE_READ);          /* op, big-endian */
    CHECK(e[2] == 0xA5 && e[3] == 0xA5);                       /* flags */
    CHECK(e[4] == 0x01 && e[5] == 0x02 && e[6] == 0x03 && e[7] == 0x04);
    CHECK(e[8] == 0 && e[9] == 0 && e[10] == 0 && e[11] == 0); /* result */
    CHECK(e[12] == 0 && e[13] == 0 && e[14] == 0 && e[15] == 0);
    CHECK(e[16] == 0x10 && e[17] == 0 && e[18] == 0 && e[19] == 0); /* arg0 */
    CHECK(e[20] == 0x20 && e[23] == 0x01);                     /* arg1 = 0x20000001 */
    CHECK(e[36] == 0x60 && e[39] == 0x05);                     /* arg5 */
    CHECK(e[40] == 0xDE && e[41] == 0xAD && e[42] == 0xBE && e[43] == 0xEF);
    CHECK(e[44] == 0 && e[45] == 0 && e[46] == 0x10 && e[47] == 0);
    CHECK(e[48] == 0 && e[49] == 0 && e[50] == 0x40 && e[51] == 0);
    for (i = 52; i < AAR_ENTRY_BYTES; i++)
        CHECK(e[i] == 0);                                      /* reserved */
    aar_entry_get_request(e, &rq2);
    CHECK(memcmp(&rq, &rq2, sizeof(rq)) == 0);

    /* Completion: result is a signed big-endian word. */
    memset(&c, 0, sizeof(c));
    c.op = AAR_OP_FILE_SEEK;
    c.flags = 0x0102;
    c.id = 0x0A0B0C0Du;
    c.result = AAR_E_STALE;
    for (i = 0; i < AAR_ENTRY_OUTS; i++)
        c.out[i] = 0xF0000000u | i;
    memset(e, 0xFF, AAR_ENTRY_BYTES);
    aar_entry_put_completion(e, &c);
    CHECK(e[0] == 0 && e[1] == AAR_OP_FILE_SEEK && e[2] == 1 && e[3] == 2);
    CHECK(e[4] == 0x0A && e[7] == 0x0D);
    CHECK(e[8] == 0xFF && e[9] == 0xFF && e[10] == 0xFF && e[11] == 0xF0); /* -16 */
    CHECK(e[12] == 0 && e[15] == 0);
    CHECK(e[16] == 0xF0 && e[19] == 0 && e[36] == 0xF0 && e[39] == 5);
    for (i = 40; i < AAR_ENTRY_BYTES; i++)
        CHECK(e[i] == 0);
    aar_entry_get_completion(e, &c2);
    CHECK(memcmp(&c, &c2, sizeof(c)) == 0);
    CHECK_EQ(c2.result, AAR_E_STALE);

    /* Message: opaque bytes, no swapping, header op MSG. */
    for (i = 0; i < AAR_MSG_BYTES; i++)
        msg[i] = (uint8_t)(0x80 + i);
    memset(e, 0xFF, AAR_ENTRY_BYTES);
    aar_entry_put_msg(e, msg);
    CHECK(e[0] == 0 && e[1] == AAR_OP_MSG);
    CHECK(e[16] == 0x80 && e[47] == 0x9F);
    for (i = 48; i < AAR_ENTRY_BYTES; i++)
        CHECK(e[i] == 0);
    aar_entry_get_msg(e, msg2);
    CHECK(memcmp(msg, msg2, sizeof(msg)) == 0);

    /* Event: aar_event fields, each big-endian. */
    ev.type = AAR_EV_MOUSE;
    ev.code = 0x1122;
    ev.dx = -2;
    ev.dy = 300;
    ev.buttons = 5;
    ev.reserved = 0;
    ev.time_ms = 0x99AABBCCu;
    memset(e, 0xFF, AAR_ENTRY_BYTES);
    aar_entry_put_event(e, &ev);
    CHECK(e[0] == 0 && e[1] == AAR_OP_EVENT);
    CHECK(e[16] == 0 && e[17] == AAR_EV_MOUSE);
    CHECK(e[18] == 0x11 && e[19] == 0x22);
    CHECK(e[20] == 0xFF && e[21] == 0xFE);                    /* dx -2 */
    CHECK(e[22] == 0x01 && e[23] == 0x2C);                    /* dy 300 */
    CHECK(e[24] == 0 && e[25] == 5 && e[26] == 0 && e[27] == 0);
    CHECK(e[28] == 0x99 && e[29] == 0xAA && e[30] == 0xBB && e[31] == 0xCC);
    for (i = 32; i < AAR_ENTRY_BYTES; i++)
        CHECK(e[i] == 0);
    aar_entry_get_event(e, &ev2);
    CHECK(memcmp(&ev, &ev2, sizeof(ev)) == 0);
}

static void test_post_until_full(void)
{
    aar_queue sq, cq;
    aar_request r;
    uint32_t used = 99;
    const volatile uint8_t *slot = 0;
    unsigned i;

    clear_mem();
    bind_pair(&sq, &cq);
    CHECK_EQ(sq.depth, 64);

    /* Empty. */
    CHECK_EQ(aar_queue_used(&sq, &used), AAR_OK);
    CHECK_EQ(used, 0);
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 0);

    /* Post until full: exactly depth requests fit. */
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 64; i++) {
        r.op = AAR_OP_FILE_STAT;
        r.id = 1000 + i;
        r.arg[0] = i;
        CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_OK);
    }
    CHECK_EQ(aar_queue_used(&sq, &used), AAR_OK);
    CHECK_EQ(used, 64);
    CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_E_BUSY);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_TAIL), 64);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_HEAD), 0);

    /* Pop until empty, FIFO. */
    for (i = 0; i < 64; i++) {
        aar_request got;

        CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
        aar_entry_get_request(slot, &got);
        CHECK_EQ(got.id, 1000 + i);
        CHECK_EQ(got.arg[0], i);
        CHECK_EQ(got.op, AAR_OP_FILE_STAT);
        aar_queue_pop(&sq, 0);
    }
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 0);
    CHECK_EQ(aar_queue_used(&sq, &used), AAR_OK);
    CHECK_EQ(used, 0);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_HEAD), 64);

    /* One slot frees up, one post fits again. */
    CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_OK);
    /* The CQ is independent of the SQ. */
    CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 0);
}

static void test_message_reserve(void)
{
    aar_queue sq, cq;
    uint8_t msg[AAR_MSG_BYTES];
    uint8_t got[AAR_MSG_BYTES];
    aar_completion c;
    const volatile uint8_t *slot = 0;
    unsigned i;

    clear_mem();
    bind_pair(&sq, &cq);
    memset(msg, 0, sizeof(msg));
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        msg[0] = (uint8_t)i;
        msg[31] = (uint8_t)~i;
        CHECK_EQ(aar_queue_post_msg(&cq, msg, 0, 0), AAR_OK);
    }
    /* msg_capacity is depth - 1: the 64th message is refused ... */
    CHECK_EQ(aar_queue_post_msg(&cq, msg, 0, 0), AAR_E_BUSY);
    /* ... but the entry kept free still takes a completion. */
    memset(&c, 0, sizeof(c));
    c.op = AAR_OP_FILE_CLOSE;
    c.id = 77;
    CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_OK);
    CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_E_BUSY);

    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 1);
        CHECK_EQ(aar_get_be16(slot + AAR_ENTRY_OP), AAR_OP_MSG);
        aar_entry_get_msg(slot, got);
        CHECK(got[0] == (uint8_t)i && got[31] == (uint8_t)~i);
        aar_queue_pop(&cq, 0);
    }
    CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 1);
    CHECK_EQ(aar_get_be16(slot + AAR_ENTRY_OP), AAR_OP_FILE_CLOSE);
    aar_queue_pop(&cq, 0);
    CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 0);
}

static void test_wraparound(void)
{
    aar_queue sq, cq;
    aar_request r;
    const volatile uint8_t *slot = 0;
    uint32_t start[] = { 0xFFFFFFF0u, 0xFFFFFFFFu, 0xFFFFFFC0u, 0x7FFFFFF8u };
    unsigned s;
    unsigned i;

    memset(&r, 0, sizeof(r));
    for (s = 0; s < sizeof(start) / sizeof(start[0]); s++) {
        uint32_t expect_tail;
        uint32_t used = 0;

        clear_mem();
        bind_pair(&sq, &cq);
        /* Start both indices near the top of the 32-bit range. */
        aar_put_be32(mem.b + AAR_CB_SQ_TAIL, start[s]);
        aar_put_be32(mem.b + AAR_CB_SQ_HEAD, start[s]);

        /* Fill it across the wrap, drain it, then run a long steady stream. */
        for (i = 0; i < 64; i++) {
            r.op = AAR_OP_FILE_WRITE;
            r.id = 5000 + i;
            CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_OK);
        }
        CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_E_BUSY);
        CHECK_EQ(aar_queue_used(&sq, &used), AAR_OK);
        CHECK_EQ(used, 64);
        for (i = 0; i < 64; i++) {
            aar_request got;

            CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
            aar_entry_get_request(slot, &got);
            CHECK_EQ(got.id, 5000 + i);
            aar_queue_pop(&sq, 0);
        }
        CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 0);

        for (i = 0; i < 300; i++) {
            aar_request got;

            r.id = 9000 + i;
            CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_OK);
            CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
            aar_entry_get_request(slot, &got);
            CHECK_EQ(got.id, 9000 + i);
            aar_queue_pop(&sq, 0);
        }
        expect_tail = start[s] + 64u + 300u;
        CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_TAIL), expect_tail);
        CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_HEAD), expect_tail);
    }
}

static void test_corrupt_indices(void)
{
    aar_queue sq, cq;
    aar_request r;
    const volatile uint8_t *slot = 0;
    uint32_t used = 7;

    clear_mem();
    bind_pair(&sq, &cq);
    memset(&r, 0, sizeof(r));
    /* head ahead of tail looks like a huge fill. */
    aar_put_be32(mem.b + AAR_CB_SQ_HEAD, 5);
    aar_put_be32(mem.b + AAR_CB_SQ_TAIL, 3);
    CHECK_EQ(aar_queue_used(&sq, &used), AAR_E_IO);
    CHECK_EQ(used, 7);
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), AAR_E_IO);
    CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_E_IO);
    /* tail more than depth ahead. */
    aar_put_be32(mem.b + AAR_CB_SQ_HEAD, 0);
    aar_put_be32(mem.b + AAR_CB_SQ_TAIL, 65);
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), AAR_E_IO);

    /* Depth validation. */
    CHECK(aar_queue_depth_valid(64));
    CHECK(aar_queue_depth_valid(4096));
    CHECK(!aar_queue_depth_valid(0));
    CHECK(!aar_queue_depth_valid(32));
    CHECK(!aar_queue_depth_valid(96));
    CHECK(!aar_queue_depth_valid(8192));
    CHECK_EQ(aar_queue_init(&sq, mem.b, mem.b, mem.b, 100), AAR_E_INVAL);
    CHECK_EQ(aar_queue_init(&sq, 0, mem.b, mem.b, 64), AAR_E_INVAL);
}

static void test_unsolicited(void)
{
    aar_queue sq, cq;
    aar_request r;
    aar_completion c;
    aar_event ev;
    aar_event got_ev;
    const volatile uint8_t *slot = 0;
    uint8_t msg[AAR_MSG_BYTES];
    unsigned i;
    unsigned events = 0;
    unsigned completions = 0;
    unsigned messages = 0;

    clear_mem();
    bind_pair(&sq, &cq);
    memset(&r, 0, sizeof(r));
    memset(msg, 0x5A, sizeof(msg));

    /* The app posts a request; before the client answers it the CQ carries
     * input events and a message. The completion arrives in order behind them
     * and is told apart by op and id. */
    r.op = AAR_OP_DISPLAY_FLIP;
    r.id = 42;
    CHECK_EQ(aar_queue_post_request(&sq, &r, 0, 0), AAR_OK);

    ev.type = AAR_EV_KEY_DOWN;
    ev.code = 0x45;
    ev.dx = 0;
    ev.dy = 0;
    ev.buttons = 0;
    ev.reserved = 0;
    ev.time_ms = 1234;
    CHECK_EQ(aar_queue_post_event(&cq, &ev, 0, 0), AAR_OK);
    ev.type = AAR_EV_STOP;
    ev.time_ms = 1240;
    CHECK_EQ(aar_queue_post_event(&cq, &ev, 0, 0), AAR_OK);
    CHECK_EQ(aar_queue_post_msg(&cq, msg, 0, 0), AAR_OK);
    memset(&c, 0, sizeof(c));
    c.op = AAR_OP_DISPLAY_FLIP;
    c.id = 42;
    c.result = AAR_OK;
    c.out[AAR_R_DISPLAY_FLIP_DROPPED] = 3;
    CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_OK);

    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
    aar_entry_get_request(slot, &r);
    CHECK_EQ(r.id, 42);
    aar_queue_pop(&sq, 0);

    for (i = 0; i < 4; i++) {
        uint16_t op;

        CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 1);
        op = aar_get_be16(slot + AAR_ENTRY_OP);
        if (op == AAR_OP_EVENT) {
            CHECK_EQ(aar_get_be32(slot + AAR_ENTRY_ID), 0);
            aar_entry_get_event(slot, &got_ev);
            CHECK_EQ(got_ev.type, events == 0 ? AAR_EV_KEY_DOWN : AAR_EV_STOP);
            events++;
        } else if (op == AAR_OP_MSG) {
            messages++;
        } else if (op == AAR_OP_DISPLAY_FLIP) {
            aar_completion got;

            aar_entry_get_completion(slot, &got);
            CHECK_EQ(got.id, 42);
            CHECK_EQ(got.result, AAR_OK);
            CHECK_EQ(got.out[AAR_R_DISPLAY_FLIP_DROPPED], 3);
            completions++;
        }
        aar_queue_pop(&cq, 0);
    }
    CHECK_EQ(events, 2);
    CHECK_EQ(messages, 1);
    CHECK_EQ(completions, 1);
    CHECK_EQ(aar_queue_peek(&cq, 0, &slot), 0);

    /* An unsolicited event into a full CQ is refused, never overwritten. */
    for (i = 0; i < 64; i++)
        CHECK_EQ(aar_queue_post_event(&cq, &ev, 0, 0), AAR_OK);
    CHECK_EQ(aar_queue_post_event(&cq, &ev, 0, 0), AAR_E_BUSY);
}

static void test_barriers(void)
{
    aar_queue sq, cq;
    aar_request r;
    const volatile uint8_t *slot = 0;
    volatile uint8_t *wslot;

    clear_mem();
    bind_pair(&sq, &cq);
    memset(&r, 0, sizeof(r));
    r.op = AAR_OP_LOG;
    r.id = 1;

    /* Counts: a post runs acquire once (before reading the tail) and release
     * once (before writing it). */
    acquire_calls = release_calls = 0;
    CHECK_EQ(aar_queue_post_request(&sq, &r, count_acquire, count_release), AAR_OK);
    CHECK_EQ(acquire_calls, 1);
    CHECK_EQ(release_calls, 1);
    /* A peek runs acquire once, a pop release once. */
    acquire_calls = release_calls = 0;
    CHECK_EQ(aar_queue_peek(&sq, count_acquire, &slot), 1);
    CHECK_EQ(acquire_calls, 1);
    CHECK_EQ(release_calls, 0);
    aar_queue_pop(&sq, count_release);
    CHECK_EQ(release_calls, 1);
    /* An empty peek still runs acquire; a refused post never runs release. */
    acquire_calls = release_calls = 0;
    CHECK_EQ(aar_queue_peek(&sq, count_acquire, &slot), 0);
    CHECK_EQ(acquire_calls, 1);

    /* Order on the producer side: entry written, tail unchanged, then the
     * release hook, then the tail. */
    probe_queue = &sq;
    probe_tail_before = aar_get_be32(sq.tail);
    probe_order_ok = 0;
    CHECK_EQ(aar_queue_prepare(&sq, 0, 0, &wslot), AAR_OK);
    aar_entry_put_request(wslot, &r);
    aar_queue_commit(&sq, release_before_tail);
    CHECK_EQ(probe_order_ok, 1);
    CHECK_EQ(aar_get_be32(sq.tail), probe_tail_before + 1);

    /* Order on the consumer side: the release hook runs before the head
     * moves. */
    probe_head_before = aar_get_be32(sq.head);
    probe_order_ok = 0;
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
    aar_queue_pop(&sq, release_before_head);
    CHECK_EQ(probe_order_ok, 1);
    CHECK_EQ(aar_get_be32(sq.head), probe_head_before + 1);

    /* A prepared but uncommitted slot is invisible to the consumer. */
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 0);
    CHECK_EQ(aar_queue_prepare(&sq, 0, 0, &wslot), AAR_OK);
    aar_entry_put_request(wslot, &r);
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 0);
    aar_queue_commit(&sq, 0);
    CHECK_EQ(aar_queue_peek(&sq, 0, &slot), 1);
}

static void test_control_block(void)
{
    aar_queue sq, cq;
    uint32_t handle = aar_handle_make(0x0102, 0x0304);

    clear_mem();
    CHECK_EQ(aar_cb_check(mem.b), AAR_E_IO); /* zeroed: no magic yet */

    CHECK_EQ(aar_cb_init(mem.b, handle, AAR_PROFILE_A64, 64, 0xC0DE0000u,
                         0x1000, 0xC0DE0000u, 0x2000), AAR_OK);
    CHECK_EQ(aar_cb_check(mem.b), AAR_OK);
    CHECK(mem.b[0] == 'A' && mem.b[1] == 'A' && mem.b[2] == 'R' && mem.b[3] == 'C');
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_VERSION), 1);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SIZE), 0x1000);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_HANDLE), handle);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_PROFILE), AAR_PROFILE_A64);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_TOKEN), 0xC0DE0000u);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_OFFSET), 0x1000);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_DEPTH), 64);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_ENTRY_BYTES), 64);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_CQ_DESC + AAR_QD_OFFSET), 0x2000);
    /* Big-endian on the wire: the handle's bytes. */
    CHECK(mem.b[AAR_CB_HANDLE] == 0x03 && mem.b[AAR_CB_HANDLE + 1] == 0x04 &&
          mem.b[AAR_CB_HANDLE + 2] == 0x01 && mem.b[AAR_CB_HANDLE + 3] == 0x02);
    /* Everything else is zero. */
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_STATE), 0);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_MSG_CONSUMED), 0);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_SQ_TAIL), 0);
    CHECK_EQ(aar_get_be32(mem.b + AAR_CB_CQ_HEAD), 0);
    CHECK_EQ(aar_get_be32(mem.b + 0xFFC), 0);

    CHECK_EQ(aar_cb_queue_bind(&sq, mem.b, ring_sq(), AAR_QUEUE_SQ), AAR_OK);
    CHECK_EQ(aar_cb_queue_bind(&cq, mem.b, ring_cq(), AAR_QUEUE_CQ), AAR_OK);
    CHECK_EQ(sq.depth, 64);
    CHECK(sq.head == mem.b + AAR_CB_SQ_HEAD && sq.tail == mem.b + AAR_CB_SQ_TAIL);
    CHECK(cq.head == mem.b + AAR_CB_CQ_HEAD && cq.tail == mem.b + AAR_CB_CQ_TAIL);
    CHECK_EQ(aar_cb_queue_bind(&sq, mem.b, ring_sq(), 2), AAR_E_INVAL);

    /* A bad depth is refused at init and, if the block was corrupted, at bind. */
    CHECK_EQ(aar_cb_init(mem.b, handle, AAR_PROFILE_A32, 48, 0, 0, 0, 0), AAR_E_INVAL);
    aar_put_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_DEPTH, 48);
    CHECK_EQ(aar_cb_queue_bind(&sq, mem.b, ring_sq(), AAR_QUEUE_SQ), AAR_E_IO);
    aar_put_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_DEPTH, 64);
    aar_put_be32(mem.b + AAR_CB_SQ_DESC + AAR_QD_ENTRY_BYTES, 32);
    CHECK_EQ(aar_cb_queue_bind(&sq, mem.b, ring_sq(), AAR_QUEUE_SQ), AAR_E_IO);
    aar_put_be32(mem.b + AAR_CB_VERSION, 2);
    CHECK_EQ(aar_cb_check(mem.b), AAR_E_IO);
    aar_put_be32(mem.b + AAR_CB_VERSION, 1);
    aar_put_be32(mem.b + AAR_CB_SIZE, 0x800);
    CHECK_EQ(aar_cb_check(mem.b), AAR_E_IO);

    /* The depth can differ per block: a 128-deep pair reports capacity 127. */
    {
        static union {
            uint8_t b[0x1000 + 2 * 128 * 64];
            uint32_t w[(0x1000 + 2 * 128 * 64) / 4];
        } big;
        aar_queue bsq, bcq;
        aar_request r;
        unsigned i;
        int rc = AAR_OK;

        memset(&big, 0, sizeof(big));
        CHECK_EQ(aar_cb_init(big.b, handle, AAR_PROFILE_A32, 128, 1,
                             AAR_PAIR_SQ_OFFSET, 1, aar_pair_cq_offset(128)),
                 AAR_OK);
        CHECK_EQ(aar_cb_queue_bind(&bsq, big.b, big.b + AAR_PAIR_SQ_OFFSET,
                                   AAR_QUEUE_SQ), AAR_OK);
        CHECK_EQ(aar_cb_queue_bind(&bcq, big.b, big.b + aar_pair_cq_offset(128),
                                   AAR_QUEUE_CQ), AAR_OK);
        CHECK_EQ(bsq.depth, 128);
        CHECK_EQ(AAR_MSG_CAPACITY_FOR(bsq.depth), 127);
        memset(&r, 0, sizeof(r));
        r.op = AAR_OP_LOG;
        for (i = 0; i < 128 && rc == AAR_OK; i++)
            rc = aar_queue_post_request(&bsq, &r, 0, 0);
        CHECK_EQ(i, 128);
        CHECK_EQ(aar_queue_post_request(&bsq, &r, 0, 0), AAR_E_BUSY);
        (void)bcq;
    }
}

/* ---- Message credits ------------------------------------------------------------------ */

/* The platform side of the client-to-app direction: a pump that moves entries
 * from the CQ into a bounded inbox, the way the runtime does. A message that
 * finds the inbox full is left in the CQ and the pump stops (a stall): the
 * completion behind it is then unreachable. The app consumes from the inbox and
 * the platform publishes the count in AAR_CB_MSG_CONSUMED. */
struct platform {
    uint32_t inbox[AAR_MSG_CAPACITY]; /* sequence numbers, a FIFO */
    unsigned in_head, in_n;
    uint32_t consumed;     /* free-running, mirrors AAR_CB_MSG_CONSUMED */
    unsigned stalls;       /* times a message found the inbox full */
    unsigned completions;  /* completions the pump reached */
    uint32_t last_id;
    unsigned events;
};

static void platform_start(struct platform *p, uint32_t consumed)
{
    memset(p, 0, sizeof(*p));
    p->consumed = consumed;
    aar_put_be32(mem.b + AAR_CB_MSG_CONSUMED, consumed);
}

/* Runs until the CQ is empty or a message cannot be taken. */
static void platform_pump(struct platform *p, const aar_queue *cq)
{
    const volatile uint8_t *slot;

    while (aar_queue_peek(cq, 0, &slot) == 1) {
        unsigned op = aar_get_be16(slot + AAR_ENTRY_OP);

        if (op == AAR_OP_MSG) {
            uint8_t m[AAR_MSG_BYTES];

            if (p->in_n == AAR_MSG_CAPACITY) {
                p->stalls++;
                return;
            }
            aar_entry_get_msg(slot, m);
            p->inbox[(p->in_head + p->in_n) % AAR_MSG_CAPACITY] =
                (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 |
                (uint32_t)m[3] << 24;
            p->in_n++;
        } else if (op == AAR_OP_EVENT) {
            p->events++;
        } else {
            p->completions++;
            p->last_id = aar_get_be32(slot + AAR_ENTRY_ID);
        }
        aar_queue_pop(cq, 0);
    }
}

/* The app takes one message from the inbox; the platform publishes the count. */
static int platform_app_take(struct platform *p, uint32_t *seq)
{
    if (p->in_n == 0u)
        return 0;
    *seq = p->inbox[p->in_head];
    p->in_head = (p->in_head + 1u) % AAR_MSG_CAPACITY;
    p->in_n--;
    p->consumed++;
    aar_put_be32(mem.b + AAR_CB_MSG_CONSUMED, p->consumed);
    return 1;
}

static void msg_with_seq(uint8_t msg[AAR_MSG_BYTES], uint32_t seq)
{
    memset(msg, 0, AAR_MSG_BYTES);
    msg[0] = (uint8_t)seq;
    msg[1] = (uint8_t)(seq >> 8);
    msg[2] = (uint8_t)(seq >> 16);
    msg[3] = (uint8_t)(seq >> 24);
}

static void test_credit_arithmetic(void)
{
    /* The arithmetic is modulo 2^32, so it holds across the wrap. */
    CHECK_EQ(aar_msg_outstanding(0, 0, 63), 0);
    CHECK_EQ(aar_msg_credits_left(0, 0, 63), 63);
    CHECK_EQ(aar_msg_credits_left(62, 0, 63), 1);
    CHECK_EQ(aar_msg_credits_left(63, 0, 63), 0);
    CHECK(aar_msg_credit_ok(62, 0, 63));
    CHECK(!aar_msg_credit_ok(63, 0, 63));
    CHECK_EQ(aar_msg_credits_left(100, 40, 63), 3);
    CHECK_EQ(aar_msg_credits_left(100, 37, 63), 0);

    /* posted has wrapped past 2^32, consumed has not. */
    CHECK_EQ(aar_msg_outstanding(0x0000000Au, 0xFFFFFFF0u, 63), 26);
    CHECK_EQ(aar_msg_credits_left(0x0000000Au, 0xFFFFFFF0u, 63), 37);
    CHECK(aar_msg_credit_ok(0x0000000Au, 0xFFFFFFF0u, 63));
    CHECK_EQ(aar_msg_outstanding(0x0000001Eu, 0xFFFFFFF0u, 63), 46);
    CHECK_EQ(aar_msg_credits_left(0x0000002Eu, 0xFFFFFFF0u, 63), 1);
    CHECK_EQ(aar_msg_credits_left(0x0000002Fu, 0xFFFFFFF0u, 63), 0);
    CHECK(aar_msg_credit_ok(0x0000002Eu, 0xFFFFFFF0u, 63));
    CHECK(!aar_msg_credit_ok(0x0000002Fu, 0xFFFFFFF0u, 63));
    /* The last credit on either side of the wrap. */
    CHECK(aar_msg_credit_ok(0xFFFFFFFFu, 0xFFFFFFFFu - 62u, 63));
    CHECK(!aar_msg_credit_ok(0u, 0xFFFFFFFFu - 62u, 63));
    CHECK(aar_msg_credit_ok(1u, 0xFFFFFFFFu - 60u, 63));
    CHECK(!aar_msg_credit_ok(2u, 0xFFFFFFFFu - 60u, 63));

    /* Another depth: capacity is depth - 1. */
    CHECK_EQ(aar_msg_credits_left(100, 0, AAR_MSG_CAPACITY_FOR(128)), 27);
    CHECK(!aar_msg_credit_ok(127, 0, AAR_MSG_CAPACITY_FOR(128)));
    CHECK(aar_msg_credit_ok(126, 0, AAR_MSG_CAPACITY_FOR(128)));

    /* A consumed word ahead of posted, or more outstanding than the capacity,
     * is corrupt: never a credit. */
    CHECK_EQ(aar_msg_outstanding(5, 6, 63), AAR_E_IO);
    CHECK_EQ(aar_msg_credits_left(5, 6, 63), AAR_E_IO);
    CHECK(!aar_msg_credit_ok(5, 6, 63));
    CHECK_EQ(aar_msg_credits_left(64, 0, 63), AAR_E_IO);
    CHECK(!aar_msg_credit_ok(64, 0, 63));
}

static void test_credit_post(void)
{
    aar_queue sq, cq;
    uint8_t msg[AAR_MSG_BYTES];
    uint32_t posted;
    uint32_t tail;
    unsigned i;
    int pass;

    /* Once from zero and once with both counts about to wrap. */
    for (pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 0xFFFFFFE0u : 0u;
        struct platform p;
        aar_completion c;

        clear_mem();
        bind_pair(&sq, &cq);
        platform_start(&p, start);
        posted = start;

        /* 63 messages, then the credit is spent: BUSY, queue untouched. */
        for (i = 0; i < AAR_MSG_CAPACITY; i++) {
            msg_with_seq(msg, i);
            CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_OK);
        }
        CHECK_EQ(posted, start + AAR_MSG_CAPACITY);
        tail = aar_get_be32(mem.b + AAR_CB_CQ_TAIL);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_E_BUSY);
        CHECK_EQ(posted, start + AAR_MSG_CAPACITY);
        CHECK_EQ(aar_get_be32(mem.b + AAR_CB_CQ_TAIL), tail);

        /* A completion still fits behind 63 messages and is reached with the app
         * consuming nothing: the inbox takes all 63 and never stalls. */
        memset(&c, 0, sizeof(c));
        c.op = AAR_OP_FILE_CLOSE;
        c.id = 4242;
        CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_OK);
        platform_pump(&p, &cq);
        CHECK_EQ(p.stalls, 0);
        CHECK_EQ(p.completions, 1);
        CHECK_EQ(p.last_id, 4242);
        CHECK_EQ(p.in_n, AAR_MSG_CAPACITY);

        /* The queue is empty again, yet the credit is still spent: it returns
         * only when the app consumes, not when the queue drains. */
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_E_BUSY);
        {
            uint32_t seq = 0;

            CHECK(platform_app_take(&p, &seq));
            CHECK_EQ(seq, 0);
        }
        CHECK_EQ(aar_cb_msg_consumed(mem.b), start + 1u);
        msg_with_seq(msg, 1000);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_OK);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_E_BUSY);
        platform_pump(&p, &cq);
        CHECK_EQ(p.stalls, 0);
        CHECK_EQ(p.in_n, AAR_MSG_CAPACITY);

        /* A corrupt consumed word stops posts with AAR_E_IO. */
        aar_put_be32(mem.b + AAR_CB_MSG_CONSUMED, posted + 1u);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_E_IO);
    }
}

/* Without credits a client can fill the inbox and then queue a completion
 * behind more messages: the platform cannot reach it. With credits it can. */
static void test_credit_head_of_line(void)
{
    aar_queue sq, cq;
    struct platform p;
    uint8_t msg[AAR_MSG_BYTES];
    aar_completion c;
    uint32_t posted = 0;
    unsigned i;

    memset(&c, 0, sizeof(c));
    c.op = AAR_OP_CONSOLE_READ;
    c.id = 9;

    /* No credits: the hazard. */
    clear_mem();
    bind_pair(&sq, &cq);
    platform_start(&p, 0);
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        msg_with_seq(msg, i);
        CHECK_EQ(aar_queue_post_msg(&cq, msg, 0, 0), AAR_OK);
    }
    platform_pump(&p, &cq);
    CHECK_EQ(p.in_n, AAR_MSG_CAPACITY);
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        msg_with_seq(msg, 100 + i);
        CHECK_EQ(aar_queue_post_msg(&cq, msg, 0, 0), AAR_OK);
    }
    CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_OK);
    platform_pump(&p, &cq);
    CHECK(p.stalls > 0);
    CHECK_EQ(p.completions, 0); /* unreachable behind the messages */

    /* With credits the second batch is refused, so the completion is reached. */
    clear_mem();
    bind_pair(&sq, &cq);
    platform_start(&p, 0);
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        msg_with_seq(msg, i);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_OK);
    }
    platform_pump(&p, &cq);
    CHECK_EQ(p.in_n, AAR_MSG_CAPACITY);
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        msg_with_seq(msg, 100 + i);
        CHECK_EQ(aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0), AAR_E_BUSY);
    }
    CHECK_EQ(aar_queue_post_completion(&cq, &c, 0, 0), AAR_OK);
    platform_pump(&p, &cq);
    CHECK_EQ(p.stalls, 0);
    CHECK_EQ(p.completions, 1);
    CHECK_EQ(p.last_id, 9);
}

/* A long randomised run of a client that respects credits against the pump.
 * The app consumes at random but, while it waits for a completion, not at all.
 * Invariants: the inbox never overflows, the pump never stalls, messages come
 * out in order, and every completion is reached while nothing is consumed. */
static void test_credit_model_run(void)
{
    static const uint32_t starts[] = {0u, 0xFFFFFF00u, 0xFFFFFFC1u};
    unsigned s;

    for (s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
        aar_queue sq, cq;
        struct platform p;
        uint8_t msg[AAR_MSG_BYTES];
        aar_completion c;
        uint32_t posted = starts[s];
        uint32_t next_seq = 0, expect_seq = 0, id = 1;
        uint32_t rng = 0x12345678u + s;
        unsigned step, done_completions = 0, wanted_completions = 0;
        unsigned busy = 0, posted_msgs = 0;

        clear_mem();
        bind_pair(&sq, &cq);
        platform_start(&p, starts[s]);
        memset(&c, 0, sizeof(c));
        c.op = AAR_OP_FILE_READ;

        for (step = 0; step < 200000; step++) {
            unsigned r;
            uint32_t inflight;

            rng = rng * 1664525u + 1013904223u;
            r = (rng >> 16) & 0xFFu;

            if (r < 140u) {
                /* The client posts a message when it can. */
                int rc;

                msg_with_seq(msg, next_seq);
                rc = aar_cb_post_msg(&cq, mem.b, &posted, msg, 0, 0);
                CHECK(rc == AAR_OK || rc == AAR_E_BUSY);
                if (rc == AAR_OK) {
                    next_seq++;
                    posted_msgs++;
                } else {
                    busy++;
                }
            } else if (r < 160u) {
                aar_event ev;

                memset(&ev, 0, sizeof(ev));
                ev.type = AAR_EV_MOUSE;
                (void)aar_queue_post_event(&cq, &ev, 0, 0);
            } else if (r < 200u) {
                /* The app asks for something and waits: it consumes nothing
                 * until the completion is reached. */
                c.id = id++;
                if (aar_queue_post_completion(&cq, &c, 0, 0) == AAR_OK) {
                    unsigned before = p.completions;

                    wanted_completions++;
                    platform_pump(&p, &cq);
                    CHECK_EQ(p.completions, before + 1u);
                    CHECK_EQ(p.last_id, c.id);
                    done_completions++;
                }
            } else if (r < 230u) {
                platform_pump(&p, &cq);
            } else {
                uint32_t seq = 0;
                unsigned k = r & 3u;

                while (k-- && platform_app_take(&p, &seq)) {
                    CHECK_EQ(seq, expect_seq);
                    expect_seq++;
                }
            }

            /* Messages in the CQ plus the inbox never exceed the capacity. */
            inflight = posted - p.consumed;
            CHECK(inflight <= AAR_MSG_CAPACITY);
            CHECK_EQ(p.stalls, 0);
            CHECK(p.in_n <= AAR_MSG_CAPACITY);
        }
        CHECK_EQ(done_completions, wanted_completions);
        CHECK(wanted_completions > 1000u);
        CHECK(posted_msgs > 1000u);
        CHECK(busy > 100u); /* the credit limit was really reached */
    }
}

int main(void)
{
    test_handles();
    test_be_helpers();
    test_entry_layout();
    test_post_until_full();
    test_message_reserve();
    test_wraparound();
    test_corrupt_indices();
    test_unsolicited();
    test_barriers();
    test_control_block();
    test_credit_arithmetic();
    test_credit_post();
    test_credit_head_of_line();
    test_credit_model_run();
    printf("transport_model_test: %lu checks, %lu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
