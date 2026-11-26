<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# aar.library client API (0.5)

This is the reference for programs that run AAR apps from the Amiga side: a
launcher, a test tool, a debugger. An app (the ARM side) uses
[aar-api.md](aar-api.md). A platform port uses [transport.md](transport.md).

`aar.library` is a normal Exec library. It hides how the app runs and how the
Amiga reaches it. A client opens a handle, loads an image, starts it, serves
the app's requests, watches its status and closes the handle. The same calls
work on every platform that implements the standard. The normative contract is
[spec section 9](../spec/aar.md).

The files, all under `client/`:

| File | What it holds |
|---|---|
| `include/libraries/aar.h` | the structures, tags and constants of this page |
| `include/libraries/aar_zz9000.h` | the ZZ9000 platform words and its client extension table |
| `include/proto/aar.h` | what a program includes: the prototypes, and for GCC the inline callers |
| `include/clib/aar_protos.h`, `pragmas/aar_pragmas.h`, `inline/aar.h` | generated from `fd/aar_lib.fd`, never edited |
| `fd/aar_lib.fd` | the vector list |

The headers reuse `include/aar/app.h` and `include/aar/transport.h` and do not
repeat them. The error codes `AAR_E_*`, the refusal reasons `AAR_REASON_*`, the
lifecycle values `AAR_STATE_*`, `AAR_END_*` and `AAR_FAULT_*`, the groups
`AAR_GROUP_*`, the profiles `AAR_PROFILE_*` and the platform ids
`AAR_PLATFORM_*` are defined there and described in [aar-api.md](aar-api.md).
The request ops `AAR_OP_*`, the argument and result indexes `AAR_A_*` and
`AAR_R_*`, the control block flags `AAR_CBF_*` and the audio routes
`AAR_AUDIO_ROUTE_*` are in [transport.md](transport.md). Compile with
`-Iclient/include -Iinclude`.

## Using the library

```c
#include <proto/aar.h>

struct Library *AARBase;

int main(void)
{
    AARHandle *handle;
    ULONG reason;
    LONG rc;

    AARBase = OpenLibrary(AAR_LIB_NAME, AAR_LIB_VERSION);
    if (!AARBase)
        return 20;
    rc = AAROpen(&handle, NULL, &reason);
    if (rc == AAR_OK) {
        const char *argv[] = { "hello" };

        AARSetTags(handle, AAR_TAG_PRIVATE_MIB, 64, TAG_DONE);
        if (AARLoadFile(handle, "Work:apps/hello.elf") == AAR_OK &&
            AARStart(handle, argv, 1) == AAR_OK) {
            /* serve requests with AARServiceNext until AARStatus reports
             * an end state, then read the end reason */
        }
        AARClose(handle);
    }
    CloseLibrary(AARBase);
    return 0;
}
```

The library version is `AAR_LIB_VERSION`, which is `AAR_API_MAJOR`. Both are 0
until the API reaches 1.0, so `OpenLibrary(AAR_LIB_NAME, 0)` works. The
revision is the implementation's own.

Use `ULONG` and `LONG` for every word that you pass by address (`reason`,
`got`, `token`). On the 68k they are `unsigned long` and `long`, which are not
the types that `uint32_t` and `int32_t` name, and the compiler warns about
the mismatch.

### Structure rules

Every structure uses the AmigaOS m68k layout: big-endian, `LONG`, `ULONG` and
`APTR` of four bytes, alignment of two. Each structure that a call fills or
reads starts with `size`, and the caller sets it to the `sizeof` it was built
with. A call reads or writes at most the smaller of `size` and the library's
own size, zeroes the rest of what it fills, and never changes `size`. A `size`
below 4 is `AAR_E_INVAL`. A structure only grows at the end, so a program built
against an older header keeps working. Reserved words are zero.

### Handles and ownership

A handle belongs to the task that called `AAROpen`. Only that task may use it
or close it. Another task gets `AAR_E_OWNER`. `AARMessageSignal` is the one
exception: any task may call it.

A task may hold as many handles as admission allows. Closing the last base
of `aar.library` that a task has open closes all that task's handles. If a task
exits or is removed without closing, the library notices within about a
second, stops the app, releases it and frees the handle. Do not rename a task
that owns a handle, because the library takes the new name for a different
task.

A handle pointer is valid until `AARClose`. After that a call with it returns
`AAR_E_STALE`, as does a call on a handle whose claim the platform has
revoked, unless a later `AAROpen` has been given the same address. Do not rely
on the pointer of a closed handle for anything.

A handle may be used by one call at a time. The calls do not block on each
other for other handles.

### The sequence

1. `AARQuery` says what the platform offers. It is optional.
2. `AARProbeFile` or `AARProbeMemory` reads the image's note (optional).
3. `AAROpen` claims an app instance.
4. `AARSetTagList` sets the options, before the load.
5. `AARLoadFile` or `AARLoadMemory` loads the image.
6. `AARStart` starts it.
7. While it runs: serve its requests (`AARServiceNext`, `AARServiceReply`),
   exchange messages, post events, read audio, watch `AARStatus`.
8. `AARStop` asks it to stop. `AARStatus` reports the end.
9. `AARClose` releases everything.

### Notification

Every owner task has one signal bit, allocated at its first `AAROpen` and
freed when the task closes its last `aar.library` base, not at its last
`AARClose`. All its handles share it. `AARMessageSignal` returns
the mask. The signal is set when the app has sent a message, ended (exit or
fault) or queued a service request. After the signal, poll each handle:
`AARGetMessage` until `AAR_E_BUSY`, `AARServiceNext` until `AAR_E_BUSY`, and
`AARStatus`.

If a notify port was given to `AAROpen`, the library also posts one `AAREvent`
there at a time, for that handle, after the client has taken the previous one
off the port. The event carries the status when it was posted and a type:

| Type | Meaning |
|---|---|
| `AAR_NOTIFY_MESSAGE` | a message from the app is waiting |
| `AAR_NOTIFY_COMPLETE` | the app ended: exit, stop or forced end |
| `AAR_NOTIFY_FAULT` | the app ended in a fault, a watchdog end or a lease end |
| `AAR_NOTIFY_REQUEST` | a service request is queued |

Events coalesce. The type is `AAR_NOTIFY_MESSAGE` if a message is waiting, else
`AAR_NOTIFY_COMPLETE` or `AAR_NOTIFY_FAULT` once the app has ended, else
`AAR_NOTIFY_REQUEST`. Take the event off the port, drain the messages, then
serve the requests. Before `AARClose` drain the port and then the message ring.

The end of the app is never lost behind an earlier event. If the app ends while
an event is still on the port, the library posts nothing at that moment. Until
the client has taken the old event, the owner is signalled at each vertical
blank. After that the library posts one event that carries the end status. Its
type is `AAR_NOTIFY_MESSAGE` while a message is still waiting, else
`AAR_NOTIFY_COMPLETE` or `AAR_NOTIFY_FAULT`. Read `AARStatus` after every
event, because the status in an event is the one of the moment it was posted.

Only handles that have been armed are watched: calling `AARMessageSignal` or
giving a notify port arms one. A client that polls from its own timer (aar-run
does, every millisecond by default) never arms and costs the library nothing.
The library's dispatcher does not visit a handle that is not armed and has no
doorbell or mouse event waiting, so the cost of the library does not grow with
the number of such handles. A client that needs a faster response polls
`AARServiceNext` itself.

**ZZ9000 note.** The library looks at armed handles at each vertical blank,
so a wake-up takes up to 20 ms on a PAL machine.

On a platform that publishes event counts, an armed handle that has nothing
pending costs one read per vertical blank for all such handles together. The
library reads one word, the platform's global event count. It reads the control
block of an armed handle (three reads, each slow on a 68060) only when the count
of its group changed, while the handle has a request or message waiting, or
until its end has been announced. On a platform without event counts each armed
handle costs three reads of its control block per blank.
The counts are only a hint; the queues stay authoritative, so nothing about
notification differs for the client.

A call that takes a handle finds it in a table in one or two steps, however many
handles are open, and `AAROpen` and `AARClose` do not walk the handles either.

### The lease

The platform revokes a claim whose client stops answering. The library renews
the lease from a vertical-blank interrupt, so a client that is blocked for
a long time, in a requester or under `Forbid()`, keeps its app. A client that
is stuck for good but still exists keeps its app too, until someone kills the
client. Then the library releases it as for any dead owner. If the library's
own dispatcher process dies or stalls for five seconds, the library stops
renewing and the platform's lease expires, so a crashed library cannot hold an
app for ever. The interrupt renews one fifth of the claims at each vertical
blank, so each claim is renewed once in five blanks (about 100 ms) and the
interrupt's work is proportional to a fifth of the open handles.

The dispatcher process starts with the first call that needs the platform
(`AARQuery`, `AAROpen`, `AARExt`), and the last `CloseLibrary` stops it and
returns only after it has ended and its interrupt is gone. Neither needs a free
signal bit of the calling task, so a task that has opened no handle can start
and stop it. Expunging the library waits for the same, so a library that is
flushed from memory leaves no process running in freed memory.

## Structures

### AARInfo

What `AARQuery` returns (128 bytes). A field is 0 when the platform has no such
limit or does not report it.

| Offset | Field | Meaning |
|---|---|---|
| 0 | `size` | sizeof, set by the caller |
| 4 | `api_major` | the API major the platform implements |
| 8 | `api_minor` | the API minor |
| 12 | `platform_id` | `AAR_PLATFORM_*` |
| 16 | `profiles` | mask of `AAR_PROFILEF_*` |
| 20 | `groups` | `AAR_GROUP_*` the platform provides |
| 24 | `flags` | `AAR_INFOF_*` |
| 28 | `max_apps` | apps at once; 0: bounded by platform resources (spec section 5.6) |
| 32 | `max_image_bytes` | the largest image `AARLoadFile` takes |
| 36 | `max_args` | the most strings `AARStart` takes |
| 40 | `max_arg_bytes` | the most bytes of one string, NUL included |
| 44 | `shared_granule` | publish and acquire granularity, in bytes |
| 48 | `msg_capacity` | messages in flight per direction |
| 52 | `stall_limit_ms` | the watchdog's stall limit |
| 56 | `stop_grace_ms` | the time an app gets to honour a stop request |
| 60 | `shared_default_mib` | the default shared-buffer limit |
| 64 | `shared_max_mib` | the largest `AAR_TAG_SHARED_MIB` the platform grants |
| 68 | `weight_max` | the largest `AAR_TAG_WEIGHT`; 0: no weighting |
| 72 | `queue_depth` | entries per queue of a new claim |
| 76 | `reserved0` | 0 |
| 80 | `platform` | eight platform words; `platform_id` says what they mean |
| 112 | `reserved1` | four reserved words, 0 |

### AARImageInfo

What the probe reports (48 bytes). It is what the image declares in its note.
The probe does not judge it against the platform. Compare `profile` with
`AARInfo.profiles` and the group masks with `AARInfo.groups` yourself.

| Offset | Field | Meaning |
|---|---|---|
| 0 | `size` | sizeof, set by the caller |
| 4 | `profile` | `AAR_PROFILE_A32` or `AAR_PROFILE_A64` |
| 8 | `api_major` | the major the app was built against |
| 12 | `api_minor_required` | the lowest minor with every call the app needs |
| 16 | `stack_bytes` | as in the note; 0 is the 1 MiB default |
| 20 | `min_heap_mib` | private heap committed at load, in MiB |
| 24 | `groups_required` | `AAR_GROUP_*` bits the app needs |
| 28 | `groups_optional` | `AAR_GROUP_*` bits, a hint |
| 32 | `note_version` | `AAR_NOTE_VERSION` |
| 36 | `image_bytes` | length of the file or memory block |
| 40 | `reserved` | two reserved words, 0 |

### AARStatusInfo

A snapshot of one app (160 bytes). `AARU64` is two `ULONG`, high word first.
The fields from `state` to `end_reason` come first so that a poll with
`size = 16` reads three words of the control block instead of about thirty.

| Offset | Field | Meaning |
|---|---|---|
| 0 | `size` | sizeof, set by the caller |
| 4 | `state` | `AAR_STATE_*` |
| 8 | `generation` | the handle's generation, from the handle, no read |
| 12 | `end_reason` | `AAR_END_*` |
| 16 | `exit_code` | the exit code, a `LONG` |
| 20 | `flags` | `AAR_CBF_*` |
| 24 | `profile` | `AAR_PROFILE_*` of the loaded image |
| 28 | `fault_class` | `AAR_FAULT_*` |
| 32 | `fault_syndrome` | |
| 36 | `fault_pstate` | |
| 40 | `fault_pc` | `AARU64` |
| 48 | `fault_sp` | `AARU64` |
| 56 | `fault_lr` | `AARU64` |
| 64 | `fault_addr` | `AARU64` |
| 72 | `load_base` | `AARU64` |
| 80 | `reason` | the last `AAR_REASON_*` of a refused open, load or start |
| 84 | `lease_period_ms` | |
| 88 | `msg_consumed` | client-to-app messages the app has taken |
| 92 | `reserved` | 0 |
| 96 | `progress` | `AARU64`, load progress in bytes |
| 104 | `shared_used` | `AARU64` |
| 112 | `private_committed` | `AARU64` |
| 120 | `sq_full` | times the app side found the submission queue full |
| 124 | `log_dropped` | log records dropped |
| 128 | `events_dropped` | events the client dropped (completion queue full) |
| 132 | `cq_full` | times the client found the completion queue full |
| 136 | `stack_sp` | `AARU64`, the app address of the first byte of the stack snapshot |
| 144 | `stack_bytes` | bytes of the snapshot, 0 when there is none (see [AARReadStack](#aarreadstack)) |
| 148 | `profile_hz` | the sampling rate the platform granted, 0 when it samples nothing |
| 152 | `profile_samples` | the samples the platform has written so far, modulo 2^32 (the ring's head) |
| 156 | `profile_dropped` | ticks at which the platform could not record a sample |

The fault fields also describe a watchdog end. For `AAR_END_STALL` the platform
fills `fault_pc`, `fault_sp` and `fault_lr` with where the app was when it ended
it, `fault_class` is `AAR_FAULT_NONE` and the other fault fields are 0.

### AAREvent

What the library posts to a notify port (188 bytes): a `struct Message` of 20
bytes with no reply port (do not `ReplyMsg` it), `type` (an `AAR_NOTIFY_*`
value), `handle` (the `AARHandle *`, kept as an `APTR`) and the `AARStatusInfo`
at the time of posting.

### AARMessage

One message in either direction: `AAR_MSG_BYTES` (32) opaque bytes in `bytes`.
The library never swaps them. The app and the client agree on their layout.

### struct AARSharedMap

A mapped shared buffer (32 bytes). It has no typedef, because its name is also
the name of the call that fills it. Write `struct AARSharedMap`.

| Offset | Field | Meaning |
|---|---|---|
| 0 | `size` | sizeof, set by the caller |
| 4 | `base` | the 68k pointer to the first byte |
| 8 | `bytes` | |
| 12 | `haddr` | the token the app gave |
| 16 | `flags` | `AAR_MAPF_UNCACHED` or `AAR_MAPF_COHERENT` |
| 20 | `granule` | publish and acquire granularity |
| 24 | `reserved` | two words; the first belongs to the library |

`AAR_MAPF_UNCACHED` says the map is cache-inhibited. `AAR_MAPF_COHERENT` says
the platform is coherent end to end, so the client needs no cache care.

### AARServiceReq

One service request from the app and the reply the client builds in the same
structure (96 bytes). Which words a request uses is given by the `AAR_A_*`
indexes for `arg` and the `AAR_R_*` indexes for `out` of each op, in
[transport.md](transport.md).

| Offset | Field | Direction | Meaning |
|---|---|---|---|
| 0 | `size` | in | sizeof, set by the caller |
| 4 | `op` | in | `AAR_OP_*` |
| 8 | `id` | in | echoed in the completion |
| 12 | `result` | out | `AAR_OK` or a negative `AAR_E_*` |
| 16 | `arg` | in | six words |
| 40 | `out` | out | six words |
| 64 | `buf` | in | token of the payload buffer, 0 for none |
| 68 | `buf_off` | in | byte offset inside that buffer |
| 72 | `buf_len` | in | payload bytes |
| 76 | `flags` | in | the entry's flags word; 0 when the op has none |
| 80 | `reply_flags` | out | `AAR_DISPCAP_*` for `DISPLAY_OPEN`, `AAR_CFLAG_*` for a flip |
| 84 | `ptr` | in | the library's mapping of `buf + buf_off`, NULL when `buf` is 0 |
| 88 | `reserved` | | two words, 0 |

`ptr` is a convenience. A client that wants the map object calls
`AARSharedMap` with `buf`.

### AARU64

`ULONG hi, lo`. A 64-bit value, high word first.

## Constants

### AAR_LIB_NAME, AAR_LIB_VERSION and AAR_LIB_REVISION_READ

`AAR_LIB_NAME` is `"aar.library"`. `AAR_LIB_VERSION` is the version to ask
`OpenLibrary` for. It is `AAR_API_MAJOR`. `AAR_LIB_REVISION_READ` (2) is the first
revision of the library that has `AARReadStack` and `AARReadProfile`, vectors 26
and 27. `OpenLibrary` checks the version only, so a program that calls them
compares `AARBase->lib_Revision` with it after the open and does not start on
an older library: a call through a vector the library lacks jumps into
whatever follows its vector table.

### AAR_PROFILEF_A32 and AAR_PROFILEF_A64

The profiles as bits, for `AARInfo.profiles`. `AAR_PROFILE_A32` and
`AAR_PROFILE_A64` are values, for `AARImageInfo.profile`.

### AAR_NOTIFY_MESSAGE, AAR_NOTIFY_COMPLETE, AAR_NOTIFY_FAULT, AAR_NOTIFY_REQUEST

The values of `AAREvent.type`, 1 to 4. See [Notification](#notification).

### AAR_MAPF_UNCACHED and AAR_MAPF_COHERENT

The values of `AARSharedMap.flags`, 1 and 2.

### AAR_CFLAG_FLIP_SUBMITTED

A `reply_flags` bit for the completion of `AAR_OP_DISPLAY_FLIP`. The client has
submitted the pan or buffer change and relies on the platform's vertical blank
to show it. Without the bit the flip is complete when the completion arrives.

**ZZ9000 note.** This platform uses the submitted-pan flag.

### AAR_CCAP_DISPLAY_CLUT8, AAR_CCAP_DISPLAY_PAN_ACK, AAR_CCAP_DISPLAY_LATEST, AAR_CCAP_KNOWN

What the serving code can do, for `AAR_TAG_CLIENT_CAPS`. They are bits 0 to 2.
`AAR_CCAP_KNOWN` is the mask of all of them. A platform ignores a bit it cannot
use.

| Bit | Meaning |
|---|---|
| `AAR_CCAP_DISPLAY_CLUT8` | serves 8-bit CLUT screens and palettes |
| `AAR_CCAP_DISPLAY_PAN_ACK` | acknowledges pan flips (`AAR_CFLAG_FLIP_SUBMITTED`) |
| `AAR_CCAP_DISPLAY_LATEST` | accepts flips that the platform completes itself (`AAR_DISPLAY_LATEST`) |

### AAR_PROFILE_HZ_MAX

The largest value of `AAR_TAG_PROFILE_HZ` the library accepts, 10000. The
platform may grant less.

### AAR_TAG_BASE and AAR_TAG_PLATFORM_BASE

`AAR_TAG_BASE` is `TAG_USER + 0x00415200` and gives 256 tag ids. A platform's own
tags start at `AAR_TAG_PLATFORM_BASE(platform_id)`.

## Tags

The tags of `AARSetTagList` set options before the load. They return
`AAR_E_BUSY` once a load has begun or the app has started. A tag list is checked
in full before any tag is applied, so a list with a bad tag changes nothing.
`TAG_MORE`, `TAG_SKIP` and `TAG_IGNORE` work as everywhere.

| Tag | Data | Meaning |
|---|---|---|
| `AAR_TAG_PRIVATE_MIB` | `ULONG` MiB | cap on the app's private memory; 0 takes the platform's default; above 4095 is `AAR_E_INVAL` |
| `AAR_TAG_SHARED_MIB` | `ULONG` MiB | limit on shared buffers; 0 takes the default; above `AARInfo.shared_max_mib` or 4095 is `AAR_E_INVAL` (spec section 4.2) |
| `AAR_TAG_WEIGHT` | `ULONG` | CPU share weight, 0 to `AARInfo.weight_max`; 0 takes the default of 1; `AAR_E_NOSYS` where `weight_max` is 0 |
| `AAR_TAG_LEASE_EXTENDED` | `BOOL` | the long lease; default TRUE |
| `AAR_TAG_AUDIO_ROUTE` | `ULONG` | `AAR_AUDIO_ROUTE_PLATFORM` or `AAR_AUDIO_ROUTE_AHI`: the client says which route it serves; any other value is `AAR_E_INVAL` |
| `AAR_TAG_CLIENT_CAPS` | `ULONG` | a mask of `AAR_CCAP_*`; a bit outside `AAR_CCAP_KNOWN` is `AAR_E_INVAL` |
| `AAR_TAG_PROFILE_HZ` | `ULONG` | samples per second of the app's program counter, 0 for none; above `AAR_PROFILE_HZ_MAX` is `AAR_E_INVAL`. A platform without a profiler accepts the tag and grants 0, and it may grant a lower rate than asked: `AARStatusInfo.profile_hz` says what it grants (see [AARReadProfile](#aarreadprofile)) |

An id the library does not know is `AAR_E_INVAL`. A defined tag that the
platform cannot support is `AAR_E_NOSYS`.

What the launcher alone needs is not a tag. The root directory for relative
paths, absolute paths, the console windows and the screen mode belong to the
serving code (`aar_serve`), which decides them when it answers file, console
and display requests. The library has no call to read them back, so it does not
take them.

## Calls

All calls return a `LONG`: `AAR_OK` (0) or a negative `AAR_E_*`, except
`AARMessageSignal` and `AARExt`. Every call that takes a handle returns
`AAR_E_OWNER` when the calling task does not own the handle, and `AAR_E_STALE`
when the handle is closed or revoked. The owner is checked first, then whether
the claim is still live, and only then the arguments and any file open. A
revoked handle therefore returns `AAR_E_STALE`, not `AAR_E_INVAL` or
`AAR_E_NOENT`, and `AARGetMessage` does not return a message that arrived
before the revoke. `AARClose` is the exception: it still releases the claim
and returns `AAR_OK` when the platform has already revoked it. These codes are
not repeated below.

### AARQuery

```c
LONG AARQuery(AARInfo *info);
```

Fills `info` with what the platform offers. It needs no handle and starts
nothing. It asks the platform every time, because live counts in `platform`
may change. Returns `AAR_E_NOSYS` when the platform cannot be attached.

**ZZ9000 note.** Query needs no shared-memory mapping. Hardware and library
requirements are in the [aar-run manual](../client/run/README.md#platform-notes).

### AAROpen

```c
LONG AAROpen(AARHandle **handlep, struct MsgPort *port, ULONG *reason);
```

Claims an app instance for the calling task. `*handlep` receives the handle,
or NULL on failure. `port` may be NULL; if given, the library posts one
`AAREvent` there at a time. `reason` may be NULL. It receives an `AAR_REASON_*`
code with `AAR_E_NORESOURCE`, and `AAR_REASON_NONE` otherwise.

| Error | Cause |
|---|---|
| `AAR_E_NORESOURCE` | the platform refused the claim; `*reason` says why |
| `AAR_E_NOSYS` | no platform, or the platform cannot map the claim's shared memory |
| `AAR_E_NOMEM` | no memory for the handle, or the task has no free signal bit |
| `AAR_E_IO` | the control block or the queues did not check out |

### AARClose

```c
LONG AARClose(AARHandle *handle);
```

Stops the app if it is running, releases the claim and frees the handle. Only
the owner may close. It returns `AAR_OK` for a handle whose claim is already
gone, and the final status stays readable until this call. After it, the
pointer is invalid and every map made for the handle is gone. Drain the notify
port first.

### AARProbeFile

```c
LONG AARProbeFile(const char *path, AARImageInfo *info);
```

Reads the AAR note of an image without loading it. It needs no handle. The
probe applies the image rules of the specification (2.2 and 2.3) as the
platform's loader does. An image that breaks one is refused, and an image the
loader would accept is accepted, with or without section headers.
`tools/aar-check` is stricter: it also requires the section headers and compares
them with the segments, which is a check of build output. The probe does not
judge the image against the platform. Profile, groups, heap and stack are
reported, not checked. The note's API numbers are judged against the API this
library was built with. A note for another major, or one that needs a newer
minor, is refused.

| Error | Cause |
|---|---|
| `AAR_E_NOENT` | the file cannot be opened |
| `AAR_E_IO` | the file cannot be read |
| `AAR_E_INVAL` | not a little-endian ELF32 or ELF64 file, a header, relocation or note rule broken, no AAR note or more than one, a note with a wrong magic or version, or a profile word that differs from the ELF class |

`AAR_E_PROFILE` is not returned. The probe reports the `profile`, and the
launcher chooses.

### AARProbeMemory

```c
LONG AARProbeMemory(const void *data, ULONG length, AARImageInfo *info);
```

The same for an image in memory. `data` needs no alignment. It returns
`AAR_E_INVAL` for a NULL pointer with a length, and the errors of
`AARProbeFile` except the two file errors.

### AARSetTagList

```c
LONG AARSetTagList(AARHandle *handle, const struct TagItem *tags);
LONG AARSetTags(AARHandle *handle, Tag tag1, ...);
```

Sets the options of [Tags](#tags). `AARSetTags` is the same call with the tags
written out, ended by `TAG_DONE`. Returns `AAR_E_BUSY` once the load has begun
or the app has started, `AAR_E_INVAL` for an out-of-range value or an unknown
tag id, and `AAR_E_NOSYS` for a tag the platform cannot support.

### AARLoadFile

```c
LONG AARLoadFile(AARHandle *handle, const char *path);
```

Loads the image in the file. The library opens `path` itself and resolves
nothing, so pass a path that `Open()` can open (a launcher that searches assigns
does it first). It reads in chunks and checks `SIGBREAKF_CTRL_C` between them.
On a break it returns `AAR_E_IO` and leaves the signal set. The load may take
seconds. A load that keeps making progress is not timed out.

| Error | Cause |
|---|---|
| `AAR_E_BUSY` | a load was already tried on this handle (also one that failed or was cut short), or the app has started |
| `AAR_E_NOENT` | the file cannot be opened |
| `AAR_E_IO` | a read failed, or Ctrl-C |
| `AAR_E_INVAL` | the file is empty or larger than `AARInfo.max_image_bytes`, or the platform's loader refuses the image |
| `AAR_E_PROFILE`, `AAR_E_NOGROUP`, `AAR_E_NOMEM` | the platform's loader refuses the image (spec 2.3) |
| `AAR_E_NORESOURCE` | the platform lacks the memory; `AARStatusInfo.reason` says which |

### AARLoadMemory

```c
LONG AARLoadMemory(AARHandle *handle, const void *data, ULONG length);
```

The same for an image in memory.

Only a load that completed can be started. A load that fails after it began (a
failed chunk or end, a Ctrl-C, a read error) leaves the handle unloaded for good:
`AARStart` returns `AAR_E_INVAL` and a new load returns `AAR_E_BUSY`. Close
the handle and open another. A load the platform refused to begin (for example
`AAR_E_NORESOURCE` with a `reason`) leaves the handle as it was, and a new
load may be tried on it.

### AARStart

```c
LONG AARStart(AARHandle *handle, const char *const *argv, ULONG argc);
```

Starts the loaded image with `argc` strings, at most `AARInfo.max_args`
(`argv[0]` included), each at most `AARInfo.max_arg_bytes` bytes including
its NUL. The first is the program name. `argv` may be NULL when `argc` is 0.
Returns `AAR_E_INVAL` for a bad argument, or when no image is loaded (a load
that failed or was cut short counts as none), or the app has started already.
When the platform itself fails the start, the error is returned, the handle
cannot be started again, and the next step is `AARClose`.

### AARStop

```c
LONG AARStop(AARHandle *handle);
```

Asks the app to stop: it sets the stop request, posts the stop event and tells
the platform to stop the app. It returns when the request is delivered. The end
shows in `AARStatus`. Calling it again, or on an app that has ended, is not an
error.

### AARStatus

```c
LONG AARStatus(AARHandle *handle, AARStatusInfo *status);
```

Reads a snapshot. It reads only the words that fit in `status->size`. A poll
that needs the state and the end reason passes `size = 16`. The status stays
readable after the app has ended, until `AARClose`. `AAR_E_STALE` once the
platform has revoked the claim.

### AARSendMessage

```c
LONG AARSendMessage(AARHandle *handle, const AARMessage *message);
```

Sends 32 bytes to the app. Returns `AAR_E_BUSY` when the queue toward the app is
full. A message is never dropped.

### AARGetMessage

```c
LONG AARGetMessage(AARHandle *handle, AARMessage *message);
```

Takes the next message from the app. Returns `AAR_E_BUSY` when there is none.
Drain it until `AAR_E_BUSY` after a signal, and serve requests as well, because
a full message inbox holds up the requests behind it. Log records in front of the
message go to the sink of [`AARSetLogSink`](#aarsetlogsink), as in
`AARServiceNext`.

### AARMessageSignal

```c
ULONG AARMessageSignal(AARHandle *handle);
```

Returns the signal mask of the handle's owner, which is the same for all of
that task's handles, or 0 if the handle is not valid. Any task may call it, and
it may be called from an interrupt. The first call arms the handle for
notification. See [Notification](#notification).

### AARSharedMap

```c
LONG AARSharedMap(AARHandle *handle, ULONG haddr, ULONG bytes, ULONG flags,
                  struct AARSharedMap *map);
```

Turns a token and a byte count into a 68k pointer. `flags` is reserved: pass 0.
The library checks the range against the platform's shared aperture and makes
the memory cache-inhibited where it can. It maps a block once and reuses the
map. A map is valid until `AARSharedUnmap`, `AARClose` or the end of the app.
After `AARStatus` reports an end state, do not write through a map: the memory
may belong to another app.

| Error | Cause |
|---|---|
| `AAR_E_INVAL` | `bytes` is 0, a flag is set, the range is outside the aperture, or the range rounded up to 8 KiB does not fit in 32 bits (the same holds for the buffer of a service request, which the library refuses to the app) |
| `AAR_E_NOMEM` | no memory for the map |
| `AAR_E_NOSYS` | the platform cannot map |
| `AAR_E_STALE` | the handle's claim has been revoked |

### AARSharedUnmap

```c
LONG AARSharedUnmap(AARHandle *handle, struct AARSharedMap *map);
```

Gives the map up.

**ZZ9000 note.** Unmapping releases the library's map record but leaves the
pages cache-inhibited; see the [transport note](transport.md#visibility-of-the-control-structures).

### AARSharedPublish

```c
LONG AARSharedPublish(AARHandle *handle, struct AARSharedMap *map, ULONG offset,
                      ULONG length);
```

Makes the client's writes in `[offset, offset + length)` of the map visible to
the app (spec 4.3). The library rounds to `granule`. It does nothing on an
uncached or coherent map, and is cheap to call anyway. `AAR_E_INVAL` when the
range is outside the map.

### AARSharedAcquire

```c
LONG AARSharedAcquire(AARHandle *handle, struct AARSharedMap *map, ULONG offset,
                      ULONG length);
```

Makes the app's writes visible to the client. Same rules as publish.

### AARServiceNext

```c
LONG AARServiceNext(AARHandle *handle, AARServiceReq *req);
```

Takes the next request from the app. Messages and log records in front of it
are moved aside. Log records go to the sink of
[`AARSetLogSink`](#aarsetlogsink). Returns `AAR_OK` with `req` filled,
`AAR_E_BUSY` when none is waiting, or `AAR_E_IO` when the queue is corrupt. A
request whose buffer cannot be mapped is answered with `AAR_E_INVAL` by the
library and not returned. `req->ptr` is the mapped payload. `out`, `result` and
`reply_flags` come back zero.

### AARServiceReply

```c
LONG AARServiceReply(AARHandle *handle, const AARServiceReq *req);
```

Posts the completion for a request: its `op`, `id`, `result`, `reply_flags` and
`out`. It resumes the app if the app is parked on that request. Every request
needs one, including those that do not park the app, because the completion
releases the request's staging storage. Returns `AAR_E_BUSY` when the
completion queue is full. Keep the request and try again. A reply whose `op`
and `id` match no outstanding request is `AAR_E_INVAL` and changes nothing. The
app is not resumed and no storage is released. The late completion of a request
the app has given up on (a timeout) is not such a reply: it is accepted and
releases the storage.

The library notes an `AAR_OP_AUDIO_OPEN` completion with `AAR_OK` to attach the
audio ring for `AARAudioRead`, and `AAR_OP_AUDIO_CLOSE` to detach it. If the
ring does not check out (a bad header, a frame count that is not a power of two
or does not fit the ring), the app is not told it is open. The library posts a
refusal completion (the error as `result`, `out` zero) instead of the client's,
and `AARServiceReply` returns that error (`AAR_E_IO` or `AAR_E_INVAL`) so the
client learns that the ring was refused. The request is answered either way and
must not be replied to again.

The library does not know which ops the client serves. A client that serves
only some must answer the others with `AAR_E_NOSYS` itself, and should do the
same for an op code it does not know. `aar_serve` does. A request that nobody
completes leaves the app parked until it stops or its own wait ends.

### AARPostEvent

```c
LONG AARPostEvent(AARHandle *handle, const aar_event *event);
```

Pushes an input event to the app: a key, the mouse, a joystick, a stop request
or a break. It takes the app's event structure from `aar/app.h`, not an
`AAREvent`. The event reaches the app as given (the reserved word is 0). Mouse
motion is merged while the queue is full or an event is pending. Dropped motion
is counted, so it is never lost without a count. A posted `AAR_EV_STOP` is an
event only: it does not stop the app, which `AARStop` does. Returns
`AAR_E_BUSY` when a full queue drops the event. The library counts the drop in
`AARStatusInfo.events_dropped`. A merged mouse event that the queue had no room
for is posted by the library's dispatcher as soon as there is room (within a
vertical blank); the client does not have to call again.

### AARAudioRead

```c
LONG AARAudioRead(AARHandle *handle, void *dst, ULONG frames, ULONG *got);
```

Copies up to `frames` frames of the PCM the app wrote, for the AHI route,
into `dst` as interleaved `WORD` samples in 68k byte order. `*got` is the
number copied. Fewer than `frames` counts an underrun in the ring. Returns
`AAR_E_INVAL` when no audio ring is open, and `AAR_E_IO` when the ring is
corrupt.

With `frames` 0 the call is a query: it returns `AAR_OK` with `*got` set to the
number of frames that can be read now, consumes nothing and counts no
underrun. `dst` may be NULL then. `got` is always required.

### AARSharedToken

```c
LONG AARSharedToken(AARHandle *handle, const void *ptr, ULONG bytes, ULONG *token);
```

The inverse of `AARSharedMap`: gives the token of a range of client memory in
the platform's shared aperture, for a reply that names the client's own memory
(the surfaces of a display open). For the base of a range that `AARSharedMap`
mapped, the token equals `map.haddr`. `AAR_E_INVAL` for NULL, zero bytes or an
address outside the aperture; `AAR_E_NOSYS` where the platform cannot name host
memory to the app. A platform documents which Amiga memory is in its shared
aperture.

**ZZ9000 note.** The aperture is card memory: pixels allocated by its display
driver lie in it, but ordinary `AllocVec` memory does not.

### AARExt

```c
APTR AARExt(ULONG platform_id, ULONG ext_id);
```

Returns a platform extension table, or NULL when this platform does not have
that `platform_id` or `ext_id`, or when the platform cannot be attached. A table
is size-first and append-only. For the ZZ9000, see
[ZZ9000 extension](#zz9000-extension).

### AARSetLogSink

```c
LONG AARSetLogSink(AARHandle *handle, AARLogSink sink, APTR user);

typedef void (*AARLogSink)(APTR user, const void *data, ULONG len);
```

Sends the app's log records (`AAR_OP_LOG`) to `sink`, a function of type
`AARLogSink`, one call per record, in the order the app wrote them and once
each. `data` is the record, `len` its length (at most `AAR_LOG_MAX`; longer
records are truncated), and `user` is the pointer given here. `data` is valid
for the call only. `NULL` as `sink` removes the sink. A new handle has none, and
a record that arrives while there is none is dropped; it is not held for a sink
set later. Setting a sink again replaces the old one: from the next record on,
neither the old sink nor its `user` is called. The call needs no platform
support and works the same on every platform.

**ZZ9000 note.** Unclaimed records are counted in
`AARZZ9000Stats.logs_unclaimed`.

The library calls the sink in the owner task, from inside `AARServiceNext` or
`AARGetMessage`, so it must return quickly. From inside the sink a program may
call `AARSetLogSink` and `AARPostEvent`. It must not call `AARServiceNext`,
`AARGetMessage` or `AARClose`. When the sink is removed or replaced from inside,
the record in hand is still delivered once, and the rest of the queue goes to
the new sink or is dropped.

The sink may be removed at any time, and a program that keeps its `user` in
memory it frees must remove the sink first. `AARClose` forgets the sink, and so
does the library when it takes a handle whose owner task has gone. After the
close no call reaches the old sink or `user`. The handle calls order their
checks as every other call does: `AAR_E_OWNER`, then `AAR_E_STALE`. A task
other than the owner cannot set or remove the sink, and a call on a revoked
handle leaves the sink as it was.

The sink is called with the C calling convention of the compiler that built the
library, as are the functions of the extension tables, and not through
registers like the library's own vectors. With GCC for m68k-amigaos that
is arguments on the stack. A program built with a compiler that passes C
arguments in registers by default (SAS/C, or GCC with `-mregparm`) must declare
the sink with the stack convention (`__stdargs` in SAS/C); the library cannot
detect a mismatch.

### AARReadStack

```c
LONG AARReadStack(AARHandle *handle, void *buf, ULONG bytes, ULONG *got);
```

Copies the stack snapshot that the platform took when the app ended in a fault
(`AAR_END_USER_FAULT`) or a stall (`AAR_END_STALL`): at most `AAR_CB_STACK_MAX`
(512) bytes, from the app's stack pointer upward. The bytes are copied as they
lie in app memory, so a 32-bit app word is little-endian and the call never
swaps it. `AARStatusInfo.stack_sp` is the app address of the first byte. App
addresses are those of `fault_pc` and `fault_sp`: subtract `load_base` to find
an address in the ELF file.

Copies the smaller of `bytes` and the size of the snapshot, and sets `*got` to
the count. With `bytes` 0 it copies nothing and sets `*got` to the size of the
snapshot, and `buf` may be NULL then. `got` is always required, and `buf` is
required when `bytes` is not 0, else `AAR_E_INVAL`. `AAR_E_NOENT` when there is
no snapshot: the app has not ended in a fault or a stall, or the platform does
not take one. `AAR_E_IO` when the size in the control block is above
`AAR_CB_STACK_MAX`. The snapshot stays readable until `AARClose`.

### AARReadProfile

```c
LONG AARReadProfile(AARHandle *handle, void *buf, ULONG max_samples,
                    ULONG *got, ULONG *lost);
```

Copies the program counter samples that the platform has written since the
previous call, oldest first, up to `max_samples`. Ask for a profile with
`AAR_TAG_PROFILE_HZ` before the load. The platform samples the app's pc at that
rate or lower, and `AARStatusInfo.profile_hz` is the rate it granted.

Each sample is a 64-bit app address, high word first, which is the byte order of
the 68k, so `buf` holds `*got` pairs of `ULONG`. Subtract `load_base` for an
address in the ELF file. The first call starts at the first sample of the run.
`*got` is the number of samples copied. The platform keeps the samples in a ring
of fixed size, of which the newest one fewer than the ring holds can be trusted: a caller that comes back too late has missed the oldest samples,
and `*lost` (it may be NULL) is the number of samples that were overwritten
before this call took them. The lost samples are not in `buf`. Call it from the
serving loop often enough that the ring does not fill: `AARStatusInfo.profile_samples`
minus what has been taken (both modulo 2^32) is how far behind the caller is.

With `max_samples` 0 it copies nothing and sets `*got` to the number of samples
waiting, and `buf` may be NULL then. `got` is always required, and `buf` is
required when `max_samples` is not 0, else `AAR_E_INVAL`. `AAR_E_NOSYS` when the
platform granted no profile: none was asked for, or the platform has no
profiler. `AAR_E_IO` when the ring or its counter breaks the rules of the
control block.

## Errors

| Code | When the library returns it |
|---|---|
| `AAR_OK` | success |
| `AAR_E_IO` | a queue or the control block is corrupt, a read failed, or `AARLoadFile` was interrupted by Ctrl-C |
| `AAR_E_INVAL` | a bad argument, a `size` below 4, an unknown tag or flag, a range outside the aperture, an image that fails the probe |
| `AAR_E_NOMEM` | no memory for a handle, a map or a signal bit, or the platform lacks memory |
| `AAR_E_BUSY` | a queue is full or empty (messages, requests), an option set too late, an image loaded twice |
| `AAR_E_NOENT` | a file does not exist, or `AARReadStack` found no snapshot |
| `AAR_E_NOSYS` | no platform, or a map, tag or call the platform cannot provide (`AARReadProfile` when none was granted) |
| `AAR_E_PROFILE` | the platform's loader refuses the image's profile; the probe never returns it |
| `AAR_E_NOGROUP` | the platform's loader refuses an image that needs a group it lacks |
| `AAR_E_NORESOURCE` | admission or load refused for lack of a resource; `AAR_REASON_*` says which |
| `AAR_E_STALE` | the handle is closed, or the platform revoked its claim (for example after a lease expiry) |
| `AAR_E_OWNER` | a task other than the owner used the handle |

`AAR_E_TIMEDOUT`, `AAR_E_ACCES`, `AAR_E_FAULT`, `AAR_E_NOSPC`, `AAR_E_EXIST` and
`AAR_E_BADF` are codes of the app and service side. The library does not
return them. A client puts them in `AARServiceReq.result` as the service demands.

## ZZ9000 extension

`<libraries/aar_zz9000.h>`. Only the ZZ9000 build of the library has it. The
base calls stay the same on every platform. What a launcher needs of the card
beyond them is here. `AARInfo.platform` holds, for `AAR_PLATFORM_ZZ9000`, the
words of the table below.

| Index | Word |
|---|---|
| `AAR_ZZ9000_INFO_RECORDS_FREE` | app records free |
| `AAR_ZZ9000_INFO_TABLES_FREE` | translation tables free |
| `AAR_ZZ9000_INFO_TABLES_TOTAL` | translation tables in all |
| `AAR_ZZ9000_INFO_COMMIT_FREE` | private memory not yet committed, in bytes |
| `AAR_ZZ9000_INFO_COMMIT_TOTAL` | private memory in all, in bytes |
| `AAR_ZZ9000_INFO_HOSTHEAP_FREE` | host-visible heap free, in bytes |
| `AAR_ZZ9000_INFO_HOSTHEAP_LARGEST` | the largest free host-visible block, in bytes |
| `AAR_ZZ9000_INFO_CAPS` | a mask of `AAR_ZZ9000_CAP_Z3`, `AAR_ZZ9000_CAP_DISPLAY`, `AAR_ZZ9000_CAP_AUDIO`, `AAR_ZZ9000_CAP_GL`, `AAR_ZZ9000_CAP_CLUT8` |

`AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT)` returns a
`struct aar_client_ext_zz9000`. Test a field with
`AAR_CLIENT_EXT_ZZ9000_COVERS(ext, field)` before using it. The functions use
the C calling convention.

| Field | Call |
|---|---|
| `list` | `LONG list(ULONG cursor, AARZZ9000List *info)`: the next live claim on the card at or after `cursor`; 1 with `info` filled, 0 when there is none |
| `release_handle` | `LONG release_handle(ULONG handle)`: releases any claim by its card handle word; `AAR_E_STALE` when it is not live |
| `stats` | `LONG stats(AARHandle *handle, AARZZ9000Stats *out)`: the library's counters of one handle |
| `lease_pause` | `LONG lease_pause(AARHandle *handle, ULONG ms)`: stops renewing the lease for `ms` milliseconds, a test hook; 0 resumes |

`AARZZ9000List` is 64 bytes: `size`, `handle`, `state`, `committed`, `index`
(pass `index + 1` as the next cursor), `cpu_ms`, `service_ms`, `block_reason`,
`end_reason`, `flags` (`AAR_ZZ9000_LISTF_OVERLOAD`) and `name`, up to 20
characters. `AARZZ9000Stats` is 68 bytes: `size`, `requests`, `replies`,
`reply_busy`, `bad_buf`, `bad_entries`, `msgs_taken`, `sq_blocked`,
`msgs_sent`, `logs`, `logs_unclaimed`, `events_posted`, `events_merged`,
`events_dropped`, `kicks`, `kicks_coalesced` and `maps`.

The table has no log sink: the sink is set with
[`AARSetLogSink`](#aarsetlogsink) on any platform. The table ends at
`lease_pause`; a table grows only at the end.
