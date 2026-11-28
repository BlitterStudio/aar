<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# AAR API reference (0.6)

The public contract of `include/aar/app.h`, `note.h`, `gl.h` and
`ext_zz9000.h`. The transport headers for platform implementers are covered in
[transport.md](transport.md). The normative text is the
[specification](../spec/aar.md); this page is the call reference. How to build
and run an app is in the [developer guide](aar-developer-guide.md). The Amiga
side, `aar.library`, which a launcher uses to load, start and serve an app, is
in [aar-client-api.md](aar-client-api.md).

`AAR_API_MAJOR` is 0 and `AAR_API_MINOR` is 6. Each entry's "Minimum minor"
gives the version that introduced it. Compatibility starts at the first
shipped baseline; this draft accepts only API 0.6.

## How an app reaches the runtime

An app is a position-independent ARM ELF in profile A32 or A64. It talks to
its runtime through one function table, `aar_api`, and through nothing else.
The runtime passes the table pointer to the entry point (`r0` on A32, `x0` on
A64), and crt0 stores it in `aar_api_ptr` before it runs the init arrays and
calls `aar_main`.

- The table is the only entry into the runtime. Never cache, compare or call
  an address found any other way, and never write to the table. A runtime may
  implement a slot as a plain function or as a trap veneer.
- Call the `aar_*` wrappers. A wrapper reads a slot only when
  `AAR_API_COVERS` is true and the function pointer is non-NULL. A missing
  slot gives `AAR_E_NOSYS` from an integer wrapper, and nothing, 0 or NULL
  from a void or pointer wrapper. `aar_exit` does not return even then. A
  direct call through the table must make the same check.
- The table starts with four `uint32_t` words: `size`, `version`, `groups`,
  `platform_id`. The first slot is at offset 16 on both profiles. `size` is the
  compatibility check: a slot exists when `size` covers it. `version` names the
  runtime and is not a substitute for `size`. Apps must not gate on `version`
  alone.
- The 42 core slots, `exit` through `clip_write`, are non-NULL in every
  conforming runtime. A call that the client serves can still return
  `AAR_E_NOSYS` when the client does not serve that op. Optional slots outside
  groups (`ext`, `map_file`, `unmap_file`, `file_get_comment`,
  `file_set_comment`, `file_read_async`, `file_write_async`, `file_collect`
  and `file_request`) are NULL or absent where the platform does not provide
  them. A group is present when its bit is set in `aar_api.groups`. Every call
  of that group that
  the table's `size` covers is non-NULL, except where a slot is marked
  optional. An app that needs a group checks `groups` once. If the bit is
  clear, every slot of that group is absent or NULL.
- An app has one thread. No call is reentrant and no call takes variable
  arguments. A call that parks (files, console, display flip, `aar_msg_wait`)
  returns when the work completes, on stop (`AAR_E_IO`), or on its own
  timeout if it has one. The native file requester has no timeout. A sleep
  is also ended by a stop, but it has no return value. Nothing calls back
  into the app.
- Structures are plain C structures with fixed-width integers. Where a
  structure has pointers, they come last, so every fixed-width field has
  the same offset on A32 and A64.

Notes marked **ZZ9000** state a fact of that platform. They are not part of
the standard and another platform may differ.

## Presence tests

### AAR_API_COVERS

```c
#define AAR_API_COVERS(api, field) \
    ((api) != 0 && (uint32_t)(api)->size >= \
        (uint32_t)(offsetof(aar_api, field) + sizeof((api)->field)))
```

True when `api` is non-NULL and its `size` covers `field`. A true result does
not mean the function pointer is non-NULL: check both before a direct call.
This is the preferred gate for a single call. It is a header macro, not a
table slot.

### AAR_API_AT_LEAST

```c
#define AAR_API_AT_LEAST(api, minor) \
    ((api) != 0 && ((api)->version >> 16) == AAR_API_MAJOR && \
        ((api)->version & 0xFFFFu) >= (uint32_t)(minor))
```

True when the table names a runtime of this major and at least `minor`. It is
a convenience for a code path that needs a whole release. A single call is
still gated by `AAR_API_COVERS`, and an app must not gate on `version` alone.

### AAR_GL_COVERS

```c
#define AAR_GL_COVERS(gl, field) \
    ((gl) != 0 && (uint32_t)(gl)->size >= \
        (uint32_t)(offsetof(aar_gl, field) + sizeof((gl)->field)))
```

The same test for `aar_gl`. The `AAR_GL_VERSION_V0` fields end at
`Build2DMipmaps`; the array and client-state fields need this test. A `gl*`
wrapper whose field is past `gl->size`, or is NULL, is a no-op or returns 0.

### AAR_EXT_ZZ9000_COVERS

```c
#define AAR_EXT_ZZ9000_COVERS(ext, field) \
    ((ext) != 0 && (uint32_t)(ext)->size >= \
        (uint32_t)(offsetof(aar_ext_zz9000, field) + sizeof((ext)->field)))
```

The same test for the ZZ9000 extension table. See [Platform
extension](#the-zz9000-extension-table).

## Versioning

### AAR_API_MAJOR

`0u`. Changes only for a break. The runtime returns `AAR_E_VERSION` if a
structurally valid image declares a different API major.

### AAR_API_MINOR

`6u`. Increments for each release that adds table slots, constants or groups.
Compatible releases preserve the values of constants and the layouts and
meanings of structures used by accepted older minors. The runtime returns
`AAR_E_VERSION` if a structurally valid image needs a minor above this value
or below `AAR_API_MINOR_OLDEST`.

### AAR_API_MINOR_OLDEST

`6u`. The oldest minor of the current major whose API this runtime supports.
The 0.6 draft moved slots relative to the 0.5 draft and cannot run 0.5
binaries. A compatible release appends new calls and can keep the floor only
if it preserves the offsets, signatures and semantics of every slot available
to every accepted older minor. An incompatible release raises the floor
unless it implements the older ABI. `AAR_NOTE_EX` rejects a minor outside
the supported range at compile time. Minimum minor: 6.

### AAR_API_VERSION

`(AAR_API_MAJOR << 16) | AAR_API_MINOR`, the value of `aar_api.version`.

## Error codes

Calls return `AAR_OK` or a negative `AAR_E_*` value. The table, the client
library and the transport share one code space. A platform may map its own
transport statuses to these; that mapping is private to the platform.

### AAR_OK

`0`. The call completed. A file read that returns less than requested with this code is end of file. A console read that returns zero bytes with this code is end of input.

### AAR_E_IO

`-1`. The operation failed; a control block or queue is corrupt; a transfer ended early; or stop cancelled a parked call. For display failures in the reference client, see the [aar-run manual](../client/run/README.md#console-and-audio-routes).

### AAR_E_INVAL

`-2`. A required pointer is NULL, a flag or format is unknown, a length is zero or above the call's limit, or an id does not name an open object.

### AAR_E_NOMEM

`-3`. Memory, a quota, a table or a session limit is exhausted: the private heap pool or cap, the shared limit, the open file or list table (`aar_info.max_files`), or the decode sessions of one kind (`AAR_MIN_DECODE_SESSIONS`). `aar_alloc` reports this as a NULL pointer, not as this code.

### AAR_E_BUSY

`-4`. The queue is full (`aar_msg_send`), a resource is in use (the app's one display is already open, no audio stream is free), or a flip is still pending. It never means that another app is running: any number of apps may run at once.

### AAR_E_NOENT

`-5`. `aar_file_list_next` has no further directory entry, or the path does not exist.

### AAR_E_ACCES

`-6`. The client's path policy refused the call: a relative path that leaves the root, or a path with a volume or assign prefix when the client did not allow it.

### AAR_E_NOSYS

`-7`. The slot is absent or NULL, or the codec, algorithm or format is not implemented, or CLUT8 was requested and the display has no `AAR_DISPCAP_CLUT8`. This is the wrapper result on a runtime older than the slot.

### AAR_E_TIMEDOUT

`-8`. A file or directory call waited 30 seconds for the client (the native file requester has no timeout). The runtime retires that request before returning, so a late reply cannot satisfy a later call, and the file or list id is not reused for the rest of the app's life. `aar_msg_wait` also returns it when its timeout passes with no message queued.

### AAR_E_FAULT

`-9`. The runtime rejected a bad pointer passed to a call or to a GL entry point. Only a runtime with `AAR_INFOF_ISOLATED` validates pointers; a pointer must name the calling app's mapped memory with the access the call needs. The call returns this code instead of aborting the runtime.

### AAR_E_NOSPC

`-10`. A result does not fit. `aar_map_file` returns it for a file above the platform's limit (**ZZ9000:** 16 MiB). MP3 decode returns it when `pcm_frames` is below 1152 or the decoded frame does not fit. FLAC and Vorbis return it when the PCM capacity calculation would overflow.

### AAR_E_EXIST

`-11`. The file exists and `AAR_FILE_EXCL` was set. Returned for `aar_file_open`.

### AAR_E_BADF

`-12`. The file or list id is not open or was never issued, or it was opened for a direction the call does not use: `aar_file_write` through an id opened without `AAR_FILE_WRITE`, or `aar_file_read` through an id opened without `AAR_FILE_READ`.

### AAR_E_PROFILE

`-13`. The image's profile, or its note, is not acceptable on this platform. Returned by the load, never by a table call. The probe (`AARProbeFile`, `AARProbeMemory`) never returns it: it reports the profile and the launcher compares it with the platform's.

### AAR_E_NOGROUP

`-14`. The image requires a group the runtime lacks (`groups_required` in the note). Returned by the load.

### AAR_E_NORESOURCE

`-15`. Admission or a load was refused for lack of a resource. The reason is one of the `AAR_REASON_*` codes. Returned by the client library when an app is opened or loaded, never by a table call.

### AAR_E_STALE

`-16`. The handle generation no longer names a live app, or an async file
ticket is not current. A request that names it changes nothing.

### AAR_E_OWNER

`-17`. A handle was used by a task that does not own it.

### AAR_E_VERSION

`-18`. A structurally valid image declares a different API major or a required
minor outside `AAR_API_MINOR_OLDEST` through `AAR_API_MINOR`, inclusive.
Returned by probe and load, not by app table calls. An unknown note format
(`note_version`) remains `AAR_E_INVAL`.

### Admission refusal reasons

Reported with `AAR_E_NORESOURCE`. A refused open leaves nothing allocated. CPU
load is not an admission resource: apps slow down, they are not refused.

### AAR_REASON_NONE

`0u`. No reason.

### AAR_REASON_MEMORY

`1u`. Platform memory for the app's record and stack.

### AAR_REASON_TABLES

`2u`. Address-space translation tables.

### AAR_REASON_HOSTHEAP

`3u`. Shared memory for the control block and queues.

### AAR_REASON_LIMIT

`4u`. A platform ceiling.

## Lifecycle values

The client reads these through its status call, and they are stored in the
control block ([transport.md](transport.md)). An app does not read them; they
matter to launchers and to platform code. The states are `IDLE`, `CLAIMED`
(the handle is assigned), `LOADING`, `STARTING`, `RUNNING`, `STOPPING` and
`FAULTED`. State 4 (`PAUSED`) is not defined. After teardown the end reason,
and after a fault the fault record, stay readable until the client closes its
handle.

### Lifecycle states

### AAR_STATE_IDLE

`0u`. No app.

### AAR_STATE_LOADING

`1u`. The image is being streamed and relocated. The state stays `LOADING` after the load call returns, until `AARStart` is accepted.

### AAR_STATE_STARTING

`2u`. `AARStart` was accepted; the entry point has not been reached. A client cannot rely on seeing it, because it lasts only until the entry is reached.

### AAR_STATE_RUNNING

`3u`. The app runs.

### AAR_STATE_STOPPING

`5u`. Exit, a stop request, a fault, the watchdog, lease loss or a reset is being torn down.

### AAR_STATE_FAULTED

`6u`. The app faulted. The fault record in the control block is valid. After a fault a platform may report `FAULTED` until the handle is closed, or return to `IDLE`; a client treats both as ended and reads the end reason. **ZZ9000:** returns to `IDLE`.

### AAR_STATE_CLAIMED

`7u`. Admission passed and a handle is assigned, but loading has not begun.

### Fault classes

Profile-neutral. The raw syndrome travels beside the class (the FSR on A32,
the low 32 bits of `ESR_EL1` on A64).

### AAR_FAULT_NONE

`0u`. No fault.

### AAR_FAULT_DATA

`1u`. Bad data access.

### AAR_FAULT_FETCH

`2u`. Bad instruction fetch.

### AAR_FAULT_ILLEGAL

`3u`. Undefined or illegal instruction.

### AAR_FAULT_ALIGN

`4u`. Alignment fault.

### AAR_FAULT_OTHER

`5u`. Any other app fault.

### End reasons

Each value says why an app ended. It stays readable until the client closes its handle.

### AAR_END_NONE

`0u`. Not ended.

### AAR_END_EXIT

`1u`. The app returned from `aar_main` or called `aar_exit`.

### AAR_END_FORCED

`2u`. A stop request ran past `stop_grace_ms` and the runtime stopped the app.

### AAR_END_USER_FAULT

`3u`. A fault in app code (bad data or instruction access, undefined instruction, alignment). It ends only that app.

### AAR_END_STALL

`4u`. The app made no table call for longer than the stall limit.

### AAR_END_STUCK_CALL

`5u`. A single call stayed inside the runtime's own code, not parked in a declared wait, for longer than the runtime's stuck-call limit. A runtime may have no such watchdog, and an app cannot cause it.

### AAR_END_LEASE

`6u`. The owner did not renew the lease.

### AAR_END_PLATFORM_RESET

`7u`. The platform was reset. A reset ends every app.

### AAR_END_RUNTIME_RESTART

`8u`. A fault in runtime code ended every app and the platform restarted its runtime.

### AAR_END_QUARANTINED

`9u`. The app's memory is held after a lease loss until the old client's next request or an explicit release.

## The ELF note

An image has exactly one AAR note, in a `PT_NOTE` segment: owner name
`AAR` (`namesz` 4 with the NUL), `n_type` `AAR_NOTE_TYPE`. Name and descriptor
are each padded to 4 bytes in both ELF classes. The 40-byte descriptor is ten
little-endian `uint32_t`:

| Word | Field | Meaning |
|---|---|---|
| 0 | `magic` | `AAR_NOTE_MAGIC` |
| 1 | `note_version` | `AAR_NOTE_VERSION` |
| 2 | `profile` | `AAR_PROFILE_A32` or `AAR_PROFILE_A64` |
| 3 | `api_major` | the major the app was built against |
| 4 | `api_minor_required` | the lowest supported minor that provides the API contracts the app needs |
| 5 | `stack_bytes` | 0 for the default (1 MiB); otherwise 64 KiB up to the platform maximum, a multiple of 16 |
| 6 | `min_heap_mib` | private heap committed at load, in MiB |
| 7 | `groups_required` | `AAR_GROUP_*` bits; the load fails with `AAR_E_NOGROUP` if one is missing |
| 8 | `groups_optional` | a hint, for example for a launcher that opens a screen before start |
| 9 | `reserved` | 0 |

The runtime returns `AAR_E_PROFILE` if the note's profile does not match the
ELF class, and `AAR_E_INVAL` for structural errors such as bad note magic, an
unknown `note_version`, an invalid stack size or a nonzero reserved word.
After structural validation it returns `AAR_E_VERSION` if `api_major` differs
from its own or `api_minor_required` is outside the supported range. There is
no C structure for the descriptor: `AAR_NOTE` emits it.

### AAR_NOTE_MAGIC

`0x4E524141u`. The bytes `A`, `A`, `R`, `N` as a little-endian word.

### AAR_NOTE_TYPE

`1u`. The ELF note `n_type`.

### AAR_NOTE_VERSION

`1u`. The note descriptor layout version.

### AAR_NOTE_DESC_BYTES

`40u`. The descriptor size.

### AAR_NOTE_DEFAULT_STACK_BYTES

`1024u * 1024u`. The stack size a note value of 0 means.

### AAR_NOTE

```c
AAR_NOTE(stack_bytes, min_heap_mib, groups_required, groups_optional)
```

Declared by `aar/note.h`. Write it once at file scope, without a trailing
semicolon. It emits the note into section `.note.aar`, and the linker script
must place that section in a `PT_NOTE` segment. It declares
`api_minor_required` as `AAR_API_MINOR`, the minor of the header the app was
built with. The profile word comes from the compiler (`AAR_PROFILE_A64` when
`__aarch64__` is defined, else `AAR_PROFILE_A32`) and `api_major` is
`AAR_API_MAJOR`. All arguments must be integer constant expressions.

```c
#include <aar/app.h>
#include <aar/note.h>

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY | AAR_GROUP_INPUT, AAR_GROUP_AUDIO)
```

A host build, meaning any compiler that is not an Arm target or any build with
`AAR_HOST` defined, expands `AAR_NOTE` and `AAR_NOTE_EX` to nothing.

### AAR_NOTE_EX

```c
AAR_NOTE_EX(api_minor_required, stack_bytes, min_heap_mib,
            groups_required, groups_optional)
```

The same as `AAR_NOTE` with an explicit `api_minor_required`. Use it to declare
an older supported minor only if the app works with that minor's API. Gate any
calls added later on slot presence before using them.

## Profiles, platforms and groups

### AAR_PROFILE_A32

`1u`. ARMv7-A in A32 state, hard-float AAPCS, VFPv3-D32 and NEON, ILP32, little-endian.

### AAR_PROFILE_A64

`2u`. ARMv8-A AArch64, AAPCS64, FP and ASIMD, LP64, little-endian.

### AAR_PLATFORM_NONE

`0u`. Unspecified. Valid in `aar_api.platform_id` and as the first argument of `aar_ext`.

### AAR_PLATFORM_ZZ9000

`1u`. The ZZ9000. Ids are allocated in the standard's repository; a platform extension header defines its own.

A group is a set of table calls that a platform may or may not provide. It is
present when its bit is set in `aar_api.groups` and in `aar_info.groups`.
`AAR_GROUP_INPUT` has no table slots. It adds the input events to
`aar_event_poll`. A missing capability inside a group is `AAR_E_NOSYS` from the
call that needs it.

### AAR_GROUP_DISPLAY

`(1u << 0)`. The display calls and surfaces.

### AAR_GROUP_INPUT

`(1u << 1)`. Key, mouse, joystick and break events from `aar_event_poll`. No table slots.

### AAR_GROUP_AUDIO

`(1u << 2)`. The PCM output stream.

### AAR_GROUP_GL

`(1u << 3)`. The GL table from `aar_gl_bind`.

### AAR_GROUP_SERVICES

`(1u << 4)`. Hashing, decompression, and image and audio decode sessions.

### AAR_GROUPS_DEFINED

`0x0000001Fu`. Every group bit this version defines.

### AAR_GROUPS_EXPERIMENTAL

`0xF0000000u`. Bits 28 to 31, for experiments of a platform. Bits 5 to 27 are reserved for later versions of the standard. A platform must not set them.

## Limits the standard fixes

These are minimums a runtime must meet or constants of the interface. The
runtime reports the actual values it grants in `aar_info`.

### AAR_MSG_BYTES

`32u`. Every message is exactly this many bytes.

### AAR_PATH_MAX

`255u`. Longest path in bytes, excluding the NUL.

### AAR_NAME_MAX

`255u`. Longest directory entry name in bytes, excluding the NUL.

### AAR_FD_BASE

`3`. The lowest file and list id. Ids 0 to 2 are the console in the libc flavor.

### AAR_MIN_FILES

`16u`. Open files, and separately open directory lists, the runtime supports at least.

### AAR_MIN_ARGS

`16u`. The runtime supports at least this many `argv` entries.

### AAR_MIN_ARG_BYTES

`128u`. The longest supported argument, 127 bytes plus the NUL.

### AAR_LOG_MAX

`1024u`. A longer log record is truncated.

### AAR_HEAP_GRANULE

`(1024u * 1024u)`. The private heap grows and shrinks in whole units of this size (`aar_info.heap_granule` in v1).

### AAR_SHARED_MIN_BLOCKS

`64u`. Live shared blocks per app the runtime supports at least. The runtime's own service buffers do not count.

### AAR_SHARED_DEFAULT_MIB

`16u`. The default shared limit in MiB.

### AAR_STALL_LIMIT_MIN_MS

`10000u`. The least `aar_info.stall_limit_ms` a runtime may report.

### AAR_MIN_MSG_CAPACITY

`32u`. The least `aar_info.msg_capacity` a runtime may report.

### AAR_DISPLAY_MAX_DIM

`4096u`. Width and height a display accepts, at least.

### AAR_MIN_DECODE_SESSIONS

`4u`. Image decode sessions and, separately, audio decode sessions per app, at least.

### AAR_SHA256_BYTES

`32u`. The SHA-256 digest size.

## Runtime information

### aar_info

```c
typedef struct aar_info {
    uint32_t size;
    uint32_t profile;
    uint32_t platform_id;
    uint32_t flags;
    uint32_t groups;
    uint32_t heap_granule;
    uint32_t shared_granule;
    uint32_t msg_capacity;
    uint32_t sleep_granule_us;
    uint32_t stall_limit_ms;
    uint32_t stop_grace_ms;
    uint32_t cpu_features;
    uint64_t shared_limit;
    uint64_t shared_used;
    uint32_t max_files;
    uint32_t reserved[5];
} aar_info;
```

The structure `aar_get_info` fills. The caller sets `size`; the runtime fills
at most that many bytes and zeroes the rest, so a field the runtime does not
know stays 0. 88 bytes on both profiles.

| Field | Meaning |
|---|---|
| `size` | Set by the caller to the size of its structure. |
| `profile` | `AAR_PROFILE_*` of this runtime's app. |
| `platform_id` | `AAR_PLATFORM_*`. |
| `flags` | `AAR_INFOF_*`. |
| `groups` | `AAR_GROUP_*` the runtime implements. |
| `heap_granule` | Bytes, a power of two, 1 MiB in v1. |
| `shared_granule` | Bytes, a power of two, at least 64. Shared buffers are rounded to it. |
| `msg_capacity` | Messages per direction, at least 32 (63 on the default 64-entry queue). |
| `sleep_granule_us` | Timing granularity of `aar_sleep_us`. |
| `stall_limit_ms` | The longest gap between calls the runtime tolerates, at least 10000. |
| `stop_grace_ms` | The grace period between a stop request and a forced stop. |
| `cpu_features` | `AAR_CPU_A32_*` or `AAR_CPU_A64_*` for the profile. |
| `shared_limit` | Bytes of shared buffers this app may hold. |
| `shared_used` | Bytes of shared buffers in use, including the runtime's service buffers. |
| `max_files` | Open files, and separately lists, at least 16. |
| `reserved` | Zero. |

### aar_heap_info

```c
typedef struct aar_heap_info {
    uint64_t committed;
    uint64_t limit;
    uint64_t pool_free;
} aar_heap_info;
```

All fields are bytes. `committed` is all the private memory the app holds: its
image, its stack and its heap, each in whole `heap_granule` units. The heap that
`aar_heap_range` reports is therefore smaller than `committed`. A successful
`aar_heap_grow` of `n` bytes raises `committed` by `n`, and a successful
`aar_heap_shrink` lowers it by the bytes returned. `limit` is the effective
limit after the client's cap. It caps `committed` as a whole, so the image, the
stack and `min_heap_mib` count against it at load. The platform's own cap need
not equal its pool. `pool_free` is the memory the platform could give this app
now. It can be smaller than `limit - committed`. A grow fails with `AAR_E_NOMEM`
when either bound is used up. The values are a snapshot. Another app may grow
before a later request. 24 bytes.

### AAR_INFOF_ISOLATED

`(1u << 0)`. The runtime validates every pointer passed to a call (`AAR_E_FAULT` for a bad one), an app fault ends only that app, and apps cannot reach firmware, other apps or host memory except through granted shared buffers and surfaces. Without it a bad pointer is undefined behaviour and the platform documents what happens. Isolation protects against ordinary bugs; it is not a sandbox against hostile apps.

### AAR_INFOF_SHARED_COHERENT

`(1u << 1)`. Shared buffers are coherent between the app and the host, so `aar_publish` and `aar_acquire` are barriers only.

### CPU features

`aar_info.cpu_features` lists optional instruction support, so an app can pick
a code path without executing and trapping. The A32 and A64 bits overlap by
number, so read them by profile.

### AAR_CPU_A32_VFPV4

`(1u << 0)`. VFPv4 and fused multiply-add.

### AAR_CPU_A32_IDIV

`(1u << 1)`. Integer divide instructions.

### AAR_CPU_A32_FP16

`(1u << 2)`. Half-precision convert.

### AAR_CPU_A64_CRC32

`(1u << 0)`. CRC32 instructions.

### AAR_CPU_A64_LSE

`(1u << 1)`. LSE atomics.

### AAR_CPU_A64_AES

`(1u << 2)`. AES instructions.

### AAR_CPU_A64_SHA

`(1u << 3)`. SHA1 and SHA2 instructions.

### AAR_CPU_A64_DOTPROD

`(1u << 4)`. Dot product instructions.

### AAR_CPU_A64_FP16

`(1u << 5)`. FP16 arithmetic.

## Types

### aar_shared

```c
typedef struct aar_shared {
    uint64_t bytes;
    uint32_t haddr;
    uint32_t flags;
    void    *ptr;
} aar_shared;
```

The result of `aar_shared_alloc`. `bytes` is the granted size, a multiple of
`shared_granule`. `haddr` is the host address token: an opaque 32-bit value
that names the allocated block for the client library. It is not a pointer and not
an offset, and the app must not interpret it. `flags` is `AAR_SHAREDF_COHERENT`
when this block needs no cache maintenance. `ptr` is the app's mapping. The
pointer is at offset 16 on both profiles.

### AAR_SHAREDF_COHERENT

`(1u << 0)`. Output only, in `aar_shared.flags`: this block is coherent with the host. The flags argument of `aar_shared_alloc` must be 0; no input flag is defined in this version.

### aar_event

```c
typedef struct aar_event {
    uint16_t type, code;
    int16_t  dx, dy;
    uint16_t buttons, reserved;
    uint32_t time_ms;
} aar_event;
```

16 bytes on both profiles, native byte order. `type` is one of the `AAR_EV_*`
values. `code` is the Amiga raw key with the up bit removed, an `AAR_BREAK_*`
value, the mouse button that changed, the joystick port, or a Unicode code
point (`AAR_EV_TEXT`). `dx` and `dy` are mouse deltas or the pointer position,
joystick axes in -1, 0 and 1, wheel steps, or the new size of a resized
display. `buttons` holds key qualifiers, the `AAR_MOUSE_*` bits or the
`AAR_JOY_*` bits. `time_ms` is a timestamp in milliseconds that the client
chooses; the library does not change it. It is 32 bits and wraps. `reserved`
is 0. An app ignores an event type it does not know.

### aar_stat

```c
typedef struct aar_stat {
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;
    uint32_t protect;
} aar_stat;
```

Filled by `aar_file_stat`. `mtime` is Unix seconds, or 0 when the filesystem
has none (an Amiga `DateStamp` converted by the client). `mode` is
`AAR_MODE_FILE` or `AAR_MODE_DIR`. `protect` holds the Amiga protection bits as
`FIB_Protection` reports them, at the positions of the `AAR_PROT_*` constants;
bits 0 to 3 are set to deny, as in AmigaDOS. 24 bytes; `protect` is at offset
20. Minimum minor: 5, and `protect` is the word that 0.5 reserved.

### aar_dirent

```c
typedef struct aar_dirent {
    uint32_t mode;
    char     name[256];
} aar_dirent;
```

Filled by `aar_file_list_next`. `mode` is `AAR_MODE_FILE` or `AAR_MODE_DIR`.
`name` is NUL-terminated and at most `AAR_NAME_MAX` bytes. 260 bytes.

### aar_volume_info

```c
typedef struct aar_volume_info {
    uint32_t size;
    uint32_t block_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t flags;
    uint32_t reserved[3];
} aar_volume_info;
```

Filled by `aar_get_volume_info`. The caller sets `size`; the runtime fills at
most `size` bytes and zeroes the rest, as for `aar_info`. `block_bytes` is the
size of a block of the volume, `total_bytes` and `free_bytes` the capacity and
the free space, and `flags` is `AAR_VOLF_*`. 40 bytes. Minimum minor: 6.

### aar_locale_info

```c
typedef struct aar_locale_info {
    uint32_t size;
    int32_t  utc_offset_min;
    uint32_t flags;
    char     language[32];
    char     country[8];
    char     decimal_point[4];
    char     group_separator[4];
    uint32_t reserved[3];
} aar_locale_info;
```

Filled by `aar_get_locale_info`. The caller sets `size`; the runtime fills at
most `size` bytes and zeroes the rest. `utc_offset_min` is the offset in
minutes east of UTC (`locale.library` keeps `loc_GMTOffset` in minutes west, so
the client negates it). `flags` is `AAR_LOCALEF_*`. `language` is the Amiga
language name without `.language`, for example `english` or `deutsch`, and
`country` the vehicle registration code in `loc_CountryCode` (its non-zero
bytes, most significant first, as text, for example `D` or `GB`); both are
NUL-terminated and empty when unknown.
`decimal_point` and `group_separator` are UTF-8 and NUL-terminated. 72 bytes.
Minimum minor: 6.

### aar_surface

```c
typedef struct aar_surface {
    uint32_t id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t format;
    uint32_t layout;
    uint32_t caps;
    uint32_t reserved;
    void    *front;
    void    *back;
} aar_surface;
```

A display surface. `format` is the logical format the app requested.
`layout` is the real byte layout; store pixels by `layout`, not by `format`.
`pitch` is in bytes and may exceed `width` times the bytes per pixel. `caps` is
`AAR_DISPCAP_*`. `front` and `back` are the app's mappings, NULL when the app
does not draw that buffer. One display is open per app. After a completed
flip, call `aar_display_refresh` before trusting the pointers again. The
pointers are at offset 32 on both profiles. The old physical card addresses are
gone.

### aar_display_info

```c
typedef struct aar_display_info {
    uint32_t size;
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t screen_width, screen_height;
    uint32_t flags;
    uint32_t reserved[5];
} aar_display_info;
```

Filled by `aar_display_get_info`. The caller sets `size`; the runtime fills at
most `size` bytes and zeroes the rest. `width` and `height` are the current
surface size, `refresh_mhz` is the refresh rate of the mode shown in millihertz
(0 when unknown), `screen_width` and `screen_height` are the size of the Amiga
screen the surface is on, and `flags` is `AAR_DISPINFO_*`. 48 bytes. Minimum
minor: 6.

### aar_display_mode

```c
typedef struct aar_display_mode {
    uint32_t width, height;
    uint32_t refresh_mhz;
    uint32_t formats;
    uint32_t flags;
    uint32_t reserved;
} aar_display_mode;
```

One display mode that the client can show, as `aar_display_modes` returns it.
`refresh_mhz` is 0 when unknown. `formats` has the bit `1u << AAR_FMT_*` set
for each format the mode can show. `flags` is `AAR_MODEF_*`. 24 bytes. Minimum
minor: 6.

### aar_pointer

```c
typedef struct aar_pointer {
    uint32_t width, height;
    int32_t  hot_x, hot_y;
    uint32_t reserved[2];
    const uint32_t *argb;
} aar_pointer;
```

A custom mouse pointer for `aar_display_pointer`. `width` and `height` are 1 to
`AAR_POINTER_MAX_DIM` each, `hot_x` and `hot_y` the hot spot inside the image,
`reserved` is 0. `argb` points at `width * height` pixels of `0xAARRGGBB` in
host order; alpha 0 is transparent. The pointer is last, at offset 24 on both
profiles. Minimum minor: 6.

### aar_audio

```c
typedef struct aar_audio {
    uint32_t frames;
    uint32_t rate, channels;
    uint32_t flags;
    int16_t           *samples;
    volatile uint32_t *read_frame;
    volatile uint32_t *write_frame;
} aar_audio;
```

An interleaved S16LE ring shared by the app and its consumer: the runtime, or
on the AHI route the client library. `frames` is the capacity, a power of two.
`rate` and `channels` are what was granted. `flags` is 0. `*write_frame` is
advanced by the app and `*read_frame` by the runtime; both are free-running
`uint32_t` frame counters that wrap. The app writes ahead of `*read_frame` but
never more than `frames` ahead. The pointers are at offset 16 on both
profiles.

The three pointers point into one mapping of the ring. If `ring` is the address
of its first byte, `write_frame` is `ring`, `read_frame` is `ring + 0x40` and
`samples` is `ring + 0x80`.

The indices order the PCM. The app stores the samples first, then publishes
`*write_frame` with a release store (`__atomic_store_n` with
`__ATOMIC_RELEASE` on GCC and Clang). Before it computes the free space or
reuses a slot, it reads `*read_frame` with an acquire load
(`__atomic_load_n` with `__ATOMIC_ACQUIRE`), so it never writes a slot the
runtime has not finished with. The runtime does the converse: it acquires
`*write_frame` before it reads the samples, and releases `*read_frame` after it
is done with them. `volatile` alone gives no such ordering between the sample
stores and the index, on either side. The platform provides what makes this
order hold, a mapping that is coherent between the CPU and the consumer or a
backend that owns the ring itself. An app does not flush or discard cache lines
on the ring and does not copy the PCM a second time.

### aar_audio_stats

```c
typedef struct aar_audio_stats {
    uint32_t size;
    uint32_t underruns;
    uint32_t reserved[6];
} aar_audio_stats;
```

Filled by the optional `aar_audio_get_stats`. The caller sets `size`.
`underruns` is the cumulative count since the stream opened. 32 bytes.

### aar_service_result

```c
typedef struct aar_service_result {
    uint64_t bytes_consumed;
    uint64_t bytes_written;
    uint32_t flags;
    uint32_t reserved;
} aar_service_result;
```

Filled by `aar_decompress` only when it returns `AAR_OK`. The counts are the
completed operation, not estimates. Byte counts of whole buffers are 64-bit.
24 bytes.

### aar_image_params

```c
typedef struct aar_image_params {
    uint32_t codec;
    uint32_t output_format;
    uint32_t stride;
    uint32_t rows;
    uint32_t tile_bytes;
    uint32_t flags;
    void    *tile;
} aar_image_params;
```

The parameters of `aar_image_open`. `codec` is `AAR_IMAGE_*`.
`output_format` must be `AAR_FMT_BGRA8888`. `tile` must hold `stride * rows`
bytes, `tile_bytes` is that capacity, and the buffer stays valid for every
decode call on the session. It stays caller-owned. The pointer is last, at
offset 24 on both profiles. Pass 0 in `flags` in API 0.5; see
[spec section 8.6](../spec/aar.md) for the portable image-session contract.

### aar_image_result

```c
typedef struct aar_image_result {
    uint32_t image_width;
    uint32_t image_height;
    uint32_t bytes_consumed;
    uint32_t bytes_written;
    uint32_t flags;
} aar_image_result;
```

Geometry is 0 until the codec has a header. `flags` are the image session
result flags. Per-call chunk sizes are 32-bit. 20 bytes.

### aar_audio_decode_result

```c
typedef struct aar_audio_decode_result {
    uint32_t bytes_consumed;
    uint32_t frames_written;
    uint32_t rate;
    uint32_t channels;
} aar_audio_decode_result;
```

`frames_written` counts sample frames, not `int16_t` samples: for stereo the
PCM buffer holds `frames_written * 2` samples. `rate` and `channels` stay 0
until the codec has parsed a header. 16 bytes.

### aar_api

The function table, described under [How an app reaches the
runtime](#how-an-app-reaches-the-runtime). The slot order is fixed until 1.0:

1. Header: `size`, `version`, `groups`, `platform_id` (16 bytes).
2. Core, 42 slots, `exit` through `clip_write`: `exit`, `yield`, `should_stop`,
   `time_us`, `sleep_us`, `info`, `heap_range`, `heap_grow`, `heap_shrink`,
   `heap_info`, `shared_alloc`, `shared_free`, `publish`, `acquire`, `log`,
   `file_open`, `file_close`, `file_read`, `file_write`, `file_seek`,
   `file_stat`, `file_list_open`, `file_list_next`, `file_list_close`,
   `console_write`, `console_read`, `event_poll`, `msg_send`, `msg_poll`,
   `msg_wait`, `file_delete`, `file_rename`, `dir_create`, `file_set_date`,
   `file_set_protect`, `file_truncate`, `volume_info`, `time_unix_us`,
   `locale_info`, `env_get`, `clip_read`, `clip_write`.
3. Optional: `ext`, `map_file`, `unmap_file`, `file_get_comment`,
   `file_set_comment`.
4. Groups, in this order: display (`display_open`, `display_flip`,
   `display_flip_async`, `display_flip_idle`, `display_refresh`,
   `display_vsync`, `display_set_palette`, `display_close`,
   `display_latest_dropped`, `display_info`, `display_modes`,
   `display_resize`, `display_set_title`, `display_pointer`,
   `display_wait_vblank`, `display_last_present_us`, `display_present`,
   `display_present_poll`), audio (`audio_open`,
   `audio_close`, `audio_stats`), GL (`gl_bind`, `gl_swap`, `gl_unbind`),
   services (`sha256`, `decompress`, `image_open`, `image_decode`,
   `image_close`, `audio_decode_open`, `audio_decode`, `audio_decode_close`).
5. Optional async files: `file_read_async`, `file_write_async`, `file_collect`.
6. Optional native file requester: `file_request`.

The table has 83 pointer slots after the header. The table is const and apps
must not write to it. A wrapper has the slot's name with an `aar_` prefix, except
that a slot whose name is a type name gets a `get`: `aar_get_info`,
`aar_get_heap_info`, `aar_audio_get_stats`, `aar_get_volume_info`,
`aar_get_locale_info` and `aar_display_get_info`.

## Layout checks

The headers hold `_Static_assert` checks that are true for both data models.
These helper macros express them and apps may use them for their own
structures.

### AAR_STATIC_ASSERT

`(cond, msg)`. `static_assert` in C++, `_Static_assert` in C.

### AAR_ALIGNOF

`(type)`. `alignof` in C++, `_Alignof` in C.

### AAR_ABI_SIZE

`(type, fixed, nptr)`. The size of a structure made of `fixed` bytes of fixed-width fields followed by `nptr` pointers, rounded to the structure's own alignment.

### AAR_API_SLOT_COUNT

`83u`. The number of pointer slots in `aar_api` after the 16-byte header.

### AAR_API_CORE_SLOT_COUNT

`42u`. The number of core slots, `exit` through `clip_write`.

## Build macros

These are not defined by the headers; the build defines them.

### AAR_FREESTANDING

Defined for the sources of an app built in freestanding mode. It removes the
declarations of `aar_alloc` and `aar_free`, since there is no libc allocator.
See the [developer guide](aar-developer-guide.md#freestanding-apps).

### AAR_HOST

Define it to make `aar/note.h` expand `AAR_NOTE` and `AAR_NOTE_EX` to nothing
on an Arm compiler. A compiler that is not an Arm target already does.

## Entry contract

### aar_api_ptr

```c
extern const aar_api *aar_api_ptr;
```

The table pointer. crt0 stores it before it runs the init arrays and calls
`aar_main`. The table is const; the pointer is not, but apps must not replace
it. Apps may read it for a direct `AAR_API_COVERS` test.

### aar_main

```c
int aar_main(int argc, char **argv);
```

The app defines this. The ELF entry symbol is `aar_start`, which is crt0, not
the app. crt0 runs `.preinit_array` and `.init_array`, calls `aar_main`, and
passes the result to `exit` (libc flavor) or `aar_exit` (freestanding flavor).
Returning from the entry point itself, without exiting, is undefined.

`argv[0]` is the program name, the ELF file name. The runtime supports at
least `AAR_MIN_ARGS` arguments of up to `AAR_MIN_ARG_BYTES` minus one bytes
each; `aar_info` does not report the exact limit, the client library does. Not
a table slot. No `AAR_API_COVERS` gate. Minimum minor: 5.

On entry the registers hold: the table pointer (`r0` or `x0`), `argc` (`r1` or
`w1`), `argv` (`r2` or `x2`). The stack pointer is at the top of the app stack,
8-byte aligned on A32 and 16-byte aligned on A64. All other general and vector
registers are 0, and the float state is zero: round to nearest, flush-to-zero
and default-NaN clear. VFP and NEON (A32) or ASIMD (A64) are enabled. The
runtime does not run constructors; crt0 does.

## Lifecycle and time

### aar_exit

```c
void aar_exit(int code);
```

Does not return, including when the slot is missing. Publishes `code` as the
app's exit status. By convention the AmigaDOS return codes 0, 5, 10 and 20 are
used. The client reads the code through its status call, and a launcher returns
it to the Shell. The [aar-run manual](../client/run/README.md#exit-codes)
defines that launcher's exit codes. Gate: `AAR_API_COVERS(api, exit)`.
Minimum minor: 5.

### aar_yield

```c
void aar_yield(void);
```

Gives the runtime a chance to run other apps and its own service work. It
counts as activity for the stall watchdog. Use it in an idle loop and at least
every few seconds during a long computation. A spinning `aar_event_poll` does
not wait and does not replace it. Missing slot: returns. Minimum minor: 5.

### aar_should_stop

```c
int aar_should_stop(void);
```

Non-zero when a stop was requested. A stop request also queues `AAR_EV_STOP`.
Does not block, and counts as activity for the watchdog. A missing slot gives a
non-zero value, `AAR_E_NOSYS`; core slots are never absent on a conforming
runtime. Minimum minor: 5.

### aar_time_us

```c
uint64_t aar_time_us(void);
```

Monotonic microseconds, non-decreasing, from an epoch the platform chooses. It
is not wall-clock time; `aar_time_unix_us` is. The
resolution is 1 ms or finer. Does not block, and counts as activity. Missing
slot: 0. **ZZ9000:** the source is the global timer, which counts at half the
666666687 Hz APU clock. Minimum minor: 5.

### aar_sleep_us

```c
void aar_sleep_us(uint32_t us);
```

Parks for at least `us` microseconds of the platform clock and returns nothing.
It is not a busy wait. It ends early only for a stop: if a stop is requested
while the app sleeps, or is already pending when the call is entered, it returns
at once and `aar_should_stop` is 1. The actual granularity is
`aar_info.sleep_granule_us`. With no other app competing for the CPU, a sleep
should end no later than `us` plus four granules (each at least 1 ms) plus 10 ms.
A parked app does not count toward the stall watchdog. Missing slot: returns at
once. **ZZ9000:** the deadline is a microsecond value checked at each look at
the clock; the 5 ms scheduler tick is a backstop, not a rounding step. Minimum
minor: 5.

### aar_get_info

```c
int aar_get_info(aar_info *out);
```

Fills `out` as described under [aar_info](#aar_info): the caller sets
`out->size`, the runtime fills at most that many bytes and zeroes the rest.
NULL `out` is `AAR_E_INVAL`. The wrapper is named `aar_get_info` because
`aar_info` is a type name. Does not block. Gate: `AAR_API_COVERS(api, info)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

## Private memory

An app has private memory: its image, stack and a heap. The platform decides
where; the app is position-independent and must not assume an address. The
heap starts at the end of the image. The heap is always dynamic: it grows and
shrinks in whole `heap_granule` units (1 MiB). Growth is first come, first
served from the platform's pool after its reserve and any client cap. The
note's `min_heap_mib` is committed at load. A refusal for lack of memory is
`AAR_E_NORESOURCE` with `AAR_REASON_MEMORY`, or `AAR_E_NOMEM`, from the load
call. The runtime queues `AAR_EV_LOW_MEMORY` for every running app when the free
pool falls below its low-water mark, and again only after the pool has risen
above its high-water mark. The marks are the platform's own (**ZZ9000:** 32 MiB
and 64 MiB). A refusal because of the client's private limit is not a crossing.

### aar_heap_range

```c
void aar_heap_range(void **base, uint64_t *bytes);
```

Reports the current heap span: its base and its length in bytes. Does not
block. Missing slot: writes NULL and 0. Minimum minor: 5.

### aar_heap_grow

```c
int aar_heap_grow(uint64_t bytes, void **old_break);
```

Extends the heap by `bytes`, a whole number of `heap_granule` units. On
success `*old_break` is the old end, where the heap ended before the call. The
new memory starts there, so the heap stays contiguous. Existing pointers never
move and the new memory is zeroed.
`AAR_E_NOMEM` means the pool or the cap cannot satisfy the request.
`AAR_E_INVAL` covers zero, a count that is not a whole number of granules, and
invalid arguments. A libc-flavor app reaches this through `_sbrk`; normal code
calls `malloc`. A freestanding app calls it directly. Gate:
`AAR_API_COVERS(api, heap_grow)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_heap_shrink

```c
int aar_heap_shrink(uint64_t bytes, void **new_break);
```

Returns whole granules from the top of the heap. On success `*new_break` is the
new end. It cannot release the image, the stack or the minimum heap already
committed at load. `AAR_E_INVAL` for zero or a count that is not a whole number
of granules. Gate: `AAR_API_COVERS(api, heap_shrink)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5.

### aar_get_heap_info

```c
int aar_get_heap_info(aar_heap_info *out);
```

Writes the private-memory accounting snapshot described under
[aar_heap_info](#aar_heap_info). The limit is the smaller of the platform's cap
and the client's private-memory cap. NULL `out` is `AAR_E_INVAL`. The wrapper
is named `aar_get_heap_info` because `aar_heap_info` is a type name. Gate:
`AAR_API_COVERS(api, heap_info)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_alloc

```c
void *aar_alloc(uint64_t bytes, uint32_t align);
```

The private allocator of the libc flavor, layer 2 of libaar. It is `malloc`
with an alignment: `align` must be a power of two and at least
`sizeof(void *)`. Failure is NULL. It is not declared when `AAR_FREESTANDING`
is defined: a freestanding app allocates from `aar_heap_range` and
`aar_heap_grow`. Shared memory is `aar_shared_alloc`, not this call. Not a
table slot. Minimum minor: 5.

### aar_free

```c
void aar_free(void *p);
```

Releases a block from `aar_alloc`. NULL is ignored. A pointer from
`aar_shared_alloc` is not valid here; use `aar_shared_free`. Not declared when
`AAR_FREESTANDING` is defined. Not a table slot. Minimum minor: 5.

## Shared buffers

A shared buffer is memory the app owns that the host can read and write. It is
the only way to pass bulk data to the Amiga side. The standard never swaps
bytes: values the app and the client exchange through a message or shared
buffer follow their own protocol, and `aar_bswap16`, `aar_bswap32` and
`aar_bswap64` swap a value when that protocol says big-endian.

The hand-off is always a message. The writer makes its stores visible with
`aar_publish`, then sends the message that announces the data. The reader, once
the message has arrived, calls `aar_acquire` before it reads. A granule has one
writer at a time: between two hand-offs the app must not write a granule the
client is writing, and the reverse. `aar_msg_send`, `aar_msg_poll` and
`aar_msg_wait` order the bus (a release and an acquire point), but they do not
clean or invalidate caches, which is why publish and acquire exist. The calls
are table calls rather than inline instructions because user mode on A64 cannot
invalidate by address. The wrapper returns at once when the runtime reports
coherence. The [developer guide](aar-developer-guide.md#shared-buffers) walks
through the rules.

Each app has a shared limit (`aar_info.shared_limit`, default
`AAR_SHARED_DEFAULT_MIB`, set by the client). The limit is a cap, not a
reservation: an allocation can fail below it because the platform's shared
region is also used for other things. The runtime uses shared buffers itself
for the console, the audio ring and file payloads. They are allocated on first
use, freed on close or exit, charged to `shared_used` and to the shared limit,
and do not count toward the `AAR_SHARED_MIN_BLOCKS` live blocks an app is
promised.

### aar_shared_alloc

```c
int aar_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                     aar_shared *out);
```

Allocates a shared buffer. `bytes` and `align` are rounded up to
`shared_granule`. On success `*out` holds the granted size, the host address
token `haddr`, the `AAR_SHAREDF_COHERENT` flag and the app mapping `ptr`. The
app passes `haddr` to the client, for example inside a message; an interior range
is described by the token plus an offset. The token names the allocated block,
and is meaningful only for this app and only until `aar_shared_free`, exit,
fault or stop. `flags` must be 0; a non-zero value, a zero `bytes` or a NULL
`out` is `AAR_E_INVAL`. `AAR_E_NOMEM` means the limit, the block count or the
platform's shared region is exhausted. Gate: `AAR_API_COVERS(api,
shared_alloc)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

**ZZ9000:** the token is the board-relative Zorro III offset the firmware
computes. The block comes from the host-visible heap, which is ordinary
write-back shareable memory. The Zorro master reaches it through the Zynq ACP,
which is coherent with the ARM caches, so the ARM side needs no cache
maintenance: `aar_publish` is a store barrier only. The 68k side must still
read it through a cache-inhibited mapping or do its own cache maintenance,
because the ACP does not reach the 68k data cache.

### aar_shared_free

```c
int aar_shared_free(void *p);
```

Releases a shared buffer. `p` is the `ptr` of an `aar_shared`. A pointer that is
not the base of a live block is `AAR_E_INVAL`. The quota returns at once. The
client's mapping of the token ends. Remaining blocks are released on exit,
fault, runtime restart and reset. Gate: `AAR_API_COVERS(api, shared_free)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_publish

```c
int aar_publish(const void *p, uint64_t bytes);
```

Writer: makes the app's stores to the range visible to the host. Call it on the
written range after the data is final and before the message that announces the
data. It is a store barrier when the block is coherent
(`AAR_SHAREDF_COHERENT`, or `AAR_INFOF_SHARED_COHERENT` for the runtime) and a
cache clean otherwise. The range is an arbitrary byte range inside a block;
the runtime rounds to `shared_granule` internally. A display surface does not
need it: a flip publishes the surface. Gate: `AAR_API_COVERS(api, publish)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_acquire

```c
int aar_acquire(const void *p, uint64_t bytes);
```

Reader: drops the app's stale view of data the host wrote. Call it on the range
after the message that announces the data has arrived and before the first
read. It is a load barrier when coherent and a cache invalidate otherwise.
Gate: `AAR_API_COVERS(api, acquire)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_bswap16

```c
static inline uint16_t aar_bswap16(uint16_t v);
```

Swaps the two bytes of `v`. Header-only. Both profiles are little-endian; use
it for values a protocol defines as big-endian. Minimum minor: 5.

### aar_bswap32

```c
static inline uint32_t aar_bswap32(uint32_t v);
```

Swaps the four bytes of `v`. Header-only. Minimum minor: 5.

### aar_bswap64

```c
static inline uint64_t aar_bswap64(uint64_t v);
```

Swaps the eight bytes of `v`. Header-only. Minimum minor: 5.

## Log, console and files

The client serves file I/O and console work with dos.library. A transfer is
served in chunks of the platform's choice, and each request waits at most
30 seconds for the client. The native file requester waits for user input
without this timeout.

### aar_log

```c
void aar_log(const char *msg, uint32_t len);
```

A lossy diagnostic channel, separate from stdout, for the platform's debug
output and for the client. Records longer than `AAR_LOG_MAX` bytes are
truncated, and the platform may rate-limit. It never blocks. It is not the
console. Gate: `AAR_API_COVERS(api, log)`. Missing slot: returns.
**ZZ9000 note:** rate limits are recorded in
[C-CON.10](../conformance/TESTS.md). Minimum minor: 5.

### aar_console_write

```c
int aar_console_write(const void *buf, uint32_t len, uint32_t *out_n);
```

Writes every requested byte to the console or returns an error. It never drops
bytes: when the client's buffer is full the call parks until the client has
taken it, and stop ends the wait with `AAR_E_IO`. The platform buffers console
output: a write of up to 1024 bytes into an empty buffer returns without waiting
for the client. NULL `out_n`, or NULL `buf`
with non-zero `len`, is `AAR_E_INVAL`. The client decides what the console is:
the Shell's `Output()`, or a window opened on first use for a Workbench launch.
`printf`, `puts` and `write` of fd 1 or 2 use this call in the libc flavor.
Gate: `AAR_API_COVERS(api, console_write)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_console_read

```c
int aar_console_read(void *buf, uint32_t len, uint32_t *out_n);
```

Parks until the client has input, then copies what is available, at most `len`.
`AAR_OK` with `*out_n` equal to 0 is end of input. A `len` of 0 returns
`AAR_OK` without waiting. Stop cancels the wait with `AAR_E_IO`. `read` of fd 0
uses this call in the libc flavor. Gate: `AAR_API_COVERS(api, console_read)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### Path policy

Paths are AmigaDOS paths, at most `AAR_PATH_MAX` bytes plus the NUL; an empty or
longer path is `AAR_E_INVAL`. The client decides the root and the policy, and
the runtime passes a path to the client unchanged after checking its length. A
client that serves apps it does not trust should apply this default, the one
aar-run uses:

- A relative path resolves under a root chosen by the client: the Shell's
  current directory, the icon's directory, or the client's explicit root.
- A path that would leave the root through `..` or a leading `/` is refused
  with `AAR_E_ACCES`.
- A path with a volume or assign prefix (a `:`) is refused with `AAR_E_ACCES`
  unless the client allowed absolute paths.
- Once the client has asked the app to stop, it refuses every request that would
  create, truncate or write a file; the call returns a negative code and the
  file is not changed.

This is path hygiene, not a security boundary: the client serves the calls with
its own AmigaDOS rights. A client is not required to apply it and a platform is
not judged on it.

### aar_file_open

```c
int aar_file_open(const char *path, uint32_t flags);
```

Opens a file and returns a file id, at least `AAR_FD_BASE`, or a negative code.
`flags` is a combination of `AAR_FILE_READ`, `AAR_FILE_WRITE`,
`AAR_FILE_CREATE`, `AAR_FILE_TRUNCATE` and `AAR_FILE_EXCL`; any other bit is
`AAR_E_INVAL`. At least one of read and write is needed. `AAR_FILE_CREATE` and
`AAR_FILE_TRUNCATE` need `AAR_FILE_WRITE`, and `AAR_FILE_EXCL` needs
`AAR_FILE_CREATE`; a combination that breaks these rules is `AAR_E_INVAL`. The
flags work as `O_CREAT`, `O_TRUNC` and `O_EXCL` do in POSIX, and the client
applies them as part of this one call, so a libc `open` passes its flags
straight through. An empty path, a path longer than `AAR_PATH_MAX` bytes or a
NULL path is `AAR_E_INVAL`. A missing file without `AAR_FILE_CREATE` is
`AAR_E_NOENT`, an existing file with `AAR_FILE_CREATE | AAR_FILE_EXCL` is
`AAR_E_EXIST`, a refused path is `AAR_E_ACCES`, and no free id is
`AAR_E_NOMEM`. At least `AAR_MIN_FILES` files can be open. The id remembers how
it was opened (see `aar_file_read` and `aar_file_write`). The call parks until
the client replies, until stop (`AAR_E_IO`), or until its request has waited 30
seconds for the client (`AAR_E_TIMEDOUT`; the app measures between 29 and 36
seconds). After a timeout the id is not reused for the rest of the app's life.
Gate: `AAR_API_COVERS(api, file_open)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5. **ZZ9000:** `AAR_FILE_READ` opens with `MODE_OLDFILE`,
`AAR_FILE_WRITE` with `MODE_READWRITE`, and
`AAR_FILE_CREATE | AAR_FILE_TRUNCATE` with `MODE_NEWFILE`.

**Platform note on `AAR_FILE_EXCL`.** A client that serves files through
AmigaDOS has no atomic exclusive create. The ZZ9000 client applies
`AAR_FILE_EXCL` by checking and creating within its single serving loop. That is
exclusive against the other apps it serves, but not against other Amiga
programs that create the same file at the same moment. A client that runs on a
host with an atomic exclusive create, such as the host runner with POSIX
`open`, passes the flag on and gets full exclusion.

### aar_file_close

```c
int aar_file_close(int fd);
```

Closes one file id. A bad id is `AAR_E_BADF`. The id is released even when the
client returns an error. Parks as `aar_file_open` does. Gate:
`AAR_API_COVERS(api, file_close)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_file_read

```c
int aar_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
```

Reads up to `len` bytes. `*out_n` is the count obtained. A short read with
`AAR_OK` is end of file. NULL `out_n` is `AAR_E_INVAL`; a bad id, or an id opened
without `AAR_FILE_READ`, is `AAR_E_BADF`. The transfer is served in chunks and
each chunk waits for the
client. Gate: `AAR_API_COVERS(api, file_read)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 5.

### aar_file_write

```c
int aar_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
```

Writes up to `len` bytes. A short write means the client stopped early; the
caller may retry with the remainder or treat it as an error. NULL `out_n` is
`AAR_E_INVAL`; a bad id, or an id opened without `AAR_FILE_WRITE`, is
`AAR_E_BADF` and nothing is written. Chunk and wait rules match
`aar_file_read`. Gate: `AAR_API_COVERS(api, file_write)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5.

### aar_file_read_async

```c
int aar_file_read_async(int fd, void *buf, uint32_t len, uint32_t *ticket);
```

Submits a read at the current file position without waiting for disk I/O.
An id not open for reading gives `AAR_E_BADF`; NULL `ticket` or NULL `buf`
with nonzero `len` gives `AAR_E_INVAL`. A zero-length transfer does no I/O.
On `AAR_OK`, `*ticket` is nonzero and must be collected. `AAR_E_BUSY` means
the transfer slot or staging is in use. An error leaves `*ticket` unchanged.
Keep the buffer allocated and do not access it until collection.
All three async slots must be covered and non-NULL, otherwise `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_file_write_async

```c
int aar_file_write_async(int fd, const void *buf, uint32_t len, uint32_t *ticket);
```

Submits a write at the current file position; requires an id opened for
writing. Ticket, buffer lifetime and error rules match `aar_file_read_async`.
Do not modify the source until collection. Success does not promise the
bytes have reached durable storage. Minimum minor: 6.

### aar_file_collect

```c
int aar_file_collect(uint32_t ticket, int32_t *status, uint32_t *out_n);
```

Polls or advances a transfer without waiting for I/O. `AAR_E_BUSY` means
poll again, and `AAR_E_STALE` means the ticket is not current. A zero ticket
or NULL output is `AAR_E_INVAL`. A return of `AAR_OK` consumes the ticket and
sets `*status` to the actual I/O result and `*out_n` to the confirmed bytes,
even on a short transfer or I/O error. Other returns leave outputs untouched.
The buffer is reusable after terminal collection, but a stop or timeout does
not cancel native I/O that already started. See [section 7.1](../spec/aar.md#71-files)
for descriptor and staging rules. Minimum minor: 6.

### aar_file_seek

```c
int aar_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
```

Sets the file position. `out_pos` receives the new position and may be NULL.
`whence` must be `AAR_SEEK_SET`, `AAR_SEEK_CUR` or `AAR_SEEK_END`; anything else
is `AAR_E_INVAL`. A position before the start of the file is `AAR_E_INVAL` and
leaves the position unchanged. A position past the end may be refused with
`AAR_E_INVAL` (AmigaDOS handlers refuse it on a file opened for reading); if it
is accepted, a read there returns `AAR_OK` with 0 bytes, and what a write there
does is not specified. Parks for one client reply. Gate: `AAR_API_COVERS(api,
file_seek)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_file_stat

```c
int aar_file_stat(const char *path, aar_stat *out);
```

Fills size, modification time, mode and protection bits. NULL `out` is
`AAR_E_INVAL`. Does not
open a file id. Parks for one client reply. Gate: `AAR_API_COVERS(api,
file_stat)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_file_list_open

```c
int aar_file_list_open(const char *path);
```

Opens a directory and returns a list id, from the same numeric range as file
ids (at least `AAR_FD_BASE`) but a separate table of at least `AAR_MIN_FILES`.
Parks as `aar_file_open` does. Gate: `AAR_API_COVERS(api, file_list_open)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_file_list_next

```c
int aar_file_list_next(int list, aar_dirent *out);
```

Copies the next entry. No further entry is `AAR_E_NOENT`. A bad id is
`AAR_E_BADF`; NULL `out` is `AAR_E_INVAL`. Names longer than `AAR_NAME_MAX` bytes
do not occur. Parks for one client reply. Gate: `AAR_API_COVERS(api,
file_list_next)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_file_list_close

```c
int aar_file_list_close(int list);
```

Closes a directory id. Parks for one client reply. Gate:
`AAR_API_COVERS(api, file_list_close)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_file_delete

```c
int aar_file_delete(const char *path);
```

Removes a file or an empty directory. The path rules and the policy are those
of `aar_file_open`, and like every call that writes it is refused after a stop.
A missing object is `AAR_E_NOENT`, a directory that is not empty `AAR_E_EXIST`,
a delete-protected object `AAR_E_ACCES` and an object in use `AAR_E_BUSY`.
Parks for one client reply. A client that does not serve the op answers
`AAR_E_NOSYS`. Gate: `AAR_API_COVERS(api, file_delete)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 6.

### aar_file_rename

```c
int aar_file_rename(const char *from, const char *to, uint32_t flags);
```

Renames or moves a file or directory within one volume; paths on different
volumes are `AAR_E_INVAL`. `flags` is 0 or `AAR_RENAME_REPLACE`; another bit is
`AAR_E_INVAL`. An existing `to` is `AAR_E_EXIST` unless `AAR_RENAME_REPLACE` is
set, which deletes the target first and is not atomic on AmigaDOS. When `from`
and `to` name the same object, the call returns `AAR_OK` and changes nothing.
A missing `from` is `AAR_E_NOENT`. The path rules and the policy are those of
`aar_file_open`, and the call is refused after a stop. Parks for one client
reply. Gate: `AAR_API_COVERS(api, file_rename)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_dir_create

```c
int aar_dir_create(const char *path);
```

Creates one directory. An existing object is `AAR_E_EXIST` and a missing parent
`AAR_E_NOENT`. The path rules and the policy are those of `aar_file_open`, and
the call is refused after a stop. Parks for one client reply. Gate:
`AAR_API_COVERS(api, dir_create)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
6.

### aar_file_set_date

```c
int aar_file_set_date(const char *path, int64_t mtime);
```

Sets the modification time of a file or directory. `mtime` is Unix seconds in
UTC, as `aar_stat.mtime` reports it. A time that the filesystem cannot
represent is `AAR_E_INVAL`. The path rules and the policy are those of
`aar_file_open`, and the call is refused after a stop. Parks for one client
reply. Gate: `AAR_API_COVERS(api, file_set_date)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_file_set_protect

```c
int aar_file_set_protect(const char *path, uint32_t protect);
```

Sets the protection bits of a file or directory. `protect` is a combination of
the `AAR_PROT_*` constants, with bits 0 to 3 set to deny. A bit that no
`AAR_PROT_*` constant names is `AAR_E_INVAL`. A mask of known bits that the
platform cannot store is `AAR_E_NOSYS`, and the protection does not change;
after `AAR_OK`, `aar_file_stat` reports the mask that was set. The path rules
and the policy are those of `aar_file_open`, and the call is refused after a
stop. Parks for one client reply. Gate: `AAR_API_COVERS(api, file_set_protect)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 6.

### aar_file_truncate

```c
int aar_file_truncate(int fd, uint64_t size);
```

Sets the length of an open file to `size` bytes. A longer size extends the file
with zero bytes and a shorter one cuts it. The id must have been opened with
`AAR_FILE_WRITE`; any other id is `AAR_E_BADF`. The call is refused after a
stop. Parks for one client reply. Gate: `AAR_API_COVERS(api, file_truncate)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 6.

### aar_get_volume_info

```c
int aar_get_volume_info(const char *path, aar_volume_info *out);
```

Describes the volume that holds `path`. The caller sets `out->size`. NULL `out`
is `AAR_E_INVAL`. The path rules and the policy are those of `aar_file_open`.
Parks for one client reply. Gate: `AAR_API_COVERS(api, volume_info)`. Missing
slot: `AAR_E_NOSYS`. Minimum minor: 6.

### aar_file_get_comment

```c
int aar_file_get_comment(const char *path, void *buf, uint32_t bytes,
                         uint32_t *out_n);
```

Reads native comment bytes without charset conversion or a terminator. The
call returns `AAR_OK` with an empty result if the object has no comment.
If `bytes` is too small, it copies the prefix and returns `AAR_E_NOSPC`.
Both results set `*out_n` to the full length. To query the length without
copying bytes, pass NULL for `buf` and zero for `bytes`: the result is `AAR_OK`
for an empty comment or `AAR_E_NOSPC` for a nonempty one. `out_n` is required.
Parks for one client reply. Gate: `AAR_API_COVERS(api, file_get_comment)`.
An unsupported operation returns `AAR_E_NOSYS`. See [spec 7.1](../spec/aar.md#71-files).
Minimum minor: 6.

### aar_file_set_comment

```c
int aar_file_set_comment(const char *path, const void *data, uint32_t bytes);
```

Sets a file or directory comment. Pass the native bytes without a terminator,
or pass zero bytes to clear the comment. NULL `data` with a nonzero length,
embedded NUL, and lengths beyond `AAR_FILE_COMMENT_MAX` return `AAR_E_INVAL`.
The same path policy and stop rules apply as for other file mutations.
Parks for one client reply. Gate: `AAR_API_COVERS(api, file_set_comment)`.
An unsupported operation returns `AAR_E_NOSYS`. Minimum minor: 6.

### AAR_FILE_COMMENT_MAX

`79u`. The maximum number of native bytes in a file comment.

### aar_file_request

```c
int aar_file_request(uint32_t flags, const char *title,
                     const char *initial_path, char *path, uint32_t capacity);
```

Shows the client's native requester for one file. Pass `0` for Open or
`AAR_FILE_REQUEST_SAVE` for Save As. The optional `title` (UTF-8) and
`initial_path` (native path bytes) are NUL-terminated strings of at most
255 bytes each before the NUL. NULL and empty strings both use the client's
defaults for that input.

Returns `1` and writes a NUL-terminated native path (at most `AAR_PATH_MAX`
bytes before the NUL) on selection, or `0` on user cancellation. `path` must
be non-NULL and `capacity` must be positive, counting room for the NUL.
Unknown flags, overlong inputs or missing output storage give `AAR_E_INVAL`.
If the selected path does not fit, the call returns `AAR_E_NOSPC` without
truncating it. A client may refuse a path under its usual path policy
(`AAR_E_ACCES`). Neither cancellation nor any error changes `path`. A
selection does not open or create a file and does not grant additional
access: pass the returned path to `aar_file_open` yourself.

The call parks without a timeout until the client replies or a stop ends
the wait with `AAR_E_IO`. Gate: `AAR_API_COVERS(api, file_request)` and a
non-NULL slot. The wrapper or an unsupported client returns `AAR_E_NOSYS`.
There is no capability bit. Minimum minor: 6.

### AAR_FILE_REQUEST_SAVE

`1u`. Use Save As instead of Open with `aar_file_request`. Minimum minor: 6.

### AAR_FILE_READ

`1u`. Open for read.

### AAR_FILE_WRITE

`2u`. Open for write.

### AAR_FILE_CREATE

`4u`. Create the file if it does not exist. An existing file is opened as it is and keeps its contents. Needs `AAR_FILE_WRITE`. Without this flag a missing file is `AAR_E_NOENT`.

### AAR_FILE_TRUNCATE

`8u`. Truncate an existing file to length 0 when it is opened. Needs `AAR_FILE_WRITE`. It does not create: a missing file is `AAR_E_NOENT` unless `AAR_FILE_CREATE` is also set. `AAR_FILE_CREATE | AAR_FILE_TRUNCATE` creates or replaces, which is what `AAR_FILE_CREATE` meant before 1.0.

### AAR_FILE_EXCL

`16u`. Only with `AAR_FILE_CREATE`: fail with `AAR_E_EXIST` if the file already exists, and leave it untouched. Otherwise create it. How exclusive the check is depends on the client; see the platform note under `aar_file_open`. Any bit not listed among the `AAR_FILE_*` flags is `AAR_E_INVAL`.

### AAR_SEEK_SET

`0`. Seek from the start.

### AAR_SEEK_CUR

`1`. Seek from the current position.

### AAR_SEEK_END

`2`. Seek from the end.

### AAR_MODE_FILE

`1u`. `aar_stat.mode` and `aar_dirent.mode` for a file.

### AAR_MODE_DIR

`2u`. `aar_stat.mode` and `aar_dirent.mode` for a directory.

### AAR_RENAME_REPLACE

`(1u << 0)`. `aar_file_rename` flag: delete an existing target first. Not
atomic on AmigaDOS. Minimum minor: 6.

### AAR_PROT_DELETE

`(1u << 0)`. Protection bit, at the AmigaDOS `FIBF_DELETE` position. Bits 0 to 3
are set to deny, as in AmigaDOS: a set bit forbids the action. Minimum minor: 6.

### AAR_PROT_EXECUTE

`(1u << 1)`. Protection bit `FIBF_EXECUTE` (deny). Minimum minor: 6.

### AAR_PROT_WRITE

`(1u << 2)`. Protection bit `FIBF_WRITE` (deny). Minimum minor: 6.

### AAR_PROT_READ

`(1u << 3)`. Protection bit `FIBF_READ` (deny). Minimum minor: 6.

### AAR_PROT_ARCHIVE

`(1u << 4)`. Protection bit `FIBF_ARCHIVE`: the file has been archived. Minimum
minor: 6.

### AAR_PROT_PURE

`(1u << 5)`. Protection bit `FIBF_PURE`: the program can be made resident.
Minimum minor: 6.

### AAR_PROT_SCRIPT

`(1u << 6)`. Protection bit `FIBF_SCRIPT`: the file is a script. Minimum minor: 6.

### AAR_PROT_HOLD

`(1u << 7)`. Protection bit `FIBF_HOLD`: keep a pure program resident. Minimum
minor: 6.

### AAR_VOLF_READONLY

`(1u << 0)`. `aar_volume_info.flags`: the volume is write-protected. Minimum
minor: 6.

### aar_map_file

```c
int aar_map_file(const char *path, const void **data, uint64_t *bytes);
```

Optional. Reads a whole file into a read-only buffer that the runtime owns. The
pointer is read-only by contract and stays valid until `aar_unmap_file`, exit or
fault. NULL arguments are `AAR_E_INVAL`. A file above the platform's limit is
`AAR_E_NOSPC`. A stat or open error is returned as that call's code, and a
short read is `AAR_E_IO`. The call parks on each client wait. A platform may
return `AAR_E_NOSYS`; an app can always use `aar_file_read`. Gate:
`AAR_API_COVERS(api, map_file)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.
**ZZ9000:** implemented with a shared buffer, so the file counts against the
shared limit, and the file size limit is 16 MiB.

### aar_unmap_file

```c
void aar_unmap_file(const void *data);
```

Releases the buffer from `aar_map_file`. NULL is ignored. The same file can be
mapped again afterwards. Gate: `AAR_API_COVERS(api, unmap_file)`. Missing slot:
returns. Minimum minor: 5.

## Wall-clock time, locale, environment and clipboard

The client serves these calls. Each parks for one client reply, a client that
does not serve one answers `AAR_E_NOSYS`, and a client may refuse one by policy
with `AAR_E_ACCES`. Minimum minor: 6 for everything in this section.

### aar_time_unix_us

```c
int aar_time_unix_us(int64_t *out_us);
```

Stores the wall-clock time in `*out_us`, in microseconds since 1970-01-01
00:00:00 UTC. The client serves it from `DateStamp` plus 2922 days and its UTC
offset; `aar_get_locale_info` says whether that offset is the user's setting. A
runtime may answer from a base it fetched once plus `aar_time_us`. The value can
step when the user sets the clock, so measure intervals with `aar_time_us`.
NULL `out_us` is `AAR_E_INVAL`. A stop is `AAR_E_IO`. Gate:
`AAR_API_COVERS(api, time_unix_us)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 6.

### aar_get_locale_info

```c
int aar_get_locale_info(aar_locale_info *out);
```

Fills `*out`. The caller sets `out->size`; the runtime fills at most that many
bytes and zeroes the rest. NULL `out` is `AAR_E_INVAL`. Gate:
`AAR_API_COVERS(api, locale_info)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
6.

### aar_env_get

```c
int aar_env_get(const char *name, char *buf, uint32_t bytes, uint32_t *out_n);
```

Reads the value of `ENV:<name>` (the client uses `GetVar` with
`GVF_GLOBAL_ONLY`) as the bytes that are stored. `*out_n` is the length of the
value without a NUL, and the value is NUL-terminated when it fits. When
`bytes <= *out_n` the call returns `AAR_E_NOSPC` with `*out_n` set, so a call
with `bytes` 0 finds the length. An unset variable is `AAR_E_NOENT`. A `name`
of 0 or more than 255 bytes, or a NULL `name` or `out_n`, is `AAR_E_INVAL`.
Gate: `AAR_API_COVERS(api, env_get)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 6.

### aar_clip_read

```c
int aar_clip_read(uint32_t unit, uint32_t type, void *buf, uint32_t bytes,
                  uint32_t *out_n);
```

Reads the clipboard `unit` in `type`. `*out_n` is the full size of the clip in
that type, and the call copies `min(bytes, *out_n)` bytes. It returns
`AAR_E_NOSPC` when they did not all fit, with `*out_n` set. An empty clipboard,
or one with no data of that type, is `AAR_E_NOENT`. A `unit` of
`AAR_CLIP_UNITS` or more, or an unknown `type`, is `AAR_E_INVAL`.
`AAR_CLIP_TEXT` is UTF-8 without a NUL; the client converts it from the FTXT
charset, ISO-8859-1 unless a `CSET` chunk says otherwise (`CHRS` chunks hold
the text). The runtime moves a
large clip in several requests, and the clip it reads is one consistent clip.
Gate: `AAR_API_COVERS(api, clip_read)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 6.

### aar_clip_write

```c
int aar_clip_write(uint32_t unit, uint32_t type, const void *data,
                   uint32_t bytes);
```

Replaces the clip of `unit` with `bytes` bytes of `data` in `type`. The units,
types and `AAR_E_INVAL` cases are those of `aar_clip_read`. A text character
that the client's charset cannot represent becomes `?`. The client replaces the
clip only when the last chunk has arrived, so a write that fails part way
changes nothing. Gate: `AAR_API_COVERS(api, clip_write)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 6.

### AAR_LOCALEF_OFFSET_KNOWN

`(1u << 0)`. `aar_locale_info.flags`: `utc_offset_min` is the user's setting.
Minimum minor: 6.

### AAR_CLIP_TEXT

`1u`. Clipboard type: UTF-8 text. The client converts to and from IFF FTXT.
Minimum minor: 6.

### AAR_CLIP_IFF

`2u`. Clipboard type: one IFF FORM, passed through unchanged. Minimum minor: 6.

### AAR_CLIP_UNITS

`256u`. The number of clipboard units, 0 to 255 as `clipboard.device` has them.
Unit 0 is the primary clipboard. Minimum minor: 6.

## Events and messages

### aar_event_poll

```c
int aar_event_poll(aar_event *out, uint32_t max, uint32_t *out_n);
```

Copies up to `max` queued events and stores the count in `*out_n`. An empty
queue returns `AAR_OK` and 0. It never blocks: an app that has nothing to do
uses `aar_msg_wait`, `aar_sleep_us` or `aar_yield`, not a spin. NULL `out_n`,
or NULL `out` with non-zero `max`, is `AAR_E_INVAL`. It always returns the
lifecycle events `AAR_EV_STOP` and `AAR_EV_LOW_MEMORY`; with `AAR_GROUP_INPUT`
it also returns the input events. An app ignores an event type it does not
know. Events are delivered in order. The runtime
keeps at least 64 events in order. Once it holds that many that the app has not
taken, it may discard each further input event except mouse motion, which it
merges. It never discards `AAR_EV_STOP` or `AAR_EV_LOW_MEMORY`.
When the completion queue is full the client drops the newest event and counts
it (`AAR_CB_EVENTS_DROPPED`). Mouse motion is never lost without a count. The
client library merges it (it sums `dx` and `dy` while the buttons are
unchanged; absolute motion keeps the latest position instead)
or counts it, so an app that polls every frame does not lose input. Gate:
`AAR_API_COVERS(api, event_poll)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 5.

### AAR_EV_KEY_DOWN

`1u`. Key down. `code` is the Amiga raw key, `buttons` the qualifiers. Input group.

### AAR_EV_KEY_UP

`2u`. Key up. `code` is the raw key without the Amiga up bit. Input group.

### AAR_EV_MOUSE

`3u`. Mouse movement or a button change. `code` is 0 for motion only, or the
button whose state changed: 1 left, 2 right, 3 middle. `buttons` holds the state
of the buttons (`AAR_MOUSE_LEFT`, `AAR_MOUSE_RIGHT`, `AAR_MOUSE_MIDDLE`). `dx`
and `dy` are deltas, or with `AAR_DISPCAP_POINTER_ABS` the pointer position:
see `AAR_MOUSE_ABSOLUTE`. Input group.

### AAR_EV_JOY

`4u`. Joystick. `code` is the port: 0 and 1 are the Amiga game ports, and 2 and up
are other devices that the client numbers. `dx` and `dy` are the axes (-1, 0,
1) and `buttons` holds the `AAR_JOY_*` bits. Input group. **ZZ9000:** aar-run reads port 1 while its display window is active, and port 0 as well when the `JOYPORT0` tool type is set. It reads `JOYxDAT` and the CIA-A fire bits and does not drive the port pins.

### AAR_EV_STOP

`5u`. A stop was requested (Ctrl-C, window close, or the client's stop call). Lifecycle; always delivered. `aar_should_stop` becomes true as well. The grace period is `aar_info.stop_grace_ms` (default 500), then the runtime stops the app by force.

### AAR_EV_RESIZE

`6u`. The inner size of the display's window changed: `dx` and `dy` are the new
size in pixels. Sent only with `AAR_DISPCAP_RESIZE`; the app answers with
`aar_display_resize`. Minimum minor: 5, and the client sends it from 6.

### AAR_EV_BREAK

`7u`. Ctrl-D, Ctrl-E or Ctrl-F from the client's display window, in addition to the normal raw key event. `code` is `AAR_BREAK_CTRL_D`, `AAR_BREAK_CTRL_E` or `AAR_BREAK_CTRL_F`. These are not console end of input. Input group.

### AAR_EV_LOW_MEMORY

`8u`. The free app-memory pool crossed below its low-water mark. One event per crossing; the next is reported only after the pool has risen above the high-water mark. Lifecycle; always delivered. Every field other than `type` is 0.

### AAR_EV_TEXT

`9u`. A typed character. `code` is a Unicode code point from 1 to 0xFFFF and
`buttons` the qualifiers. Sent only with `AAR_DISPCAP_TEXT_INPUT`. Minimum minor: 6.

### AAR_EV_WHEEL

`10u`. Wheel movement. `dx` is horizontal steps (positive to the right), `dy`
vertical steps (positive down, towards the user), `buttons` the qualifiers. The
client sends it whenever it can produce it, and converts the NewMouse raw keys
0x7A and 0x7B, 0x7C and 0x7D, or `IDCMP_EXTENDEDMOUSE`, without also sending
them as raw keys. Minimum minor: 6.

### AAR_EV_CLOSE_REQUEST

`11u`. The close gadget was used. Sent only with `AAR_DISPCAP_CLOSE_REQUEST`,
and then it replaces the stop that the gadget sends otherwise. Minimum minor: 6.

### AAR_EV_ACTIVE

`12u`. The display gained input focus (`code` 1) or lost it (`code` 0). The
client sends it whenever it can produce it. Minimum minor: 6.

### AAR_MOUSE_LEFT

`(1u << 0)`. Left button, in `buttons` of `AAR_EV_MOUSE`. Minimum minor: 6.

### AAR_MOUSE_RIGHT

`(1u << 1)`. Right button. Minimum minor: 6.

### AAR_MOUSE_MIDDLE

`(1u << 2)`. Middle button. Minimum minor: 6.

### AAR_MOUSE_ABSOLUTE

`(1u << 15)`. In `buttons` of `AAR_EV_MOUSE`: `dx` and `dy` are the pointer
position relative to the surface's top-left pixel, not deltas. With
`AAR_DISPCAP_POINTER_ABS` every mouse event carries it, and the position can lie
outside the surface while a button is held. Merged motion keeps the latest
position. Minimum minor: 6.

### AAR_JOY_BUTTON1

`(1u << 0)`. In `buttons` of `AAR_EV_JOY`: fire, CD32 red. Minimum minor: 6.

### AAR_JOY_BUTTON2

`(1u << 1)`. Second button, CD32 blue. Minimum minor: 6.

### AAR_JOY_BUTTON3

`(1u << 2)`. Third button, CD32 green. Minimum minor: 6.

### AAR_JOY_BUTTON4

`(1u << 3)`. CD32 yellow. Minimum minor: 6.

### AAR_JOY_PLAY

`(1u << 4)`. CD32 play/pause. Minimum minor: 6.

### AAR_JOY_FORWARD

`(1u << 5)`. CD32 right shoulder. Minimum minor: 6.

### AAR_JOY_REVERSE

`(1u << 6)`. CD32 left shoulder. Minimum minor: 6.

### AAR_BREAK_CTRL_D

`4u`. `code` of an `AAR_EV_BREAK` event for Ctrl-D.

### AAR_BREAK_CTRL_E

`5u`. `code` for Ctrl-E.

### AAR_BREAK_CTRL_F

`6u`. `code` for Ctrl-F.

A message is exactly `AAR_MSG_BYTES` (32) opaque bytes. Each direction is a
FIFO that holds at least `aar_info.msg_capacity` messages and never drops or
reorders. The client posts a message only while fewer than `msg_capacity`
messages sent to the app are waiting unread, and otherwise its send call
answers `AAR_E_BUSY`, so a full inbox never holds up a reply. The content is
the protocol of the app and its client. A convention
is recommended but not required: word 0 is a type, and words are little-endian
when the app writes them. Messages remain readable by the client after the app
has exited, until the client closes its handle. For data that travels with a
message, see [Shared buffers](#shared-buffers).

### aar_msg_send

```c
int aar_msg_send(const void *msg);
```

Copies `AAR_MSG_BYTES` bytes toward the host. A full queue is `AAR_E_BUSY`; the
call never blocks. NULL is `AAR_E_INVAL`. A corrupt queue is `AAR_E_IO`. The
client is notified. Shorter messages are not a different call: the caller pads
to 32 bytes. Sending is a release point: all earlier stores are ordered before
the message becomes visible. Gate: `AAR_API_COVERS(api, msg_send)`. Missing
slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_msg_poll

```c
int aar_msg_poll(void *msg);
```

Copies one host-to-app message and returns 1, or returns 0 when the queue is
empty, or a negative code. NULL is `AAR_E_INVAL`; a corrupt queue is
`AAR_E_IO`. Does not block. A message that arrives is an acquire point: loads
after it are ordered after it. Gate: `AAR_API_COVERS(api, msg_poll)`. Missing
slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_msg_wait

```c
int aar_msg_wait(uint32_t timeout_us);
```

Parks the app until the host-to-app queue holds a message or `timeout_us`
microseconds pass. `AAR_OK` means a message is queued and the next
`aar_msg_poll` returns it; the wait does not consume it. A message already
queued returns at once. `AAR_E_TIMEDOUT` means the timeout passed with the queue
empty. A timeout of 0 only tests the queue and never parks. A stop request ends
the wait with `AAR_E_IO`, but a queued message is reported first, so a stop
never hides one. A corrupt queue is `AAR_E_IO`. A parked app does not count
toward the stall watchdog. A runtime without a real wait still provides the
call and may implement it as a sleep-and-poll loop. Gate:
`AAR_API_COVERS(api, msg_wait)`. Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

```c
while (!aar_should_stop()) {
    int rc = aar_msg_wait(500000u);

    if (rc == AAR_E_TIMEDOUT)
        continue;               /* idle work, then wait again */
    if (rc != AAR_OK)
        break;                  /* stop, or a corrupt queue */
    while (aar_msg_poll(message) == 1)
        handle(message);
}
```

## Optional calls

### aar_ext

```c
const void *aar_ext(uint32_t platform_id, uint32_t ext_id);
```

Returns a pointer to a size-first, append-only extension table owned by the
platform, or NULL. It is the only place for features that exist on one
platform. `platform_id` is an `AAR_PLATFORM_*` value and `ext_id` is defined by
that platform's extension header. An app that uses an extension is not portable
and should say so in its documentation. Gate: `AAR_API_COVERS(api, ext)`.
Missing slot: NULL. Minimum minor: 5.

## Display group

Requires `AAR_GROUP_DISPLAY`. One display is open per app. The client chooses
the Amiga screen or window and binds a bitmap to the surface; the memory is
whatever that bitmap is in, mapped for the app by the runtime. The app draws
into `back`, flips, and calls `aar_display_refresh` before it draws again.
**A flip publishes the surface**: before the host sees the frame the runtime
cleans the presented range, so the app needs no `aar_publish` for a surface.
Draw into the surface back buffer or into private memory and then present;
shared buffers are not a framebuffer scratch pad.

### aar_display_open

```c
int aar_display_open(uint32_t w, uint32_t h, uint32_t format,
                     uint32_t flags, aar_surface *out);
```

Asks the client to open a screen and bind it. Width and height must be 1 to
`AAR_DISPLAY_MAX_DIM`. `format` must be an `AAR_FMT_*` value and `flags` a
combination of the `AAR_DISPLAY_*` flags; unknown values and a
NULL `out` are `AAR_E_INVAL`. `AAR_FMT_CLUT8` without `AAR_DISPCAP_CLUT8` is
`AAR_E_NOSYS`. A second open, since each app has one display, is `AAR_E_BUSY`.
The call parks until the bind succeeds or stop (`AAR_E_IO`). On success `*out`
holds the id, pitch, layout, caps and the mappings. The flags are requests, and
the caps in `*out` say what took effect: `AAR_DISPLAY_LATEST` without
`AAR_DISPCAP_LATEST` in the granted caps fell back to plain
`AAR_DISPLAY_VSYNC`, and the other flags work the same way.
Gate: `AAR_API_COVERS(api, display_open)`. Missing
slot: `AAR_E_NOSYS`. Minimum minor: 5. **ZZ9000:** the client is aar-run or
another client of `aar.library`; it opens a Picasso96 screen. If it cannot open the
mode, the call returns `AAR_E_IO` and the client asks the app to stop.

### aar_display_flip

```c
int aar_display_flip(uint32_t id);
```

Presents the back buffer and parks until the frame is shown, or until stop
cancels it (`AAR_E_IO`). A flip already pending is `AAR_E_BUSY`. A bad id is
`AAR_E_INVAL`. In `AAR_DISPLAY_LATEST` mode the flip replaces a pending frame,
returns the replaced buffer to drawing and does not wait for scanout; the
`AAR_DISPLAY_VSYNC` path parks. Gate: `AAR_API_COVERS(api, display_flip)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 5.

### aar_display_flip_async

```c
int aar_display_flip_async(uint32_t id);
```

Queues a flip and returns without waiting for scanout. `AAR_OK`, or a negative
code. A flip still pending is `AAR_E_BUSY`. Call it only when
`AAR_DISPCAP_ASYNC` is set in the surface caps. Then wait with
`aar_display_flip_idle` before drawing the next frame. Gate:
`AAR_API_COVERS(api, display_flip_async)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_display_flip_idle

```c
int aar_display_flip_idle(void);
```

Returns 1 when the last queued flip has been shown and the back buffer may be
written, 0 while a flip is pending, or a negative code on error. Does not block.
Gate: `AAR_API_COVERS(api, display_flip_idle)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 5.

### aar_display_refresh

```c
int aar_display_refresh(aar_surface *s);
```

Reloads `front`, `back`, `pitch` and `layout` after a completed flip. Call it
when `aar_display_flip_idle` reports idle, or after `aar_display_flip` returns.
After it, `front` is NULL or names the buffer that holds the frame just
presented, and `back` names the buffer to draw into next. `back` has no promised
content. With `AAR_DISPCAP_LATEST` no flip completes, and `front` holds no
particular frame.
A mismatched id is `AAR_E_INVAL`; a flip still pending is `AAR_E_BUSY`. Does not
block. Gate: `AAR_API_COVERS(api, display_refresh)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5.

### aar_display_vsync

```c
int aar_display_vsync(void);
```

Pacing hint: returns `AAR_OK` once the runtime has done what it can to line the
app up with the display refresh, which may be nothing. A platform may wait for
the next vertical blank; an app must not depend on it. An app that wants display
pacing opens with `AAR_DISPLAY_VSYNC`, whose flip returns when the frame is on
scanout. No bound display is
`AAR_E_INVAL`. Gate: `AAR_API_COVERS(api, display_vsync)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5. **ZZ9000:** it records a vsync request and
yields once; it does not park for the next raster line.

### aar_display_set_palette

```c
int aar_display_set_palette(uint32_t id, uint32_t first, uint32_t count,
                            const uint32_t *rgb);
```

Writes `count` CLUT8 entries as `0x00RRGGBB` words starting at `first`. The
surface must be CLUT8. `count` must be non-zero and `first + count` at most 256.
Does not block. Gate: `AAR_API_COVERS(api, display_set_palette)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5.

### aar_display_close

```c
int aar_display_close(uint32_t id);
```

Drops the bound display and any runtime-owned back buffer. A bad id is
`AAR_E_INVAL`. The client also closes the screen on exit, fault and reset.
Gate: `AAR_API_COVERS(api, display_close)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_display_latest_dropped

```c
uint32_t aar_display_latest_dropped(void);
```

The cumulative number of frames replaced in `AAR_DISPLAY_LATEST` mode. 0 when
that mode is not active or the slot is absent. Does not block. It is valid only
if `AAR_DISPCAP_LATEST` is in effect. Gate: `AAR_API_COVERS(api,
display_latest_dropped)`. Minimum minor: 5.

### aar_display_get_info

```c
int aar_display_get_info(uint32_t id, aar_display_info *out);
```

Fills `*out` with the state of the open display: surface size, refresh rate,
the screen it is on and the `AAR_DISPINFO_*` flags. The caller sets
`out->size`. NULL `out`, or an id that is not the open display, is
`AAR_E_INVAL`. Parks for one client reply. A client that does not serve the op
answers `AAR_E_NOSYS`. Gate: `AAR_API_COVERS(api, display_info)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 6.

### aar_display_modes

```c
int aar_display_modes(aar_display_mode *out, uint32_t max, uint32_t *out_n);
```

Lists the display modes that the client can show. It needs no open display. At
most `max` records are written to `out`, and `*out_n` is the number of modes
the client has in all; when there are more than `max` the call returns
`AAR_E_NOSPC` with `*out_n` set. NULL `out_n`, or NULL `out` with non-zero
`max`, is `AAR_E_INVAL`. Parks for one client reply. A client that does not
serve the op answers `AAR_E_NOSYS`. Gate: `AAR_API_COVERS(api, display_modes)`.
Missing slot: `AAR_E_NOSYS`. Minimum minor: 6.

### aar_display_resize

```c
int aar_display_resize(uint32_t id, uint32_t w, uint32_t h, aar_surface *out);
```

Changes the surface size. Call it after `AAR_EV_RESIZE`, normally with the size
in the event. The call parks while the client reallocates the bitmap and is
answered as `aar_display_open` is; `*out` is refreshed and the app uses the new
`front` and `back` from then on. The client keeps showing the old frame until
the next flip. `AAR_E_NOSYS` without `AAR_DISPCAP_RESIZE`, `AAR_E_BUSY` while a
flip is pending, and `AAR_E_INVAL` for a size outside 1 to
`AAR_DISPLAY_MAX_DIM`, an unknown id or a NULL `out`. Gate:
`AAR_API_COVERS(api, display_resize)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 6.

### aar_display_set_title

```c
int aar_display_set_title(uint32_t id, const char *title);
```

Sets the title that the client shows in the title bar of the window or, without
one, of the screen. `title` is UTF-8 and NUL-terminated, at most 255 bytes; the
client converts it to its charset. A longer title, a NULL `title` or an unknown
id is `AAR_E_INVAL`. Parks for one client reply. Gate:
`AAR_API_COVERS(api, display_set_title)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 6.

### aar_display_pointer

```c
int aar_display_pointer(uint32_t id, uint32_t mode, const aar_pointer *image);
```

Sets the mouse pointer while it is over the display. `mode` is one of the
`AAR_POINTER_*` values. Only `AAR_POINTER_CUSTOM` uses `image`, and it requires
one with a valid size and hot spot. Another mode, a bad image or an unknown id
is `AAR_E_INVAL`. The runtime copies the pixels before it returns. Parks for
one client reply. A client that does not serve the op answers `AAR_E_NOSYS`.
Gate: `AAR_API_COVERS(api, display_pointer)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_display_wait_vblank

```c
int aar_display_wait_vblank(uint32_t timeout_us);
```

Parks until the next vertical blank of the display's screen and returns
`AAR_OK`. After `timeout_us` microseconds it returns `AAR_E_TIMEDOUT`. A
`timeout_us` of 0, or no open display, is `AAR_E_INVAL`. `AAR_E_NOSYS` where the
platform and the client cannot see a vertical blank, and `AAR_E_IO` on a stop.
The runtime may serve it itself or ask the client. `aar_display_vsync` is the
pacing hint. Gate: `AAR_API_COVERS(api, display_wait_vblank)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 6.

### aar_display_last_present_us

```c
uint64_t aar_display_last_present_us(void);
```

The `aar_time_us` value at which the runtime observed its most recent flip or
GL swap acknowledgement. 0 before the first one, and when the slot is absent.
The runtime answers it from its own record and sends no request. Does not block.
Gate: `AAR_API_COVERS(api, display_last_present_us)`. Minimum minor: 6.

### aar_display_present

```c
int aar_display_present(uint32_t id, uint32_t flags, uint64_t *frame_id);
```

Opt in to feedback for one frame. Check `AAR_DISPCAP_PRESENT_FEEDBACK` on
the surface. Pass 0 for a synchronous flip, `AAR_PRESENT_ASYNC` for an
asynchronous flip, or `AAR_PRESENT_GL` for a swap on the bound GL surface.
Keep using the existing refresh and idle rules before writing the next buffer.
`AAR_OK` and a nonzero `*frame_id` mean the frame was accepted. The result may
still be pending. On rejection `*frame_id` is zero; the frame and any pending
frame are unchanged. `AAR_E_BUSY` can mean the feedback queue is full; collect
results and then retry. See [spec section 8.2](../spec/aar.md#82-display-aar_group_display)
for the complete contract.

Gate: `AAR_API_COVERS(api, display_present)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_display_present_poll

```c
int aar_display_present_poll(aar_present_result *out);
```

Returns 1 and consumes a result, or 0 without changing `out` if none is
ready. Does not wait, pump the display backend or issue a client request.
Keep polling after display close to collect results already recorded or
cancelled by close.

Gate: `AAR_API_COVERS(api, display_present_poll)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 6.

### aar_present_result

`frame_id` identifies the submission and `completed_us` uses the `aar_time_us`
clock. `outcome` and `reason` use the constants below.

### AAR_PRESENT_ASYNC

`(1u << 0)`. Submit an asynchronous buffer flip. Requires `AAR_DISPCAP_ASYNC`.

### AAR_PRESENT_GL

`(1u << 1)`. Swap the bound GL surface. Do not combine with `AAR_PRESENT_ASYNC`.

### AAR_PRESENT_ACKNOWLEDGED

`1u`. The backend acknowledged the frame. The time is when the runtime
observed that acknowledgement, not physical scanout.

### AAR_PRESENT_REPLACED

`2u`. A newer frame superseded this LATEST frame before acknowledgement.

### AAR_PRESENT_UNCONFIRMED

`3u`. The runtime could not confirm the frame. Check `reason`.

### AAR_PRESENT_REASON_NONE

`0u`. Used with `AAR_PRESENT_ACKNOWLEDGED` and `AAR_PRESENT_REPLACED`.

### AAR_PRESENT_REASON_TIMEOUT

`1u`. A deadline expired before the runtime saw an acknowledgement.

### AAR_PRESENT_REASON_CANCELLED

`2u`. The display closed, GL unbound, or stop cancelled the pending frame.

### AAR_PRESENT_REASON_BACKEND

`3u`. The presentation backend reported a failure.

### Display formats

`format` on a surface is the logical format the app requested. Pixel stores
follow `layout`.

### AAR_FMT_RGB565

`1u`. Two bytes per pixel. Width and height 1 to `AAR_DISPLAY_MAX_DIM`.

### AAR_FMT_BGRA8888

`2u`. Four bytes per pixel. The only output format `aar_image_open` accepts.

### AAR_FMT_CLUT8

`3u`. One byte per pixel, with a palette of `0x00RRGGBB` entries. Optional: `aar_display_open` returns `AAR_E_NOSYS` unless the display has `AAR_DISPCAP_CLUT8`.

### Display layouts

The physical byte layout of the surface.

### AAR_LAYOUT_RGB565_BE

`1u`. RGB565, high byte first.

### AAR_LAYOUT_RGB565_LE

`2u`. RGB565, low byte first.

### AAR_LAYOUT_BGR565_BE

`3u`. BGR565, high byte first.

### AAR_LAYOUT_BGR565_LE

`4u`. BGR565, low byte first.

### AAR_LAYOUT_BGRA8888

`5u`. Bytes B, G, R, A.

### AAR_LAYOUT_RGBA8888

`6u`. Bytes R, G, B, A.

### AAR_LAYOUT_CLUT8

`7u`. One palette index per byte.

### Display flags and capabilities

### AAR_DISPLAY_VSYNC

`(1u << 0)`. Open flag: a flip waits for scanout acknowledgement.

### AAR_DISPLAY_LATEST

`(1u << 1)`. Open flag: a flip replaces a pending frame instead of waiting. Optional; the granted caps say whether it is in effect.

### AAR_DISPCAP_ASYNC

`(1u << 0)`. `aar_surface.caps`: `aar_display_flip_async` works.

### AAR_DISPCAP_LATEST

`(1u << 1)`. `aar_surface.caps`: `AAR_DISPLAY_LATEST` is in effect.

### AAR_DISPCAP_CLUT8

`(1u << 2)`. `aar_surface.caps`: CLUT8 surfaces and palettes work. The caps are 16 bits, because the transport carries them in the entry flags field.

### AAR_DISPLAY_WINDOW

`(1u << 2)`. Open flag: show the surface in a window on the default public
screen. The client falls back to a screen of its own when it cannot, and then
does not set `AAR_DISPCAP_WINDOW`. Minimum minor: 6.

### AAR_DISPLAY_RESIZABLE

`(1u << 3)`. Open flag: the user may resize the window. Needs
`AAR_DISPLAY_WINDOW`. Minimum minor: 6.

### AAR_DISPLAY_POINTER_ABS

`(1u << 4)`. Open flag: mouse events carry the pointer position. Minimum minor: 6.

### AAR_DISPLAY_TEXT_INPUT

`(1u << 5)`. Open flag: the client sends `AAR_EV_TEXT`. Minimum minor: 6.

### AAR_DISPLAY_CLOSE_REQUEST

`(1u << 6)`. Open flag: the client sends `AAR_EV_CLOSE_REQUEST` when the close
gadget is used. Minimum minor: 6.

### AAR_DISPLAY_SCALE

`(1u << 7)`. Open flag: the client shows the surface scaled to its screen or
window and keeps the aspect ratio. Minimum minor: 6.

### AAR_DISPCAP_WINDOW

`(1u << 3)`. `aar_surface.caps`: the surface is in a window. Minimum minor: 6.

### AAR_DISPCAP_RESIZE

`(1u << 4)`. `aar_surface.caps`: `AAR_EV_RESIZE` is sent and
`aar_display_resize` works. Minimum minor: 6.

### AAR_DISPCAP_POINTER_ABS

`(1u << 5)`. `aar_surface.caps`: mouse events are absolute. Minimum minor: 6.

### AAR_DISPCAP_TEXT_INPUT

`(1u << 6)`. `aar_surface.caps`: `AAR_EV_TEXT` is sent. Minimum minor: 6.

### AAR_DISPCAP_CLOSE_REQUEST

`(1u << 7)`. `aar_surface.caps`: `AAR_EV_CLOSE_REQUEST` is sent. Minimum minor: 6.

### AAR_DISPCAP_SCALED

`(1u << 8)`. `aar_surface.caps`: the client scales the surface. Minimum minor: 6.

### AAR_DISPCAP_PRESENT_FEEDBACK

`(1u << 9)`. The runtime supports tracked presents for this display; no open
flag is needed. `AAR_PRESENT_ASYNC` still requires `AAR_DISPCAP_ASYNC`, and
GL needs a bound context that supports tracked swaps. Minimum minor: 6.

### AAR_DISPINFO_WINDOW

`(1u << 0)`. `aar_display_info.flags`: shown in a window on a public screen.
Minimum minor: 6.

### AAR_DISPINFO_SCALED

`(1u << 1)`. `aar_display_info.flags`: the client scales the surface. Minimum
minor: 6.

### AAR_DISPINFO_ACTIVE

`(1u << 2)`. `aar_display_info.flags`: the window or screen has input focus.
Minimum minor: 6.

### AAR_MODEF_PREFERRED

`(1u << 0)`. `aar_display_mode.flags`: the client's preferred mode, its native
size. Minimum minor: 6.

### AAR_POINTER_DEFAULT

`0u`. `aar_display_pointer` mode: the client's normal pointer. Minimum minor: 6.

### AAR_POINTER_HIDDEN

`1u`. `aar_display_pointer` mode: no pointer. Minimum minor: 6.

### AAR_POINTER_BUSY

`2u`. `aar_display_pointer` mode: the busy pointer. Minimum minor: 6.

### AAR_POINTER_CUSTOM

`3u`. `aar_display_pointer` mode: the image in the `aar_pointer` argument.
Minimum minor: 6.

### AAR_POINTER_MAX_DIM

`64u`. The largest width and height of a custom pointer. Minimum minor: 6.

## Audio group

Requires `AAR_GROUP_AUDIO`. One PCM stream is open per app. The app requests a
rate and a channel count; the runtime grants a supported pair and writes it
into the `aar_audio` that `aar_audio_open` fills. A platform must support 22050
Hz stereo, and may grant 44100 and 48000 Hz. Other combinations are
`AAR_E_INVAL`. The route is the client's decision. A platform with the audio
group must provide the `AHI` route (the client reads the PCM with
`AARAudioRead`); the route `PLATFORM`, the platform's own mixer, is optional,
and a platform without it answers `AAR_E_NOSYS` at `aar_audio_open`. Only a few
streams can exist across all apps, and a stream that cannot be given is
`AAR_E_BUSY`.

### aar_audio_open

```c
int aar_audio_open(uint32_t rate, uint32_t channels, aar_audio *out);
```

Opens the PCM stream. `channels` must be 2 and `rate` one of
`AAR_AUDIO_RATE_22050`, `AAR_AUDIO_RATE_44100` and `AAR_AUDIO_RATE_48000`;
anything else, or a NULL `out`, is `AAR_E_INVAL`. A second open, or no free
stream, is `AAR_E_BUSY`. The ring is shared with its consumer: the runtime, or
on the AHI route the client library. Parks while the platform attaches the
stream; stop cancels the wait with `AAR_E_IO`. Gate:
`AAR_API_COVERS(api, audio_open)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5. **ZZ9000:** two streams exist across all apps.
The current runtime grants only 22050 Hz stereo with 8192 frames, and the ring is
non-cacheable. With the `AUDIO=AHI` aar-run option the PCM goes to AHI, which
must grant that exact stereo rate, and the converter does not resample.

### aar_audio_close

```c
int aar_audio_close(void);
```

Detaches the stream. A stream that is not open is `AAR_E_INVAL`. Parks while
the platform detaches it; stop cancels that wait with `AAR_E_IO`. Gate:
`AAR_API_COVERS(api, audio_close)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_audio_get_stats

```c
int aar_audio_get_stats(aar_audio_stats *out);
```

Optional. Fills the underrun counter of the open stream. The caller sets
`out->size`. NULL `out` is `AAR_E_INVAL`. The consumer of the ring counts one
underrun each time it needed frames and the ring held fewer: once per starved
period on a platform mixer, once per short `AARAudioRead` on the AHI route. The
unit is not a number of frames, and the value equals the ring word. The wrapper
is named `aar_audio_get_stats` for symmetry with
the other info calls. Gate: `AAR_API_COVERS(api, audio_stats)` and a non-NULL
slot. A NULL slot, which the standard allows, is `AAR_E_NOSYS`. Minimum minor:
5.

### AAR_AUDIO_RATE_22050

`22050u`. Mandatory on every platform with the audio group.

### AAR_AUDIO_RATE_44100

`44100u`. A rate a platform may grant.

### AAR_AUDIO_RATE_48000

`48000u`. A rate a platform may grant.

## GL group

Requires `AAR_GROUP_GL`. The GL group gives the app one GL 1.1 context.

### aar_gl_bind

```c
const aar_gl *aar_gl_bind(uint32_t surface_id);
```

Binds the app's one GL context to the open surface and returns the table of GL
1.1 entry points, which the `gl*` wrappers then use. Failure, including a second
bind before `aar_gl_unbind`, returns NULL. Does not block past context setup.
Gate: `AAR_API_COVERS(api, gl_bind)`. Missing slot: NULL. Minimum minor: 5.
**ZZ9000:** the GL implementation is MiniGL on RasterIX. To pass debug bind
flags use `aar_zz9000_gl_bind_flags` from the
[ZZ9000 extension table](#the-zz9000-extension-table). One app at a time
may hold a GL context.

### aar_gl_swap

```c
void aar_gl_swap(void);
```

Flushes the GL front end and passes the frame to the platform to copy and
flip. It parks as `aar_display_flip` does, but it may return before the frame
is visible. The platform keeps at most two swapped frames in flight, the frame
of this swap included. So when a swap returns, every frame from an earlier swap
except the most recent one is visible. An app that needs its last frame visible
without drawing another calls `aar_gl_swap` twice more. A pipelined platform
should also present a queued frame when the app next parks and at
`aar_gl_unbind`. A frame still queued when the app ends may never be shown.
Gate: `AAR_API_COVERS(api,
gl_swap)`. Missing slot: returns. Minimum minor: 5. **ZZ9000:** it drains
RasterIX and copies the context target into the bound P96 bitmap. A GL call that
does not return is subject to the 10 second stuck-call limit; a call longer than
50 ms is logged, not preempted.

### aar_gl_unbind

```c
void aar_gl_unbind(void);
```

Releases the app's GL context. Does not block past unbind. Gate:
`AAR_API_COVERS(api, gl_unbind)`. Missing slot: returns. Minimum minor: 5.

## Services group

Requires `AAR_GROUP_SERVICES`. The calls are synchronous buffer-in, buffer-out
helpers for functions a platform may do faster than the app. All buffers are
app memory that stays valid for the call. Byte counts of whole buffers are
`uint64_t`; per-call chunk sizes are `uint32_t`. Every call may return
`AAR_E_NOSYS` for an algorithm or codec the platform does not provide. At least
`AAR_MIN_DECODE_SESSIONS` sessions of each kind exist per app; a further one is
`AAR_E_NOMEM`. A session that is still open at exit is closed by the runtime.

**ZZ9000 note:** these calls run inline on the app's own core.

**Compressed formats.** `aar_decompress` takes one complete stream in one of
these formats:

- `DEFLATE_RAW` is RFC 1951.
- `ZLIB` is RFC 1950.
- `GZIP` is RFC 1952.
- `LZ4_BLOCK` is one LZ4 block without a frame header.
- `LZMA_ALONE` is the 13-byte `.lzma` header followed by the LZMA stream.
- `LZMA2` is one dictionary-size property byte followed by the raw LZMA2
  chunks.
- `LH1`, `LH5`, `LH6` and `LH7` are the compressed data of an LHA file of that
  method without its header.

Every stream except LHA ends itself. An LHA stream has no end marker, so
`dst_bytes` must be exactly the plain size. The specification, section 8.6, has
the details.

**Image sessions.** In API 0.5 the tile buffer must hold all image pixels and an
app passes 0 in `aar_image_params.flags`. `aar_image_decode` takes `flags` bit 0
to say that the input ends with the call. The image is complete after a call
with that bit that consumed all its input and returned result `flags` bit 1
(more output pending) clear. A call with `src_bytes` 0 is valid only with bit 0
set and drains what the decoder still holds. Result `flags` bit 0 means the
header is known. Bit 2 means the output was scaled (not used in 0.5).

**Lossy decoders** (JPEG, WebP) are not bit exact across platforms: a platform
may use a faster IDCT or a cheaper chroma upsampling than libjpeg and libwebp.
The suite accepts at most 6 per colour channel for JPEG and 3 for WebP on its
test images. PNG decode is exact.

### aar_sha256

```c
int aar_sha256(const void *data, uint64_t bytes, uint8_t digest[32]);
```

Writes the 32-byte SHA-256 digest of the buffer. NULL `data` or `digest` is
`AAR_E_INVAL`. Gate: `AAR_API_COVERS(api, sha256)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 5.

### aar_decompress

```c
int aar_decompress(uint32_t algorithm, uint32_t flags,
                   const void *src, uint64_t src_bytes,
                   void *dst, uint64_t dst_bytes,
                   aar_service_result *out);
```

Decompresses one buffer in one call. `out` is written only on `AAR_OK`. NULL
`src`, `dst` or `out` is `AAR_E_INVAL`; an unknown algorithm is `AAR_E_INVAL` or
`AAR_E_NOSYS`. `flags` is passed through. Algorithm values are `AAR_COMPRESS_*`.
Gate: `AAR_API_COVERS(api, decompress)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### aar_image_open

```c
int aar_image_open(const aar_image_params *params, uint32_t *session,
                   aar_image_result *out);
```

Starts one tile-buffer session and writes `*session`. NULL required pointers, a
zero stride, row count or tile size, or an output format other than
`AAR_FMT_BGRA8888` is `AAR_E_INVAL`. The tile buffer stays caller-owned. In
API 0.5 it must hold all image pixels and the app passes 0 in `params->flags`
(a runtime may accept bits it defines). Gate:
`AAR_API_COVERS(api, image_open)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_image_decode

```c
int aar_image_decode(uint32_t session, const void *src, uint32_t src_bytes,
                     uint32_t flags, aar_image_result *out);
```

Feeds one buffer to an open session. An unknown session, or NULL `src` or `out`,
is `AAR_E_INVAL`. `src` must remain valid for the call. `flags` bit 0 says the
input ends with this call; any other bit is `AAR_E_INVAL`. A call with
`src_bytes` 0 is valid only with bit 0 set. Gate:
`AAR_API_COVERS(api, image_decode)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_image_close

```c
int aar_image_close(uint32_t session);
```

Ends one session and frees its slot. An unknown session is `AAR_E_INVAL`. Gate:
`AAR_API_COVERS(api, image_close)`. Missing slot: `AAR_E_NOSYS`. Minimum minor:
5.

### aar_audio_decode_open

```c
int aar_audio_decode_open(uint32_t codec, uint32_t *session);
```

Opens an MP3, FLAC or Vorbis decode session. Another codec value, or a NULL
`session`, is `AAR_E_INVAL`. Playback (`aar_audio_open`) is separate from decode
sessions. Gate: `AAR_API_COVERS(api, audio_decode_open)`. Missing slot:
`AAR_E_NOSYS`. Minimum minor: 5.

### aar_audio_decode

```c
int aar_audio_decode(uint32_t session, const void *src, uint32_t src_bytes,
                     uint32_t flags, int16_t *pcm, uint32_t pcm_frames,
                     aar_audio_decode_result *out);
```

Consumes the prefix of `src` that it reports and writes interleaved S16LE
frames. An unknown session, NULL `pcm` or `out`, `pcm_frames` of 0, NULL `src`
with a non-zero length, or an unknown flag is `AAR_E_INVAL`. Provide room for
`pcm_frames * 2` samples. MP3 handles one MPEG frame per call, takes no flags,
and needs `pcm_frames` of at least 1152 (`AAR_E_NOSPC` otherwise); a short or
invalid frame is `AAR_E_IO`. FLAC and Vorbis may return `AAR_OK` with
`frames_written` 0 after consuming a header or partial unit: keep the unconsumed
tail and feed it again with later bytes. For FLAC supply room for stereo even if
the stream later proves mono. Vorbis takes the Ogg pages from the start of the
file. Gate: `AAR_API_COVERS(api, audio_decode)`. Missing slot: `AAR_E_NOSYS`.
Minimum minor: 5.

### aar_audio_decode_close

```c
int aar_audio_decode_close(uint32_t session);
```

Releases one decode session. An unknown session is `AAR_E_INVAL`. Gate:
`AAR_API_COVERS(api, audio_decode_close)`. Missing slot: `AAR_E_NOSYS`. Minimum
minor: 5.

### Compression algorithms

`aar_decompress` accepts these values. An unknown value is `AAR_E_INVAL` or
`AAR_E_NOSYS`.

### AAR_COMPRESS_NONE

`0u`. No compression. Never valid: a request for it is a bad algorithm.

### AAR_COMPRESS_DEFLATE_RAW

`1u`. Raw deflate.

### AAR_COMPRESS_ZLIB

`2u`. Zlib wrapper.

### AAR_COMPRESS_GZIP

`3u`. Gzip wrapper.

### AAR_COMPRESS_LZ4_BLOCK

`4u`. One LZ4 block.

### AAR_COMPRESS_LZMA_ALONE

`5u`. LZMA alone.

### AAR_COMPRESS_LZMA2

`6u`. LZMA2.

### AAR_COMPRESS_LH1

`7u`. LHA LH1.

### AAR_COMPRESS_LH5

`8u`. LHA LH5.

### AAR_COMPRESS_LH6

`9u`. LHA LH6.

### AAR_COMPRESS_LH7

`10u`. LHA LH7.

### Image codecs

`aar_image_open` takes `codec`. The only output format is `AAR_FMT_BGRA8888`.

### AAR_IMAGE_JPEG

`1u`. JPEG.

### AAR_IMAGE_PNG

`2u`. PNG.

### AAR_IMAGE_WEBP

`4u`. WebP. The value is 4, not 3.

### Audio decode codecs

PCM out is interleaved S16LE. Rate and channel count in the result stay 0 until
the codec has parsed a header.

### AAR_AUDIO_MP3

`1u`. One MPEG frame per call. `flags` must be 0 and `pcm_frames` at least 1152.

### AAR_AUDIO_FLAC

`2u`. A call may consume a header or partial unit and write no PCM. Supply stereo room.

### AAR_AUDIO_VORBIS

`3u`. The same feed rule as FLAC. Feed the Ogg pages from the start of the file.

### AAR_AUDIO_DECODE_EOF

`(1u << 0)`. This buffer ends the stream. FLAC and Vorbis only; illegal on MP3.

### AAR_AUDIO_DECODE_DRAIN

`(1u << 1)`. Emit buffered PCM without requiring more input. FLAC and Vorbis only; illegal on MP3. Combine with `AAR_AUDIO_DECODE_EOF` only if both are intended.

## The GL group

`include/aar/gl.h` declares the table `aar_gl` and the `gl*` wrappers over it.
The table is an in-process structure: the app and the runtime are built from
the same header for the same profile. It starts with two `uint32_t` words,
`size` and `version`, so the first pointer is at offset 8 on both profiles
with no padding. Every later field is a pointer, so the offset of field `n` is
`8 + n * sizeof(void *)`. `size` is the compatibility version: test every
field past `Build2DMipmaps` with `AAR_GL_COVERS` before use. `version` is the
GL ABI revision the runtime implements.

Integer parameters are `int32_t` or `uint32_t`. `Perspective`, `Ortho`,
`Frustum` and `DepthRange` take `double`, and `gluPerspective` takes four
doubles, not the 68k `GLfloat` signature. There is no stdcall and no GL header
from Khronos or any other vendor. The classic GL types that match are `GLenum`,
`GLbitfield` and `GLuint` as `unsigned int`, `GLint` and `GLsizei` as `int`,
`GLboolean` and `GLubyte` as `unsigned char`, `GLfloat` and `GLclampf` as
`float`, and `GLdouble` and `GLclampd` as `double`. Do not pack the structure.

The context has a depth buffer of at least 16 bits (`glGetIntegerv(GL_DEPTH_BITS)`
returns 16 or more), because no call selects a pixel format. `GL_DEPTH_TEST`,
`glDepthFunc`, `glDepthMask` and `glDepthRange` work as in GL 1.1. A platform that
cannot provide such a buffer does not set `AAR_GROUP_GL`.

An isolated runtime validates client-array pointers at draw time. An invalid
range prevents the draw and makes the next `glGetError` return
`GL_INVALID_VALUE`; it does not fault or stop the app. The `gl*` entries return
nothing the app could use, so `glGetError` is the only signal (`AAR_E_FAULT` is
the runtime's own status for the failed call). `DrawArrays` checks each
requested element; `DrawElements` checks the index buffer and every referenced
element.

The `gl*` and `glu*` wrappers forward to the table that `aar_gl_bind` returned.
Do not define them in the app. A wrapper whose field is past `gl->size`, or is
NULL, is a no-op, or returns 0. Each GL entry point below follows the GL 1.1
meaning of the name; only what differs is noted. Minimum minor: 5 for the
entries up to `glUnlockArrays`, and 6 for the 51 entries after them. The ten
from `glVertexPointer` through `glUnlockArrays` are `AAR_GL_VERSION_ARRAYS`, the
51 after them are `AAR_GL_VERSION_11`, and both need the `AAR_GL_COVERS` test. A
platform whose renderer lacks a feature leaves its field NULL.

The stencil buffer is not promised. `glGetIntegerv(GL_STENCIL_BITS)` reports its
depth, 0 for none, and without one the stencil calls have no effect. The group
stays a GL 1.1 subset through the 0.x versions.

### AAR_GL_VERSION_V0

`0u`. `aar_gl.version`: the fields through `Build2DMipmaps`.

### AAR_GL_VERSION_ARRAYS

`1u`. `aar_gl.version`: adds the array and client-state fields, `VertexPointer` through `UnlockArrays`.

### AAR_GL_VERSION_11

`2u`. `aar_gl.version`: adds the GL 1.1 completion fields, `ClearDepth` through
`PopAttrib`. Minimum minor: 6.

### AAR_GL_SLOT_COUNT

`123u`. The number of pointer fields in `aar_gl`. The layout test checks it.

### aar_gl

```c
typedef struct aar_gl {
    uint32_t size;
    uint32_t version;
    /* 123 function pointers: Begin ... PopAttrib */
} aar_gl;
```

The field names are the GL names without the `gl` prefix: `Begin`, `End`,
`Vertex2f`, and so on to `PopAttrib`; `gluPerspective` is the field
`Perspective` and `gluBuild2DMipmaps` is `Build2DMipmaps`. The full declaration
is in `include/aar/gl.h`.

### glBegin

```c
void glBegin(uint32_t mode);
```

Starts a primitive; `mode` is a GL primitive enum.

### glEnd

```c
void glEnd(void);
```

Ends the primitive started by `glBegin`.

### glVertex2f

```c
void glVertex2f(float x, float y);
```

Specifies a vertex from two floats.

### glVertex3f

```c
void glVertex3f(float x, float y, float z);
```

Specifies a vertex from three floats.

### glVertex3fv

```c
void glVertex3fv(const float *v);
```

Specifies a vertex from an array of three floats.

### glTexCoord2f

```c
void glTexCoord2f(float s, float t);
```

Sets the current texture coordinate.

### glTexCoord2fv

```c
void glTexCoord2fv(const float *v);
```

Sets the current texture coordinate from an array of two floats.

### glColor3f

```c
void glColor3f(float r, float g, float b);
```

Sets the current color with alpha 1.

### glColor3fv

```c
void glColor3fv(const float *v);
```

Sets the current color from an array of three floats.

### glColor3ubv

```c
void glColor3ubv(const uint8_t *v);
```

Sets the current color from an array of three bytes.

### glColor4f

```c
void glColor4f(float r, float g, float b, float a);
```

Sets the current color from four floats.

### glColor4fv

```c
void glColor4fv(const float *v);
```

Sets the current color from an array of four floats.

### glColor4ub

```c
void glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
```

Sets the current color from four bytes.

### glColor4ubv

```c
void glColor4ubv(const uint8_t *v);
```

Sets the current color from an array of four bytes.

### glEnable

```c
void glEnable(uint32_t cap);
```

Enables a server-side capability.

### glDisable

```c
void glDisable(uint32_t cap);
```

Disables a server-side capability.

### glIsEnabled

```c
uint8_t glIsEnabled(uint32_t cap);
```

Returns whether a capability is enabled. A missing slot returns 0.

### glAlphaFunc

```c
void glAlphaFunc(uint32_t func, float ref);
```

Sets the alpha test function and reference value.

### glBlendFunc

```c
void glBlendFunc(uint32_t sfactor, uint32_t dfactor);
```

Sets the source and destination blend factors.

### glDepthFunc

```c
void glDepthFunc(uint32_t func);
```

Sets the depth comparison function.

### glDepthMask

```c
void glDepthMask(uint8_t flag);
```

Enables or disables depth buffer writes. Takes a `uint8_t`.

### glDepthRange

```c
void glDepthRange(double n, double f);
```

Sets the depth range mapping. Takes two `double`.

### glCullFace

```c
void glCullFace(uint32_t mode);
```

Selects which faces are culled.

### glFrontFace

```c
void glFrontFace(uint32_t mode);
```

Selects the front-face winding.

### glShadeModel

```c
void glShadeModel(uint32_t mode);
```

Selects flat or smooth shading.

### glPolygonMode

```c
void glPolygonMode(uint32_t face, uint32_t mode);
```

Selects how polygons are rasterized for a face.

### glClear

```c
void glClear(uint32_t mask);
```

Clears the buffers named by the mask.

### glClearColor

```c
void glClearColor(float r, float g, float b, float a);
```

Sets the color the color buffer is cleared to.

### glViewport

```c
void glViewport(int32_t x, int32_t y, int32_t width, int32_t height);
```

Sets the viewport rectangle.

### glScissor

```c
void glScissor(int32_t x, int32_t y, int32_t width, int32_t height);
```

Sets the scissor rectangle.

### glMatrixMode

```c
void glMatrixMode(uint32_t mode);
```

Selects the current matrix stack.

### glLoadIdentity

```c
void glLoadIdentity(void);
```

Replaces the current matrix with the identity.

### glLoadMatrixf

```c
void glLoadMatrixf(const float *m);
```

Replaces the current matrix with 16 floats, column-major.

### glPushMatrix

```c
void glPushMatrix(void);
```

Pushes the current matrix stack.

### glPopMatrix

```c
void glPopMatrix(void);
```

Pops the current matrix stack.

### glRotatef

```c
void glRotatef(float angle, float x, float y, float z);
```

Multiplies the current matrix by a rotation. The angle is in degrees.

### glTranslatef

```c
void glTranslatef(float x, float y, float z);
```

Multiplies the current matrix by a translation.

### glScalef

```c
void glScalef(float x, float y, float z);
```

Multiplies the current matrix by a scale.

### glOrtho

```c
void glOrtho(double left, double right, double bottom, double top,
    double zNear, double zFar);
```

Multiplies the current matrix by an orthographic projection. Takes six `double`.

### glFrustum

```c
void glFrustum(double left, double right, double bottom, double top,
    double zNear, double zFar);
```

Multiplies the current matrix by a perspective frustum. Takes six `double`.

### glTexImage2D

```c
void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat,
    int32_t width, int32_t height, int32_t border,
    uint32_t format, uint32_t type, const void *pixels);
```

Specifies a two-dimensional texture image. `pixels` is read during the call.

### glTexSubImage2D

```c
void glTexSubImage2D(uint32_t target, int32_t level,
    int32_t xoffset, int32_t yoffset,
    int32_t width, int32_t height,
    uint32_t format, uint32_t type, const void *pixels);
```

Replaces part of a two-dimensional texture image.

### glTexParameteri

```c
void glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
```

Sets an integer texture parameter.

### glTexParameterf

```c
void glTexParameterf(uint32_t target, uint32_t pname, float param);
```

Sets a float texture parameter.

### glTexEnvi

```c
void glTexEnvi(uint32_t target, uint32_t pname, int32_t param);
```

Sets an integer texture environment parameter.

### glTexEnvf

```c
void glTexEnvf(uint32_t target, uint32_t pname, float param);
```

Sets a float texture environment parameter.

### glGenTextures

```c
void glGenTextures(int32_t n, uint32_t *textures);
```

Generates `n` texture names.

### glDeleteTextures

```c
void glDeleteTextures(int32_t n, const uint32_t *textures);
```

Deletes `n` texture names.

### glBindTexture

```c
void glBindTexture(uint32_t target, uint32_t texture);
```

Binds a texture name to a target.

### glFinish

```c
void glFinish(void);
```

Blocks until all issued GL commands have completed.

### glFlush

```c
void glFlush(void);
```

Forces issued GL commands to start executing.

### glDrawBuffer

```c
void glDrawBuffer(uint32_t mode);
```

Selects the buffer that drawing goes to.

### glReadBuffer

```c
void glReadBuffer(uint32_t mode);
```

Selects the buffer that pixel reads come from.

### glReadPixels

```c
void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height,
    uint32_t format, uint32_t type, void *pixels);
```

Reads a block of pixels from the read buffer into `pixels`.

### glGetString

```c
const uint8_t *glGetString(uint32_t name);
```

Returns a NUL-terminated string such as the renderer or version. A missing slot returns NULL.

### glGetIntegerv

```c
void glGetIntegerv(uint32_t pname, int32_t *params);
```

Reads integer state.

### glGetFloatv

```c
void glGetFloatv(uint32_t pname, float *params);
```

Reads float state.

### glGetError

```c
uint32_t glGetError(void);
```

Returns and clears the error flag. A missing slot returns 0, which is `GL_NO_ERROR`.

### glHint

```c
void glHint(uint32_t target, uint32_t mode);
```

Sets an implementation hint.

### glPixelStorei

```c
void glPixelStorei(uint32_t pname, int32_t param);
```

Sets a pixel storage mode.

### gluPerspective

```c
void gluPerspective(double fovy, double aspect, double zNear, double zFar);
```

Multiplies the current matrix by a perspective projection. Takes four `double`.

### gluBuild2DMipmaps

```c
int32_t gluBuild2DMipmaps(uint32_t target, int32_t internalFormat,
    int32_t width, int32_t height,
    uint32_t format, uint32_t type, const void *data);
```

Builds a mipmap chain and uploads it. Returns 0 on success; a missing slot returns 0.

### glVertexPointer

```c
void glVertexPointer(int32_t size, uint32_t type, int32_t stride,
    const void *pointer);
```

Sets the vertex array. Array version of GL ABI revision `AAR_GL_VERSION_ARRAYS`.

### glTexCoordPointer

```c
void glTexCoordPointer(int32_t size, uint32_t type, int32_t stride,
    const void *pointer);
```

Sets the texture coordinate array.

### glColorPointer

```c
void glColorPointer(int32_t size, uint32_t type, int32_t stride,
    const void *pointer);
```

Sets the color array.

### glEnableClientState

```c
void glEnableClientState(uint32_t cap);
```

Enables a client-side array.

### glDisableClientState

```c
void glDisableClientState(uint32_t cap);
```

Disables a client-side array.

### glClientActiveTextureARB

```c
void glClientActiveTextureARB(uint32_t texture);
```

Selects the client texture unit for the texture coordinate array.

### glDrawArrays

```c
void glDrawArrays(uint32_t mode, int32_t first, int32_t count);
```

Draws primitives from the enabled arrays. Every requested element is range-checked on an isolated runtime.

### glDrawElements

```c
void glDrawElements(uint32_t mode, int32_t count, uint32_t type,
    const void *indices);
```

Draws indexed primitives from the enabled arrays. The index buffer and every referenced element are range-checked on an isolated runtime.

### glLockArrays

```c
void glLockArrays(uint32_t first, int32_t count);
```

Locks a range of the enabled arrays for reuse (compiled vertex arrays).

### glUnlockArrays

```c
void glUnlockArrays(void);
```

Unlocks the arrays locked by `glLockArrays`.

### glClearDepth

```c
void glClearDepth(double depth);
```

Sets the depth clear value. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glColorMask

```c
void glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
```

Enables or disables writing of each colour channel. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glPolygonOffset

```c
void glPolygonOffset(float factor, float units);
```

Sets the depth offset of filled polygons. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glStencilFunc

```c
void glStencilFunc(uint32_t func, int32_t ref, uint32_t mask);
```

Sets the stencil test. No effect without a stencil buffer. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glStencilOp

```c
void glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass);
```

Sets the stencil update operations. No effect without a stencil buffer.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glStencilMask

```c
void glStencilMask(uint32_t mask);
```

Sets the stencil write mask. No effect without a stencil buffer.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glClearStencil

```c
void glClearStencil(int32_t s);
```

Sets the stencil clear value. No effect without a stencil buffer.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glMultMatrixf

```c
void glMultMatrixf(const float *m);
```

Multiplies the current matrix by the 16 floats at `m`, column-major.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glLoadMatrixd

```c
void glLoadMatrixd(const double *m);
```

Loads the 16 doubles at `m`, column-major, as the current matrix.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glMultMatrixd

```c
void glMultMatrixd(const double *m);
```

Multiplies the current matrix by the 16 doubles at `m`, column-major.
`AAR_GL_VERSION_11`. Minimum minor: 6.

### glGetDoublev

```c
void glGetDoublev(uint32_t pname, double *params);
```

Reads a state value as doubles. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glGetBooleanv

```c
void glGetBooleanv(uint32_t pname, uint8_t *params);
```

Reads a state value as booleans. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glActiveTextureARB

```c
void glActiveTextureARB(uint32_t texture);
```

Selects the texture unit that the texture calls address; the client-side
counterpart is `glClientActiveTextureARB`. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glMultiTexCoord2fARB

```c
void glMultiTexCoord2fARB(uint32_t target, float s, float t);
```

Sets the texture coordinate of one texture unit. `AAR_GL_VERSION_11`. Minimum
minor: 6.

### glMultiTexCoord2fvARB

```c
void glMultiTexCoord2fvARB(uint32_t target, const float *v);
```

The vector form of `glMultiTexCoord2fARB`. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glNormal3f

```c
void glNormal3f(float nx, float ny, float nz);
```

Sets the current normal. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glNormal3fv

```c
void glNormal3fv(const float *v);
```

The vector form of `glNormal3f`. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glNormalPointer

```c
void glNormalPointer(uint32_t type, int32_t stride, const void *pointer);
```

Sets the normal array. The runtime validates the range at draw time like the
other array pointers. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glLightf

```c
void glLightf(uint32_t light, uint32_t pname, float param);
```

Sets a light parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glLightfv

```c
void glLightfv(uint32_t light, uint32_t pname, const float *params);
```

Sets a light parameter from a vector. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glLightModelf

```c
void glLightModelf(uint32_t pname, float param);
```

Sets a lighting model parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glLightModelfv

```c
void glLightModelfv(uint32_t pname, const float *params);
```

Sets a lighting model parameter from a vector. `AAR_GL_VERSION_11`. Minimum
minor: 6.

### glLightModeli

```c
void glLightModeli(uint32_t pname, int32_t param);
```

Sets an integer lighting model parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glMaterialf

```c
void glMaterialf(uint32_t face, uint32_t pname, float param);
```

Sets a material parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glMaterialfv

```c
void glMaterialfv(uint32_t face, uint32_t pname, const float *params);
```

Sets a material parameter from a vector. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glColorMaterial

```c
void glColorMaterial(uint32_t face, uint32_t mode);
```

Makes a material parameter follow the current colour. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glFogf

```c
void glFogf(uint32_t pname, float param);
```

Sets a fog parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glFogfv

```c
void glFogfv(uint32_t pname, const float *params);
```

Sets a fog parameter from a vector. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glFogi

```c
void glFogi(uint32_t pname, int32_t param);
```

Sets an integer fog parameter. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glLineWidth

```c
void glLineWidth(float width);
```

Sets the width of lines. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glPointSize

```c
void glPointSize(float size);
```

Sets the size of points. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glColor3ub

```c
void glColor3ub(uint8_t r, uint8_t g, uint8_t b);
```

Sets the current colour from three bytes. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glVertex2i

```c
void glVertex2i(int32_t x, int32_t y);
```

Specifies a vertex from two integers. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glVertex2fv

```c
void glVertex2fv(const float *v);
```

Specifies a vertex from a two-float vector. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glTexCoord2i

```c
void glTexCoord2i(int32_t s, int32_t t);
```

Sets the texture coordinate from two integers. `AAR_GL_VERSION_11`. Minimum
minor: 6.

### glRectf

```c
void glRectf(float x1, float y1, float x2, float y2);
```

Draws a rectangle. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glGetTexLevelParameteriv

```c
void glGetTexLevelParameteriv(uint32_t target, int32_t level, uint32_t pname,
                              int32_t *params);
```

Reads a parameter of a texture level. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glIsTexture

```c
uint8_t glIsTexture(uint32_t texture);
```

Tells whether `texture` names a texture. Returns 0 when the field is absent or
NULL. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glCopyTexImage2D

```c
void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat,
                      int32_t x, int32_t y, int32_t width, int32_t height,
                      int32_t border);
```

Defines a texture from the framebuffer. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glCopyTexSubImage2D

```c
void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset,
                         int32_t yoffset, int32_t x, int32_t y, int32_t width,
                         int32_t height);
```

Replaces part of a texture from the framebuffer. `AAR_GL_VERSION_11`. Minimum
minor: 6.

### glClipPlane

```c
void glClipPlane(uint32_t plane, const double *equation);
```

Sets a clip plane from the four doubles at `equation`. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glGenLists

```c
uint32_t glGenLists(int32_t range);
```

Reserves `range` display list names and returns the first, or 0 when the field
is absent or NULL. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glNewList

```c
void glNewList(uint32_t list, uint32_t mode);
```

Starts recording a display list. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glEndList

```c
void glEndList(void);
```

Ends the display list being recorded. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glCallList

```c
void glCallList(uint32_t list);
```

Runs a display list. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glCallLists

```c
void glCallLists(int32_t n, uint32_t type, const void *lists);
```

Runs `n` display lists named by the array at `lists`. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glDeleteLists

```c
void glDeleteLists(uint32_t list, int32_t range);
```

Deletes `range` display lists starting at `list`. `AAR_GL_VERSION_11`. Minimum
minor: 6.

### glListBase

```c
void glListBase(uint32_t base);
```

Sets the offset that `glCallLists` adds to each name. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glTexEnvfv

```c
void glTexEnvfv(uint32_t target, uint32_t pname, const float *params);
```

Sets a texture environment parameter from a vector. `AAR_GL_VERSION_11`.
Minimum minor: 6.

### glPushAttrib

```c
void glPushAttrib(uint32_t mask);
```

Pushes the attribute groups in `mask`. `AAR_GL_VERSION_11`. Minimum minor: 6.

### glPopAttrib

```c
void glPopAttrib(void);
```

Pops the attribute groups that `glPushAttrib` pushed. `AAR_GL_VERSION_11`.
Minimum minor: 6.

## The ZZ9000 extension table

`aar/ext_zz9000.h` holds what is specific to the ZZ9000 and that the standard
does not define: the GL bind flags (through `aar_zz9000_gl_bind_flags`) and two
source revision strings. Only the ZZ9000 runtime provides it, and an app that
uses it is not portable. Get it with:

```c
const aar_ext_zz9000 *z = (const aar_ext_zz9000 *)
    aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN);
```

The result is NULL on another platform or an older runtime. The table is
size-first and append-only: two `uint32_t` words come first, so the first
pointer is at offset 8 on both profiles. Test fields with
`AAR_EXT_ZZ9000_COVERS`.

### AAR_EXT_ZZ9000_MAIN

`1u`. The second argument of `aar_ext` that returns the ZZ9000 extension table.

### AAR_ZZ9000_GL_BIND_DIRECT_PRESENT

`1u`. Request direct present of the GL target when the runtime supports it. Specific to the ZZ9000 renderer.

### AAR_ZZ9000_GL_BIND_SYNC_PRESENT

`2u`. Debug: drain and copy every frame before `aar_gl_swap` returns.

### AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED

`4u`. Debug: keep the RasterIX command feed synchronous for A/B comparison.

### AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED

`8u`. Debug: keep the asynchronous RasterIX ring but push command words inline from the calling path.

### aar_ext_zz9000

```c
typedef struct aar_ext_zz9000 {
    uint32_t size;
    uint32_t reserved;
    const aar_gl *(*gl_bind_flags)(uint32_t surface_id, uint32_t flags);
    const char *build_id;
    const char *gl_build_id;
} aar_ext_zz9000;
```

| Field | Meaning |
|---|---|
| `size` | Size of the table the runtime built. |
| `reserved` | 0. |
| `gl_bind_flags` | The same as `aar_gl_bind` plus `AAR_ZZ9000_GL_BIND_*` flags. NULL when the GL group is absent. Pass 0 for no flags. Call it through `aar_zz9000_gl_bind_flags`, which also passes the table to the `gl*` wrappers. |
| `build_id` | The resident firmware source revision, NUL-terminated, or NULL. |
| `gl_build_id` | The GL module source revision, NUL-terminated, or NULL. |

### aar_zz9000_gl_bind_flags

```c
const aar_gl *aar_zz9000_gl_bind_flags(uint32_t surface_id, uint32_t flags);
```

Binds the app's GL context with `AAR_ZZ9000_GL_BIND_*` flags, and is the call to
use instead of reaching for the `gl_bind_flags` slot yourself. It looks up the
extension table with `aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)`,
checks with `AAR_EXT_ZZ9000_COVERS` that the table has `gl_bind_flags` and that
it is not NULL, calls it, and records the returned table where `aar_gl_bind`
records its own. The `gl*` wrappers forward to that table until
`aar_gl_unbind`, as after `aar_gl_bind`. Calling the slot directly returns a
table too, but libaar does not record it, so the `gl*` wrappers would do
nothing. Returns NULL, and records nothing, when the extension table, the slot
or the GL group is absent, or the bind fails. Does not block past context
setup. Pass 0 for no flags. On any other platform the extension table is
absent and the call returns NULL.
