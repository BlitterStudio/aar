/* Internal bookkeeping shared by runtimes. Synchronize at the frame owner
 * transition; this queue does not take locks or allocate memory. */
/* SPDX-License-Identifier: MIT */
#ifndef AAR_DETAIL_PRESENT_FEEDBACK_H
#define AAR_DETAIL_PRESENT_FEEDBACK_H

#include <aar/app.h>

#define AAR_PRESENT_QUEUE_BITS 4u
#define AAR_PRESENT_QUEUE_CAPACITY (1u << AAR_PRESENT_QUEUE_BITS)
#define AAR_PRESENT_QUEUE_MASK (AAR_PRESENT_QUEUE_CAPACITY - 1u)
#define AAR_PRESENT_QUEUE_ALL ((1u << AAR_PRESENT_QUEUE_CAPACITY) - 1u)

typedef struct aar_present_queue {
    uint64_t sequence;
    uint32_t used;
    uint32_t pending;
    unsigned head, count;
    uint8_t ready[AAR_PRESENT_QUEUE_CAPACITY];
    aar_present_result frames[AAR_PRESENT_QUEUE_CAPACITY];
} aar_present_queue;

static inline uint64_t aar_present_reserve(aar_present_queue *q)
{
    uint32_t free_slots = AAR_PRESENT_QUEUE_ALL & ~q->used;
    unsigned slot;
    uint32_t bit;
    uint64_t id;

    if (!free_slots || q->sequence == (UINT64_MAX >> AAR_PRESENT_QUEUE_BITS))
        return 0;
    slot = (unsigned)__builtin_ctz(free_slots);
    bit = 1u << slot;
    id = (++q->sequence << AAR_PRESENT_QUEUE_BITS) | slot;
    q->frames[slot].frame_id = id;
    q->used |= bit;
    q->pending |= bit;
    return id;
}

static inline void aar_present_discard(aar_present_queue *q, uint64_t id)
{
    unsigned slot = (unsigned)id & AAR_PRESENT_QUEUE_MASK;
    uint32_t bit = 1u << slot;

    if ((q->pending & bit) && q->frames[slot].frame_id == id) {
        q->used &= ~bit;
        q->pending &= ~bit;
    }
}

static inline void aar_present_finish(aar_present_queue *q, uint64_t id,
                                      uint32_t outcome, uint32_t reason,
                                      uint64_t completed_us)
{
    unsigned slot = (unsigned)id & AAR_PRESENT_QUEUE_MASK;
    uint32_t bit = 1u << slot;
    aar_present_result *record = &q->frames[slot];

    if (!(q->pending & bit) || record->frame_id != id)
        return;
    record->completed_us = completed_us;
    record->outcome = outcome;
    record->reason = reason;
    q->pending &= ~bit;
    q->ready[(q->head + q->count) & AAR_PRESENT_QUEUE_MASK] = (uint8_t)slot;
    ++q->count;
}

static inline int aar_present_poll(aar_present_queue *q, aar_present_result *out)
{
    unsigned slot;

    if (!q->count)
        return 0;
    slot = q->ready[q->head];
    *out = q->frames[slot];
    q->used &= ~(1u << slot);
    q->head = (q->head + 1u) & AAR_PRESENT_QUEUE_MASK;
    --q->count;
    return 1;
}

static inline void aar_present_cancel(aar_present_queue *q, uint64_t completed_us)
{
    while (q->pending) {
        unsigned slot = (unsigned)__builtin_ctz(q->pending);
        aar_present_finish(q, q->frames[slot].frame_id, AAR_PRESENT_UNCONFIRMED,
                           AAR_PRESENT_REASON_CANCELLED, completed_us);
    }
}

#endif
