<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# The AAR transport contract

This page is for people who implement the standard on a platform, or who write
a client library over an existing transport. App authors do not need it: an
app sees only the table described in the [API reference](aar-api.md).

The contract is `include/aar/transport.h`. The header pins every layout as a
byte offset and carries a reference model of the producer and the consumer
that the firmware, the Amiga library and the host tests share. This page
explains the layouts and the rules. When the two disagree, the header and
its tests (`tests/transport_model_test.c`) are right and this page has a bug.

## What a platform supplies

A platform provides the ARM runtime (loader, table, limits), the host-side
transport, and `aar.library` on top of the transport. The library's own calls
are in [aar-client-api.md](aar-client-api.md). The transport is the
same on every platform. Per app there is one **control block** and one
**request/completion queue pair** in shared memory, large payloads travel as
**shared-buffer tokens**, and each direction has a **doorbell**. A platform
supplies three things:

1. **Shared memory** that the ARM runtime and the client both reach. It is
   allocated per app at admission (the control block and the queues) and per
   buffer on use. Its coherency is the platform's job, or the platform
   provides real `aar_publish` and `aar_acquire`.
2. **A doorbell** in each direction. It is a hint that something changed; the
   queues stay authoritative, so a platform that can only poll is correct.
3. **A client mapping**: a way for the library to turn a token into a 68k
   pointer and to learn its cache mode.

## Byte order and encoding

Every word in the control block and in a queue entry is **big-endian**, so the
68k reads native longs. The ARM side uses the byte-reversing accessors. A
64-bit value is two words, **high word first** (a `_HI` offset, then a `_LO`
offset). Nothing in the header is a C structure laid over shared memory:
layouts are byte offsets, so they hold for any compiler and either profile.

Two things are not big-endian. Message bytes (`AAR_MSG_BYTES` of them in an
entry) are opaque and never swapped. The audio ring buffer is little-endian,
because the app, the runtime and the S16LE samples are; the client library
swaps the words it reads.

| Symbol | Meaning |
|---|---|
| `AAR_NATIVE_BIG_ENDIAN` | 1 when the compiler's byte order is big-endian, else 0. The accessors skip the swap when it is 1. |
| `aar_get_be16`, `aar_get_be32`, `aar_get_be64` | Aligned volatile reads, 2, 4 and 4+4 bytes. Use them for every word of the control block and of an entry. |
| `aar_put_be16`, `aar_put_be32`, `aar_put_be64` | The matching writes. |

## Handles and admission

A handle is a `uint32_t`: the process-record index in the low 16 bits and a
generation in the high 16 bits. Handle 0 is invalid. Generation 0 is never
issued, so every issued handle is non-zero. A platform that reuses an index
advances the generation with `aar_handle_next_gen`; 0xFFFF wraps to 1. A
request that names a handle whose generation is not the live one fails with
`AAR_E_STALE` and changes nothing.

Claim is a platform control call that must reply. It either refuses with
`AAR_E_NORESOURCE` and an `AAR_REASON_*` code, or returns the handle and the
tokens of the control block and the queue pair. The index selects a process
record that the platform allocates at admission; the count of live apps is
bounded by resources, not by a constant. Recommended order: process record and
translation tables first, then the control block and queues from shared
memory. The commit for the image and the shared limit are reserved when the
load begins. Everything reserved is released on every failure path.

| Symbol | Meaning |
|---|---|
| `AAR_HANDLE_INDEX_BITS` | 16, the width of the index field. |
| `AAR_HANDLE_INDEX_MASK` | `0x0000FFFF`. |
| `AAR_HANDLE_GEN_MAX` | `0xFFFF`, the largest generation. |
| `aar_handle_make` | Builds a handle from an index and a generation. |
| `aar_handle_index`, `aar_handle_generation` | Take a handle apart. |
| `aar_handle_next_gen` | The generation after a given one: 1 to 0xFFFF, then 1 again. Any input, including 0, yields a value in 1 to 0xFFFF. |
| `aar_handle_check` | Checks a request's handle against the generation that is live for its index (0 when the index is free). `AAR_OK` on a match, `AAR_E_INVAL` for handle 0 or generation 0, `AAR_E_STALE` otherwise. |

## The control block

`AAR_CB_BYTES` (4 KiB) per app, zero-filled at admission. Offsets are bytes
from the start of the block. **Every word has exactly one writer.** A word that
changes hands (a queue index, the lease counter) sits at the start of its own
64-byte line (`AAR_CB_LINE_BYTES`), so two writers never share a line. Unlisted
space is reserved and zero.

| Symbol | Value |
|---|---|
| `AAR_CB_BYTES` | `0x1000` |
| `AAR_CB_LINE_BYTES` | 64 |
| `AAR_CB_MAGIC_VALUE` | `0x41415243`, the bytes `AARC` |
| `AAR_CB_LAYOUT_VERSION` | 1 |

### Header line, written by the platform at admission

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_MAGIC` | `0x000` | `AAR_CB_MAGIC_VALUE`. Written last, so a reader that sees it sees the rest. |
| `AAR_CB_VERSION` | `0x004` | `AAR_CB_LAYOUT_VERSION`. |
| `AAR_CB_SIZE` | `0x008` | Bytes of this block. |
| `AAR_CB_HANDLE` | `0x00C` | The app's handle. |
| `AAR_CB_PROFILE` | `0x010` | `AAR_PROFILE_*`. |

### Lifecycle line, written by the platform

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_STATE` | `0x040` | `AAR_STATE_*`. |
| `AAR_CB_END_REASON` | `0x044` | `AAR_END_*`. |
| `AAR_CB_EXIT_CODE` | `0x048` | The exit code, an `int32`. |
| `AAR_CB_FLAGS` | `0x04C` | `AAR_CBF_*`. |
| `AAR_CB_PROGRESS_HI`, `AAR_CB_PROGRESS_LO` | `0x050`, `0x054` | Load progress in bytes. It advances at every step, and the lease extends while it does. |
| `AAR_CB_LEASE_PERIOD_MS` | `0x058` | The lease period the platform applies. |
| `AAR_CB_MSG_CONSUMED` | `0x05C` | A free-running count of the client-to-app messages the app has taken from its inbox. The client reads it to know how many message credits it has (see [Message credits](#message-credits)). |

The lifecycle flags in `AAR_CB_FLAGS`:

| Symbol | Meaning |
|---|---|
| `AAR_CBF_DISPLAY` | A display is open. |
| `AAR_CBF_AUDIO` | An audio stream is open. |
| `AAR_CBF_GL` | A GL context is bound. |
| `AAR_CBF_CONSOLE` | The console has been used. A client opens its console window on this. |

### Requests from the client, written by the client

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_STOP_REQUEST` | `0x080` | Non-zero asks the app to stop. |
| `AAR_CB_CLIENT_PRESENT` | `0x084` | Non-zero while a client owns the app. |

### Lease and client accounting, written by the client

The lease counter is advanced from a timer or interrupt context that does not
depend on the owner task. The platform only reads it. The two counters are the
client's own tally of what it could not post.

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_LEASE_COUNTER` | `0x0C0` | The renewal counter. Own 64-byte line. |
| `AAR_CB_EVENTS_DROPPED` | `0x0C4` | Events the client found the completion queue too full to take. |
| `AAR_CB_CQ_FULL` | `0x0C8` | Times the client found the completion queue full. |

### Fault record, written by the platform

Valid once the state is FAULTED or the end reason is `AAR_END_USER_FAULT`.
Addresses are app virtual addresses; on A32 the high word is 0. The syndrome is
the FSR on A32 and the low 32 bits of `ESR_EL1` on A64. The processor state is
the CPSR on A32 and the low 32 bits of PSTATE on A64. A client that wants a
symbolised trace subtracts the load base.

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_FAULT_CLASS` | `0x100` | `AAR_FAULT_*`. |
| `AAR_CB_FAULT_SYNDROME` | `0x104` | The raw syndrome. |
| `AAR_CB_FAULT_PSTATE` | `0x108` | The processor state. |
| `AAR_CB_FAULT_PC_HI`, `AAR_CB_FAULT_PC_LO` | `0x110`, `0x114` | Program counter. |
| `AAR_CB_FAULT_SP_HI`, `AAR_CB_FAULT_SP_LO` | `0x118`, `0x11C` | Stack pointer. |
| `AAR_CB_FAULT_LR_HI`, `AAR_CB_FAULT_LR_LO` | `0x120`, `0x124` | Link register. |
| `AAR_CB_FAULT_ADDR_HI`, `AAR_CB_FAULT_ADDR_LO` | `0x128`, `0x12C` | The faulting address. |
| `AAR_CB_LOAD_BASE_HI`, `AAR_CB_LOAD_BASE_LO` | `0x130`, `0x134` | Where the image was loaded. |

### Limits, written by the client before the load

The platform reads them when the load begins. 0 selects the default. The shared
limit is the limit on shared (host-visible) memory, including the runtime's own
service buffers.

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_PRIVATE_MIB` | `0x140` | Cap on private memory, MiB. |
| `AAR_CB_SHARED_MIB` | `0x144` | Limit on shared memory, MiB. |
| `AAR_CB_WEIGHT` | `0x148` | CPU share weight, default 1. |

### Accounting, written by the platform

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_CB_SHARED_USED_HI`, `AAR_CB_SHARED_USED_LO` | `0x180`, `0x184` | Shared bytes in use. |
| `AAR_CB_PRIVATE_COMMITTED_HI`, `AAR_CB_PRIVATE_COMMITTED_LO` | `0x188`, `0x18C` | Private bytes committed. |
| `AAR_CB_SQ_FULL` | `0x190` | Times a post found the submission queue full. |
| `AAR_CB_LOG_DROPPED` | `0x194` | Log records dropped because the submission queue was full. |

### Queue descriptors and indices

Queue 0 is the submission queue (SQ, app side to client), queue 1 the
completion queue (CQ, client to app side). Per queue there are three lines:

- a **descriptor** line, written by the platform at admission: where the ring
  is and how deep;
- a **tail** line, written by the producer;
- a **head** line, written by the consumer.

The ring is `entry_bytes * depth` bytes at `offset` inside the shared buffer
named by `token`. The default pair layout puts both rings in the same buffer as
the control block, so both tokens are the control block's.

| Symbol | Value or offset | Meaning |
|---|---|---|
| `AAR_QUEUE_SQ`, `AAR_QUEUE_CQ` | 0, 1 | Queue numbers. |
| `AAR_CB_Q_STRIDE` | `0xC0` | Distance between the two queues' lines. |
| `AAR_CB_Q_DESC(q)`, `AAR_CB_Q_TAIL(q)`, `AAR_CB_Q_HEAD(q)` | `0x1C0`, `0x200`, `0x240` plus `q * 0xC0` | The descriptor, tail and head of queue `q`. |
| `AAR_CB_SQ_DESC` | `0x1C0` | SQ descriptor. |
| `AAR_CB_SQ_TAIL` | `0x200` | SQ tail. The app side writes it. |
| `AAR_CB_SQ_HEAD` | `0x240` | SQ head. The client writes it. |
| `AAR_CB_CQ_DESC` | `0x280` | CQ descriptor. |
| `AAR_CB_CQ_TAIL` | `0x2C0` | CQ tail. The client writes it. |
| `AAR_CB_CQ_HEAD` | `0x300` | CQ head. The app side writes it. |

The descriptor words, added to `AAR_CB_Q_DESC(q)`:

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_QD_TOKEN` | `0x00` | The shared-buffer token (`haddr`) of the ring's buffer. |
| `AAR_QD_OFFSET` | `0x04` | Byte offset of the first entry in that buffer. |
| `AAR_QD_DEPTH` | `0x08` | Entries, a power of two. |
| `AAR_QD_ENTRY_BYTES` | `0x0C` | `AAR_ENTRY_BYTES`. |

### Control block helpers

| Symbol | Meaning |
|---|---|
| `aar_cb_init` | Platform, at admission: zeroes the block, then writes the header and both queue descriptors. The magic goes last. `AAR_E_INVAL` for an invalid depth. The client must not see the block before this returns, and the caller orders the magic with its own release barrier before it hands out the block's token. |
| `aar_cb_check` | `AAR_OK` for a block with the right magic, a layout version this header understands and a size of at least `AAR_CB_BYTES`; `AAR_E_IO` otherwise. |
| `aar_cb_queue_bind` | Builds the `aar_queue` view of queue `AAR_QUEUE_SQ` or `AAR_QUEUE_CQ` from the control block and the mapped ring. The depth and entry size come from the descriptor: `AAR_E_IO` if they are not valid. |

## The queue pair

The submission queue carries entries from the app side to the client. The
completion queue carries entries from the client to the app. Each holds `depth`
entries of `AAR_ENTRY_BYTES` (64) bytes, with free-running `uint32_t` indices
that wrap. Used entries are `tail - head`, and the slot of index `n` is
`n & (depth - 1)`. The producer writes the tail and the consumer writes the
head.

| Symbol | Value | Meaning |
|---|---|---|
| `AAR_ENTRY_BYTES` | 64 | Entry size. |
| `AAR_ENTRY_WORDS` | 16 | Entry size in 32-bit words. |
| `AAR_QUEUE_DEPTH_DEFAULT` | 64 | The reference depth. |
| `AAR_QUEUE_DEPTH_MIN` | 64 | The least power of two that keeps `msg_capacity` at least 32. |
| `AAR_QUEUE_DEPTH_MAX` | 4096 | The largest depth. |
| `AAR_MSG_RESERVE` | 1 | Entries a message post keeps free for the call in flight. |
| `AAR_MSG_CAPACITY` | 63 | Messages per direction at the default depth. |
| `AAR_MSG_CAPACITY_FOR(depth)` | `depth - 1` | The same for another depth. |

The default layout of a pair is one 4 KiB aligned shared buffer holding the
control block, then the SQ ring, then the CQ ring. For another depth the CQ
ring starts at `AAR_PAIR_SQ_OFFSET + depth * AAR_ENTRY_BYTES`.

| Symbol | Value | Meaning |
|---|---|---|
| `AAR_PAIR_CONTROL_OFFSET` | `0x0000` | The control block. |
| `AAR_PAIR_SQ_OFFSET` | `0x1000` | The SQ ring. |
| `AAR_PAIR_CQ_OFFSET` | `0x2000` | The CQ ring at depth 64. |
| `AAR_PAIR_BYTES` | `0x3000` | The size of a default pair. |
| `AAR_PAIR_ALIGN` | `0x1000` | The alignment of the buffer. |
| `aar_pair_bytes` | | Bytes of a pair for a given depth: the control block plus two rings. |
| `aar_pair_cq_offset` | | Offset of the CQ ring for a given depth. |

### Entry layout

All fields are big-endian. Every entry is 64 bytes.

| Offset | Size | Symbol | Field |
|---|---|---|---|
| 0 | 2 | `AAR_ENTRY_OP` | `op`, an `AAR_OP_*` value. |
| 2 | 2 | `AAR_ENTRY_FLAGS` | `flags`; see the op, 0 when none is defined. |
| 4 | 4 | `AAR_ENTRY_ID` | `id`, chosen by the producer and echoed in the completion. |
| 8 | 4 | `AAR_ENTRY_RESULT` | `result`, an `int32` `AAR_OK` or `AAR_E_*`, in completions. |
| 12 | 4 | `AAR_ENTRY_RESERVED` | Reserved, 0. |
| 16 | 48 | `AAR_ENTRY_BODY`, `AAR_ENTRY_BODY_BYTES` | The body, which depends on the kind of entry. |

**Request body.** `arg[6]` at offset 16 (`AAR_ENTRY_ARG(i)`, `AAR_ENTRY_ARGS`
words), then `buf` at 40 (`AAR_ENTRY_BUF`, a shared-buffer token, 0 for none),
`buf_off` at 44 (`AAR_ENTRY_BUF_OFF`) and `buf_len` at 48
(`AAR_ENTRY_BUF_LEN`). Bytes 52 to 63 are reserved and zero.

**Completion body.** `out[6]` at offset 16 (`AAR_ENTRY_OUT(i)`,
`AAR_ENTRY_OUTS` words). Bytes 40 to 63 are reserved and zero.

**Message entry**, in either direction: `AAR_MSG_BYTES` opaque bytes at offset
16 (`AAR_ENTRY_MSG`), not byte swapped. Bytes 48 to 63 are reserved and zero.

**Event entry**, completion queue only, with no matching request: the
`aar_event` fields, each big-endian, in this order:

| Symbol | Offset | Field |
|---|---|---|
| `AAR_ENTRY_EV_TYPE` | 16 | `type`, `uint16`. |
| `AAR_ENTRY_EV_CODE` | 18 | `code`, `uint16`. |
| `AAR_ENTRY_EV_DX` | 20 | `dx`, `int16`. |
| `AAR_ENTRY_EV_DY` | 22 | `dy`, `int16`. |
| `AAR_ENTRY_EV_BUTTONS` | 24 | `buttons`, `uint16`. |
| `AAR_ENTRY_EV_RESERVED` | 26 | `reserved`, `uint16`. |
| `AAR_ENTRY_EV_TIME_MS` | 28 | `time_ms`, `uint32`. |

Native-order forms and the encoders:

| Symbol | Meaning |
|---|---|
| `aar_request` | A decoded request: `op`, `flags`, `id`, `arg[6]`, `buf`, `buf_off`, `buf_len`. Nothing in it points into shared memory. |
| `aar_completion` | A decoded completion: `op`, `flags`, `id`, `result`, `out[6]`. |
| `aar_entry_clear` | Zeroes all 16 words of a slot, so reserved bytes and unused body bytes never carry stale data. Every put below starts with it. |
| `aar_entry_put_header` | Writes `op`, `flags`, `id` and `result`. |
| `aar_entry_put_request`, `aar_entry_get_request` | Encode and decode a request. |
| `aar_entry_put_completion`, `aar_entry_get_completion` | Encode and decode a completion. |
| `aar_entry_put_msg`, `aar_entry_get_msg` | Copy a message without swapping. |
| `aar_entry_put_event`, `aar_entry_get_event` | Encode and decode an event entry. |

## Ordering and barriers

The reference model implements these rules. The barriers are caller-supplied
hooks: a hook orders memory for the caller's platform, and a NULL hook means no
barrier, which is correct on a strongly ordered CPU such as the 68k and in a
single-threaded test.

**Producer** (writes entries, owns the tail):

1. Reserve a slot: read the consumer's head, run the acquire barrier, check
   that enough entries are free.
2. Write the entry.
3. Run the release barrier, then write the new tail.

**Consumer** (reads entries, owns the head):

1. Read the producer's tail, run the acquire barrier.
2. Read the entry.
3. Run the release barrier, then write the new head.

The release barrier before the head write is **stricter than the one-line rule
of the standard** ("the consumer reads the tail, then a load barrier, then the
entry, then writes the head"). ARMv7 and ARMv8 may reorder a load with a later
store to a different address. Without the barrier the producer could see the
freed slot and overwrite an entry the consumer is still reading. The same
reason is why the producer runs an acquire barrier after it reads the head and
before it writes the entry. On the 68k the hooks are NULL or a no-op.

An index is only ever written by its owner, so an owner may read its own index
without a barrier. Every index is a free-running `uint32_t`; a count above
`depth` means the block is corrupt and is `AAR_E_IO`.

| Symbol | Meaning |
|---|---|
| `aar_barrier_fn` | The hook type: `void (*)(void)`. |
| `aar_barrier_run` | Runs a hook; a NULL hook does nothing. |
| `aar_queue` | A view of one ring: pointers to the ring, the head word and the tail word, and the depth. It points into shared memory and is not itself shared. |
| `aar_queue_depth_valid` | True for a power of two from `AAR_QUEUE_DEPTH_MIN` to `AAR_QUEUE_DEPTH_MAX`. |
| `aar_queue_init` | Fills an `aar_queue`. `AAR_E_INVAL` for an invalid depth or a NULL pointer. |
| `aar_queue_used` | Entries queued now. `AAR_E_IO` when the indices say more than `depth`. |
| `aar_queue_prepare` | Producer step 1: finds the slot for the next entry. `reserve` entries are kept free (0 for requests, completions, events and logs, `AAR_MSG_RESERVE` for messages). `AAR_E_BUSY` when fewer than `reserve + 1` are free, `AAR_E_IO` on corrupt indices. Nothing is visible to the consumer until the commit. |
| `aar_queue_commit` | Producer step 3: the release barrier, then the new tail. |
| `aar_queue_post_request`, `aar_queue_post_completion`, `aar_queue_post_event` | Prepare, encode and commit in one call, with no reserve. |
| `aar_queue_post_msg` | The same for a message, with `AAR_MSG_RESERVE`: `AAR_E_BUSY` once `msg_capacity` (`depth - 1`) entries are queued. |
| `aar_queue_peek` | Consumer step 1: returns 1 with the oldest slot, 0 when empty, `AAR_E_IO` on corrupt indices. The slot stays valid until the pop. |
| `aar_queue_pop` | Consumer step 3: the release barrier, then the new head. Call it only after a peek that returned 1. |

### Message credits

Messages and completions share the completion queue, and the consumer releases
a FIFO in order. If the client could post messages without limit, the runtime's
inbox would fill, the runtime would have to leave a message in the queue, and a
completion behind it could not be reached until the app drained its inbox,
which an app that waits for that completion cannot do. **Message credits**
prevent this.

The platform writes `AAR_CB_MSG_CONSUMED`: the number of messages the app has
taken from its inbox, not the number moved out of the queue. The client keeps
its own free-running count of the messages it has posted. Both are `uint32_t`
and wrap. The client posts an `AAR_OP_MSG` only while

```
posted - consumed < msg_capacity        (modulo 2^32)
```

and otherwise returns `AAR_E_BUSY` without touching the queue. Messages in the
queue plus the inbox therefore never exceed `msg_capacity` (`AAR_MSG_CAPACITY`
at the default depth), so an inbox of that size always has room, the runtime
never stops draining at a message, and a completion is always reachable. The
one entry `AAR_MSG_RESERVE` keeps free is a second guard for the completion of
the call in flight. The app-to-client direction needs no credit word: the
runtime reads the client's own head index.

| Symbol | Meaning |
|---|---|
| `aar_msg_outstanding` | `posted - consumed`, or `AAR_E_IO` when it exceeds the capacity (the word is corrupt or ahead of `posted`). |
| `aar_msg_credits_left` | The capacity minus the outstanding messages, 0 when none, or `AAR_E_IO`. |
| `aar_msg_credit_ok` | 1 when the client may post one more message. |
| `aar_cb_msg_consumed` | Reads `AAR_CB_MSG_CONSUMED` from a control block. |
| `aar_cb_post_msg` | The client's message post: checks the credit, posts on the completion queue with `aar_queue_post_msg`, and advances the caller's `posted` count on success. `AAR_E_BUSY` without credit or room, `AAR_E_IO` on a corrupt word. |

## Operations

An entry's `op` says what it is. A **request** is a submission-queue entry
that expects a completion: the client replies with a completion-queue entry
with the same `op` and `id`, a `result`, and `out[]`. `AAR_OP_MSG`,
`AAR_OP_EVENT` and `AAR_OP_LOG` are not requests: they take no completion.

The app is single-threaded, so at most one request **parks** it, meaning the
app waits for that request's completion before it continues. A request does
not have to park. Some API calls submit a request and return without waiting
for the completion, for example a console write or a display palette update.
Such a request is still a request: its completion arrives later and the
runtime must consume it. An asynchronous flip likewise may leave a second
request in flight. So other requests can be in flight beside the parking
one, and the runtime must not assume one outstanding request.

A runtime that submits a request without waiting must keep every buffer the
request names unchanged and valid until the completion arrives. The API call
returning, or the client having read the entry, does not release that storage.
A runtime may coalesce palette updates that are queued and not yet submitted,
provided each changed palette index ends with the value from the latest
update, and indices that no update named are left as they were. Neither the
return of `aar_display_set_palette` nor a display flip promises the new
colors are visible; the transport only promises the completion.

| Symbol | Value | Direction | Meaning |
|---|---|---|---|
| `AAR_OP_NONE` | `0x00` | | Not an operation. |
| `AAR_OP_MSG` | `0x01` | either | 32 opaque bytes at `AAR_ENTRY_MSG`. Handled by the client's get and send message calls, not by its service loop. A post keeps `AAR_MSG_RESERVE` entry free. On the CQ the client also posts only while it has a message credit, else `AAR_E_BUSY`. |
| `AAR_OP_EVENT` | `0x02` | CQ only | Unsolicited: one event at the `AAR_ENTRY_EV_*` offsets, id 0. A full CQ drops the new event, and the client counts the drop in `AAR_CB_EVENTS_DROPPED`. The queue is shared with completions and messages, so a runtime keeps at least 64 events and never promises more. The client coalesces mouse motion (sums `dx` and `dy`, combines the buttons) before it posts. |
| `AAR_OP_LOG` | `0x03` | SQ only | One-way and lossy. `buf`, `buf_off` and `buf_len` name one record in a shared log buffer, at most `AAR_LOG_MAX` bytes and never wrapping. The app side may overwrite it after the client has consumed the entry. A full SQ drops the record and counts it in `AAR_CB_LOG_DROPPED`. |
| `AAR_OP_FILE_OPEN` | `0x10` | request | Path in `buf`; args `AAR_A_FILE_OPEN_FD`, `AAR_A_FILE_OPEN_FLAGS`. The flags word is the `AAR_FILE_*` combination of `aar_file_open` (read, write, create, truncate, exclusive). The client applies create, truncate and exclusive create as part of this one request. |
| `AAR_OP_FILE_CLOSE` | `0x11` | request | Arg `AAR_A_FILE_FD`. |
| `AAR_OP_FILE_READ` | `0x12` | request | Args `AAR_A_FILE_FD`, `AAR_A_FILE_LEN`. The bytes go to `buf`; out `AAR_R_FILE_COUNT`. |
| `AAR_OP_FILE_WRITE` | `0x13` | request | Args `AAR_A_FILE_FD`, `AAR_A_FILE_LEN`. The bytes come from `buf`; out `AAR_R_FILE_COUNT`. |
| `AAR_OP_FILE_SEEK` | `0x14` | request | Args `AAR_A_FILE_FD`, `AAR_A_FILE_OFF_HI`, `AAR_A_FILE_OFF_LO`, `AAR_A_FILE_WHENCE`; out `AAR_R_FILE_POS_HI`, `AAR_R_FILE_POS_LO`. |
| `AAR_OP_FILE_STAT` | `0x15` | request | Path in `buf`; out `AAR_R_STAT_SIZE_HI`, `AAR_R_STAT_SIZE_LO`, `AAR_R_STAT_MTIME_HI`, `AAR_R_STAT_MTIME_LO`, `AAR_R_STAT_MODE`. |
| `AAR_OP_LIST_OPEN` | `0x16` | request | Path in `buf`; arg `AAR_A_LIST_ID`. |
| `AAR_OP_LIST_NEXT` | `0x17` | request | Arg `AAR_A_LIST_ID`. The name goes to `buf`; out `AAR_R_LIST_MODE`. `AAR_E_NOENT` at the end. |
| `AAR_OP_LIST_CLOSE` | `0x18` | request | Arg `AAR_A_LIST_ID`. |
| `AAR_OP_CONSOLE_WRITE` | `0x20` | request | The bytes are in a console buffer named by `buf`, `buf_off`, `buf_len`; out `AAR_R_CONSOLE_COUNT` consumed. |
| `AAR_OP_CONSOLE_READ` | `0x21` | request | `buf_len` is the room; the client fills it; out `AAR_R_CONSOLE_COUNT`, 0 at the end of input. |
| `AAR_OP_DISPLAY_OPEN` | `0x30` | request | Args `AAR_A_DISPLAY_OPEN_W`, `_H`, `_FORMAT`, `_FLAGS`; out `AAR_R_DISPLAY_OPEN_ID`, `_PITCH`, `_LAYOUT`, `_FRONT`, `_BACK`, `_SPARE`. The reply's `flags` carry the `AAR_DISPCAP_*` caps. |
| `AAR_OP_DISPLAY_FLIP` | `0x31` | request | Args `AAR_A_DISPLAY_ID`, `AAR_A_DISPLAY_FLIP_FLAGS`. Replied when shown; out `AAR_R_DISPLAY_FLIP_FRONT`, `_BACK`, `_PITCH`, `_LAYOUT`, `_DROPPED`. A platform that owns the display buffers (a card-owned latest-frame mode) may complete a flip itself and never send it to the client. |
| `AAR_OP_DISPLAY_PALETTE` | `0x32` | request | Args `AAR_A_DISPLAY_ID`, `AAR_A_DISPLAY_PALETTE_FIRST`, `AAR_A_DISPLAY_PALETTE_COUNT`. The colors are big-endian `0x00RRGGBB` words in `buf`, which stay unchanged until the completion. The API call that submits it does not park the app, so more requests can be in flight. |
| `AAR_OP_DISPLAY_CLOSE` | `0x33` | request | Arg `AAR_A_DISPLAY_ID`. |
| `AAR_OP_AUDIO_OPEN` | `0x40` | request | Args `AAR_A_AUDIO_OPEN_RATE`, `_CHANNELS`, `_ROUTE`. The runtime allocates the audio ring first and names it in `buf` and `buf_len`; out `AAR_R_AUDIO_OPEN_RATE`, `_CHANNELS`, `_RING` (the request's `buf`, echoed), `_RING_BYTES`, `_FRAMES`. |
| `AAR_OP_AUDIO_CLOSE` | `0x41` | request | No arguments. |

File and list ids are chosen by the runtime and are small integers from
`AAR_FD_BASE`. Paths and names are NUL-terminated bytes in the payload buffer.
File payloads, paths, console bytes and palettes travel as shared buffers named
by `buf`, `buf_off` and `buf_len`, never inline.

### Argument and result indices

`A_` names index `arg[]` of a request, `R_` names index `out[]` of a
completion. A 64-bit value is `_HI` then `_LO`.

| Symbol | Index | Used by |
|---|---|---|
| `AAR_A_FILE_OPEN_FD` | 0 | `AAR_OP_FILE_OPEN` |
| `AAR_A_FILE_OPEN_FLAGS` | 1 | `AAR_OP_FILE_OPEN` |
| `AAR_A_FILE_FD` | 0 | close, read, write, seek |
| `AAR_A_FILE_LEN` | 1 | read, write |
| `AAR_A_FILE_OFF_HI`, `AAR_A_FILE_OFF_LO` | 1, 2 | seek |
| `AAR_A_FILE_WHENCE` | 3 | seek |
| `AAR_R_FILE_COUNT` | 0 | read, write |
| `AAR_R_FILE_POS_HI`, `AAR_R_FILE_POS_LO` | 1, 2 | seek |
| `AAR_R_STAT_SIZE_HI`, `AAR_R_STAT_SIZE_LO` | 0, 1 | stat |
| `AAR_R_STAT_MTIME_HI`, `AAR_R_STAT_MTIME_LO` | 2, 3 | stat |
| `AAR_R_STAT_MODE` | 4 | stat |
| `AAR_A_LIST_ID` | 0 | list open, next, close |
| `AAR_R_LIST_MODE` | 0 | list next |
| `AAR_R_CONSOLE_COUNT` | 0 | console write, read |
| `AAR_A_DISPLAY_OPEN_W`, `AAR_A_DISPLAY_OPEN_H` | 0, 1 | display open |
| `AAR_A_DISPLAY_OPEN_FORMAT`, `AAR_A_DISPLAY_OPEN_FLAGS` | 2, 3 | display open |
| `AAR_R_DISPLAY_OPEN_ID` | 0 | display open |
| `AAR_R_DISPLAY_OPEN_PITCH`, `AAR_R_DISPLAY_OPEN_LAYOUT` | 1, 2 | display open |
| `AAR_R_DISPLAY_OPEN_FRONT`, `AAR_R_DISPLAY_OPEN_BACK` | 3, 4 | display open. Tokens of the client's bitmap memory. |
| `AAR_R_DISPLAY_OPEN_SPARE` | 5 | display open |
| `AAR_A_DISPLAY_ID` | 0 | flip, palette, close |
| `AAR_A_DISPLAY_FLIP_FLAGS` | 1 | flip; `AAR_FLIPF_*` |
| `AAR_A_DISPLAY_PALETTE_FIRST`, `AAR_A_DISPLAY_PALETTE_COUNT` | 1, 2 | palette |
| `AAR_R_DISPLAY_FLIP_FRONT`, `AAR_R_DISPLAY_FLIP_BACK` | 0, 1 | flip |
| `AAR_R_DISPLAY_FLIP_PITCH`, `AAR_R_DISPLAY_FLIP_LAYOUT` | 2, 3 | flip |
| `AAR_R_DISPLAY_FLIP_DROPPED` | 4 | flip. Cumulative frames replaced in latest mode. |
| `AAR_A_AUDIO_OPEN_RATE`, `AAR_A_AUDIO_OPEN_CHANNELS` | 0, 1 | audio open |
| `AAR_A_AUDIO_OPEN_ROUTE` | 2 | audio open; `AAR_AUDIO_ROUTE_*` |
| `AAR_R_AUDIO_OPEN_RATE`, `AAR_R_AUDIO_OPEN_CHANNELS` | 0, 1 | audio open, the granted values |
| `AAR_R_AUDIO_OPEN_RING` | 2 | audio open. Token of the audio ring buffer: the `buf` of the request, echoed. |
| `AAR_R_AUDIO_OPEN_RING_BYTES` | 3 | audio open |
| `AAR_R_AUDIO_OPEN_FRAMES` | 4 | audio open. Ring capacity, a power of two. |

| Symbol | Value | Meaning |
|---|---|---|
| `AAR_FLIPF_ASYNC` | `1 << 0` | Flip flag: the app does not wait for the completion. |
| `AAR_AUDIO_ROUTE_PLATFORM` | 0 | The platform's own mixer or fabric. |
| `AAR_AUDIO_ROUTE_AHI` | 1 | AHI through the client, which reads PCM with its audio read call. |

### The audio ring buffer

The shared buffer that the runtime allocates at audio open and names in the
request's `buf`; the completion echoes it in `AAR_R_AUDIO_OPEN_RING`. Unlike the control block it
is **little-endian**. Each index has its own line and one writer.
The three pointers of `aar_audio` point into one mapping of the ring: if `ring`
is the address of its first byte, `write_frame` is `ring`, `read_frame` is
`ring + 0x40` and `samples` is `ring + 0x80`.

| Symbol | Offset | Meaning |
|---|---|---|
| `AAR_AUDIO_RING_WRITE_FRAME` | `0x00` | Free-running frame count. The app advances it. |
| `AAR_AUDIO_RING_RATE` | `0x04` | Granted rate. Written by the runtime at open. |
| `AAR_AUDIO_RING_CHANNELS` | `0x08` | Granted channels. Runtime, at open. |
| `AAR_AUDIO_RING_FORMAT` | `0x0C` | `AAR_AUDIO_FORMAT_S16LE`. Runtime, at open. |
| `AAR_AUDIO_RING_READ_FRAME` | `0x40` | Free-running frame count. The consumer advances it. |
| `AAR_AUDIO_RING_UNDERRUNS` | `0x44` | Cumulative underruns. The consumer counts them. |
| `AAR_AUDIO_RING_HDR_BYTES` | `0x80` | The samples start here: interleaved S16LE, `frames` frames. |
| `AAR_AUDIO_FORMAT_S16LE` | 1 | The only sample format. |

## Visibility of the control structures

The control block and the queues sit in ordinary shared memory with the same
mapping as an app's shared buffer. A platform that is not coherent between the
ARM side and the host must map these structures non-cacheable on the ARM side,
or use real `aar_publish` and `aar_acquire` internally. The ordering above is
about the CPU; it does not clean or invalidate caches. Prove the platform's
mapping with a probe and a negative control (the same test with the shared
attribute removed must fail) before the runtime relies on it.

**ZZ9000.** The control block and queues come from the ordinary host-visible
heap with the normal write-back shareable mapping, which the Zorro master
reaches coherently through the Zynq ACP. The ARM side orders data before the
index with barriers and needs no cache maintenance. The 68k side maps them
cache-inhibited. A probe with this layout passed on an A4000 with a 68060 in
November 2026: firmware code on both ARM cores wrote and read 64-entry rings
of 64-byte entries and a control block in host-visible memory, idle and under
load, with no stale index, torn entry or wrong word on either side, while the
same firmware with the shared attribute removed from the host-visible mapping
failed at once.

## Doorbell

A doorbell is a **coalescing hint**. The client rings it after it posts a
completion entry or frees submission space. The runtime rings it after it posts
a submission entry. A missed or merged ring must not lose an entry, because the
receiver always re-reads the queue indices; the indices are authoritative. A
platform that can only poll is correct, and a platform that can raise an
interrupt or a signal is faster. The ring carries no data, and it is not part
of the control block.

**ZZ9000.** The client-to-ARM ring is a mailbox write that names the handle.
The ARM-to-client direction is a vertical-blank poll of the queue indices today;
an interrupt is possible.

## Event counts (optional)

A platform MAY publish **event counts**: monotonically increasing 32-bit
counts that a client reads as a hint, to avoid reading the control block of
every armed handle at every poll. There is one global count and one count per
group of 32 process records (record index = the handle's low 16 bits). Each time
the runtime makes something new visible to a client for a record (it publishes a
submission entry, or writes a non-zero end reason), it orders that store before
the increments (a barrier), then increments the count of the record's group and
then the global count, each by one with an atomic read-modify-write, as the
runtime may post from more than one core. A record that has no group (its group
would lie beyond the block) increments the global count only; a client treats
such a record as changed whenever the global count changed. Counts wrap around;
a client compares them for difference only. The counts only go up, so a client
needs nothing to clear and races with nothing.

A client that reads the global count and finds it equal to the one it saw last
knows that nothing was posted in between and skips the queues of its handles;
when it differs, the client reads the group counts and looks at the handles of the
groups that changed. A client that ignores the counts is still correct: they
carry no data and the indices stay authoritative.

**ZZ9000.** The block is 64 bytes from the host-visible heap, written with the
normal write-back shareable mapping and zero-filled at start: big-endian word 0
is the global count and words 1 to 15 the counts of groups 0 to 14 (records 0 to
479). The QUERY reply carries its board offset at offset 48 (0 when absent); a
reply shorter than 52 bytes has no such word, and the client then does not use
counts.

## Lease and stale handles

The client keeps the app alive with a lease. The platform stops an app whose
owner has not advanced `AAR_CB_LEASE_COUNTER` within the lease period
(`AAR_END_LEASE`). The library must renew it from an interrupt or timer
context, so that a requester that blocks the owner task does not stop the app.
Every request names the handle, and a stale generation fails with
`AAR_E_STALE`. After a lease loss, a platform that shares memory with the client
must not reuse that memory for another app while the old client could still
write to it. The ZZ9000 keeps the app's control block, queues and shared buffers
allocated until the old client's next stale request or an explicit release.

## Reusing the library core

The internal transport interface is not part of the standard. It is named here
so that a port can reuse the shared library code above it:

| Operation | Purpose |
|---|---|
| `claim` | Admission. Returns the handle and the tokens, or a refusal with a reason. |
| `release` | Frees the record, tables, control block and queues after teardown. |
| `load_begin`, `load_chunk`, `load_end` | Stream the image through a shared buffer, with progress in the control block. |
| `start`, `stop` | Control calls. |
| `status` | Reads the control block. |
| `queue_post`, `queue_pop` | Produce and consume entries with the barriers above. |
| `resolve` | Turns a token into a 68k pointer and sets the 68k cache mode. |
| `cache_publish`, `cache_acquire` | 68k data cache maintenance, or a no-op. |
| `lease_renew` | One store into `AAR_CB_LEASE_COUNTER`, from the timer or interrupt context. The reference library core does it, so a platform supplies nothing. |
| `doorbell_ring`, `doorbell_arm` | The doorbell, where the platform can raise an interrupt or a signal. |
| `events_sample`, `claim_news` | Optional event counts: the sample reads the global count once per pass (and the group counts if it changed); `claim_news` says, with no card access, whether a claim's group changed. A platform without them leaves both NULL. |

In the library's source the platform interface is `client/src/aar_plat.h`: a
table of operations (attach, query, claim, release, the three load steps, start,
stop, map and token, the doorbell, the extension table and the barriers) and a
set of constants. The library core reads and writes the control block and the
queues itself with the accessors of `transport.h`, so a platform does not hide
them. The library renews the lease itself, from a vertical-blank interrupt, for
every live handle. The handle's owner and the one signal bit per owner task are
the library's business and not the platform's.
