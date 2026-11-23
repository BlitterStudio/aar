/*
 * AAR transport contract (spec section 10): handles, the control block, the
 * request/completion queue pair, the op codes, and a reference model of the
 * producer and consumer that the firmware, the Amiga library and the host
 * tests share.
 *
 * Per app, in shared memory that both sides reach:
 *   - one control block (AAR_CB_BYTES, 4 KiB): lifecycle words, fault record,
 *     limits, accounting, and the descriptors and indices of two queues;
 *   - one submission queue (SQ, app side to client) and one completion queue
 *     (CQ, client to app side), each `depth` entries of AAR_ENTRY_BYTES (64)
 *     bytes, depth a power of two (64 by default);
 *   - large payloads travel as shared-buffer tokens (aar_shared.haddr), never
 *     inline.
 * A doorbell in each direction is a coalescing hint only. The queue indices are
 * authoritative: a receiver always re-reads them, and a missed or merged ring
 * never loses an entry.
 *
 * Byte order: every word in the control block and in a queue entry is
 * BIG-ENDIAN, so the 68k reads native longs. The ARM side uses the byte
 * reversing accessors below. 64-bit values are two words, high word first.
 * Nothing here is a C structure over shared memory: layouts are byte offsets,
 * so they hold for any compiler and either profile. (Service buffers named by
 * an entry, the audio ring in particular, have their own rules and are
 * described where they are defined.)
 *
 * Ordering rules (the reference model implements them):
 *   Producer: reserve a slot, write the entry, release barrier, write the tail.
 *   Consumer: read the tail, acquire barrier, read the entry, release barrier,
 *             write the head.
 * The release barrier before the head write is stricter than the one-line rule
 * of spec 10.4. ARMv7 and ARMv8 may reorder a load with a later store to a
 * different address, so without it the producer could see the freed slot and
 * overwrite an entry the consumer is still reading. On the 68k the hooks are
 * NULL or a no-op (the 68k does not reorder).
 *
 * Index naming follows spec 10.3: the producer writes the TAIL, the consumer
 * writes the HEAD. Both are free-running uint32_t that wrap; used = tail - head.
 * The slot of index n is n & (depth - 1).
 *
 * Every function below is a pure inline function: no OS calls, no allocation.
 * The barriers are caller-supplied hooks (a NULL hook is "no barrier").
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_TRANSPORT_H
#define AAR_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Handles (spec 10.2) ----------------------------------------------------- */

/*
 * A handle is a uint32_t: the process-record index in the low 16 bits and a
 * generation in the high 16 bits. Handle 0 is invalid. Generation 0 is never
 * issued: a platform that reuses an index advances the generation with
 * aar_handle_next_gen (0xFFFF wraps to 1), so every issued handle is non-zero.
 * A request that names a handle whose generation is not the live one fails
 * with AAR_E_STALE and changes nothing.
 */
#define AAR_HANDLE_INDEX_BITS 16u
#define AAR_HANDLE_INDEX_MASK 0x0000FFFFu
#define AAR_HANDLE_GEN_MAX    0xFFFFu

static inline uint32_t aar_handle_make(uint32_t index, uint32_t generation)
{
    return ((generation & AAR_HANDLE_GEN_MAX) << AAR_HANDLE_INDEX_BITS) |
           (index & AAR_HANDLE_INDEX_MASK);
}

static inline uint32_t aar_handle_index(uint32_t handle)
{
    return handle & AAR_HANDLE_INDEX_MASK;
}

static inline uint32_t aar_handle_generation(uint32_t handle)
{
    return handle >> AAR_HANDLE_INDEX_BITS;
}

/* The generation that follows `generation`: 1 .. 0xFFFF, then 1 again. Any
 * input, including 0, yields a value in 1 .. 0xFFFF. */
static inline uint32_t aar_handle_next_gen(uint32_t generation)
{
    generation &= AAR_HANDLE_GEN_MAX;
    return generation == AAR_HANDLE_GEN_MAX || generation == 0u ? 1u
                                                                : generation + 1u;
}

/* Checks a handle from a request against the generation that is live for its
 * index (0 when the index is free). AAR_OK when they match; AAR_E_INVAL for
 * handle 0 or generation 0, which are never issued; AAR_E_STALE otherwise. */
static inline int aar_handle_check(uint32_t handle, uint32_t live_generation)
{
    uint32_t generation = aar_handle_generation(handle);

    if (handle == 0u || generation == 0u)
        return AAR_E_INVAL;
    return generation == (live_generation & AAR_HANDLE_GEN_MAX) ? AAR_OK
                                                                 : AAR_E_STALE;
}

/* ---- Control block layout (spec 10.3) -------------------------------------------------- */

/*
 * Offsets are bytes from the start of the control block. Every word has one
 * writer. A word that changes hands (a queue index, the lease counter) sits at
 * the start of its own 64-byte line. 64-bit values are _HI then _LO.
 * Unlisted space is reserved and zero. The block is zero-filled at admission.
 */
#define AAR_CB_BYTES          0x1000u
#define AAR_CB_LINE_BYTES     64u
#define AAR_CB_MAGIC_VALUE    0x41415243u /* 'AARC' */
#define AAR_CB_LAYOUT_VERSION 1u

/* Header line. Writer: platform, at admission. */
#define AAR_CB_MAGIC          0x000u
#define AAR_CB_VERSION        0x004u /* AAR_CB_LAYOUT_VERSION */
#define AAR_CB_SIZE           0x008u /* bytes of this block */
#define AAR_CB_HANDLE         0x00Cu
#define AAR_CB_PROFILE        0x010u /* AAR_PROFILE_* */

/* Lifecycle line. Writer: platform. */
#define AAR_CB_STATE          0x040u /* AAR_STATE_* */
#define AAR_CB_END_REASON     0x044u /* AAR_END_* */
#define AAR_CB_EXIT_CODE      0x048u /* int32 */
#define AAR_CB_FLAGS          0x04Cu /* AAR_CBF_* */
#define AAR_CB_PROGRESS_HI    0x050u /* load progress, bytes, advances every step */
#define AAR_CB_PROGRESS_LO    0x054u
#define AAR_CB_LEASE_PERIOD_MS 0x058u
#define AAR_CB_MSG_CONSUMED   0x05Cu /* client-to-app messages the app has taken from its inbox, free-running */

/* Requests from the client. Writer: client. */
#define AAR_CB_STOP_REQUEST   0x080u /* non-zero asks the app to stop */
#define AAR_CB_CLIENT_PRESENT 0x084u

/* Lease and client accounting. Writer: client. The renewal counter is
 * advanced from a timer or interrupt context; the platform only reads it.
 * The two counters are the client's own tally of what it could not post. */
#define AAR_CB_LEASE_COUNTER  0x0C0u
#define AAR_CB_EVENTS_DROPPED 0x0C4u /* events the client found the CQ too full to take */
#define AAR_CB_CQ_FULL        0x0C8u /* times the client found the CQ full */

/* Fault record. Writer: platform. Valid once state is FAULTED or END_REASON is
 * AAR_END_USER_FAULT. Addresses are app virtual addresses; on A32 the high
 * word is 0. syndrome is the FSR (A32) or the low 32 bits of ESR_EL1 (A64);
 * pstate is the CPSR (A32) or the low 32 bits of PSTATE (A64). */
#define AAR_CB_FAULT_CLASS    0x100u /* AAR_FAULT_* */
#define AAR_CB_FAULT_SYNDROME 0x104u
#define AAR_CB_FAULT_PSTATE   0x108u
#define AAR_CB_FAULT_PC_HI    0x110u
#define AAR_CB_FAULT_PC_LO    0x114u
#define AAR_CB_FAULT_SP_HI    0x118u
#define AAR_CB_FAULT_SP_LO    0x11Cu
#define AAR_CB_FAULT_LR_HI    0x120u
#define AAR_CB_FAULT_LR_LO    0x124u
#define AAR_CB_FAULT_ADDR_HI  0x128u
#define AAR_CB_FAULT_ADDR_LO  0x12Cu
#define AAR_CB_LOAD_BASE_HI   0x130u
#define AAR_CB_LOAD_BASE_LO   0x134u

/* Limits. Writer: client, before the load begins; the platform reads them at
 * load begin. 0 selects the default. The shared limit is the limit on shared
 * (host-visible) memory, including the runtime's service buffers. */
#define AAR_CB_PRIVATE_MIB    0x140u
#define AAR_CB_SHARED_MIB     0x144u
#define AAR_CB_WEIGHT         0x148u /* CPU share weight, default 1 */

/* Accounting. Writer: platform. */
#define AAR_CB_SHARED_USED_HI 0x180u
#define AAR_CB_SHARED_USED_LO 0x184u
#define AAR_CB_PRIVATE_COMMITTED_HI 0x188u
#define AAR_CB_PRIVATE_COMMITTED_LO 0x18Cu
#define AAR_CB_SQ_FULL        0x190u /* times a post found the SQ full */
#define AAR_CB_LOG_DROPPED    0x194u /* log records dropped because the SQ was full */

/* Lifecycle flags, AAR_CB_FLAGS. */
#define AAR_CBF_DISPLAY       (1u << 0) /* a display is open */
#define AAR_CBF_AUDIO         (1u << 1) /* an audio stream is open */
#define AAR_CBF_GL            (1u << 2) /* a GL context is bound */
#define AAR_CBF_CONSOLE       (1u << 3) /* the console has been used */

/*
 * Queue descriptor and indices. Queue 0 is the SQ, queue 1 the CQ. Per queue:
 *   desc line   (writer: platform at admission): where the ring is, how deep
 *   tail line   (writer: the producer)
 *   head line   (writer: the consumer)
 * The ring is `entry_bytes * depth` bytes at `offset` inside the shared buffer
 * named by `token`. The default pair layout (below) puts both rings in the same
 * buffer as the control block, so both tokens are the control block's.
 */
#define AAR_QUEUE_SQ 0u
#define AAR_QUEUE_CQ 1u
#define AAR_CB_Q_STRIDE 0xC0u
#define AAR_CB_Q_DESC(q) (0x1C0u + (q) * AAR_CB_Q_STRIDE)
#define AAR_CB_Q_TAIL(q) (0x200u + (q) * AAR_CB_Q_STRIDE)
#define AAR_CB_Q_HEAD(q) (0x240u + (q) * AAR_CB_Q_STRIDE)

#define AAR_CB_SQ_DESC AAR_CB_Q_DESC(AAR_QUEUE_SQ) /* 0x1C0 */
#define AAR_CB_SQ_TAIL AAR_CB_Q_TAIL(AAR_QUEUE_SQ) /* 0x200: app side writes */
#define AAR_CB_SQ_HEAD AAR_CB_Q_HEAD(AAR_QUEUE_SQ) /* 0x240: client writes */
#define AAR_CB_CQ_DESC AAR_CB_Q_DESC(AAR_QUEUE_CQ) /* 0x280 */
#define AAR_CB_CQ_TAIL AAR_CB_Q_TAIL(AAR_QUEUE_CQ) /* 0x2C0: client writes */
#define AAR_CB_CQ_HEAD AAR_CB_Q_HEAD(AAR_QUEUE_CQ) /* 0x300: app side writes */

/* Descriptor word offsets, added to AAR_CB_Q_DESC(q). */
#define AAR_QD_TOKEN       0x00u /* shared-buffer token (haddr) of the ring's buffer */
#define AAR_QD_OFFSET      0x04u /* byte offset of the first entry in that buffer */
#define AAR_QD_DEPTH       0x08u /* entries, a power of two */
#define AAR_QD_ENTRY_BYTES 0x0Cu /* AAR_ENTRY_BYTES */

/* ---- Queue entries (spec 10.4) ------------------------------------------------------------------- */

#define AAR_ENTRY_BYTES        64u
#define AAR_ENTRY_WORDS        16u
#define AAR_QUEUE_DEPTH_DEFAULT 64u
#define AAR_QUEUE_DEPTH_MIN    64u   /* keeps msg_capacity at least 32 */
#define AAR_QUEUE_DEPTH_MAX    4096u
/* Messages per direction: one entry is kept free for the call in flight. */
#define AAR_MSG_RESERVE        1u
#define AAR_MSG_CAPACITY       (AAR_QUEUE_DEPTH_DEFAULT - AAR_MSG_RESERVE)
#define AAR_MSG_CAPACITY_FOR(depth) ((depth) - AAR_MSG_RESERVE)

/* Default layout of a pair in one 4 KiB aligned shared buffer (depth 64): the
 * control block, then the SQ ring, then the CQ ring. For another depth the CQ
 * ring starts at AAR_PAIR_SQ_OFFSET + depth * AAR_ENTRY_BYTES. */
#define AAR_PAIR_CONTROL_OFFSET 0x0000u
#define AAR_PAIR_SQ_OFFSET      0x1000u
#define AAR_PAIR_CQ_OFFSET      0x2000u
#define AAR_PAIR_BYTES          0x3000u
#define AAR_PAIR_ALIGN          0x1000u

/* Entry byte offsets. All fields big-endian. */
#define AAR_ENTRY_OP       0u  /* uint16 AAR_OP_* */
#define AAR_ENTRY_FLAGS    2u  /* uint16; see the op, 0 when none is defined */
#define AAR_ENTRY_ID       4u  /* uint32 chosen by the producer, echoed in the completion */
#define AAR_ENTRY_RESULT   8u  /* int32 AAR_OK or AAR_E_*, completions */
#define AAR_ENTRY_RESERVED 12u /* 0 */
#define AAR_ENTRY_BODY     16u /* 48 bytes */
#define AAR_ENTRY_BODY_BYTES 48u

/* Request body: arg[6] at 16, then buf, buf_off, buf_len. Bytes 52 to 63 are
 * reserved and zero. */
#define AAR_ENTRY_ARG(i)   (16u + (i) * 4u)
#define AAR_ENTRY_ARGS     6u
#define AAR_ENTRY_BUF      40u /* shared-buffer token of the payload, 0 for none */
#define AAR_ENTRY_BUF_OFF  44u /* byte offset inside that buffer */
#define AAR_ENTRY_BUF_LEN  48u /* payload bytes */
/* Completion body: out[6] at 16. Bytes 40 to 63 are reserved and zero. */
#define AAR_ENTRY_OUT(i)   (16u + (i) * 4u)
#define AAR_ENTRY_OUTS     6u
/* A message entry (either direction): AAR_MSG_BYTES opaque bytes at 16, not
 * byte swapped. Bytes 48 to 63 are reserved and zero. */
#define AAR_ENTRY_MSG      16u
/* An input event (CQ, no matching request): the aar_event fields, each
 * big-endian, in this order. */
#define AAR_ENTRY_EV_TYPE     16u /* uint16 */
#define AAR_ENTRY_EV_CODE     18u /* uint16 */
#define AAR_ENTRY_EV_DX       20u /* int16 */
#define AAR_ENTRY_EV_DY       22u /* int16 */
#define AAR_ENTRY_EV_BUTTONS  24u /* uint16 */
#define AAR_ENTRY_EV_RESERVED 26u /* uint16 */
#define AAR_ENTRY_EV_TIME_MS  28u /* uint32 */

/* ---- Op codes ------------------------------------------------------------------------------------------ */

/*
 * An entry's `op` says what it is. A request is an SQ entry that expects a
 * completion: the client replies with a CQ entry that has the same op and id,
 * `result`, and out[]. The app is single-threaded, so at most one request
 * parks it at a time (an asynchronous flip may leave a second in flight).
 * MSG, EVENT and LOG are not requests: they take no completion.
 *
 *   AAR_OP_MSG   either queue. 32 opaque bytes at AAR_ENTRY_MSG. Handled by
 *                AARGetMessage and AARSendMessage, not AARServiceNext. A
 *                message post keeps AAR_MSG_RESERVE entry free. On the CQ the
 *                client also posts only while it has a credit (see Message
 *                credits below): posted - AAR_CB_MSG_CONSUMED < msg_capacity,
 *                else AAR_E_BUSY.
 *   AAR_OP_EVENT CQ only, unsolicited: one event at AAR_ENTRY_EV_*, id 0. The
 *                client posts it with AARPostEvent. A full CQ drops the new
 *                event and the client counts the drop in AAR_CB_EVENTS_DROPPED.
 *                The client may coalesce mouse motion before it posts.
 *   AAR_OP_LOG   SQ only, one-way, lossy. buf, buf_off, buf_len name one
 *                record in a shared log buffer (at most AAR_LOG_MAX bytes, never
 *                wrapping), which the app side may overwrite after the client
 *                has consumed the entry. A full SQ drops the record and counts
 *                it in AAR_CB_LOG_DROPPED.
 */
#define AAR_OP_NONE          0u
#define AAR_OP_MSG           1u
#define AAR_OP_EVENT         2u
#define AAR_OP_LOG           3u

/* Files. Paths and names are NUL-terminated bytes in the payload buffer. File
 * and list ids are chosen by the runtime and are small integers from
 * AAR_FD_BASE. */
#define AAR_OP_FILE_OPEN     0x10u /* path in buf; arg: FD, FLAGS (AAR_FILE_*) */
#define AAR_OP_FILE_CLOSE    0x11u /* arg: FD */
#define AAR_OP_FILE_READ     0x12u /* arg: FD, LEN; bytes go to buf; out: COUNT */
#define AAR_OP_FILE_WRITE    0x13u /* arg: FD, LEN; bytes come from buf; out: COUNT */
#define AAR_OP_FILE_SEEK     0x14u /* arg: FD, OFF_HI, OFF_LO, WHENCE; out: POS_HI, POS_LO */
#define AAR_OP_FILE_STAT     0x15u /* path in buf; out: SIZE_HI/LO, MTIME_HI/LO, MODE */
#define AAR_OP_LIST_OPEN     0x16u /* path in buf; arg: ID */
#define AAR_OP_LIST_NEXT     0x17u /* arg: ID; name goes to buf; out: MODE; AAR_E_NOENT at the end */
#define AAR_OP_LIST_CLOSE    0x18u /* arg: ID */

/* Console. The bytes sit in a console buffer named by buf, buf_off, buf_len. */
#define AAR_OP_CONSOLE_WRITE 0x20u /* bytes in buf; out: COUNT consumed */
#define AAR_OP_CONSOLE_READ  0x21u /* buf_len is the room; client fills it; out: COUNT, 0 at end of input */

/* Display. */
#define AAR_OP_DISPLAY_OPEN    0x30u /* arg: W, H, FORMAT, FLAGS; out: ID, PITCH, LAYOUT, FRONT, BACK, SPARE; reply flags: AAR_DISPCAP_* */
#define AAR_OP_DISPLAY_FLIP    0x31u /* arg: ID, FLAGS; replied when shown; out: FRONT, BACK, PITCH, LAYOUT, DROPPED. A platform that owns the display buffers may complete a flip itself without sending it to the client */
#define AAR_OP_DISPLAY_PALETTE 0x32u /* arg: ID, FIRST, COUNT; colors in buf, big-endian 0x00RRGGBB words */
#define AAR_OP_DISPLAY_CLOSE   0x33u /* arg: ID */

/* Audio. */
#define AAR_OP_AUDIO_OPEN      0x40u /* arg: RATE, CHANNELS, ROUTE; ring named in buf, buf_len (allocated by the runtime); out: RATE, CHANNELS, RING (echoes buf), RING_BYTES, FRAMES */
#define AAR_OP_AUDIO_CLOSE     0x41u

/* Argument and result word indices (into arg[] and out[]). A_ is a request
 * argument, R_ is a completion result. 64-bit values are _HI then _LO. */
#define AAR_A_FILE_OPEN_FD     0u
#define AAR_A_FILE_OPEN_FLAGS  1u
#define AAR_A_FILE_FD          0u /* CLOSE, READ, WRITE, SEEK */
#define AAR_A_FILE_LEN         1u /* READ, WRITE */
#define AAR_A_FILE_OFF_HI      1u /* SEEK */
#define AAR_A_FILE_OFF_LO      2u
#define AAR_A_FILE_WHENCE      3u
#define AAR_R_FILE_COUNT       0u /* READ, WRITE */
#define AAR_R_FILE_POS_HI      1u /* SEEK */
#define AAR_R_FILE_POS_LO      2u
#define AAR_R_STAT_SIZE_HI     0u
#define AAR_R_STAT_SIZE_LO     1u
#define AAR_R_STAT_MTIME_HI    2u
#define AAR_R_STAT_MTIME_LO    3u
#define AAR_R_STAT_MODE        4u
#define AAR_A_LIST_ID          0u /* OPEN, NEXT, CLOSE */
#define AAR_R_LIST_MODE        0u /* NEXT */
#define AAR_R_CONSOLE_COUNT    0u

#define AAR_A_DISPLAY_OPEN_W      0u
#define AAR_A_DISPLAY_OPEN_H      1u
#define AAR_A_DISPLAY_OPEN_FORMAT 2u
#define AAR_A_DISPLAY_OPEN_FLAGS  3u
#define AAR_R_DISPLAY_OPEN_ID     0u
#define AAR_R_DISPLAY_OPEN_PITCH  1u
#define AAR_R_DISPLAY_OPEN_LAYOUT 2u
#define AAR_R_DISPLAY_OPEN_FRONT  3u /* tokens of the client's bitmap memory */
#define AAR_R_DISPLAY_OPEN_BACK   4u
#define AAR_R_DISPLAY_OPEN_SPARE  5u
#define AAR_A_DISPLAY_ID          0u /* FLIP, PALETTE, CLOSE */
#define AAR_A_DISPLAY_FLIP_FLAGS  1u /* AAR_FLIPF_* */
#define AAR_A_DISPLAY_PALETTE_FIRST 1u
#define AAR_A_DISPLAY_PALETTE_COUNT 2u
#define AAR_R_DISPLAY_FLIP_FRONT   0u
#define AAR_R_DISPLAY_FLIP_BACK    1u
#define AAR_R_DISPLAY_FLIP_PITCH   2u
#define AAR_R_DISPLAY_FLIP_LAYOUT  3u
#define AAR_R_DISPLAY_FLIP_DROPPED 4u /* cumulative frames replaced in LATEST mode */
#define AAR_FLIPF_ASYNC (1u << 0)     /* the app does not wait for the completion */

#define AAR_A_AUDIO_OPEN_RATE     0u
#define AAR_A_AUDIO_OPEN_CHANNELS 1u
#define AAR_A_AUDIO_OPEN_ROUTE    2u /* AAR_AUDIO_ROUTE_* */
#define AAR_R_AUDIO_OPEN_RATE     0u /* granted */
#define AAR_R_AUDIO_OPEN_CHANNELS 1u
#define AAR_R_AUDIO_OPEN_RING     2u /* token of the audio ring buffer: the request's buf, echoed */
#define AAR_R_AUDIO_OPEN_RING_BYTES 3u
#define AAR_R_AUDIO_OPEN_FRAMES   4u /* ring capacity, power of two */
#define AAR_AUDIO_ROUTE_PLATFORM 0u /* the platform's own mixer or fabric */
#define AAR_AUDIO_ROUTE_AHI      1u /* AHI through the client; it reads PCM with AARAudioRead */

/*
 * Audio ring buffer. The runtime allocates it and names it in the AUDIO_OPEN
 * request (buf, buf_len); the completion echoes it in AAR_R_AUDIO_OPEN_RING.
 * Unlike the control block it is LITTLE-endian, because the app, the runtime and the
 * S16LE samples are: the client library byte swaps the words it reads. Each
 * index has its own line and one writer.
 *   0x00 write_frame  free-running frame count, the app advances
 *   0x04 rate, 0x08 channels, 0x0C format (AAR_AUDIO_FORMAT_S16LE): runtime, at open
 *   0x40 read_frame   free-running frame count, the consumer advances
 *   0x44 underruns    cumulative, the consumer counts
 *   0x80 samples, interleaved S16LE, `frames` frames
 * The three pointers of aar_audio point into one mapping of the ring: with
 * `ring` the address of its first byte, write_frame is ring, read_frame is
 * ring + 0x40 and samples is ring + 0x80.
 */
#define AAR_AUDIO_RING_WRITE_FRAME 0x00u
#define AAR_AUDIO_RING_RATE        0x04u
#define AAR_AUDIO_RING_CHANNELS    0x08u
#define AAR_AUDIO_RING_FORMAT      0x0Cu
#define AAR_AUDIO_RING_READ_FRAME  0x40u
#define AAR_AUDIO_RING_UNDERRUNS   0x44u
#define AAR_AUDIO_RING_HDR_BYTES   0x80u
#define AAR_AUDIO_FORMAT_S16LE     1u

/* ---- Big-endian accessors ----------------------------------------------------------------------------------- */

/* Aligned volatile accesses (2, 4 and 4 bytes for a 64-bit value). Use them for
 * every word in the control block and in a queue entry. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AAR_NATIVE_BIG_ENDIAN 1
#else
#define AAR_NATIVE_BIG_ENDIAN 0
#endif

static inline uint16_t aar_get_be16(const volatile void *p)
{
    uint16_t v = *(const volatile uint16_t *)p;

    return AAR_NATIVE_BIG_ENDIAN ? v : aar_bswap16(v);
}

static inline uint32_t aar_get_be32(const volatile void *p)
{
    uint32_t v = *(const volatile uint32_t *)p;

    return AAR_NATIVE_BIG_ENDIAN ? v : aar_bswap32(v);
}

static inline uint64_t aar_get_be64(const volatile void *p)
{
    const volatile uint8_t *b = (const volatile uint8_t *)p;

    return ((uint64_t)aar_get_be32(b) << 32) | aar_get_be32(b + 4);
}

static inline void aar_put_be16(volatile void *p, uint16_t v)
{
    *(volatile uint16_t *)p = AAR_NATIVE_BIG_ENDIAN ? v : aar_bswap16(v);
}

static inline void aar_put_be32(volatile void *p, uint32_t v)
{
    *(volatile uint32_t *)p = AAR_NATIVE_BIG_ENDIAN ? v : aar_bswap32(v);
}

static inline void aar_put_be64(volatile void *p, uint64_t v)
{
    volatile uint8_t *b = (volatile uint8_t *)p;

    aar_put_be32(b, (uint32_t)(v >> 32));
    aar_put_be32(b + 4, (uint32_t)v);
}

/* ---- Barrier hooks ------------------------------------------------------------------------------------------- */

/* A hook orders memory for the caller's platform: a store barrier before an
 * index is published (release), a load barrier after one is read (acquire).
 * NULL means none, which is correct on a strongly ordered CPU and in a single
 * threaded test. */
typedef void (*aar_barrier_fn)(void);

static inline void aar_barrier_run(aar_barrier_fn fn)
{
    if (fn != 0)
        fn();
}

/* ---- Entry encode and decode ---------------------------------------------------------------------------------- */

/* Decoded forms of an entry, in native byte order. Nothing here points into
 * shared memory. */
typedef struct aar_request {
    uint16_t op;
    uint16_t flags;
    uint32_t id;
    uint32_t arg[AAR_ENTRY_ARGS];
    uint32_t buf;          /* shared-buffer token, 0 for none */
    uint32_t buf_off;
    uint32_t buf_len;
} aar_request;

typedef struct aar_completion {
    uint16_t op;           /* echoes the request's op */
    uint16_t flags;
    uint32_t id;           /* echoes the request's id */
    int32_t  result;       /* AAR_OK or a negative AAR_E_* */
    uint32_t out[AAR_ENTRY_OUTS];
} aar_completion;

/* Zeroes all 16 words of a slot, so reserved bytes and unused body bytes never
 * carry stale data. Every put below starts with it. */
static inline void aar_entry_clear(volatile void *entry)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;
    uint32_t i;

    for (i = 0; i < AAR_ENTRY_BYTES; i += 4u)
        aar_put_be32(e + i, 0u);
}

static inline void aar_entry_put_header(volatile void *entry, uint16_t op,
                                        uint16_t flags, uint32_t id,
                                        int32_t result)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;

    aar_put_be16(e + AAR_ENTRY_OP, op);
    aar_put_be16(e + AAR_ENTRY_FLAGS, flags);
    aar_put_be32(e + AAR_ENTRY_ID, id);
    aar_put_be32(e + AAR_ENTRY_RESULT, (uint32_t)result);
}

static inline void aar_entry_put_request(volatile void *entry,
                                         const aar_request *r)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;
    uint32_t i;

    aar_entry_clear(entry);
    aar_entry_put_header(entry, r->op, r->flags, r->id, 0);
    for (i = 0; i < AAR_ENTRY_ARGS; i++)
        aar_put_be32(e + AAR_ENTRY_ARG(i), r->arg[i]);
    aar_put_be32(e + AAR_ENTRY_BUF, r->buf);
    aar_put_be32(e + AAR_ENTRY_BUF_OFF, r->buf_off);
    aar_put_be32(e + AAR_ENTRY_BUF_LEN, r->buf_len);
}

static inline void aar_entry_get_request(const volatile void *entry,
                                         aar_request *r)
{
    const volatile uint8_t *e = (const volatile uint8_t *)entry;
    uint32_t i;

    r->op = aar_get_be16(e + AAR_ENTRY_OP);
    r->flags = aar_get_be16(e + AAR_ENTRY_FLAGS);
    r->id = aar_get_be32(e + AAR_ENTRY_ID);
    for (i = 0; i < AAR_ENTRY_ARGS; i++)
        r->arg[i] = aar_get_be32(e + AAR_ENTRY_ARG(i));
    r->buf = aar_get_be32(e + AAR_ENTRY_BUF);
    r->buf_off = aar_get_be32(e + AAR_ENTRY_BUF_OFF);
    r->buf_len = aar_get_be32(e + AAR_ENTRY_BUF_LEN);
}

static inline void aar_entry_put_completion(volatile void *entry,
                                            const aar_completion *c)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;
    uint32_t i;

    aar_entry_clear(entry);
    aar_entry_put_header(entry, c->op, c->flags, c->id, c->result);
    for (i = 0; i < AAR_ENTRY_OUTS; i++)
        aar_put_be32(e + AAR_ENTRY_OUT(i), c->out[i]);
}

static inline void aar_entry_get_completion(const volatile void *entry,
                                            aar_completion *c)
{
    const volatile uint8_t *e = (const volatile uint8_t *)entry;
    uint32_t i;

    c->op = aar_get_be16(e + AAR_ENTRY_OP);
    c->flags = aar_get_be16(e + AAR_ENTRY_FLAGS);
    c->id = aar_get_be32(e + AAR_ENTRY_ID);
    c->result = (int32_t)aar_get_be32(e + AAR_ENTRY_RESULT);
    for (i = 0; i < AAR_ENTRY_OUTS; i++)
        c->out[i] = aar_get_be32(e + AAR_ENTRY_OUT(i));
}

/* A message entry: AAR_MSG_BYTES opaque bytes, copied without swapping. */
static inline void aar_entry_put_msg(volatile void *entry, const void *msg)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;
    const uint8_t *m = (const uint8_t *)msg;
    uint32_t i;

    aar_entry_clear(entry);
    aar_entry_put_header(entry, AAR_OP_MSG, 0, 0, 0);
    for (i = 0; i < AAR_MSG_BYTES; i++)
        e[AAR_ENTRY_MSG + i] = m[i];
}

static inline void aar_entry_get_msg(const volatile void *entry, void *msg)
{
    const volatile uint8_t *e = (const volatile uint8_t *)entry;
    uint8_t *m = (uint8_t *)msg;
    uint32_t i;

    for (i = 0; i < AAR_MSG_BYTES; i++)
        m[i] = e[AAR_ENTRY_MSG + i];
}

/* An unsolicited event entry for the CQ. */
static inline void aar_entry_put_event(volatile void *entry, const aar_event *ev)
{
    volatile uint8_t *e = (volatile uint8_t *)entry;

    aar_entry_clear(entry);
    aar_entry_put_header(entry, AAR_OP_EVENT, 0, 0, 0);
    aar_put_be16(e + AAR_ENTRY_EV_TYPE, ev->type);
    aar_put_be16(e + AAR_ENTRY_EV_CODE, ev->code);
    aar_put_be16(e + AAR_ENTRY_EV_DX, (uint16_t)ev->dx);
    aar_put_be16(e + AAR_ENTRY_EV_DY, (uint16_t)ev->dy);
    aar_put_be16(e + AAR_ENTRY_EV_BUTTONS, ev->buttons);
    aar_put_be16(e + AAR_ENTRY_EV_RESERVED, ev->reserved);
    aar_put_be32(e + AAR_ENTRY_EV_TIME_MS, ev->time_ms);
}

static inline void aar_entry_get_event(const volatile void *entry, aar_event *ev)
{
    const volatile uint8_t *e = (const volatile uint8_t *)entry;

    ev->type = aar_get_be16(e + AAR_ENTRY_EV_TYPE);
    ev->code = aar_get_be16(e + AAR_ENTRY_EV_CODE);
    ev->dx = (int16_t)aar_get_be16(e + AAR_ENTRY_EV_DX);
    ev->dy = (int16_t)aar_get_be16(e + AAR_ENTRY_EV_DY);
    ev->buttons = aar_get_be16(e + AAR_ENTRY_EV_BUTTONS);
    ev->reserved = aar_get_be16(e + AAR_ENTRY_EV_RESERVED);
    ev->time_ms = aar_get_be32(e + AAR_ENTRY_EV_TIME_MS);
}

/* ---- Queue reference model ------------------------------------------------------------------------------------- */

/* A view of one ring. It holds pointers into shared memory and the depth; it is
 * not itself shared. `head` and `tail` point at the index words (in the control
 * block, or anywhere else the platform puts them). */
typedef struct aar_queue {
    volatile uint8_t *ring; /* first entry; depth * AAR_ENTRY_BYTES bytes */
    volatile uint8_t *head; /* big-endian word the consumer writes */
    volatile uint8_t *tail; /* big-endian word the producer writes */
    uint32_t depth;         /* power of two, AAR_QUEUE_DEPTH_MIN .. MAX */
} aar_queue;

static inline int aar_queue_depth_valid(uint32_t depth)
{
    return depth >= AAR_QUEUE_DEPTH_MIN && depth <= AAR_QUEUE_DEPTH_MAX &&
           (depth & (depth - 1u)) == 0u;
}

/* Returns AAR_E_INVAL for a depth that is not valid. */
static inline int aar_queue_init(aar_queue *q, volatile void *ring,
                                 volatile void *head, volatile void *tail,
                                 uint32_t depth)
{
    if (!aar_queue_depth_valid(depth) || ring == 0 || head == 0 || tail == 0)
        return AAR_E_INVAL;
    q->ring = (volatile uint8_t *)ring;
    q->head = (volatile uint8_t *)head;
    q->tail = (volatile uint8_t *)tail;
    q->depth = depth;
    return AAR_OK;
}

/* Entries queued now. AAR_E_IO when the indices say more than `depth` (the
 * block is corrupt or was written by something else); *used is left alone. */
static inline int aar_queue_used(const aar_queue *q, uint32_t *used)
{
    uint32_t head = aar_get_be32(q->head);
    uint32_t tail = aar_get_be32(q->tail);
    uint32_t n = tail - head;

    if (n > q->depth)
        return AAR_E_IO;
    *used = n;
    return AAR_OK;
}

/* Producer, step 1. Finds the slot for the next entry. `reserve` entries are
 * kept free: 0 for requests, completions, events and logs, AAR_MSG_RESERVE for
 * messages. AAR_E_BUSY when fewer than reserve + 1 entries are free, AAR_E_IO
 * on corrupt indices. Reads the consumer's head, then runs `acquire`, so the
 * consumer's reads of the slot are done before it is overwritten. Nothing is
 * visible to the consumer until aar_queue_commit. */
static inline int aar_queue_prepare(const aar_queue *q, uint32_t reserve,
                                    aar_barrier_fn acquire,
                                    volatile uint8_t **slot)
{
    uint32_t head = aar_get_be32(q->head);
    uint32_t tail;
    uint32_t used;

    aar_barrier_run(acquire);
    tail = aar_get_be32(q->tail);
    used = tail - head;
    if (used > q->depth)
        return AAR_E_IO;
    if (q->depth - used <= reserve)
        return AAR_E_BUSY;
    *slot = q->ring + (size_t)(tail & (q->depth - 1u)) * AAR_ENTRY_BYTES;
    return AAR_OK;
}

/* Producer, step 3, after the entry is written: `release` (the store barrier),
 * then the new tail. */
static inline void aar_queue_commit(const aar_queue *q, aar_barrier_fn release)
{
    uint32_t tail = aar_get_be32(q->tail);

    aar_barrier_run(release);
    aar_put_be32(q->tail, tail + 1u);
}

static inline int aar_queue_post_request(const aar_queue *q,
                                         const aar_request *r,
                                         aar_barrier_fn acquire,
                                         aar_barrier_fn release)
{
    volatile uint8_t *slot;
    int rc = aar_queue_prepare(q, 0u, acquire, &slot);

    if (rc != AAR_OK)
        return rc;
    aar_entry_put_request(slot, r);
    aar_queue_commit(q, release);
    return AAR_OK;
}

static inline int aar_queue_post_completion(const aar_queue *q,
                                            const aar_completion *c,
                                            aar_barrier_fn acquire,
                                            aar_barrier_fn release)
{
    volatile uint8_t *slot;
    int rc = aar_queue_prepare(q, 0u, acquire, &slot);

    if (rc != AAR_OK)
        return rc;
    aar_entry_put_completion(slot, c);
    aar_queue_commit(q, release);
    return AAR_OK;
}

/* A message: AAR_E_BUSY once msg_capacity (depth - 1) entries are queued. */
static inline int aar_queue_post_msg(const aar_queue *q, const void *msg,
                                     aar_barrier_fn acquire,
                                     aar_barrier_fn release)
{
    volatile uint8_t *slot;
    int rc = aar_queue_prepare(q, AAR_MSG_RESERVE, acquire, &slot);

    if (rc != AAR_OK)
        return rc;
    aar_entry_put_msg(slot, msg);
    aar_queue_commit(q, release);
    return AAR_OK;
}

/* An unsolicited event on the CQ. AAR_E_BUSY when full: the caller counts the
 * drop. */
static inline int aar_queue_post_event(const aar_queue *q, const aar_event *ev,
                                       aar_barrier_fn acquire,
                                       aar_barrier_fn release)
{
    volatile uint8_t *slot;
    int rc = aar_queue_prepare(q, 0u, acquire, &slot);

    if (rc != AAR_OK)
        return rc;
    aar_entry_put_event(slot, ev);
    aar_queue_commit(q, release);
    return AAR_OK;
}

/* Consumer, step 1. 1 with *slot set to the oldest entry, 0 when empty, or
 * AAR_E_IO on corrupt indices. Reads the producer's tail, then runs `acquire`,
 * so the entry that follows is at least as new as the tail. The slot stays
 * valid until aar_queue_pop. */
static inline int aar_queue_peek(const aar_queue *q, aar_barrier_fn acquire,
                                 const volatile uint8_t **slot)
{
    uint32_t head = aar_get_be32(q->head);
    uint32_t tail = aar_get_be32(q->tail);
    uint32_t used;

    aar_barrier_run(acquire);
    used = tail - head;
    if (used > q->depth)
        return AAR_E_IO;
    if (used == 0u)
        return 0;
    *slot = q->ring + (size_t)(head & (q->depth - 1u)) * AAR_ENTRY_BYTES;
    return 1;
}

/* Consumer, step 3, after the entry has been read: `release`, then the new
 * head. Call it only after aar_queue_peek returned 1. */
static inline void aar_queue_pop(const aar_queue *q, aar_barrier_fn release)
{
    uint32_t head = aar_get_be32(q->head);

    aar_barrier_run(release);
    aar_put_be32(q->head, head + 1u);
}

/* ---- Control block helpers ----------------------------------------------------------------------------------------- */

/* Bytes of a pair for a given depth: the control block plus two rings. */
static inline uint32_t aar_pair_bytes(uint32_t depth)
{
    return AAR_PAIR_SQ_OFFSET + 2u * depth * AAR_ENTRY_BYTES;
}

/* Offset of the CQ ring in the default one-buffer layout. */
static inline uint32_t aar_pair_cq_offset(uint32_t depth)
{
    return AAR_PAIR_SQ_OFFSET + depth * AAR_ENTRY_BYTES;
}

/* Platform, at admission: zero the block, then write the header and both queue
 * descriptors. Tokens and offsets say where each ring is (see AAR_CB_Q_DESC).
 * AAR_E_INVAL for a depth that is not valid. The client must not see the block
 * before this returns. */
static inline int aar_cb_init(volatile void *cb, uint32_t handle,
                              uint32_t profile, uint32_t depth,
                              uint32_t sq_token, uint32_t sq_offset,
                              uint32_t cq_token, uint32_t cq_offset)
{
    volatile uint8_t *b = (volatile uint8_t *)cb;
    uint32_t i;

    if (!aar_queue_depth_valid(depth))
        return AAR_E_INVAL;
    for (i = 0; i < AAR_CB_BYTES; i += 4u)
        aar_put_be32(b + i, 0u);
    aar_put_be32(b + AAR_CB_VERSION, AAR_CB_LAYOUT_VERSION);
    aar_put_be32(b + AAR_CB_SIZE, AAR_CB_BYTES);
    aar_put_be32(b + AAR_CB_HANDLE, handle);
    aar_put_be32(b + AAR_CB_PROFILE, profile);
    aar_put_be32(b + AAR_CB_SQ_DESC + AAR_QD_TOKEN, sq_token);
    aar_put_be32(b + AAR_CB_SQ_DESC + AAR_QD_OFFSET, sq_offset);
    aar_put_be32(b + AAR_CB_SQ_DESC + AAR_QD_DEPTH, depth);
    aar_put_be32(b + AAR_CB_SQ_DESC + AAR_QD_ENTRY_BYTES, AAR_ENTRY_BYTES);
    aar_put_be32(b + AAR_CB_CQ_DESC + AAR_QD_TOKEN, cq_token);
    aar_put_be32(b + AAR_CB_CQ_DESC + AAR_QD_OFFSET, cq_offset);
    aar_put_be32(b + AAR_CB_CQ_DESC + AAR_QD_DEPTH, depth);
    aar_put_be32(b + AAR_CB_CQ_DESC + AAR_QD_ENTRY_BYTES, AAR_ENTRY_BYTES);
    /* The magic is written last: a reader that sees it sees the rest. The
     * caller orders this with its own release barrier before it hands out the
     * block's token. */
    aar_put_be32(b + AAR_CB_MAGIC, AAR_CB_MAGIC_VALUE);
    return AAR_OK;
}

/* AAR_OK for a block with the right magic, a layout version this header
 * understands and a size of at least AAR_CB_BYTES; AAR_E_IO otherwise. */
static inline int aar_cb_check(const volatile void *cb)
{
    const volatile uint8_t *b = (const volatile uint8_t *)cb;

    if (aar_get_be32(b + AAR_CB_MAGIC) != AAR_CB_MAGIC_VALUE ||
        aar_get_be32(b + AAR_CB_VERSION) != AAR_CB_LAYOUT_VERSION ||
        aar_get_be32(b + AAR_CB_SIZE) < AAR_CB_BYTES)
        return AAR_E_IO;
    return AAR_OK;
}

/* Builds the view of queue `which` (AAR_QUEUE_SQ or AAR_QUEUE_CQ) from the
 * control block and the mapped ring. The depth and entry size come from the
 * descriptor: AAR_E_IO if they are not valid. */
static inline int aar_cb_queue_bind(aar_queue *q, volatile void *cb,
                                    volatile void *ring, uint32_t which)
{
    volatile uint8_t *b = (volatile uint8_t *)cb;
    uint32_t desc;
    uint32_t depth;

    if (which > AAR_QUEUE_CQ)
        return AAR_E_INVAL;
    desc = AAR_CB_Q_DESC(which);
    depth = aar_get_be32(b + desc + AAR_QD_DEPTH);
    if (aar_get_be32(b + desc + AAR_QD_ENTRY_BYTES) != AAR_ENTRY_BYTES ||
        aar_queue_init(q, ring, b + AAR_CB_Q_HEAD(which),
                       b + AAR_CB_Q_TAIL(which), depth) != AAR_OK)
        return AAR_E_IO;
    return AAR_OK;
}

/* ---- Message credits (client to app direction) ---------------------------------------------- */

/*
 * Messages and completions share the CQ, which is a FIFO released in order. If
 * the client could post messages without limit, the platform would have to
 * stop taking entries once its inbox was full, and a completion queued behind
 * those messages could not be reached. Credits remove that: the platform writes
 * AAR_CB_MSG_CONSUMED, a free-running count of the messages the app has taken
 * from its inbox (not merely moved out of the CQ); the client keeps its own
 * free-running count of the messages it has posted. The client posts an
 * AAR_OP_MSG only while
 *
 *     posted - consumed < msg_capacity       (modulo 2^32)
 *
 * and answers AAR_E_BUSY otherwise. Then the messages in the CQ plus the inbox
 * never exceed msg_capacity, an inbox of msg_capacity entries always has room,
 * and the platform never has to leave a message in the CQ.
 *
 * msg_capacity is AAR_MSG_CAPACITY_FOR(depth). The count posted is the client's
 * own state; only AAR_CB_MSG_CONSUMED is shared.
 */

/* Messages posted and not yet consumed, or AAR_E_IO when consumed is ahead of
 * posted or more than `capacity` are outstanding (the word is corrupt). */
static inline int aar_msg_outstanding(uint32_t posted, uint32_t consumed,
                                      uint32_t capacity)
{
    uint32_t n = posted - consumed;

    return n > capacity ? AAR_E_IO : (int)n;
}

/* Credits left: capacity minus the outstanding messages, 0 when none, or
 * AAR_E_IO as above. */
static inline int aar_msg_credits_left(uint32_t posted, uint32_t consumed,
                                       uint32_t capacity)
{
    int n = aar_msg_outstanding(posted, consumed, capacity);

    return n < 0 ? n : (int)capacity - n;
}

/* 1 when the client may post one more message, else 0 (no credit, or the
 * consumed word is corrupt). */
static inline int aar_msg_credit_ok(uint32_t posted, uint32_t consumed,
                                    uint32_t capacity)
{
    return aar_msg_credits_left(posted, consumed, capacity) > 0;
}

/* The platform's count of consumed messages, from the control block. */
static inline uint32_t aar_cb_msg_consumed(const volatile void *cb)
{
    return aar_get_be32((const volatile uint8_t *)cb + AAR_CB_MSG_CONSUMED);
}

/* The client's message post: checks the credit, then posts on the CQ and
 * advances *posted by one. AAR_E_BUSY when there is no credit or the queue has
 * no room, AAR_E_IO on a corrupt consumed word or corrupt indices. `cq` is the
 * CQ view of this control block. *posted is unchanged on failure. */
static inline int aar_cb_post_msg(const aar_queue *cq, const volatile void *cb,
                                  uint32_t *posted, const void *msg,
                                  aar_barrier_fn acquire,
                                  aar_barrier_fn release)
{
    uint32_t capacity = AAR_MSG_CAPACITY_FOR(cq->depth);
    int left = aar_msg_credits_left(*posted, aar_cb_msg_consumed(cb), capacity);
    int rc;

    if (left < 0)
        return left;
    if (left == 0)
        return AAR_E_BUSY;
    rc = aar_queue_post_msg(cq, msg, acquire, release);
    if (rc == AAR_OK)
        *posted += 1u;
    return rc;
}

/* ---- Layout checks -------------------------------------------------------------------------------------------------------------- */

AAR_STATIC_ASSERT(AAR_CB_SQ_DESC == 0x1C0u && AAR_CB_SQ_TAIL == 0x200u &&
                  AAR_CB_SQ_HEAD == 0x240u, "SQ control block offsets");
AAR_STATIC_ASSERT(AAR_CB_CQ_DESC == 0x280u && AAR_CB_CQ_TAIL == 0x2C0u &&
                  AAR_CB_CQ_HEAD == 0x300u, "CQ control block offsets");
AAR_STATIC_ASSERT((AAR_CB_SQ_TAIL % AAR_CB_LINE_BYTES) == 0u &&
                  (AAR_CB_SQ_HEAD % AAR_CB_LINE_BYTES) == 0u &&
                  (AAR_CB_CQ_TAIL % AAR_CB_LINE_BYTES) == 0u &&
                  (AAR_CB_CQ_HEAD % AAR_CB_LINE_BYTES) == 0u &&
                  (AAR_CB_LEASE_COUNTER % AAR_CB_LINE_BYTES) == 0u,
                  "every index and the lease counter own a 64-byte line");
AAR_STATIC_ASSERT(AAR_CB_CQ_HEAD + AAR_CB_LINE_BYTES <= AAR_CB_BYTES,
                  "queues fit in the control block");
AAR_STATIC_ASSERT(AAR_CB_MSG_CONSUMED == 0x05Cu &&
                  AAR_CB_MSG_CONSUMED / AAR_CB_LINE_BYTES ==
                      AAR_CB_STATE / AAR_CB_LINE_BYTES,
                  "msg_consumed is in the platform-written lifecycle line");
AAR_STATIC_ASSERT(AAR_ENTRY_BYTES == 4u * AAR_ENTRY_WORDS, "entry words");
AAR_STATIC_ASSERT(AAR_ENTRY_BODY + AAR_ENTRY_BODY_BYTES == AAR_ENTRY_BYTES,
                  "entry body");
AAR_STATIC_ASSERT(AAR_ENTRY_ARG(AAR_ENTRY_ARGS) == AAR_ENTRY_BUF,
                  "request args end at buf");
AAR_STATIC_ASSERT(AAR_ENTRY_BUF_LEN + 4u <= AAR_ENTRY_BYTES, "request fits");
AAR_STATIC_ASSERT(AAR_ENTRY_OUT(AAR_ENTRY_OUTS) <= AAR_ENTRY_BYTES,
                  "completion fits");
AAR_STATIC_ASSERT(AAR_ENTRY_MSG + AAR_MSG_BYTES <= AAR_ENTRY_BYTES, "message fits");
AAR_STATIC_ASSERT(AAR_ENTRY_EV_TIME_MS + 4u <= AAR_ENTRY_BYTES, "event fits");
AAR_STATIC_ASSERT(AAR_MSG_CAPACITY == 63u, "msg_capacity is depth - 1");
AAR_STATIC_ASSERT(AAR_PAIR_SQ_OFFSET == AAR_CB_BYTES &&
                  AAR_PAIR_CQ_OFFSET == AAR_PAIR_SQ_OFFSET +
                      AAR_QUEUE_DEPTH_DEFAULT * AAR_ENTRY_BYTES &&
                  AAR_PAIR_BYTES == AAR_PAIR_CQ_OFFSET +
                      AAR_QUEUE_DEPTH_DEFAULT * AAR_ENTRY_BYTES,
                  "default pair layout");
AAR_STATIC_ASSERT(AAR_AUDIO_RING_HDR_BYTES >= AAR_AUDIO_RING_UNDERRUNS + 4u,
                  "audio ring header");

#ifdef __cplusplus
}
#endif

#endif
