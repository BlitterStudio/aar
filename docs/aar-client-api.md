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
work on every platform that implements the standard. The ZZ9000 is the
reference platform.

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
that owns a handle: the library takes the new name for a different task.

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
freed at its last close. All its handles share it. `AARMessageSignal` returns
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
an event is still on the port, the library posts nothing at that moment; as
soon as the client has taken the old event, it posts one that carries the end
status (its type is `AAR_NOTIFY_MESSAGE` while a message is still waiting,
else `AAR_NOTIFY_COMPLETE` or `AAR_NOTIFY_FAULT`). Until then the owner is
signalled at each vertical blank. Read `AARStatus` after every event: the
status in an event is the one of the moment it was posted.

Only handles that have been armed are watched: calling `AARMessageSignal` or
giving a notify port arms one. A client that polls from its own timer (aar-run
does, every millisecond by default) never arms and costs the library nothing:
a handle that is not armed and has no doorbell or mouse event waiting is not
visited at all by the library's dispatcher, so the cost of the library does not
grow with the number of such handles. On the ZZ9000 the library looks at armed
handles at each vertical blank, so a wake-up takes up to 20 ms on a PAL machine,
and each armed handle costs three reads of its control block per blank. A client
that needs a faster response polls `AARServiceNext` itself.

A call that takes a handle finds it in a table in one or two steps, however many
handles are open, and `AAROpen` and `AARClose` do not walk the handles either.

### The lease

The platform revokes a claim whose client stops answering. The library renews
the lease from a vertical-blank interrupt, so a client that is blocked for
a long time, in a requester or under `Forbid()`, keeps its app. A client that
is stuck for good but still exists keeps its app too, until someone kills the
client, then the library releases it as for any dead owner. If the library's
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
| 28 | `max_apps` | apps at once; 0: limited by resources only (ZZ9000) |
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
The probe does not judge it against the platform: compare `profile` with
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

A snapshot of one app (136 bytes). `AARU64` is two `ULONG`, high word first.
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

### AAREvent

What the library posts to a notify port (164 bytes): a `struct Message` of 20
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

### AAR_LIB_NAME and AAR_LIB_VERSION

`AAR_LIB_NAME` is `"aar.library"`. `AAR_LIB_VERSION` is the version to ask
`OpenLibrary` for. It is `AAR_API_MAJOR`.

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
The ZZ9000 uses it; other platforms ignore it.

### AAR_CCAP_DISPLAY_CLUT8, AAR_CCAP_DISPLAY_PAN_ACK, AAR_CCAP_DISPLAY_LATEST, AAR_CCAP_KNOWN

What the serving code can do, for `AAR_TAG_CLIENT_CAPS`. They are bits 0 to 2.
`AAR_CCAP_KNOWN` is the mask of all of them. A platform ignores a bit it cannot use.

| Bit | Meaning |
|---|---|
| `AAR_CCAP_DISPLAY_CLUT8` | serves 8-bit CLUT screens and palettes |
| `AAR_CCAP_DISPLAY_PAN_ACK` | acknowledges pan flips (`AAR_CFLAG_FLIP_SUBMITTED`) |
| `AAR_CCAP_DISPLAY_LATEST` | accepts flips that the platform completes itself (`AAR_DISPLAY_LATEST`) |

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
| `AAR_TAG_SHARED_MIB` | `ULONG` MiB | limit on its shared buffers; 0 takes the default; above 4095 is `AAR_E_INVAL`; the platform may grant less |
| `AAR_TAG_WEIGHT` | `ULONG` | CPU share weight, 0 to `AARInfo.weight_max`; 0 takes the default of 1; `AAR_E_NOSYS` where `weight_max` is 0 |
| `AAR_TAG_LEASE_EXTENDED` | `BOOL` | the long lease; default TRUE |
| `AAR_TAG_AUDIO_ROUTE` | `ULONG` | `AAR_AUDIO_ROUTE_PLATFORM` or `AAR_AUDIO_ROUTE_AHI`: the client says which route it serves; any other value is `AAR_E_INVAL` |
| `AAR_TAG_CLIENT_CAPS` | `ULONG` | a mask of `AAR_CCAP_*`; a bit outside `AAR_CCAP_KNOWN` is `AAR_E_INVAL` |

An id the library does not know is `AAR_E_INVAL`. A defined tag that the
platform cannot carry is `AAR_E_NOSYS`.

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
`AAR_E_NOENT`, and `AARGetMessage` does not hand out a message that arrived
before the revoke. `AARClose` is the exception: it still releases the claim
and returns `AAR_OK` when the platform has already revoked it. These codes are
not repeated below.

### AARQuery

```c
LONG AARQuery(AARInfo *info);
```

Fills `info` with what the platform offers. It needs no handle, starts nothing
and does not need `mmu.library`. It asks the platform every time, because the
live counts in `platform` change. On the ZZ9000 that is one mailbox call, about
2 ms. Returns `AAR_E_NOSYS` when there is no platform: no board, a
`zz9k.library` that is too old, or firmware without the app runtime.

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
| `AAR_E_NOSYS` | no platform, or `mmu.library` is missing (ZZ9000) |
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
probe applies the image rules of `tools/aar-check` and returns what the note
says. The note's API numbers are judged against the API this library was built
with: a note for another major, or one that needs a newer minor, is refused.

| Error | Cause |
|---|---|
| `AAR_E_NOENT` | the file cannot be opened |
| `AAR_E_IO` | the file cannot be read |
| `AAR_E_INVAL` | not a little-endian ELF32 or ELF64 file, a header, relocation or note rule broken, no AAR note or more than one |

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
tag id, and `AAR_E_NOSYS` for a tag the platform cannot carry.

### AARLoadFile

```c
LONG AARLoadFile(AARHandle *handle, const char *path);
```

Loads the image in the file. The library opens `path` itself and resolves
nothing: pass a path that `Open()` can open (a launcher that searches assigns
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

Starts the loaded image with `argc` strings, at most `AARInfo.max_args`, each
under `AARInfo.max_arg_bytes` bytes with its NUL. The first is the program
name. `argv` may be NULL when `argc` is 0. Returns `AAR_E_INVAL` for a bad
argument, or when no image is loaded (a load that failed or was cut short counts as
none), or the app has started already. When the platform itself fails the start,
the error is returned, the handle cannot be started again, and the next step is
`AARClose`.

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
Drain it until `AAR_E_BUSY` after a signal, and serve requests as well: a full
message inbox holds up the requests behind it. Log records in front of the
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
| `AAR_E_NOSYS` | the platform cannot map (ZZ9000: no `mmu.library`) |
| `AAR_E_STALE` | the handle's claim has been revoked |

### AARSharedUnmap

```c
LONG AARSharedUnmap(AARHandle *handle, struct AARSharedMap *map);
```

Gives the map up. On the ZZ9000 this is bookkeeping only: restoring the page
properties takes most of a second, so the pages stay cache-inhibited.

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
are moved aside; log records go to the sink of [`AARSetLogSink`](#aarsetlogsink). Returns `AAR_OK` with `req` filled, `AAR_E_BUSY` when none is
waiting, or `AAR_E_IO` when the queue is corrupt. A request whose buffer cannot
be mapped is answered with `AAR_E_INVAL` by the library and not returned.
`req->ptr` is the mapped payload. `out`, `result` and `reply_flags` come back
zero.

### AARServiceReply

```c
LONG AARServiceReply(AARHandle *handle, const AARServiceReq *req);
```

Posts the completion for a request: its `op`, `id`, `result`, `reply_flags` and
`out`. It resumes the app if the app is parked on that request. Every request
needs one, including those that do not park the app, because the completion
releases the request's staging storage. Returns `AAR_E_BUSY` when the
completion queue is full: keep the request and try again. A reply whose `op`
and `id` match no outstanding request is `AAR_E_INVAL`. The library notes an
`AAR_OP_AUDIO_OPEN` completion with `AAR_OK` to attach the audio ring for
`AARAudioRead`, and `AAR_OP_AUDIO_CLOSE` to detach it. If the ring does not
check out (a bad header, a frame count that is not a power of two or does not
fit the ring), the app is not told it is open: the library posts a refusal
completion (the error as `result`, `out` zero) instead of the client's, and
`AARServiceReply` returns that error (`AAR_E_IO` or `AAR_E_INVAL`) so the
client learns that the ring was refused. The request is answered either way and
must not be replied to again.

The library does not know which ops the client serves. A client that serves
only some must answer the others with `AAR_E_NOSYS` itself. `aar_serve` does.

### AARPostEvent

```c
LONG AARPostEvent(AARHandle *handle, const aar_event *event);
```

Pushes an input event to the app: a key, the mouse, a joystick, a stop request
or a break. It takes the app's event structure from `aar/app.h`, not an
`AAREvent`. Mouse motion is merged while the queue is full or an event is
pending. Returns `AAR_E_BUSY` when a full queue drops the event; the library
counts the drop in `AARStatusInfo.events_dropped`. A merged mouse event that the
queue had no room for is posted by the library's dispatcher as soon as there is
room (within a vertical blank); the client does not have to call again.

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

The inverse of `AARSharedMap`: gives the token of a 68k address in the
platform's shared aperture, for a reply that names the client's own memory (the
bitmap of a display). `AAR_E_INVAL` for an address outside it.

### AARExt

```c
APTR AARExt(ULONG platform_id, ULONG ext_id);
```

Returns a platform extension table, or NULL when this platform does not have
that `platform_id` or `ext_id`, or when the platform cannot be attached (no
board, a `zz9k.library` that is too old). A table is size-first and
append-only. For the ZZ9000, see [ZZ9000 extension](#zz9000-extension).

### AARSetLogSink

```c
LONG AARSetLogSink(AARHandle *handle, AARLogSink sink, APTR user);

typedef void (*AARLogSink)(APTR user, const void *data, ULONG len);
```

Sends the app's log records (`AAR_OP_LOG`) to `sink`, a function of type
`AARLogSink`, one call per record, in the order the app wrote them and once
each. `data` is the record, `len` its
length (at most `AAR_LOG_MAX`, longer records are truncated), and `user` is the
pointer given here. `data` is valid for the call only. `NULL` as `sink` removes
the sink. A new handle has none, and a record that arrives while there is none
is dropped and counted in `AARZZ9000Stats.logs_unclaimed`; it is not held for a
sink set later. Setting a sink again replaces the old one: from the next record
on, neither the old sink nor its `user` is called. The call needs no platform
support and works the same on every platform.

The library calls the sink in the owner task, from inside `AARServiceNext` or
`AARGetMessage`, so it must return quickly. From inside the sink a program may
call `AARSetLogSink` (to remove or replace the sink: the record in hand is still
delivered once, and the rest of the queue goes to the new sink or is dropped)
and `AARPostEvent`. It must not call `AARServiceNext`, `AARGetMessage` or
`AARClose`.

The sink may be removed at any time, and a program that keeps its `user` in
memory it frees must remove the sink first. `AARClose` forgets the sink, and so
does the library when it takes a handle whose owner task has gone: after the
close no call reaches the old sink or `user`. The handle calls order their
checks as every other call does: `AAR_E_OWNER`, then `AAR_E_STALE`. A task
other than the owner cannot set or remove the sink, and a call on a revoked
handle leaves the sink as it was.

The sink is called with the C calling convention of the compiler that built the
library, not through registers like the library's own vectors (the way the
functions of the extension tables are called). With GCC for m68k-amigaos that
is arguments on the stack. A program built with a compiler that passes C
arguments in registers by default (SAS/C, or GCC with `-mregparm`) must declare
the sink with the stack convention (`__stdargs` in SAS/C); the library cannot
detect a mismatch.

## Errors

| Code | When the library returns it |
|---|---|
| `AAR_OK` | success |
| `AAR_E_IO` | a queue or the control block is corrupt, a read failed, or `AARLoadFile` was interrupted by Ctrl-C |
| `AAR_E_INVAL` | a bad argument, a `size` below 4, an unknown tag or flag, a range outside the aperture, an image that fails the probe |
| `AAR_E_NOMEM` | no memory for a handle, a map or a signal bit, or the platform lacks memory |
| `AAR_E_BUSY` | a queue is full or empty (messages, requests), an option set too late, an image loaded twice |
| `AAR_E_NOENT` | a file does not exist |
| `AAR_E_NOSYS` | no platform, `mmu.library` missing, a tag or call the platform cannot do |
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
base calls stay the same on every platform; what a launcher needs of the card
beyond them lives here. `AARInfo.platform` holds, for `AAR_PLATFORM_ZZ9000`, the
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

The table has no log sink. It had a `set_log_sink` field and the type
`AARZZ9000LogSink` while the log sink was a ZZ9000 call; both are gone, and the
sink is set with [`AARSetLogSink`](#aarsetlogsink) on any platform. A program
that tested `AAR_CLIENT_EXT_ZZ9000_COVERS(ext, set_log_sink)` no longer
compiles. The table now ends at `lease_pause`; as before, a table grows only at
the end.

## Where this differs from the draft in spec 9

The spec draft listed 22 calls. The library has 25: `AARSharedToken`, `AARExt`
and `AARSetLogSink` are new. The other changes:

- The launcher's own policy (root directory, absolute paths, console in and out,
  screen mode) is not a tag. It is configuration of the serving code.
- There is one signal bit per owner task, not one per handle. A task has only
  about a dozen free bits.
- The library version is the API major, 0 for now.
- `AARPostEvent` takes the app's `aar_event`. The draft named `AAREvent`, which
  is the notify message.
- `struct AARSharedMap` has no typedef.
- The notification types are `AAR_NOTIFY_*`, and there is a fourth for a queued
  request.
- `AARStatusInfo` has the fields of the control block that a launcher reads
  (`progress`, `reason`, `lease_period_ms`, the accounting and the counters),
  and `AARServiceReq` has `flags`, `reply_flags` and `ptr`.
- The library cannot answer requests of an op the client does not serve, because
  it does not know which ops those are. The serving code does.
