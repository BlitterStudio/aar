<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# AAR: Amiga ARM Runtime, API 0.5 (review draft 1)

Status: review draft 1, the first public draft. This is the specification of AAR, the standard that lets an AmigaOS program start a native ARM program on hardware next to or under the Amiga. It is published so that the people who write platforms (PiStorm with Emu68, a Raspberry Pi CM5 on a Zorro III card, other ARM add-ons) and the authors of apps and launchers can read it and answer the questions in section 13 before the API is frozen. The ZZ9000 is the one existing implementation. Please send comments, corrections and answers by opening an issue in [BlitterStudio/aar](https://github.com/BlitterStudio/aar/issues).

The headers pin every layout, constant, slot order and transport offset: [`include/aar/`](../include/aar) for the app side and the transport, and [`client/include/libraries/`](../client/include/libraries) for the Amiga side. If this text and a header disagree on one of those, one of them has a bug; please report it. The call reference is [`docs/aar-api.md`](../docs/aar-api.md) (app side) and [`docs/aar-client-api.md`](../docs/aar-client-api.md) (`aar.library`). The transport is explained in [`docs/transport.md`](../docs/transport.md), and [`docs/porting.md`](../docs/porting.md) is the guide for a platform implementer. The conformance suite is in [`conformance/`](../conformance). Pre-1.0 the API is `0.x`: any minor may break (3.6).

Normative words: MUST, MUST NOT, SHOULD and MAY are used in the usual way. A statement of behaviour in sections 2 to 11 that has no keyword, for example "`aar_time_us` returns monotonic microseconds", is a requirement as if it said MUST. Text that explains, gives an example or says what one platform does is non-normative. It is introduced by "Non-normative", "Note" or "For example", or it is a parenthesis that names an implementation (for example "the ZZ9000: ...").

## 1. Scope and terms

### 1.1 What the standard covers

AAR defines how an AmigaOS program starts a native ARM program, called an *app*, on hardware next to or under the Amiga, how the two exchange data, and what the app can ask the Amiga side to do for it. It defines:

1. the file an app is shipped as (ELF profile, note);
2. the interface the app sees (entry contract and call table);
3. the memory and visibility rules for buffers shared with the Amiga;
4. the lifecycle, limits and failure behaviour;
5. the Amiga-side library that starts and watches apps;
6. conformance tests.

It does not define a sandbox against hostile apps (the user chooses what to run), threads inside an app, sockets, dynamic linking or shared libraries for apps, a GUI toolkit, or how a platform gets its ARM code running.

### 1.2 Terms

| Term | Meaning |
|---|---|
| host | The side that serves the app: AmigaOS (exec, dos.library, Intuition, P96 or CGX, AHI, input.device) on every platform, always a 68k-API AmigaOS in this version. The host runner is a PC program that stands in for it in tests; "host" in "host runner", "host tests" and "64-bit hosts" means that development PC. |
| platform | The hardware and firmware that run ARM code for the host: ZZ9000 (Zynq, Zorro III), PiStorm with Emu68, a Zorro III card with a Raspberry Pi CM5, others. |
| runtime | The platform's ARM-side code that loads apps, enforces limits and implements the call table. |
| app | One ELF image in one of the profiles, running under the runtime, in user mode unless the platform documents otherwise (4.4). |
| client | An AmigaOS task that owns one or more apps through `aar.library`, one handle per app. `aar-run` is a client. |
| handle | The client's reference to one app instance (`AARHandle`). |
| table | The `aar_api` structure the runtime passes to the app. It is the only entry into the runtime. |
| group | A set of table calls that a platform may or may not provide. |
| shared buffer | App-side memory that the host can reach, and its host address token. |
| host address token (`haddr`) | An opaque 32-bit value that names a shared buffer for the client library. It is not a pointer and not an offset the app may interpret. |
| generation | A counter that makes a handle's claim unique over time. |

### 1.3 Byte order and structure rules

- App-side structures (the table, `aar_event`, `aar_info` and so on) use the native byte order and C layout of the profile. Both profiles are little-endian. The table is never a wire image: both sides MUST be built from the same header for the same profile. Where a structure carries pointers they come last, at an offset that is a multiple of 8, so every fixed-width field has the same offset on A32 and A64.
- Client-side structures (`AARInfo`, `AARStatusInfo`, `AAREvent`, ...) use the AmigaOS m68k layout: big-endian, `LONG`/`ULONG` fields, 4-byte multiples, pointers as 32-bit `APTR`. Every such structure begins with a `size` field and only grows at the end.
- Shared buffers are plain bytes. The runtime and the library never swap bytes. Multi-byte values that the app and the client exchange through a shared buffer or a message are defined by their protocol. The app header provides `aar_bswap16/32/64` helpers. The reference tests store their shared counters big-endian so the 68k reads them as native longs.
- Any structure that is read or written by the host or stored in a shared buffer MUST use fixed-width integers, explicit padding and no pointers. In-process structures (app and runtime in the same address space) MAY contain pointers.
- The control block and queue entries of the transport (section 10) are big-endian words, like the ZZ9000 mailbox payloads and window words, so the 68k reads them as native longs; the ARM side uses the byte-reversing accessors `aar_get_be16/32/64` and `aar_put_be16/32/64`. A 64-bit value is two words, high word first. Message bytes inside an entry are opaque and never swapped. The audio ring buffer is the one exception: it is little-endian, because the app, the runtime and the S16LE samples are (8.4).
- Sizes that can exceed 4 GiB are `uint64_t`. Per-call transfer lengths are `uint32_t` and are bounded by the platform (see `AARInfo`).

## 2. Profiles and what an app file declares

### 2.1 Profiles

| | `AAR_PROFILE_A32` (1) | `AAR_PROFILE_A64` (2) |
|---|---|---|
| ISA | ARMv7-A, A32 instruction state only (the entry point and all code are A32; Thumb-2 interworking is not required) | ARMv8-A AArch64 |
| Data model | ILP32 | LP64 |
| Float | hard-float AAPCS, VFPv3-D32 plus NEON (the Cortex-A9 `-mfpu=neon` set). VFPv4, FMA and half precision MAY be present and MUST be discovered, not assumed | AAPCS64, FP and ASIMD (mandatory in the architecture). No crypto, CRC32, LSE, dot-product or SVE assumed |
| Byte order | little-endian | little-endian |
| Typical cores | Cortex-A9 (ZZ9000). Another core only if the platform runs AArch32 at EL0 (a question in section 13) | Cortex-A72 (Pi 4, CM4), Cortex-A76 (Pi 5, CM5) |
| Toolchain | `arm-none-eabi` or Linux `arm-linux-gnueabihf` with PIC libc | `aarch64-none-elf` or `aarch64-linux-gnu` with PIC libc |

The A64 baseline assumes no optional extension: crypto and CRC32 are optional in ARMv8-A, and LSE (ARMv8.1) is absent from a Cortex-A72 although a Cortex-A76 has it. Optional instructions are discovered through `aar_info.cpu_features` (section 8.7), never by executing and trapping.

A platform implements one or more profiles. It declares them in `AARInfo.profiles`. An app image belongs to exactly one profile. An app that wants to run everywhere ships one image per profile, named `name.a64.elf` and `name.a32.elf`; `AARProbeFile` (section 9) lets a launcher pick. Given `name.elf` or `name`, `aar-run` uses that file when the platform runs it, and otherwise the first of `name.a64.elf` and `name.a32.elf` (and, for `name.elf`, `name`) that the probe accepts and the platform runs, taking the profiles the platform declares first and A64 before A32. Fat images are not defined (section 13 asks whether they should be).

### 2.2 ELF rules (both profiles)

The runtime MUST refuse an image unless all of these hold. The code is `AAR_E_INVAL`, with two exceptions: a note whose profile word differs from the ELF class gives `AAR_E_PROFILE`, and an image of a profile the platform does not run gives `AAR_E_PROFILE` or `AAR_E_INVAL` (a runtime that parses only its own ELF class cannot tell such an image from a corrupt file).

1. `e_type == ET_DYN`. The image is a position-independent executable linked at virtual address 0 and relocated by the loader.
2. `ELFCLASS`, `ELFDATA2LSB`, `EM_*` and `e_flags` match the profile:

   | | A32 | A64 |
   |---|---|---|
   | `EI_CLASS` | `ELFCLASS32` | `ELFCLASS64` |
   | `EI_DATA` | `ELFDATA2LSB` | `ELFDATA2LSB` |
   | `e_machine` | `EM_ARM` (40) | `EM_AARCH64` (183) |
   | `e_flags` | EABI version 5, `EF_ARM_ABI_FLOAT_HARD` set | 0 |

3. No `PT_INTERP`. No `DT_NEEDED`. At most 4 `PT_LOAD`. No segment is both writable and executable. `p_memsz >= p_filesz`. Segments do not overlap and fit inside the memory the runtime offers.
4. Relocations come from the `PT_DYNAMIC` segment, not from section headers: `DT_REL`/`DT_RELSZ`/`DT_RELENT` or `DT_RELA`/`DT_RELASZ`/`DT_RELAENT` (A32), `DT_RELA`/`DT_RELASZ`/`DT_RELAENT` (A64). The image has exactly one `PT_DYNAMIC`, even when it has no relocations. Section headers MAY be absent.
5. Only these relocation types are allowed, with symbol index 0:

   | | A32 | A64 |
   |---|---|---|
   | Table type | `REL` (8-byte entries, addend in the target word) or `RELA` (12-byte entries) | `RELA` (24-byte entries) |
   | Type | `R_ARM_RELATIVE` (23) | `R_AARCH64_RELATIVE` (1027) |
   | Target alignment | 4 | 8 |

   `DT_RELR` and every other type are refused in v1. The target of every relocation (`r_offset`) lies inside the memory of a `PT_LOAD` segment (from `p_vaddr` up to `p_vaddr + p_memsz`), with room for the target word.
6. The entry point `e_entry` is inside an executable segment. For A32 its low bit is 0.
7. Exactly one AAR note is present (2.3), in a `PT_NOTE` segment.

Non-normative: stock GNU ld `-pie --no-dynamic-linker` output and the linker script `libaar/a32/aar.ld` (with `.rel.dyn`, `.dynamic`, the note and a `/DISCARD/` of unwind tables) satisfy these rules for A32; `libaar/a64/aar.ld` does the same for A64 with `.rela.dyn`. The portable C loader in [`loader/`](../loader) (MIT) implements these rules and 2.3 for both profiles: `aar_load_plan` checks an image and plans its segments, and `aar_load_place` copies and relocates it. A runtime may use it instead of writing its own; [`docs/porting.md`](../docs/porting.md) describes how.

### 2.3 The AAR note

An ELF note with owner name `AAR` (namesz 4 including the NUL), `n_type` 1. Name and descriptor are each padded to 4 bytes in both ELF classes. The descriptor is 40 bytes, ten little-endian `uint32_t`:

| Word | Field | Meaning |
|---|---|---|
| 0 | `magic` | `0x4E524141` (bytes `AARN`) |
| 1 | `note_version` | 1 |
| 2 | `profile` | `AAR_PROFILE_*` |
| 3 | `api_major` | major the app was built against |
| 4 | `api_minor_required` | lowest minor that has every call the app needs |
| 5 | `stack_bytes` | 0 for the default (1 MiB). Otherwise 64 KiB up to the platform maximum (at least 1 MiB), multiple of 16 |
| 6 | `min_heap_mib` | private heap committed at load, MiB |
| 7 | `groups_required` | `AAR_GROUP_*` bits. The runtime MUST refuse the load with `AAR_E_NOGROUP` if it lacks one |
| 8 | `groups_optional` | hint, for example for a launcher that wants to open a screen before start |
| 9 | `reserved` | 0 |

The heap is always dynamic. The note holds a minimum and does not fix a region.

The runtime MUST also refuse a note whose `api_major` differs from its own or whose `api_minor_required` is higher than its own minor, with `AAR_E_INVAL`. A note whose profile word differs from the ELF class is refused with `AAR_E_PROFILE`. The macros in [`aar/note.h`](../include/aar/note.h) emit the note from C, so third-party builds need no assembler file. `AAR_NOTE(stack_bytes, min_heap_mib, groups_required, groups_optional)` declares the minor of the header, and `AAR_NOTE_EX` takes an explicit `api_minor_required`. The note layout has its own version (`AAR_NOTE_VERSION`), which is not the API minor.

### 2.4 Check tool

`tools/aar-check [--profile a32|a64] <elf>...` (Python, MIT, in aar) applies sections 2.2 and 2.3, including that the float ABI matches the profile (`EF_ARM_ABI_FLOAT_HARD` for A32). The profile is inferred from the ELF class when omitted. `--expect-note <elf> STACK_BYTES MIN_HEAP_MIB GROUPS_REQUIRED GROUPS_OPTIONAL` compares the note words, and `--no-libc <map>` verifies a freestanding link (section 3.8). The exit code is 0 for a pass, 1 for a failed check and 2 for a usage error. `aar-check` also requires the section headers and compares the relocation and note sections with the segments. This is a check of build output. A runtime does not refuse an image for these.

## 3. App ABI

### 3.1 Calling convention

Table entries use the platform C ABI of the profile (AAPCS hard-float, AAPCS64). The runtime MAY implement an entry as a plain function or as a trap veneer (the ZZ9000 uses user-mode SVC veneers). Apps MUST NOT cache, compare or call any address except through the table, and MUST NOT assume the table memory is writable. No table call takes variable arguments. No call is reentrant: an app has one thread.

### 3.2 Entry contract

The runtime starts the app by entering `e_entry` with this state. Nothing else is guaranteed except that all other general and vector registers are 0.

| | A32 | A64 |
|---|---|---|
| Table pointer | `r0` | `x0` |
| `argc` (int) | `r1` | `w1` |
| `argv` (`char **`) | `r2` | `x2` |
| Stack pointer | top of the app stack, 8-byte aligned (`sp` points one past the last usable byte) | top of the app stack, 16-byte aligned |
| Link register | 0 | `x30 = 0`, `x29 = 0` |
| Mode | user mode (unprivileged) unless the runtime does not isolate apps (see below), A32 state, little-endian, interrupts enabled | EL0 unless the runtime does not isolate apps (see below), AArch64, little-endian |
| Float state | `FPSCR = 0` (round to nearest, FZ and DN clear). VFP and NEON enabled. `d0` to `d31` are 0 | `FPCR = 0`, `FPSR = 0` (round to nearest, FZ and DN clear). ASIMD enabled. `v0` to `v31` are 0 |
| Memory | image loaded and relocated, BSS zeroed, `argv` strings in app memory | same |

Note. The ZZ9000 sets this state by building a zeroed entry frame. The standard guarantees the zero `FPSCR`. The A32 and A64 profiles do not give bit-identical results for denormal inputs in NEON/ASIMD code: A32 NEON always flushes denormals to zero, A64 ASIMD does not unless `FPCR.FZ` is set. Apps that need identical results across profiles must set the mode themselves.

An app cannot read its mode: in user mode `MRS CPSR` returns only the APSR bits, and `CurrentEL` and `DAIF` trap at EL0. The runtime takes interrupts while the app runs, and the app cannot mask them. The mode shows in two effects. The app runs in the profile's instruction state, little-endian. On a runtime that reports `AAR_INFOF_ISOLATED`, an instruction that needs privilege faults like any undefined instruction (5.3). A runtime that does not isolate apps (4.4) MAY run them with more privilege and MUST say so in its documentation.

`argv[0]` is the program name (the ELF file name, as `aar-run` passes it). The runtime MUST support at least 16 arguments, `argv[0]` included, of up to 127 bytes each, which is 128 bytes with the NUL. `AARInfo.max_args` counts `argv[0]` and `AARInfo.max_arg_bytes` counts the NUL; they are at least 16 and 128 (`AAR_MIN_ARGS`, `AAR_MIN_ARG_BYTES`).

The runtime does not run constructors. The `crt0` supplied in `libaar` (3.8) MUST run `.preinit_array` and `.init_array` before `aar_main`, and the aar `crt0` for both profiles does. The ELF entry symbol is `aar_start`, which is `crt0`, not the app. `crt0` stores the table pointer in `aar_api_ptr`, calls `aar_main(argc, argv)` and passes the result to `exit` (libc flavor) or `aar_exit` (freestanding flavor). Returning from the entry point without exiting is undefined.

### 3.3 The table

The table is the `aar_api` structure of [`aar/app.h`](../include/aar/app.h) (call reference: [`docs/aar-api.md`](../docs/aar-api.md)). It begins with four `uint32_t` header words: `size` (bytes of the table the runtime built), `version` (`AAR_API_VERSION`; names the runtime), `groups` (the `AAR_GROUP_*` bits the runtime implements) and `platform_id` (`AAR_PLATFORM_*`, 0 for unspecified). Every later field is a function pointer, so the first slot is at offset 16 on both profiles. The header pins the slot order until 1.0:

1. Core, 30 slots, `exit` through `msg_wait`: lifecycle and time (`exit`, `yield`, `should_stop`, `time_us`, `sleep_us`, `info`); private heap (`heap_range`, `heap_grow`, `heap_shrink`, `heap_info`); shared buffers and visibility (`shared_alloc`, `shared_free`, `publish`, `acquire`); log, files and console (`log`, the nine `file_*` calls, `console_write`, `console_read`); events and messages (`event_poll`, `msg_send`, `msg_poll`, `msg_wait`).
2. Optional, three slots: `ext`, `map_file`, `unmap_file`.
3. Groups, in this order: display (`display_open` through `display_latest_dropped`, nine slots), audio (`audio_open`, `audio_close`, `audio_stats`), GL (`gl_bind`, `gl_swap`, `gl_unbind`) and services (`sha256`, `decompress`, `image_open`, `image_decode`, `image_close`, `audio_decode_open`, `audio_decode`, `audio_decode_close`). The input group has no slots.

The table has 56 slots in all (`AAR_API_SLOT_COUNT`). A wrapper is named after the slot, with an `aar_` prefix, except that the slots `info`, `heap_info` and `audio_stats` are reached through `aar_get_info`, `aar_get_heap_info` and `aar_audio_get_stats`, because `aar_info` and `aar_heap_info` are type names. The signatures are in the header.

Rules:

1. **Append-only.** A new call is added at the end and raises the minor number. Fields are never moved, removed or re-typed within a major.
2. **Presence.** A slot exists if `size` covers it (`AAR_API_COVERS(api, field)`). A slot that exists and is NULL means "not implemented" and the wrapper returns `AAR_E_NOSYS` (or 0, NULL or nothing for a void or pointer wrapper; `aar_exit` never returns). In addition, `groups` is authoritative for optional groups in both directions. If a group bit is set, every call of that group that `size` covers is non-NULL, except where the header marks a slot optional (`audio_stats`). If it is clear, every slot of that group is absent or NULL. `display_flip_async` and `display_latest_dropped` are present but usable only with `AAR_DISPCAP_ASYNC` and `AAR_DISPCAP_LATEST`. An app that needs a group checks `groups` once; it does not have to call and see `AAR_E_NOSYS`.
3. **Core calls** are all non-NULL in every conforming runtime: every slot from `exit` through `msg_wait`. `ext`, `map_file` and `unmap_file` are optional and are NULL or absent where not provided; the slots of a group follow rule 2.
4. **Header words.** The four `uint32_t` header fields are 16 bytes, so the first pointer is at offset 16 on both profiles with no padding. `aar_gl` (8.5) and the ZZ9000 extension table (8.8) start with two `uint32_t` for the same reason, so their first slot is at offset 8. The header's `_Static_assert`s check this for both data models.
5. **`version`** names the runtime. Apps MUST NOT gate on `version` alone. They gate on `size`, `groups` and `aar_info`; `AAR_API_AT_LEAST` is allowed only as a convenience combined with an `AAR_API_COVERS` check for each call used.

### 3.4 Types shared by all profiles

The structures are defined in `aar/app.h`, which is authoritative (reference: [`docs/aar-api.md`](../docs/aar-api.md), "Types"). Three rules hold for all of them. They use fixed-width integers only (plain `int` appears only in the signatures of table calls, as a return code or exit status). Every fixed-width field has the same offset on A32 and A64. Where a structure carries pointers they come last, at an offset that is a multiple of 8, so no padding differs. The header's `_Static_assert`s check both data models. What the spec adds beyond the header:

- `aar_info` (88 bytes). `shared_used` includes the runtime's own service buffers (4.2). `msg_capacity` is at least 32 and is 63 on the default queue (section 6). `max_files` counts open files and, separately, lists. `stall_limit_ms` is at least 10000. `cpu_features` uses the `AAR_CPU_A32_*` or `AAR_CPU_A64_*` bits of 8.7. It does not report the argument limits; the client library does (`AARInfo`).
- `aar_heap_info` (24 bytes): `committed`, `limit` (after the client's cap) and `pool_free` (a snapshot), all `uint64_t`. `committed` is all the private memory the app holds: its image, its stack and its heap, each in whole `heap_granule` units. `limit` caps `committed` as a whole, so the image, the stack and `min_heap_mib` count against it at load. A successful `heap_grow` of `n` bytes raises `committed` by `n`, and a successful `heap_shrink` of `n` bytes lowers it by `n`. The heap that `heap_range` reports is therefore smaller than `committed`.
- `aar_shared`: `bytes` and `haddr` and `flags` first, then `ptr` at offset 16. `flags` is output only (`AAR_SHAREDF_COHERENT`).
- `aar_event` (16 bytes), `aar_stat` (24 bytes, `int64_t mtime`) and `aar_dirent` (260 bytes: `mode` and a 256-byte name).
- `aar_surface`, `aar_audio`, `aar_image_params` and the service result structures are described with their groups (8.2, 8.4, 8.6).

A runtime and its client library describe one platform. These values MUST be equal in `aar_info` and in `AARInfo` (9.2): `platform_id`, `groups`, `flags`, `shared_granule`, `msg_capacity`, `stall_limit_ms` and `stop_grace_ms`. `aar_info.profile` is one of the profiles in `AARInfo.profiles`. The other fields differ by design: `shared_limit`, `shared_used` and `max_files` are per app, and `AARInfo` carries the limits of the platform itself.

For `aar_get_info` (slot `info`), the caller sets `size`, and the runtime MUST fill at most `size` bytes of the caller's structure and zero the rest. A runtime that does not know a field leaves it 0.

### 3.5 Error codes

One code space for the table and the client library. `AAR_OK` is 0. Errors are negative.

| Code | Value | Meaning |
|---|---|---|
| `AAR_E_IO` | -1 | operation failed, control block or queue corrupt, wait cancelled by stop |
| `AAR_E_INVAL` | -2 | bad argument, unknown flag, unknown format, zero or oversize length, unknown id |
| `AAR_E_NOMEM` | -3 | memory, quota, table or session limit |
| `AAR_E_BUSY` | -4 | queue full, resource in use, flip pending |
| `AAR_E_NOENT` | -5 | no such file, or no further directory entry |
| `AAR_E_ACCES` | -6 | refused by the client's policy |
| `AAR_E_NOSYS` | -7 | slot absent or NULL, codec or format not implemented |
| `AAR_E_TIMEDOUT` | -8 | file wait expired, or `msg_wait` timeout |
| `AAR_E_FAULT` | -9 | bad pointer in a call (isolated runtimes only) |
| `AAR_E_NOSPC` | -10 | a result does not fit (a file too large for `map_file`, a decode output that does not fit its buffer) |
| `AAR_E_EXIST` | -11 | the file exists and `AAR_FILE_EXCL` was set |
| `AAR_E_BADF` | -12 | bad id, or an id used for a direction it was not opened for |
| `AAR_E_PROFILE` | -13 | image profile (or note profile word) not acceptable on this platform |
| `AAR_E_NOGROUP` | -14 | image requires a group the runtime lacks |
| `AAR_E_NORESOURCE` | -15 | admission or load refused for lack of a resource; the reason is an `AAR_REASON_*` code (5.6) |
| `AAR_E_STALE` | -16 | the handle's generation no longer names a live app |
| `AAR_E_OWNER` | -17 | handle used by a task that does not own it |

A platform may map its own transport status to these; the mapping is platform-private.

Per-call meaning is stated in the call reference ([`docs/aar-api.md`](../docs/aar-api.md)).

### 3.6 Versioning

- `AAR_API_MAJOR` changes only for a break. `AAR_API_MINOR` increments for each release that appends table slots, constants or groups. Constant values and structure meanings never change within a major.
- The call reference states, for each call and constant, the minor that introduced it ("Minimum minor"); API 0.5 is the first AAR version, so every symbol there has minimum minor 5. A header also provides `AAR_API_AT_LEAST(api, minor)` for convenience; the preferred gate is still `AAR_API_COVERS`.
- Pre-1.0: numbers are `0.x` and any minor may break. 1.0 is cut when a second platform passes conformance.
- The client library has its own library version and revision (Exec). `AARQuery` returns the API major and minor the platform runtime implements; the library revision is not the API minor. The note layout (`AAR_NOTE_VERSION`) and the control block layout (`AAR_CB_LAYOUT_VERSION`) are versioned separately and are not the API minor either.

### 3.7 Concurrency and signals

One thread per app. Calls are synchronous. A call that parks (files, console, display flip, `msg_wait`) returns when the work completes, on stop (`AAR_E_IO`), or on its own timeout. A sleep is also ended by a stop, but it has no return value (7.3). There are no asynchronous callbacks into the app.

A stop request does not prevent calls. A call made after it that needs the client MAY complete if the client answers at once, or MAY end with `AAR_E_IO` without waiting; an app that has seen the stop SHOULD finish and exit, and MUST NOT depend on a file read or write completing. A request that was already published when the wait ended is not cancelled (7.1), so a later call on the same entry can be `AAR_E_BUSY`.

### 3.8 libaar and the two layers

`libaar` (MIT) provides:

1. `aar/app.h`, `aar/note.h` and `aar/gl.h`, and for ZZ9000-only code `aar/ext_zz9000.h`;
2. `crt0` for each profile (`libaar/a32/crt0.S`, `libaar/a64/crt0.S`; 3.2);
3. wrappers `aar_*` that gate on `AAR_API_COVERS` and a non-NULL pointer and return `AAR_E_NOSYS`, `0`, NULL or nothing otherwise, plus the `gl*` wrappers and `aar_zz9000_gl_bind_flags`. `aar_gl_bind` and `aar_zz9000_gl_bind_flags` record the GL table they return, and the `gl*` wrappers forward to that record; a table obtained by calling the extension's `gl_bind_flags` slot directly is not recorded, so the wrappers would do nothing;
4. linker scripts for each profile (`libaar/a32/aar.ld`, `libaar/a64/aar.ld`), `mk/aar.mk` and `cmake/` toolchain files, and the SDK image `ghcr.io/blitterstudio/aar-sdk` (published on `v*` tags and manual runs only);
5. layer 1, **core**, with no libc: no `malloc`; weak `memcpy`, `memmove`, `memset`, `memcmp` for compiler-generated calls. This is the freestanding mode (build flag `AAR_FREESTANDING`), which does not declare `aar_alloc` and `aar_free`. In this layer private memory comes from `heap_range`/`heap_grow` only;
6. layer 2, **libc glue**, for a PIC newlib build: `_sbrk` over `heap_grow`/`heap_shrink`, `_write`/`_read` for fd 0 to 2 over the console, and `_open`, `_close`, `_read`, `_write`, `_lseek`, `_fstat` and `_stat` for fd 3 and above over `aar_file_*`, so `fopen` works. `O_CREAT`, `O_TRUNC` and `O_EXCL` pass straight into `AAR_FILE_CREATE`, `_TRUNCATE` and `_EXCL`. A descriptor opened with `O_APPEND` seeks to the end before every write. `_stat` and `_fstat` set `EOVERFLOW` when a size does not fit `off_t` (A32).

The private allocator `aar_alloc(bytes, align)` belongs to layer 2. The shared allocator is core.

## 4. Memory model

### 4.1 Private memory

- An app has private memory: its image, stack and a heap. The platform decides where; apps are position-independent and MUST NOT assume an address.
- The heap starts at the end of the image. `aar_heap_range` returns its current base and length. `aar_heap_grow(bytes, &old_break)` extends it by a whole number of `heap_granule` units (1 MiB) and returns the old end, which MUST equal the previous end (contiguous). Existing pointers never move. New memory is zeroed. `aar_heap_shrink` returns whole granules at the top. Failure is `AAR_E_NOMEM` (limit or pool) or `AAR_E_INVAL` (zero, or not a whole number of granules).
- `heap_info` reports `committed`, `limit` and `pool_free` (3.4 defines `committed`). `limit` is the smaller of the platform's cap and the client's `AAR_TAG_PRIVATE_MIB`. The platform's cap is the most private memory one app can hold; it belongs to the platform and need not equal the pool. `pool_free` is what the platform could give this app now, a snapshot; it can be smaller than `limit - committed`. A grow fails with `AAR_E_NOMEM` when either bound is used up.
- The runtime queues `AAR_EV_LOW_MEMORY` for every running app when the pool of free private memory, the memory all apps draw on, falls below its low-water mark, and queues it again only after the pool has risen above its high-water mark. The marks are the platform's own, with 0 < low-water < high-water, and the pool with nothing committed lies above the high-water mark. An empty pool is below any low-water mark, so the event has been queued by the time a `heap_grow` fails because the pool is empty. A refusal because of the client's private limit is not a crossing. The event is delivered through `event_poll`; fields other than `type` are 0. (Non-normative: the ZZ9000 uses 32 MiB and 64 MiB.)
- `min_heap_mib` in the note is committed at load. It is a 32-bit count of MiB; the standard sets no maximum. A runtime MUST compare it with its limits without converting it to bytes in a type that can wrap, so that every value of the word is either committed or refused. A refusal for lack of memory at load is `AAR_E_NORESOURCE` with `AAR_REASON_MEMORY` (the reason is in `AARStatusInfo.reason`); `AAR_E_NOMEM` is also accepted from a load. A client treats both as not enough memory (5.6).

### 4.2 Shared buffers

A shared buffer is memory the app owns that the host can read and write.

`aar_shared_alloc(bytes, align, flags, &out)` and `aar_shared_free(p)` are declared in `aar/app.h`. `flags` MUST be 0 (a non-zero value is `AAR_E_INVAL`; `AAR_SHAREDF_COHERENT` is an output flag in `aar_shared.flags`). A pointer that is not the base of a live block is `AAR_E_INVAL` from `aar_shared_free`.

- `bytes` and `align` are rounded up to `shared_granule`. `ptr` is the app mapping. `haddr` is the host address token.
- The app passes `haddr` to the client (for example inside a message). The client resolves it with `AARSharedMap` (section 9.5). The token is meaningful only for this handle, only until `shared_free`, exit, fault or stop, and only for the whole block (not interior addresses; an interior range is described by token plus offset in the message).
- The token is 32 bits because the host is a 68k and its address space is 32-bit. On ZZ9000 it is the board-relative Zorro offset of the buffer. On a platform where the Amiga address space is the same RAM, it is the absolute Amiga address. The app MUST NOT interpret it.
- Each app has a shared limit (`shared_limit`, default 16 MiB (`AAR_SHARED_DEFAULT_MIB`), settable by the client with `AAR_TAG_SHARED_MIB`, 0 for default). The limit is a cap and reserves no memory: an allocation can fail below it because the platform's shared region is shared with other uses. `AARInfo.shared_max_mib` is the largest limit a client may ask for, and at most 4095. `AARSetTagList` refuses a larger value with `AAR_E_INVAL` (9.2). A limit the platform cannot grant when the load starts fails the load (`AAR_E_NORESOURCE` with a reason, or `AAR_E_NOMEM`). That includes a limit above `shared_max_mib` that reached the load, and a limit at or below it that the platform has no room for. A runtime MUST support at least 64 live blocks per app (`AAR_SHARED_MIN_BLOCKS`).
- The runtime itself uses shared buffers for the console and audio rings and for file payloads. They are allocated on first use and freed on close or exit, from the same memory as the app's own buffers. They are charged to `shared_used` and to the shared limit (one limit word covers both, 10.3), and they do not count toward the 64 live blocks above.
- Free memory the client still maps: see 5.5 and 9.5.

### 4.3 Visibility rules: publish and acquire

Goal: after a data hand-off, the other side reads what the writer wrote, on any platform. The hand-off is always a message (section 6), except for the audio ring, which orders its data by its own indices (8.4). Two operations make it correct on platforms whose caches or buses are not coherent, and are free where they are:

```c
int aar_publish(const void *p, uint64_t bytes);  /* writer: make my stores visible to the host */
int aar_acquire(const void *p, uint64_t bytes);  /* reader: drop my stale view of host-written data */
```

App side rules:

1. After writing data that the client will read, the app MUST call `aar_publish` on the written range, and only then send the message that announces the data. `aar_publish` is a no-op plus a store barrier when `AAR_SHAREDF_COHERENT` is set for the block (or `AAR_INFOF_SHARED_COHERENT` is set for the runtime), and a cache clean otherwise.
2. After receiving a message that announces data the client wrote, the app MUST call `aar_acquire` on that range before reading it. It is a load barrier when coherent and a cache invalidate otherwise.
3. `aar_msg_send` and `aar_msg_poll`/`aar_msg_wait` are release and acquire points for ordering on the bus: all earlier stores are ordered before the message becomes visible, and loads after a received message are ordered after it. They do not clean or invalidate caches. That is why rule 1 and 2 exist.
4. The ranges are arbitrary byte ranges inside a block; the runtime rounds to `shared_granule` internally. **A granule has one writer at a time.** The app MUST NOT write into a granule that the client is writing, and vice versa, between two hand-offs.
5. The runtime provides `aar_publish`/`aar_acquire` as table calls, not as inline instructions, because user mode on AArch64 cannot invalidate by address (`DC IVAC` is privileged) and a platform with a non-coherent bridge needs privileged maintenance. The libaar wrapper returns at once when the runtime says coherent.

Client side rules (the library implements them; section 9.5):

1. The client maps a shared buffer with `AARSharedMap`. The map reports `AAR_MAPF_UNCACHED` (the 68k data cache does not hold the range) and `AAR_MAPF_COHERENT` (no maintenance is ever needed on the 68k side).
2. After writing data for the app, the client MUST call `AARSharedPublish` on the range before posting the message. It pushes dirty 68k cache lines (and is a no-op when the map is uncached or coherent).
3. Before reading data the app published, the client MUST call `AARSharedAcquire` on the range, after it has received the message. It invalidates the range in the 68k data cache.
4. The client MUST NOT write to a range it is about to acquire. On a copyback 68k cache an invalidate-by-push (`CacheClearE` with `CACRF_ClearD`) first writes dirty lines back, and a dirty line that overlaps the app's fresh data would overwrite it with stale bytes. This is the reason for the granule and ownership rules.
5. A client that always calls Publish and Acquire is correct on every platform. A client that skips them is correct only on maps that report both uncached and coherent.

**The 68k-cache case.** A frequent question is why the client sometimes sees old data when the ARM app renders into a shared buffer and the 68k client reads it. Three different things can cause that:

- The platform's own path. On the ZZ9000 2.x firmware the Zorro master reaches DDR through the Zynq ACP, which snoops the ARM L1 caches, and non-cacheable shareable memory is also coherent for Zorro readers. The standard does not rely on this. A platform that is not coherent implements `aar_publish`/`aar_acquire` with real cache maintenance. On the ZZ9000, `publish` is a store barrier only. This was checked on the card with a test that ran plain and under load, and a negative control with the shared bit cleared made it fail.
- The 68k's own data cache. No card, including an ACP, reaches the 68k's caches. A 68040 or 68060 client that has read a line of a shared buffer once can read the old copy again from its own cache. The fix is on the client: map the range cache-inhibited (what the ZZ9000 library does with `mmu.library` for the control block, the queues and every shared buffer it maps), or call `AARSharedAcquire` before each read pass. The library reports which one it did in `AARSharedMap.flags`.
- The transport's own structures. The control block and queue pair (section 10) sit in ordinary shared memory with the same mapping as an app's shared buffer. The control structures have a different writer pattern (the runtime and the client write indices and entries). See 10.5.

The conformance test (section 11, C-SHARED) runs an app that writes a counter and a pattern into a shared buffer while a 68k client reads it, and requires zero stale or torn reads. The test runs with the map the library gives and calls `AARSharedAcquire` before each read pass, as client rule 5 says. On a platform whose maps report uncached the Acquire does nothing and is not exercised; on any other platform it is what makes the test pass. A way to ask for a deliberately cacheable map is planned for API 0.6 (appendix).

### 4.4 Isolation

When `AAR_INFOF_ISOLATED` is set, the runtime validates every pointer passed to a call (`AAR_E_FAULT` for a bad one), an app fault ends only that app, and apps cannot reach firmware, other apps or the host's memory except through granted shared buffers and surfaces. When it is not set, a bad pointer is undefined behaviour, and the platform MUST document it. The standard does not require isolation (section 13 asks what each platform can do).

The address space of an app holds its image, heap and stack, the shared buffers and surfaces it owns, and what the platform documents besides. Where `AAR_INFOF_ISOLATED` is set, every other address is unmapped for the app, and an access to it is a fault (5.3).

Non-normative: the suite does not test isolation as a whole. It tests what a client or an app can see: a bad pointer in a call returns `AAR_E_FAULT` (C-MEM.13) and a fault of app code ends that app with a fault report (C-LIFE). The platform vendor is responsible for the rest.

## 5. Lifecycle and limits

### 5.1 States

Seen through `AARStatus` (client side):

`IDLE` (0), `CLAIMED` (7), `LOADING` (1), `STARTING` (2), `RUNNING` (3), `STOPPING` (5), `FAULTED` (6). State 4 is not defined.

| From | To | Cause |
|---|---|---|
| IDLE | CLAIMED | `AAROpen` succeeds (admission passed, handle assigned) |
| CLAIMED | LOADING | `AARLoadFile`/`AARLoadMemory` begins |
| LOADING | STARTING | `AARStart` accepted (after the load completed) |
| STARTING | RUNNING | entry reached |
| RUNNING | STOPPING | exit, stop request, fault, watchdog, lease loss, reset |
| STOPPING | IDLE or FAULTED | teardown finished. The end reason and, after a fault, the fault data are retained and readable until `AARClose` |

`AARLoadFile` and `AARLoadMemory` do not change the state: it is `LOADING` from the start of the load until `AARStart` is accepted, including after the load call has returned. A client cannot rely on observing `STARTING`, because it lasts only until the entry is reached.

After a fault a platform MAY report `FAULTED` until `AARClose` or return to `IDLE`. A client MUST treat `IDLE` and `FAULTED` alike as ended and read `end_reason` (and, for `USER_FAULT`, the fault record) to learn why. The ZZ9000 returns to `IDLE`.

### 5.2 Load, start, stop

- Open claims one app instance (admission, 5.6) and returns a handle bound to the calling task. It can refuse with `AAR_E_NORESOURCE` and a reason code, or `AAR_E_NOMEM`.
- Load streams an image. The runtime parses it incrementally so that no single step stalls other work for more than about one millisecond (the ZZ9000 bounds each pass). The load call returns `AAR_E_PROFILE`, `AAR_E_NOGROUP`, `AAR_E_INVAL`, `AAR_E_NOMEM` or `AAR_E_NORESOURCE`. The platform reports the maximum image size, which is at least 16 MiB.
- Start passes `argv`. The app runs from the entry contract (3.2).
- Stop sets `should_stop`, queues `AAR_EV_STOP`, and starts the grace period (`stop_grace_ms`, default 500). If the app has not exited when it ends, the runtime stops it by force, and the end reason is `AAR_END_FORCED`. The suite measures the forced end no earlier than the grace period and no later than the grace period plus 2 s (C-LIFE.7).
- The app's exit code is the return value of `aar_main` or the argument to `aar_exit`. By convention the AmigaDOS return codes 0, 5, 10, 20 are used. The client reads it through `AARStatus`. `aar-run` returns it to the Shell.

### 5.3 Faults and the end report

- A fault of app code (bad data or instruction access, undefined instruction, alignment) ends that app only. A fault in runtime code ends every app and the platform restarts its runtime (`AAR_END_RUNTIME_RESTART`).
- `AARStatusInfo` carries a profile-neutral fault class and the raw syndrome, so the client can print a useful line without knowing the CPU:

  | Field | A32 | A64 |
  |---|---|---|
  | `fault_class` | `AAR_FAULT_DATA`, `_FETCH`, `_ILLEGAL`, `_ALIGN`, `_OTHER` | same |
  | `fault_pc`, `fault_sp`, `fault_lr`, `fault_addr` | 32-bit in the low word, high 0 | 64-bit |
  | `fault_syndrome` | FSR, in whichever format the host CPU reports it (the short-descriptor DFSR, or the long-descriptor form when AArch32 runs under an AArch64 kernel) | ESR_EL1 (low 32 bits) |
  | `fault_pstate` | CPSR | PSTATE (low 32 bits) |

  Addresses are app virtual addresses. A client that wants a symbolised trace subtracts the load base, which `AARStatus` also reports (`load_base`).

- A fault (`AAR_END_USER_FAULT`) and a watchdog stall (`AAR_END_STALL`, 5.4) both fill the fault record. For a stall `fault_pc`, `fault_sp` and `fault_lr` are where the app was when the watchdog ended it, `fault_class` is `AAR_FAULT_NONE`, and `fault_syndrome`, `fault_pstate` and `fault_addr` are 0.

- Both ends also take a stack snapshot, if the platform can read the app's stack: up to `AAR_CB_STACK_MAX` (512) bytes from the app's `sp` upward, copied as they lie in app memory (little-endian app words, never swapped), with the app address of the first byte. Bytes the app cannot read are not copied. `AARReadStack` (9.2) returns the snapshot and `AARStatusInfo.stack_sp` and `stack_bytes` describe it; `stack_bytes` is 0 when there is none. A client may print the snapshot, or give it to a tool that lists the words that point into the app's code, which is a guess at a backtrace: the platform does not unwind the stack.

- `AAR_FAULT_ALIGN` is a fault the CPU raises because an access is not aligned. On A32 it is a data abort with fault status 00001 in the short-descriptor FSR, or 100001 when the host reports the abort in the long-descriptor or ESR form. On A64 it is a data abort with `DFSC` 100001 or a PC or SP alignment exception (`EC` 0x22, 0x26). An exclusive or atomic access that is not aligned to its size always raises it, so a runtime MUST classify that fault as `ALIGN`. Whether a plain load or store to normal memory that is not aligned faults is the platform's choice, and an app MUST NOT depend on either result. (The A64 encodings and the AArch32-on-AArch64 case are from the Arm architecture manual and have not been run on a platform; section 13 asks the platforms to confirm them.)
- End reasons (`AARStatusInfo.end_reason`): `NONE` 0, `EXIT` 1, `FORCED` 2, `USER_FAULT` 3, `STALL` 4, `STUCK_CALL` 5, `LEASE` 6, `PLATFORM_RESET` 7, `RUNTIME_RESTART` 8, `QUARANTINED` 9.

### 5.4 Watchdog

An app MUST make a table call at least every 5 seconds while it computes. The runtime MUST tolerate at least 10 seconds between calls (`stall_limit_ms`) and MAY stop an app that goes longer, with `AAR_END_STALL`. A runtime MAY end an app whose single call stays inside the runtime's own code, not parked in a declared wait, for longer than a stuck-call limit. The limit is at least 10 s and the end reason is `AAR_END_STUCK_CALL`. An app cannot cause this (every long call is a declared wait), so the suite does not test it. A call that is parked inside a declared wait does not count toward either limit. `yield`, `should_stop`, `time_us`, `msg_wait` and every file or console call count as activity.

When the runtime ends an app with `AAR_END_STALL` it fills the fault record and the stack snapshot (5.3), so that the client can say where a hang was.

Note: the ZZ9000 values (30 s stall, 10 s stuck) satisfy this. The 5 ms scheduler quantum, the 5% service floor and the "API body is unpreemptible" rule are platform behaviour; apps cannot depend on them, and `sleep_granule_us` tells them the timing granularity.

### 5.5 Leases and dead-client release

The client library keeps the app alive. Without it, the runtime would run an app for ever whose owner has gone.

1. **Lease.** The runtime stops an app whose owner has not renewed the lease within the lease period. The period while an app runs is platform-defined (ZZ9000: 10 s by default, because `AAR_TAG_LEASE_EXTENDED` is on; 2 s when the client clears it). While `CLAIMED` or `LOADING` it is longer and is extended by load progress (ZZ9000: 30 s). The end reason is `AAR_END_LEASE`.
2. **Renewal.** The library MUST renew the lease from an interrupt or timer context that does not depend on the owner task, so that a requester or file dialog that blocks the owner for minutes does not stop the app. On the ZZ9000 this is the vertical-blank server.
3. **Dead owner.** If the owning task exits without `AARClose`, or the owning task's last `CloseLibrary` happens, the library MUST release the handle and stop the app. It MUST detect a dead task without relying on a task address alone, because Exec can reuse an address (the ZZ9000 library compares task address, name pointer, node type, CLI task number and the allocated signal bit, and checks about once a second). A client can observe the release through the base API in two ways. Calls on the old handle return `AAR_E_STALE` and no longer `AAR_E_OWNER`. The claim's admission resources are free again (a new claim succeeds, and a platform with a ceiling has the slot back). The suite allows 10 s for the release (C-LIFE.20). The runtime stops the app before it reuses the app's memory (rule 5). The base API has no call that shows that the app has stopped; a platform MAY offer one in its extension (the ZZ9000: the claim list).
4. **Stale handles.** A handle names its app by an index and a generation (10.2); the transport carries a 32-bit word, while the client API passes an `AARHandle` pointer. Every request names the handle. A request that reaches the platform (stop, load, start, map, doorbell) with a handle whose generation no longer names a live app fails with `AAR_E_STALE` and changes nothing. The library then treats the handle as revoked for every later call. The calls that only read or write the claim's shared memory (`AARStatus`, `AARGetMessage`, `AARSendMessage`, `AARServiceNext`, `AARServiceReply`, `AARPostEvent`, `AARAudioRead`, `AARSharedPublish`, `AARSharedAcquire`) MAY succeed until the library has seen that answer. The end reason in the status says why the platform ended the app. Rule 5 keeps those writes from harming another app.
5. **Late writes.** After a lease loss, a platform that shares memory with the client MUST NOT reuse that memory for another app while the old client could still write to it. The ZZ9000 keeps the app's control block, queues and shared buffers allocated ("quarantine") until the old client's next stale request or an explicit release. Other platforms may use other means; the requirement is the outcome.
6. **Reset.** A platform reset ends every app with `AAR_END_PLATFORM_RESET`.

### 5.6 Concurrent apps

`AARInfo.max_apps` is 0 when the number of loaded apps is bounded only by platform resources, including a process-record table that the platform sizes at start from memory (the ZZ9000: 231 records on a board with 590 MiB for apps). A platform whose limit is a fixed number that does not follow from resources reports that number in `max_apps` and refuses the next open with `AAR_REASON_LIMIT`. A client treats 0 as no stated ceiling and finds the limit by the refusal. A client does not see slot or index numbers: each `AAROpen` returns its own opaque handle (10.2). Admission refuses with `AAR_E_NORESOURCE` and a reason that the library can print:

- `AAR_REASON_MEMORY` (1): platform memory for the app's record and stack.
- `AAR_REASON_TABLES` (2): address-space translation tables.
- `AAR_REASON_HOSTHEAP` (3): shared memory for the control block and queues.
- `AAR_REASON_LIMIT` (4): a platform ceiling.

A load that cannot get a resource answers the same way (4.1). A refused open leaves nothing allocated. CPU load is not an admission resource: apps slow down and are not refused.

**Sharing the CPU.** The standard does not define a scheduler. A platform that runs several apps SHOULD give runnable apps CPU in proportion to their weight (`AAR_TAG_WEIGHT`, default 1). The standard states no tolerance; the suite accepts a work ratio within 30 percent of the weight ratio for two compute-bound apps. A platform MUST NOT let an app that is parked in a wait consume CPU: a compute-bound app beside a parked app keeps at least 90 percent of the work it does alone (the suite's bound). Note: a platform keeps its own service work ahead of apps; the standard does not require it. The ZZ9000 uses weighted fair share by virtual run time: the app furthest behind runs next.

## 6. Messages and waiting

- A message is exactly 32 bytes (`AAR_MSG_BYTES`), opaque to the runtime and the library. It travels in one queue entry (10.4). Each direction is a FIFO that holds at least `msg_capacity` messages (at least 32) and never drops or reorders. `msg_capacity` is a minimum: the client library takes messages off the queue into an inbox of its own, so the first `AAR_E_BUSY` can come later (the reference library's inbox holds 64). An app MUST NOT depend on the exact number. The default queue has 64 entries and a message post keeps one entry free for the call in flight (`AAR_MSG_RESERVE`), so `msg_capacity` is 63 (`AAR_MSG_CAPACITY`); a queue of depth `d` holds `d - 1`.
- `aar_msg_send(msg)` copies 32 bytes toward the host. On a full queue it returns `AAR_E_BUSY` and never blocks. `AARSendMessage` is the same toward the app and returns `AAR_E_BUSY` when full.
- `aar_msg_poll(msg)`: returns 1 and copies one message, or 0 when empty, or a negative code. `AARGetMessage` returns `AAR_E_BUSY` when no message from the app is queued.
- `aar_msg_wait(timeout_us)`: parks until a host-to-app message is queued. Returns `AAR_OK` with the message still queued, `AAR_E_TIMEDOUT` if the timeout passes with the ring empty, `AAR_E_IO` on a stop request. A message already queued is reported first, so a stop never hides one. Timeout 0 only tests the ring. A parked app is exempt from the stall watchdog. A runtime without the wait still has to provide the call; it may implement it as a sleep-and-poll loop.
- Message content is the protocol of the app and its client. A convention is recommended but not required: word 0 is a type, words are little-endian when the app writes them, and any 64-bit counters or addresses use the app's own byte order documented in the app. The reference tests use eight little-endian `uint32_t` words and a shared counter stored big-endian.
- Notification to the client: see 9.4.
- Ordering with shared buffers: section 4.3.
- Messages remain readable by the client after the app has exited, until `AARClose`. The client drains them before closing.

## 7. Files, console, log, time

All of this is performed by the client (section 9.6): the app's calls go to the client, which serves them with dos.library.

### 7.1 Files

- Paths are AmigaDOS paths, at most 255 bytes plus NUL (`AAR_PATH_MAX`). An empty path, or one longer than that, is `AAR_E_INVAL` for `file_open`, `file_stat` and `file_list_open`. The limit is the app interface's: a client's filesystem may accept less (names of 30 characters on an OFS volume), and the call then fails with that filesystem's error. The client decides the root and the policy. The runtime passes a path to the client unchanged after checking its length. A client that serves apps it does not trust SHOULD apply this default, which is the one `aar-run` applies:
  - Relative paths resolve under a root directory chosen by the client (the Shell's current directory, the directory of the app image, or the root given to the serving loop, `aar_serve_config.root`).
  - A path that would leave the root through `..`, a leading `/` or a link is refused with `AAR_E_ACCES`.
  - A path with a volume or assign prefix (`:`) is refused unless the client allows it (`AAR_SERVEF_ABS_PATHS`). Allowing those does not relax the check on relative paths.

  This is path hygiene, not a security boundary. A client is not required to apply it and a platform is not judged on it. A refused path is `AAR_E_ACCES`.

  Once the client asks the app to stop, it refuses every request that would create, truncate or write a file. The call then returns a negative code (`AAR_E_ACCES` from the client, or `AAR_E_IO` when the runtime ends the wait first) and the file is not changed by a request the client refused.
- File ids are small integers, at least 3 (`AAR_FD_BASE`). At least 16 files and, in a separate table, 16 directory lists can be open (`max_files`). A bad id is `AAR_E_BADF`.
- Flags work as `O_CREAT`, `O_TRUNC` and `O_EXCL` do in POSIX: `AAR_FILE_READ` 1, `AAR_FILE_WRITE` 2, `AAR_FILE_CREATE` 4 (create the file if it is missing; an existing file keeps its contents), `AAR_FILE_TRUNCATE` 8 (empty an existing file; it does not create) and `AAR_FILE_EXCL` 16 (with `AAR_FILE_CREATE`: fail with `AAR_E_EXIST` if the file exists, leaving it untouched). The old create-or-replace is `AAR_FILE_CREATE | AAR_FILE_TRUNCATE`. At least one of read and write is required; `CREATE` and `TRUNCATE` need `WRITE`; `EXCL` needs `CREATE`. Any other combination or bit is `AAR_E_INVAL`. An id remembers how it was opened. `file_write` through an id opened without `AAR_FILE_WRITE`, and `file_read` through an id opened without `AAR_FILE_READ`, fail with `AAR_E_BADF` and change nothing; the runtime or the client may do the check. A missing file without `CREATE` is `AAR_E_NOENT`. The client applies create, truncate and exclusive create as part of the one open request, so a libc `open` passes its flags straight through. Seek: `AAR_SEEK_SET/CUR/END`.
- AmigaDOS has no atomic exclusive create. A client that serves files through dos.library applies `AAR_FILE_EXCL` by checking and then creating inside its single serving loop. That is exclusive against the other apps the client serves, not against other Amiga programs that create the same file at the same moment. A client on a host with an atomic exclusive create (the host runner with POSIX `open`) passes the flag on and gets full exclusion. On the ZZ9000, `AAR_FILE_READ` opens with `MODE_OLDFILE`, `AAR_FILE_WRITE` with `MODE_READWRITE`, `AAR_FILE_CREATE | AAR_FILE_TRUNCATE` with `MODE_NEWFILE`.
- `file_read`: `*out_n` is the byte count obtained. A short read with `AAR_OK` is end of file. `file_write`: a short write means the client stopped early; the caller may retry with the rest or treat it as an error.
- `file_seek` returns the new position (`AAR_SEEK_SET`, `AAR_SEEK_CUR`, `AAR_SEEK_END`). A position before the start of the file is `AAR_E_INVAL` and leaves the position unchanged. A position past the end of the file may be refused: a platform MAY return `AAR_E_INVAL` (AmigaDOS handlers refuse it on a file opened for reading), and if it accepts the position a read there returns `AAR_OK` with 0 bytes. This version does not say what a write there does, so an app that wants to extend a file writes the data and does not seek past the end.
- A transfer is served in chunks of the platform's choice. A request waits 30 seconds for the client, counted from when it is posted, and then ends with `AAR_E_TIMEDOUT`. As the app measures it with `aar_time_us` the wait ends between 29 and 36 seconds after the call started. A shorter limit does not conform. A transfer of several chunks waits for each chunk's request in turn, so a whole call can last longer. After a timeout the id MUST NOT be reused for the rest of the app's life, so a late reply cannot be taken for a later call. A stop ends the wait with `AAR_E_IO`. Ending the wait does not cancel a published request or release its borrowed storage (9.6). A later call may return `AAR_E_BUSY` while that storage or request slot is still borrowed; consuming the matching late completion permits reuse.
- `file_stat`: `size`, `mtime` (Unix seconds, 0 when the filesystem has none), `mode` (1 file, 2 directory). `file_list_next` returns names of at most 255 bytes, `AAR_E_NOENT` at the end.
- `aar_map_file` (optional, with `aar_unmap_file`): reads a whole file into a read-only buffer that the runtime owns, valid until `aar_unmap_file`, exit or fault. The ZZ9000 implements it with a shared buffer and counts it against the shared limit (its file size limit is 16 MiB; a larger file is `AAR_E_NOSPC`). A platform MAY return `AAR_E_NOSYS`; an app can always use `file_read`. The call stays optional in 0.5.

### 7.2 Console and log

- `console_write(buf, len, &n)` writes every byte or returns an error: it never drops bytes and parks when the client's buffer is full. The runtime buffers console output, so a `console_write` of up to 1024 bytes into an empty buffer MUST return without waiting for the client. `console_read(buf, len, &n)` parks until the client has input and returns `AAR_OK` with `*n == 0` at end of input. A stop ends the wait with `AAR_E_IO`.
- The client decides what the console is: the Shell's `Output()` and `Input()`, or a window opened on first use for a Workbench launch. Ctrl-\ and keyboard EOF behaviour is the client's (on OS 3.2 Ctrl-\ does not end console input; the recommended workaround is a redirected handle).
- File descriptors 0, 1 and 2 are the console in the libc flavor. `isatty` returns 0.
- `aar_log(msg, len)` is a lossy diagnostic channel, separate from stdout, for the platform's debug output and for the client, which receives the records through `AARSetLogSink` (9.2). Records over 1024 bytes are truncated (a truncated record is delivered, not dropped) and the platform MAY rate-limit (the ZZ9000: 32 per second to the client, 8 per second to its UART). A record that the platform discards before the client receives it, for any reason (a rate limit, no free queue entry, a full SQ), is a dropped record and is counted in `log_dropped` (`AAR_CB_LOG_DROPPED`, 10.4). A separate limit on another sink, such as the ZZ9000's UART, is not a drop.

### 7.3 Time

- `aar_time_us()` returns monotonic microseconds, non-decreasing, from an epoch the platform chooses (not wall-clock). Resolution SHOULD be 1 ms or finer.
- `aar_sleep_us(us)` parks for at least `us` microseconds of the platform clock and returns nothing. It ends early only for a stop: if a stop is requested while the app sleeps, or is already pending when the call is entered, the call returns at once and `aar_should_stop()` is 1 (3.7). The sleep reports no code for it; the app reads `aar_should_stop` or the `AAR_EV_STOP` event. `sleep_granule_us` is the platform's timer tick. With no other app competing for the CPU, a sleep SHOULD end no later than `us` plus four granules (each at least 1 ms) plus 10 ms.
- No wall-clock call is defined in API 0.5. A client-served `aar_time_unix_us` would be an appended call through the service queue in a later minor; section 13 asks whether it comes before 1.0.

## 8. Optional groups

### 8.1 Discovery

A group is present if its bit is set in `aar_api.groups` (and in `aar_info.groups`) and the table covers its slots. Groups: `AAR_GROUP_DISPLAY` (1<<0), `AAR_GROUP_INPUT` (1<<1), `AAR_GROUP_AUDIO` (1<<2), `AAR_GROUP_GL` (1<<3), `AAR_GROUP_SERVICES` (1<<4); `AAR_GROUPS_DEFINED` is their union. The input group has no table slots. It adds the input events to `event_poll`. Bits 5 to 27 are reserved for later versions of the standard. A platform MUST NOT set one in `aar_api.groups`, `aar_info.groups` or `AARInfo.groups`. Bits 28 to 31 are for experiments of a platform (`AAR_GROUPS_EXPERIMENTAL`). A platform that does not have a group the note lists in `groups_required` refuses the load with `AAR_E_NOGROUP`, whether the bit is defined, reserved or experimental. Bits of `groups_optional` that a runtime does not know are ignored, because the word is a hint. The client learns the same set from `AARInfo.groups` before it loads anything, so a launcher can refuse early (the note's `groups_required` does it at load).

Capabilities inside a group are reported in a small per-group structure obtained through the group's open call or `aar_info`; a missing capability is `AAR_E_NOSYS` from the call that needs it.

### 8.2 Display (`AAR_GROUP_DISPLAY`)

`aar_surface` (`aar/app.h`) holds `id`, `width`, `height`, `pitch`, `format` (as requested), `layout`, `caps` and a reserved word, then `front` and `back` (the app mapping, NULL if the app does not draw that buffer) at offset 32 on both profiles. `caps` is `AAR_DISPCAP_ASYNC` 1, `AAR_DISPCAP_LATEST` 2, `AAR_DISPCAP_CLUT8` 4; it is 16 bits wide because the transport carries it in the entry `flags` of the `DISPLAY_OPEN` reply. The calls are `aar_display_open(w, h, format, flags, &surface)`, `aar_display_flip`, `aar_display_flip_async` (only with `AAR_DISPCAP_ASYNC`), `aar_display_flip_idle` (1 shown, 0 pending, negative error), `aar_display_refresh`, `aar_display_vsync`, `aar_display_set_palette`, `aar_display_close` and `aar_display_latest_dropped`.

`aar_display_vsync` is a pacing hint. It returns `AAR_OK` once the runtime has done what it can to line the app up with the display refresh, which may be nothing (the ZZ9000 yields once), or `AAR_E_INVAL` when no display is open. A platform MAY wait for the next vertical blank; an app MUST NOT depend on it. An app that wants display pacing opens with `AAR_DISPLAY_VSYNC`, whose flip returns when the frame is on scanout.

- Width and height are 1 to 4096 (`AAR_DISPLAY_MAX_DIM`; a platform may allow more). Formats: `AAR_FMT_RGB565` 1, `AAR_FMT_BGRA8888` 2, `AAR_FMT_CLUT8` 3 (optional, `AAR_DISPCAP_CLUT8`). A `format` value other than 1, 2 or 3 is `AAR_E_INVAL`. A defined format that the platform does not provide (`AAR_FMT_CLUT8` when `AAR_DISPCAP_CLUT8` is not granted) is `AAR_E_NOSYS`. `layout` gives the real byte order and the app MUST store pixels by `layout`: `RGB565_BE/LE`, `BGR565_BE/LE`, `BGRA8888`, `RGBA8888`, `CLUT8`. `pitch` is in bytes and can exceed `width * bytes_per_pixel`.
- One display per app. The client chooses the Amiga screen or window (P96, CGX, a mode id) and binds a bitmap to the surface through the service queue (section 9.6). The memory is the memory of the client's bitmap, mapped for the app by the runtime. The app writes `back`, flips, and then calls `refresh` before writing again. When a flip has completed (`aar_display_flip` returned `AAR_OK`, or `aar_display_flip_idle` returned 1) and `aar_display_refresh` has run, `front` is NULL or names the buffer that holds the frame just presented, and `back` names the buffer to draw the next frame into. `back` has no promised content: it may hold the frame before the last, or an older one. With `AAR_DISPCAP_LATEST` granted no flip completes, and `front` is not promised to hold any particular frame. `pitch` and `layout` are those of the `DISPLAY_OPEN` reply and apply to both `front` and `back`. When the reply names one buffer only, the app draws into runtime memory of the same size and each flip copies it to the client's bitmap byte for byte: row `y` of the copy holds the bytes of row `y` of `back`, at `pitch` bytes per row, with no conversion.
- A flip publishes the surface. Before the host sees the frame, the runtime cleans the presented range. The app needs no `aar_publish` for a surface.
- `AAR_DISPLAY_VSYNC` (1): flip waits for scanout acknowledgement. `AAR_DISPLAY_LATEST` (2) replaces a pending frame instead of waiting (ZZ9000 "mailbox" mode); `aar_display_latest_dropped()` returns the cumulative count of replaced frames and is valid only with `AAR_DISPCAP_LATEST` in the granted caps. LATEST is a request: without that cap the open fell back to plain VSYNC.

### 8.3 Input (`AAR_GROUP_INPUT`) and events

`event_poll` is core and always returns lifecycle events: `AAR_EV_STOP` 5, `AAR_EV_LOW_MEMORY` 8. With `AAR_GROUP_INPUT` it also returns `AAR_EV_KEY_DOWN` 1, `KEY_UP` 2, `MOUSE` 3, `JOY` 4, `RESIZE` 6 (defined, never sent in v1), `BREAK` 7.

- `code`: Amiga raw key (up bit removed), or `AAR_BREAK_CTRL_D/E/F` (4, 5, 6) for `BREAK`. `dx`, `dy`: mouse deltas, or joystick axes -1, 0, 1. `buttons`: key qualifiers, or mouse buttons in bits 0 to 2 (left, right, middle), or joystick fire in bit 0. `time_ms`: a timestamp in milliseconds that the client chooses (the library does not change it).
- Events are delivered in order; the client posts them with `AARPostEvent`, as unsolicited completion-queue entries (10.4). `AARPostEvent` passes the event to the app as given: `type`, `code`, `dx`, `dy`, `buttons` and `time_ms` arrive unchanged and the reserved word arrives as 0. The only change is the mouse merge below, where the merged event carries the sums and the `time_ms` of the latest motion. An `AAR_EV_STOP` posted with `AARPostEvent` is an event only: it does not set the stop request and does not stop the app, which `AARStop` does. A runtime delivers one `AAR_EV_STOP` for a stop request even if the client also posted one.
- Mouse motion is never lost without a count. A client library either merges it (it sums `dx` and `dy`, saturating at the `int16_t` range, while `buttons` and `code` are unchanged, and keeps the `time_ms` of the latest motion) or counts the refused event in `AAR_CB_EVENTS_DROPPED`. The reference `aar.library` merges while the completion queue is full or an event is already pending and posts the merged event as soon as there is room, so it counts a refused event only for a non-motion event or a change of buttons; another library MAY count motion instead of merging. Any other event that finds the queue full is dropped (the new event, never an old one) and counted.
- The runtime keeps at least 64 events in order. Once it holds that many that the app has not taken, it MAY discard each further input event except mouse motion, which it merges as the client does (and queues as soon as the app takes an event). It MUST NOT discard `AAR_EV_STOP` or `AAR_EV_LOW_MEMORY`, so it reserves room for them. An event the runtime discards is not counted in `AAR_CB_EVENTS_DROPPED`, which counts only events the client could not post; API 0.5 has no client-readable count of the runtime's drops. An app that polls once per frame does not lose input; the default completion queue holds 64 entries shared with completions and messages.
- The call never blocks. An app that has nothing to do uses `aar_msg_wait`, `aar_sleep_us` or `aar_yield`, not a spin.

### 8.4 Audio (`AAR_GROUP_AUDIO`)

`aar_audio` (`aar/app.h`) is filled by `aar_audio_open(rate, channels, &out)`: `frames` (capacity, a power of two), `rate`, `channels` (granted) and `flags` (none defined, 0), then three pointers at offset 16 on both profiles: `samples` (interleaved S16LE), `read_frame` (the runtime advances) and `write_frame` (the app advances). `aar_audio_close()` detaches the stream. `aar_audio_stats` (`aar_audio_get_stats`) carries a `size` that the caller sets and a cumulative `underruns` count.

- The app requests a rate and channel count; the runtime grants a supported pair and writes it into `out`. The rates are 22050, 44100 and 48000 Hz, stereo; a platform MUST support 22050 Hz. Other combinations return `AAR_E_INVAL`. (Non-normative: the ZZ9000 runtime grants 22050 Hz stereo only.)
- The ring is shared between the app and its consumer: the runtime, or on the AHI route the client library. Indices are free-running `uint32_t` counters that wrap; capacity is a power of two. The app writes ahead of `*read_frame` but never more than `frames` ahead. The app stores samples before a release store of `write_frame` and reads `read_frame` with an acquire load; it does not call `aar_publish` for the ring. The platform MUST give every consumer, the 68k client included, a mapping on which this order holds (coherent, or non-cacheable), and the library maps the ring accordingly. `channels` MUST be 2. The consumer of the ring counts underruns in the ring header (the word at 0x44): one count each time it needed frames and the ring held fewer, that is, once per starved period for a platform mixer that plays in periods and once per `AARAudioRead` that returns fewer frames than asked for on the AHI route. The unit is the consumer's, not a number of frames. The count is cumulative from open, never decreases, and `aar_audio_get_stats` (slot `audio_stats`, which may be NULL and then returns `AAR_E_NOSYS`) returns the same value as the ring word when present.
- The ring is one shared buffer; `AAR_OP_AUDIO_OPEN` returns its token, byte size and `frames` (9.6). Unlike the control block it is little-endian, because the app, the runtime and the S16LE samples are, and the client library swaps the words it reads. Header (`AAR_AUDIO_RING_*` in `transport.h`): `write_frame` at 0x00 (the app), `rate` at 0x04, `channels` at 0x08 and `format` at 0x0C (`AAR_AUDIO_FORMAT_S16LE`, value 1, the only sample format in API 0.5; the runtime writes these three at open), `read_frame` at 0x40 and `underruns` at 0x44 (the consumer), samples from 0x80. Each index has its own line and one writer. The three pointers of `aar_audio` point into one mapping of the ring in the app's address space. If `ring` is the address of its first byte, `write_frame` is `ring`, `read_frame` is `ring + 0x40` and `samples` is `ring + 0x80`.
- Route is the client's decision (`AAR_TAG_AUDIO_ROUTE`). A platform with the audio group MUST support the route `AHI`: the runtime allocates the ring, names it in the `AUDIO_OPEN` request with route 1, does not play the ring itself and leaves `read_frame` to the client, which reads PCM with `AARAudioRead`. The route `PLATFORM` (the platform's own mixer or fabric) is optional: a platform without one completes `AUDIO_OPEN` for it with `AAR_E_NOSYS`, and `aar_audio_open` returns that. On the route `PLATFORM` the suite measures that the runtime advances `read_frame` at the granted rate within 25 percent over one second (C-AUDIO.5).
- Only a few streams can exist across all apps (ZZ9000: two); the open call returns `AAR_E_BUSY`.

### 8.5 GL (`AAR_GROUP_GL`)

- `const aar_gl *aar_gl_bind(uint32_t surface_id)` binds the app's one GL context to the open surface and returns a table of GL 1.1 entry points. `aar_gl_swap()` flushes the GL front end and passes the frame to the platform to copy and flip. It parks as `aar_display_flip` does, but it MAY return before the frame is visible (a pipelined present). A platform keeps at most two swapped frames in flight, the frame passed in by this swap included. When a swap returns, every frame passed in by an earlier swap except the most recent one is visible, so an app that needs its last frame visible without drawing another calls `aar_gl_swap` twice more. A pipelined platform SHOULD also present a queued frame when the app next parks (`aar_yield`, `aar_sleep_us`, `aar_msg_wait`) and at `aar_gl_unbind`. A frame still queued when the app ends MAY never be shown. `aar_gl_unbind()` releases the context.
- The context has a depth buffer of at least 16 bits (`glGetIntegerv(GL_DEPTH_BITS)` returns 16 or more), because no call selects a pixel format. `GL_DEPTH_TEST`, `glDepthFunc`, `glDepthMask` and `glDepthRange` work as in GL 1.1. A platform that cannot provide such a buffer does not set `AAR_GROUP_GL`.
- `aar_gl` ([`aar/gl.h`](../include/aar/gl.h)) starts with `uint32_t size; uint32_t version;` and then 72 pointers (`AAR_GL_SLOT_COUNT`), so slots start at offset 8 on both profiles. `size` is the compatibility version: test every field past `Build2DMipmaps` with `AAR_GL_COVERS(gl, field)`. `version` is the GL ABI revision the runtime implements: `AAR_GL_VERSION_V0` (fields through `Build2DMipmaps`) or `AAR_GL_VERSION_ARRAYS` (adds the `Pointer`, `ClientState`, `DrawArrays/Elements` and `LockArrays` entries). The source revision of a GL module is not in the table; the ZZ9000 offers it in its extension (8.8).
- Integer parameters are `int32_t`/`uint32_t`. Perspective, Ortho, Frustum and DepthRange use `double`. `gluPerspective` takes four doubles. There is no stdcall and no GL header from Khronos or any other vendor.
- An isolated runtime MUST validate client-array pointers at draw time. For a bad range it MUST NOT draw, MUST make the next `glGetError` return `GL_INVALID_VALUE` and MUST NOT fault or stop the app. The `gl*` entries return nothing the app could use for this, so `glGetError` is the app's only signal; `AAR_E_FAULT` is the runtime's own status for the failed call.
- Debug bind flags are not part of the standard. They are in the ZZ9000 extension table (8.8): `aar_ext_zz9000.gl_bind_flags`, reached through `aar_zz9000_gl_bind_flags(surface_id, flags)`. The flags are `AAR_ZZ9000_GL_BIND_DIRECT_PRESENT` 1, `_SYNC_PRESENT` 2, `_SYNC_RIX_FEED` 4 and `_INLINE_RIX_FEED` 8, defined in `aar/ext_zz9000.h`. The wrapper also records the returned table for the `gl*` wrappers (3.8).
- Each platform has its own GL implementation. The ZZ9000's is MiniGL on RasterIX, licensed `PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later` and not part of this standard.

### 8.6 Services (`AAR_GROUP_SERVICES`)

Synchronous buffer-in, buffer-out helpers for functions a platform may do faster than the app: `aar_sha256`, `aar_decompress(algorithm, ...)`, image decode sessions (`aar_image_open/decode/close`) and audio decode sessions (`aar_audio_decode_open/decode/close`). All buffers are app memory valid for the call. Widths: byte counts of whole buffers are `uint64_t` (`aar_sha256`, `aar_decompress`, `aar_service_result`); per-call chunk sizes in the decode sessions are `uint32_t` (`aar_image_result`, `aar_audio_decode_result`). `aar_image_params` has its tile pointer last, at offset 24 on both profiles. Algorithm and codec numbers are `AAR_COMPRESS_*` (`DEFLATE_RAW` 1, `ZLIB` 2, `GZIP` 3, `LZ4_BLOCK` 4, `LZMA_ALONE` 5, `LZMA2` 6, `LH1` 7, `LH5` 8, `LH6` 9, `LH7` 10), `AAR_IMAGE_*` (`JPEG` 1, `PNG` 2, `WEBP` 4) and `AAR_AUDIO_*` (`MP3` 1, `FLAC` 2, `VORBIS` 3) in `aar/app.h` (`AAR_COMPRESS_NONE` is never valid). A runtime supports at least four sessions per kind per app (`AAR_MIN_DECODE_SESSIONS`). A further one is `AAR_E_NOMEM`, and the runtime closes sessions still open at exit. A platform may implement any subset; per-algorithm availability is `AAR_E_NOSYS` from the call. The group is an accelerator: apps can always bring their own codecs.

**Compressed formats.** `aar_decompress` takes one complete stream in `src` and writes the plain bytes to `dst`. The inputs are: `DEFLATE_RAW`, RFC 1951; `ZLIB`, RFC 1950; `GZIP`, RFC 1952; `LZ4_BLOCK`, one LZ4 block without a frame header; `LZMA_ALONE`, the 13-byte `.lzma` header (properties byte, 32-bit dictionary size, 64-bit plain size or all ones when unknown) followed by the LZMA stream; `LZMA2`, one byte holding the LZMA2 dictionary-size property followed by the raw LZMA2 chunk stream (the data of an `.xz` LZMA2 filter, without the `.xz` container); `LH1`, `LH5`, `LH6` and `LH7`, the compressed data of an LHA file of that method without the LHA header. Every stream except LHA ends itself. For those, `dst_bytes` may be larger than the plain size, `bytes_written` is the plain size, `bytes_consumed` is the length of the stream, and a destination that is too small, or a stream that is cut, is an error that writes nothing past `dst_bytes`. An LHA stream has no end marker, so `dst_bytes` MUST be exactly the plain size and the decoder produces that many bytes; `bytes_consumed` is at most `src_bytes`, and a caller does not rely on its value.

**Image sessions.** A session decodes into the caller's tile buffer (`tile`, `stride` bytes per row, `rows` rows, `tile_bytes` at least `stride * rows`), in `AAR_FMT_BGRA8888`. In API 0.5 the tile buffer MUST hold the whole image. An app MUST pass 0 in `aar_image_params.flags`. A runtime MAY accept bits it defines (the ZZ9000 accepts a fit bit and a preserve-aspect bit, listed in the appendix). `aar_image_decode(session, src, src_bytes, flags, out)` takes `src` non-NULL and `flags` bit 0, which means that the input ends with this call. Any other bit is `AAR_E_INVAL`. A call with `src_bytes` 0 is valid only with bit 0 set: it asks the decoder to emit what it still holds, which is how the app drains after the last input. The result has `image_width` and `image_height` 0 until the header is parsed. `bytes_consumed` is how much of `src` the decoder took, and the app submits the rest again. `bytes_written` is informational, and an app MUST NOT rely on its exact value. In the result `flags`, bit 0 means the header is known, bit 1 that more output is pending (call again with `src_bytes` 0 and bit 0 set), and bit 2 that the output was scaled (not used in 0.5). The image is complete after a call with bit 0 set that consumed all its input and returned bit 1 clear; the tile buffer then holds the image. The tile position and a session state are not reported in 0.5. Constants for these bits are planned for API 0.6 (appendix).

**Lossy decoders** (JPEG, WebP) are not bit exact across platforms. A platform MAY use a faster IDCT or a cheaper chroma upsampling than the reference (libjpeg with its accurate integer IDCT, libwebp). The suite accepts a difference of at most 6 per colour channel for JPEG and 3 for WebP on its test images. The standard promises no bound for other images. PNG decode is exact. MP3 and Vorbis decoders are not bit exact either: the suite accepts a root-mean-square error of at most 64 against the reference PCM (C-SERVICES.7), and FLAC decode must be exact.

### 8.7 CPU features

`aar_info.cpu_features` lists optional instruction support so an app can pick a code path without trapping:

| A32 bit | meaning | A64 bit | meaning |
|---|---|---|---|
| 0 | VFPv4/FMA | 0 | CRC32 |
| 1 | integer divide | 1 | LSE atomics |
| 2 | half-precision convert | 2 | AES |
| | | 3 | SHA1/SHA2 |
| | | 4 | dot product |
| | | 5 | FP16 arithmetic |

### 8.8 Platform extensions

`const void *aar_ext(uint32_t platform_id, uint32_t ext_id)` returns a pointer to a size-first, append-only extension table owned by the platform, or NULL. It is the only place for features that exist on one platform: ZZ9000 GL bind debug flags, ZZ9000 mailbox presentation internals, a Pi's hardware video overlay. An app that uses an extension is not portable and SHOULD say so in its documentation. `aar_api.platform_id` tells the app which platform it is on. Platform ids are allocated in the standard's repository (`AAR_PLATFORM_ZZ9000` is 1).

The ZZ9000 table is `aar_ext_zz9000` in [`aar/ext_zz9000.h`](../include/aar/ext_zz9000.h), returned by `aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)`. Two `uint32_t` words (`size`, `reserved`) come first, so the first slot is at offset 8 on both profiles. It holds `gl_bind_flags` (NULL when the GL group is absent), `build_id` (resident firmware source revision) and `gl_build_id` (GL module source revision), the strings NUL-terminated or NULL. Test fields with `AAR_EXT_ZZ9000_COVERS`.

## 9. The Amiga client library: `aar.library`

### 9.1 Conventions

- A normal Exec library, name `aar.library`, version = API major (0 until the API reaches 1.0), revision = implementation revision. Vectors are register-based (`a6` base), listed in an `.fd` file, with the usual `clib`, `pragma`, `inline` and `proto` headers generated from it. The library MUST open on every platform that implements the standard.
- Calls return a `LONG` in `d0`: `AAR_OK` (0) or a negative `AAR_E_*`. The exceptions are calls that return a mask or a pointer, noted below.
- Structures use the 68k layout of section 1.3 and begin with `size`.
- `aar.library` is a thin, portable interface. How it reaches the ARM side is up to the platform (section 10).

### 9.2 Calls

The contract is in the aar repository: [`client/fd/aar_lib.fd`](../client/fd/aar_lib.fd) (27 vectors from -30), [`client/include/libraries/aar.h`](../client/include/libraries/aar.h) (structures with 68k offsets, tags, `AAR_NOTIFY_*`), [`client/include/libraries/aar_zz9000.h`](../client/include/libraries/aar_zz9000.h) (the ZZ9000 extension) and [`docs/aar-client-api.md`](../docs/aar-client-api.md) (every call for client authors). Where this section and those files differ, please report it as an issue. The vectors:

```
AARQuery(info)(a0)
AAROpen(handlep,port,reason)(a0/a1/a2)
AARClose(handle)(a0)
AARProbeFile(path,info)(a0/a1)
AARProbeMemory(data,length,info)(a0/d0/a1)
AARSetTagList(handle,tags)(a0/a1)
AARLoadFile(handle,path)(a0/a1)
AARLoadMemory(handle,data,length)(a0/a1/d0)
AARStart(handle,argv,argc)(a0/a1/d0)
AARStop(handle)(a0)
AARStatus(handle,status)(a0/a1)
AARSendMessage(handle,message)(a0/a1)
AARGetMessage(handle,message)(a0/a1)
AARMessageSignal(handle)(a0)
AARSharedMap(handle,haddr,bytes,flags,map)(a0/d0/d1/d2/a1)
AARSharedUnmap(handle,map)(a0/a1)
AARSharedPublish(handle,map,offset,length)(a0/a1/d0/d1)
AARSharedAcquire(handle,map,offset,length)(a0/a1/d0/d1)
AARServiceNext(handle,req)(a0/a1)
AARServiceReply(handle,req)(a0/a1)
AARPostEvent(handle,event)(a0/a1)
AARAudioRead(handle,dst,frames,got)(a0/a1/d0/a2)
AARSharedToken(handle,ptr,bytes,token)(a0/a1/d0/a2)
AARExt(platform_id,ext_id)(d0/d1)
AARSetLogSink(handle,sink,user)(a0/a1/a2)
AARReadStack(handle,buf,bytes,got)(a0/a1/d0/a2)
AARReadProfile(handle,buf,max_samples,got,lost)(a0/a1/d0/a2/a3)
```

| Call | Behaviour |
|---|---|
| `AARQuery(AARInfo *)` | Fills `AARInfo`: API major and minor, `platform_id`, `profiles`, `groups`, `max_apps` (5.6), `max_image_bytes`, `max_args`, `max_arg_bytes`, `shared_granule`, `msg_capacity`, `stall_limit_ms`, `stop_grace_ms`, `shared_default_mib`, `shared_max_mib`, `weight_max`, `queue_depth`, `flags` and eight platform words. Needs no handle and starts nothing. |
| `AAROpen(AARHandle **, struct MsgPort *notify_port, ULONG *reason)` | Claims an app instance for the calling task (admission, 5.6). `notify_port` may be NULL. `reason` may be NULL; on `AAR_E_NORESOURCE` it receives an `AAR_REASON_*` code. The task's first open allocates one signal bit, which all of that task's handles share (a bit per handle would stop a task at about 12 handles). |
| `AARClose(handle)` | Stops the app if it is running and releases the claim. Only the owner may close. |
| `AARProbeFile/Memory(src, AARImageInfo *)` | Reads the note without loading and reports profile, `api_major`, `api_minor_required`, groups, stack, min heap, `note_version` and the image length. Lets a launcher choose between `.a32` and `.a64` images and fail early. No handle. It applies the image rules of 2.2 and 2.3 as the loader does: an image that breaks one is refused, and an image the loader would accept is accepted, with or without section headers. It does not judge the image against the platform: profile, groups, heap and stack are reported, not checked. It returns `AAR_E_NOENT` when the file cannot be opened, `AAR_E_IO` when it cannot be read, and `AAR_E_INVAL` for anything that is not a valid AAR image: not an ELF, a broken rule of 2.2, no note, more than one note, a note with a wrong magic or version, a note whose profile word differs from the ELF class. It never returns `AAR_E_PROFILE`: the profile is reported and the launcher chooses. On failure the structure is left unchanged. |
| `AARSetTagList(handle, tags)` | Sets options before the load: `AAR_TAG_PRIVATE_MIB` (cap on private memory, 0 for the pool), `AAR_TAG_SHARED_MIB` (shared limit, 0 for the default), `AAR_TAG_WEIGHT` (CPU share weight, 0 for the default 1; `AAR_E_NOSYS` where the platform has no weighting), `AAR_TAG_AUDIO_ROUTE`, `AAR_TAG_LEASE_EXTENDED` (on by default), `AAR_TAG_CLIENT_CAPS`, `AAR_TAG_PROFILE_HZ` (samples per second of the app's pc, 0 for none, at most `AAR_PROFILE_HZ_MAX` (10000); a platform without a profiler accepts it and grants 0, and a platform may grant a lower rate, which `AARStatusInfo.profile_hz` reports). Returns `AAR_E_BUSY` once a load has begun, `AAR_E_INVAL` for an out-of-range value (limits above 4095 MiB), `AAR_E_NOSYS` for an option the platform cannot support (a missing `PLATFORM` audio route shows at `aar_audio_open`, not here). Launcher policy (root directory, absolute paths, console redirection, screen mode) is configuration of the serving loop (`aar_serve`, 9.6) and not a library option. |
| `AARLoadFile(handle, path)`, `AARLoadMemory(handle, data, length)` | Loads the image. Maximum size reported by `AARQuery` (at least 16 MiB). Waits are progress-aware: a load that keeps making progress is not timed out; one that stalls is, after about 5 s. The result codes are those of 5.2. |
| `AARStart(handle, argv, argc)` | Starts with up to `max_args` strings (`argv[0]` included) of up to `max_arg_bytes`. The first is the program name. |
| `AARStop(handle)` | Requests a stop of the generation this handle loaded. |
| `AARStatus(handle, AARStatusInfo *)` | Snapshot: state, generation, end reason, exit code, fault data, load base, profile, the stack snapshot (`stack_sp`, `stack_bytes`) and the profiler's counters (`profile_hz`, `profile_samples`, `profile_dropped`). |
| `AARSendMessage`, `AARGetMessage` | 32 bytes. `AAR_E_BUSY` when the queue toward the app is full or no message from the app is queued. |
| `AARMessageSignal(handle)` | Returns the signal mask (`ULONG`) of the handle's owner task, shared by all of that task's handles; 0 if the handle is invalid. Set when an app of that task sends a message, exits, faults, or queues a service request; after the signal the client checks each of its handles. |
| `AARSharedMap`, `Unmap`, `Publish`, `Acquire` | 9.5. |
| `AARServiceNext`, `AARServiceReply`, `AARPostEvent`, `AARAudioRead` | 9.6. |
| `AARSharedToken(handle, ptr, bytes, &token)` | The token of a range of client memory inside the platform's shared aperture, the reverse of `AARSharedMap`. The client uses it to name its own memory to the app in a reply: the surfaces of a `DISPLAY_OPEN` reply (8.2, 9.6). For the base of a range that `AARSharedMap` mapped, the token equals `map.haddr`. `AAR_E_INVAL` for NULL, zero bytes or an address outside the aperture; `AAR_E_NOSYS` where the platform cannot name host memory to the app. |
| `AARExt(platform_id, ext_id)` | Returns a platform extension table (size-first, append-only) or NULL. No handle. The ZZ9000 table: claim list, administrative release, stats, lease pause (`aar_zz9000.h`). |
| `AARSetLogSink(handle, sink, user)` | Installs the function that receives the app's log records (`AAR_OP_LOG`), one call per record, in the owner task, from inside `AARServiceNext` or `AARGetMessage`. A new handle has none, and a record that arrives while there is none is dropped. It needs no platform support and works the same on every platform. |
| `AARReadStack(handle, buf, bytes, &got)` | Copies the stack snapshot of a fault or stall end (5.3), at most `bytes` bytes, and sets `got`. With `bytes` 0 it only sets `got` to the size of the snapshot. `AAR_E_NOENT` when there is none. |
| `AARReadProfile(handle, buf, max_samples, &got, &lost)` | Copies the pc samples that the platform has written since the previous call, at most `max_samples`, as big-endian 64-bit app addresses, oldest first. `lost` is how many were overwritten in the platform's ring before the call took them. With `max_samples` 0 it only sets `got` to the number waiting. `AAR_E_NOSYS` when the platform granted no profile. |

### 9.3 Handle ownership

A handle belongs to the task that called `AAROpen`. Only that task can use or close it; another task gets `AAR_E_OWNER`. If the owner exits without `AARClose`, the library releases the handle (5.5). A task may hold as many handles as admission allows. Closing the last `aar.library` base of a task closes all its handles.

### 9.4 Notification

- Every owner task has one signal bit, allocated at its first `AAROpen` and freed when the task closes its last `aar.library` base (`CloseLibrary`), not at its last `AARClose`. The mask `AARMessageSignal` returns is valid until then. It is set when one of the task's apps has sent a message, ended (exit or fault), or queued a service request, and the handles of a task share it. A client with several handles checks them all after the signal, or gives each handle its own notify port, whose `AAREvent` names the handle.
- If a `notify_port` was given, the library posts one `AAREvent` (188 bytes, never replied; its `handle` field names the handle) there at a time. It is posted only after the previous one has been taken off the port. Events coalesce: the type is `AAR_NOTIFY_MESSAGE` if a message is waiting, else `AAR_NOTIFY_COMPLETE` or `AAR_NOTIFY_FAULT` when the app has ended, else `AAR_NOTIFY_REQUEST`.
- Order of use: take the event off the port, then drain messages with `AARGetMessage` until `AAR_E_BUSY`. Before `AARClose`, drain the port and then the message ring.
- The library SHOULD wake the owner from the platform's doorbell where there is one, and from a timer or vertical-blank poll of the queue indices where there is not (10.6). The ZZ9000 library polls at the vertical blank and visits only handles with work. That is client-side work, or an armed owner whose group the platform's event counts (10.6) say has changed.

### 9.5 Shared buffers on the client

`struct AARSharedMap` (32 bytes: `size`, `base`, `bytes`, `haddr`, `flags`, `granule`, `reserved[2]`; `reserved[0]` belongs to the library) has no typedef, because the call of the same name would clash with it. The `flags` argument of the call is reserved (pass 0); `map.flags` reports `AAR_MAPF_UNCACHED` or `AAR_MAPF_COHERENT`.

- `AARSharedMap(handle, haddr, bytes, flags, &map)`: turns a token plus a byte count into a 68k pointer. It MUST check the range against the platform's shared aperture and SHOULD check it against the handle's current generation where the platform can tell. The library does not check that the range lies inside one live block of the app: the client side has no list of blocks. A client that maps more than the block named in the message gets whatever lies behind it, and MUST NOT write outside the block. It sets the map cache-inhibited when it can (ZZ9000: `mmu.library`) and reports `AAR_MAPF_UNCACHED`; where the platform is coherent end to end it reports `AAR_MAPF_COHERENT`. Failure: `AAR_E_INVAL`, `AAR_E_NOMEM`, `AAR_E_NOSYS` (cannot map), `AAR_E_STALE`.
- `AARSharedUnmap(handle, &map)`: releases the mapping. Maps are valid only until Unmap, `AARClose`, or the end of the app. After `AARStatus` reports an end state the client MUST unmap and MUST NOT write through a map, because the memory can belong to another app. The library SHOULD revoke maps where the platform allows.
- `AARSharedPublish`, `AARSharedAcquire(handle, &map, offset, length)`: rules in 4.3. The library rounds to `granule`. They are no-ops when the map is uncached or coherent and are cheap to call anyway.
- `AARSharedToken` accepts an address only if it lies in the platform's shared aperture. Where the aperture is not ordinary Amiga memory (the ZZ9000: the card's memory), the pixel memory of a bitmap that the platform's own display driver allocated is in it and `AllocVec` memory is not. A platform MUST document which Amiga memory is in its aperture. A client-side allocator for shared memory is planned for API 0.6 (appendix).

### 9.6 Services: how the app's files, console, display, input and audio calls reach the client

The app's service calls become *requests* in the app's queue pair (section 10.4); most of them park the app until the client replies. `aar.library` decodes a request entry into an `AARServiceReq`.

`AARServiceReq` is 96 bytes ([`client/include/libraries/aar.h`](../client/include/libraries/aar.h)): `size`, `op`, `id`, `result`, `arg[6]`, `out[6]`, `buf`, `buf_off`, `buf_len`, the entry's `flags` (in), `reply_flags` (out: `AAR_DISPCAP_*` for `DISPLAY_OPEN`, `AAR_CFLAG_*`), `ptr` (the library-mapped `buf + buf_off`, NULL when `buf` is 0) and `reserved[2]`.

| Op | Code | In (`arg[]` and buffer) | Out (`out[]` and buffer) |
|---|---|---|---|
| `AAR_OP_FILE_OPEN` | 0x10 | path in buf; arg0 fd chosen by the runtime, arg1 flags (`AAR_FILE_*`) | result |
| `FILE_CLOSE` | 0x11 | arg0 fd | |
| `FILE_READ` | 0x12 | arg0 fd, arg1 length | bytes go to buf; out0 count |
| `FILE_WRITE` | 0x13 | arg0 fd, arg1 length; bytes come from buf | out0 count |
| `FILE_SEEK` | 0x14 | arg0 fd, arg1 and arg2 offset (high, low), arg3 whence | out1 and out2 position (high, low) |
| `FILE_STAT` | 0x15 | path in buf | out0 and out1 size, out2 and out3 mtime (high, low), out4 mode |
| `LIST_OPEN` | 0x16 | path in buf; arg0 list id | |
| `LIST_NEXT` | 0x17 | arg0 list id | name goes to buf; out0 mode; `AAR_E_NOENT` at the end |
| `LIST_CLOSE` | 0x18 | arg0 list id | |
| `CONSOLE_WRITE` | 0x20 | the bytes sit in a console buffer named by buf, buf_off and buf_len | out0 count consumed |
| `CONSOLE_READ` | 0x21 | buf_len is the room; the client fills it | out0 count (0 at end of input) |
| `DISPLAY_OPEN` | 0x30 | arg0 to arg3: width, height, format, flags | out0 surface id, out1 pitch, out2 layout, out3 to out5 front, back and spare tokens (the client's bitmap memory); the reply's entry `flags` carry `AAR_DISPCAP_*`. A reply with no back token names one buffer |
| `DISPLAY_FLIP` | 0x31 | arg0 id, arg1 flags (`AAR_FLIPF_ASYNC`: the app does not wait) | replied when shown; out0 to out4: front, back, pitch, layout, dropped (cumulative frames replaced in latest mode). A platform that owns the display buffers may complete a flip itself, so the client sees no entry (the ZZ9000 LATEST mode) |
| `DISPLAY_PALETTE` | 0x32 | arg0 id, arg1 first, arg2 count; colours in buf as big-endian `0x00RRGGBB` words. Does not park the app. The runtime keeps buf valid and unchanged until completion; queued updates may coalesce if the latest value per changed index wins and untouched indices are preserved. | |
| `DISPLAY_CLOSE` | 0x33 | arg0 id | |
| `AUDIO_OPEN` | 0x40 | arg0 rate, arg1 channels, arg2 route (`AAR_AUDIO_ROUTE_PLATFORM` 0, `AAR_AUDIO_ROUTE_AHI` 1); the runtime allocates the audio ring as a shared buffer first and names it in buf, buf_len | out0 granted rate, out1 granted channels, out2 ring token (echoes buf), out3 ring bytes, out4 frames; the ring is freed on close |
| `AUDIO_CLOSE` | 0x41 | | |

`AAR_OP_MSG` (1), `AAR_OP_EVENT` (2) and `AAR_OP_LOG` (3) are not requests (10.4). The argument and result word indices are the `AAR_A_*` and `AAR_R_*` constants in `transport.h`. The display caps of the `DISPLAY_OPEN` reply travel in `reply_flags`.

**Which requests park the app.** A request parks the app until its completion arrives, except these three, which return to the app at once: `CONSOLE_WRITE` (the runtime copies the bytes into its own console buffer and posts the request), `DISPLAY_PALETTE`, and `DISPLAY_FLIP` with `AAR_FLIPF_ASYNC`. Every other op (the file and list ops, `CONSOLE_READ`, `DISPLAY_OPEN`, `DISPLAY_CLOSE`, a flip without `AAR_FLIPF_ASYNC`, `AUDIO_OPEN`, `AUDIO_CLOSE`) parks the app. `AAR_OP_MSG`, `AAR_OP_EVENT` and `AAR_OP_LOG` never park it. A call can also wait for a reason other than its own request (a full console buffer, a flip still pending); that is a wait of the call, not a request that parks the app.

- Messages are queue entries too (`AAR_OP_MSG`) but are handled by `AARGetMessage` and `AARSendMessage`, not by `AARServiceNext`. Events (`AAR_OP_EVENT`) and log records (`AAR_OP_LOG`) take no completion either.
- Messages use the credits and backpressure rules in 10.4; a full receiver returns `AAR_E_BUSY`, without dropping a message.
- `AARServiceNext(handle, &req)` returns `AAR_OK` with a request, or `AAR_E_BUSY` when none is waiting. The handle's signal tells the client when to call it. `AARServiceReply(handle, &req)` posts the completion entry; it resumes the app only if the app is parked for that request. Non-parking requests still require completion to release their staging storage.
- Payloads are shared buffers named by token. The client reaches them with `AARSharedMap`; the library keeps each map until the app frees the buffer, so the console ring or a file buffer is mapped once and reused. A published request borrows every range the client may still access, including runtime staging storage. Those ranges MUST remain allocated and MUST NOT be repurposed until the completion with the original `op` and `id`, or a terminal release after client quiescence. Request input remains unchanged unless the operation defines a concurrent ring/stream protocol; output ranges cannot be reused for another operation while borrowed. A timeout, cancelled wait or missed doorbell is not a completion. Wrong-id, wrong-op and duplicate replies cannot release another request's storage. `AARServiceReply` returns `AAR_E_INVAL` for such a reply and changes nothing: the app is not resumed and no storage is released. The completion of a request the app has already given up on (a timeout, 7.1) is not such a reply: it is accepted and releases the storage.
- Request completion does not by itself end a longer-lived resource session. An AHI consumer may retain the audio ring after AUDIO_OPEN completes. Failed AUDIO_CLOSE, including failure before publication, MUST retain the ring while that consumer can access it; release requires confirmed close/quiescence or terminal claim release. Expiry alone is not proof that the consumer has stopped.
- `AARPostEvent(handle, const aar_event *)` pushes an input event (key, mouse, joystick, stop request) as an unsolicited completion-queue entry; it takes the app's event structure from `aar/app.h`. Mouse motion is merged or counted as in 8.3 (the reference library merges, and the dispatcher posts a merged event as soon as there is room); a full queue that drops an event counts it in `AARStatusInfo.events_dropped`. `AARAudioRead(handle, dst, frames, &got)` copies PCM that the app has written, for the AHI route; `frames` 0 returns the frames available.
- The serving loop is `aar_serve.c` (MIT), which the SDK provides. It handles every request op with dos.library, Intuition, P96 and AHI, and returns when the app ends. `aar-run` is `AAROpen`, `AARSetTagList`, `AARLoadFile`, `AARStart`, then that loop, then `AARClose`. Any other Amiga program can embed the loop or write its own. The library does not know which ops a client serves. A client that serves only some ops MUST complete the others with `AAR_E_NOSYS` itself, because every request needs a completion to release its storage; `aar_serve` does this for the groups its configuration leaves out. A client SHOULD also answer an op code it does not know with `AAR_E_NOSYS`. A request that nobody completes leaves the app parked until it stops or its own wait ends.

### 9.7 Status structure

`AARStatusInfo` is 160 bytes and `AAREvent` 188 bytes (`struct Message`, `type` `AAR_NOTIFY_*`, `handle`, then an `AARStatusInfo`); `AARMessage` is 32 opaque bytes; `AARInfo` is 128 bytes and `AARImageInfo` 48. The layouts, with their 68k offsets, are in [`client/include/libraries/aar.h`](../client/include/libraries/aar.h), checked by the aar CI on 64-bit and 32-bit hosts and in the m68k toolchain.

## 10. The transport contract and implementing the standard on a platform

A platform provides the ARM runtime (loader, table, limits), the host-side transport, and `aar.library` on top of the transport. The library's code above the transport is shared. The transport is the same on every platform: per app one control block and one request/completion queue pair in shared memory, large payloads as shared-buffer tokens, and a doorbell. The layouts are pinned in [`aar/transport.h`](../include/aar/transport.h) as byte offsets (not C structures), with a reference producer and consumer model that the firmware, the library and the host tests share; [`docs/transport.md`](../docs/transport.md) explains them. Where this section and the header differ, the header and its test (`tests/transport_model_test.c`) are right.

### 10.1 What a platform supplies

1. Shared memory that the ARM runtime and the client both reach. It is allocated per app at admission (control block and queues) and per buffer on use (4.2). Its coherency is the platform's job, or the platform provides real `publish`/`acquire` (4.3).
2. A doorbell in each direction (10.6): a hint that something changed. Queues stay authoritative, so a platform that can only poll is correct.
3. A client mapping: a way for the library to turn a token into a 68k pointer and to learn its cache mode (`resolve`, 9.5).

### 10.2 Handles and admission

Claim is a platform control call that must reply (ZZ9000: a mailbox call). It either refuses with `AAR_E_NORESOURCE` and an `AAR_REASON_*` code (5.6), or returns the handle and the tokens of the control block and queue pair. A handle is a `uint32_t`: the process-record index in the low 16 bits and a generation in the high 16 bits (`aar_handle_make`, `aar_handle_index`, `aar_handle_generation`). Handle 0 is invalid, and generation 0 is never issued. A platform that reuses an index advances the generation with `aar_handle_next_gen` (1 to 0xFFFF, then 1 again), so every issued handle is non-zero. That is the transport word. The client API takes a pointer: a NULL pointer, a closed handle or any pointer that is not a live handle of the library is `AAR_E_STALE`, and nothing is changed. Only `aar_handle_check` at the transport returns `AAR_E_INVAL` for word 0 or generation 0. The index selects a process record that the platform allocates at admission; the count of live apps is bounded by resources, not by a constant. The generation changes each time an index is reused, so a stale handle is detected: `aar_handle_check` returns `AAR_E_INVAL` for handle 0 or generation 0 and `AAR_E_STALE` for a generation that is not the live one, and a request that names a stale handle changes nothing. A generation wraps after 65,535 reuses of one index. A platform need not retire the index at the wrap: a handle word that old names the live app again. The library does not use a handle after it has learned of its release or revocation, so only a program that keeps a raw handle word across that many claims is affected.

Recommended order: process record and translation tables first, then the control block and queues from shared memory; the commit for the image and the shared limit are reserved when the load begins. Everything reserved is released on every failure path.

### 10.3 Control block

The control block is 4 KiB (`AAR_CB_BYTES`), zero-filled at admission, with layout version `AAR_CB_LAYOUT_VERSION` (1). The layout is a set of byte offsets in `aar/transport.h` (`AAR_CB_*`), not a C structure; [`docs/transport.md`](../docs/transport.md) lists every word. Every word has exactly one writer. Each queue index and the lease counter sits at the start of its own 64-byte line, so two writers never share a line. 64-bit values are two words, high word first. The platform writes the magic last (`aar_cb_init`), so a reader that sees it sees the rest, and a client checks the block with `aar_cb_check` before it uses it.

| Offset | Contents | Writer |
|---|---|---|
| 0x000 header | magic, layout version, size, handle, profile | platform, at admission |
| 0x040 lifecycle | state, end reason, exit code, flags (`AAR_CBF_DISPLAY`, `_AUDIO`, `_GL`, `_CONSOLE`), load progress (64-bit, advances at every step), lease period, message-consumed counter at 0x05C | platform |
| 0x080 requests | stop request, client present | client |
| 0x0C0 lease and client accounting | lease counter (its own line), events dropped, CQ full | client |
| 0x100 fault record | class, syndrome, pstate, pc, sp, lr, addr, load base (64-bit values) | platform |
| 0x140 limits | private cap (MiB), shared limit (MiB), weight, profile rate (Hz); 0 selects the default (for the profile rate, off); set before the load begins | client writes, platform reads at load begin |
| 0x180 accounting | shared bytes used, private bytes committed (64-bit), SQ full count, log records dropped | platform |
| 0x1C0 to 0x33F queues | per queue a descriptor line (platform, at admission), a tail line (the producer) and a head line (the consumer); SQ at 0x1C0, 0x200, 0x240, CQ at 0x280, 0x2C0, 0x300 | as stated |
| 0x340 stack snapshot | app address of the first byte (64-bit), size in bytes (written last, 0: none); the bytes themselves at 0x400, at most 512 | platform, with the fault record |
| 0x380 profile | granted rate (written last, 0: no profile), token and size of the sample ring, ticks dropped | platform, at load begin |
| 0x3C0 profile head | count of samples written, one word, modulo 2^32 | platform |

There is one shared-limit word: `AAR_CB_SHARED_MIB` limits all shared (host-visible) memory, including the runtime's own service buffers, and there is no separate host-visible limit word. The four queue counters have fixed writers: events dropped and CQ full are written by the client (its own tally of what it could not post, not of what the runtime later discards), SQ full and log dropped by the platform.

The stack snapshot (`AAR_CB_STACK_*`) is written for `AAR_END_USER_FAULT` and `AAR_END_STALL` before the state and the end reason change; the fault record is written for both too (5.3). The profile words (`AAR_CB_PROFILE_*`) describe a ring of big-endian 64-bit app pcs in a shared buffer that the platform allocates when the load begins, if the client asked for a rate in `AAR_CB_PROFILE_HZ` and the platform has a profiler. Sample number `n` is at byte `(n mod count) * 8` of the buffer, `count` being the ring size in bytes over 8, a power of two. The head is one 32-bit word, the number of samples written modulo 2^32, stored after the sample with a release barrier; a reader works in modular arithmetic and treats a head more than 2^31 ahead of what it has taken as corrupt. The platform may already be writing the slot of the next sample, which holds the oldest one, so a reader trusts only the `count - 1` newest samples (those with index at least the head, read after the copy, minus `count - 1`); older ones it had not taken, or took while the head moved past them, are lost, and it counts them itself. The word at 0x380 is written last, so a reader that reads a rate other than 0 may use the token and the size. A platform without a profiler leaves them 0. [`docs/transport.md`](../docs/transport.md) lists every word.

### 10.4 Queue pair

The submission queue (SQ) carries entries from the app side to the client; the completion queue (CQ) carries entries from the client to the app. Each holds `depth` entries of `AAR_ENTRY_BYTES` (64) bytes. `depth` is a power of two from 64 (`AAR_QUEUE_DEPTH_MIN`, the least that keeps `msg_capacity` at least 32) to 4096 (`AAR_QUEUE_DEPTH_MAX`); the default is 64. The indices are free-running `uint32_t` that wrap: used entries are `tail - head`, the slot of index `n` is `n & (depth - 1)`, and a count above `depth` means the block is corrupt (`AAR_E_IO`). Every reader of the indices MUST apply this rule. The client library fails the call with `AAR_E_IO`, and a serving loop stops the app; the runtime fails the call or wait that found it with `AAR_E_IO`. A conforming writer never produces such a count, so the rule is checked against the reference model (`tests/transport_model_test.c`), not by the conformance suite. The producer writes the tail and the consumer writes the head. A queue is described by a descriptor in the control block (10.3): the token of the shared buffer that holds the ring, the byte offset of the first entry in that buffer (token plus offset), the depth and the entry size. The default pair is one 4 KiB-aligned shared buffer holding the control block, then the SQ ring at offset 0x1000, then the CQ ring (offset 0x2000 at depth 64, `0x1000 + depth * 64` in general), 0x3000 bytes in all at depth 64.

Ordering. Producer: read the consumer's head, run an acquire barrier, check for room, write the entry, run a release barrier, write the tail. Consumer: read the tail, run an acquire barrier, read the entry, run a release barrier, write the head. The release barrier before the head write is needed because ARMv7 and ARMv8 may reorder a load with a later store to a different address: without it the producer could see the freed slot and overwrite an entry the consumer is still reading. The producer's acquire after it reads the head covers the other side. On the 68k the barriers are no-ops. The reference model in `transport.h` (`aar_queue_*`) implements these rules with caller-supplied barrier hooks.

The app is single-threaded, so at most one service request parks it at a time. Non-parking console writes, palette updates and asynchronous flips can leave additional requests in flight (9.6). A message post keeps one entry free for a service call (`AAR_MSG_RESERVE`), so `msg_capacity` is `depth - 1`: 63 at the default depth.

For client-to-app message credits, the platform writes `AAR_CB_MSG_CONSUMED` (0x05C), the free-running count of messages the app has taken from its inbox, not merely moved out of the CQ. The client posts only while `posted - consumed < msg_capacity`, otherwise returning `AAR_E_BUSY`. The CQ and runtime inbox together then contain at most `msg_capacity` unread messages, so the pump can always reach a completion. `aar_cb_post_msg` implements the client-side rule.

The app-to-client direction uses SQ backpressure: when the client library's inbox (the messages it has taken off the SQ and holds for `AARGetMessage`) is full, it leaves the next message in the SQ rather than popping and dropping it. The app observes the unchanged head and eventually gets `AAR_E_BUSY`. A request behind that message waits until the client takes messages; the client must keep draining messages while serving an app that uses them.

The entry layout is pinned in `transport.h` (`AAR_ENTRY_*`); all fields are big-endian (1.3). The first 16 bytes hold `op` (`uint16`), `flags` (`uint16`, per op, 0 when none is defined), `id` (`uint32`, chosen by the producer and echoed in the completion), `result` (`int32`, in completions) and a reserved word; the last 48 bytes are the body. The producer clears the whole slot before it writes, so reserved bytes are 0. A request body holds `arg[6]`, then `buf` (a shared-buffer token), `buf_off` and `buf_len`; a completion body holds `out[6]`; a message entry carries the 32 message bytes unswapped; an event entry carries the `aar_event` fields, each big-endian, and has no matching request.

Operation codes: `AAR_OP_NONE` 0 (never valid), `AAR_OP_MSG` 1 (either queue), `AAR_OP_EVENT` 2 (CQ only, unsolicited, id 0), `AAR_OP_LOG` 3 (SQ only), the file ops 0x10 to 0x18, console 0x20 and 0x21, display 0x30 to 0x33 and audio 0x40 and 0x41 (table in 9.6). A request is an SQ entry that expects a completion with the same `op` and `id`; MSG, EVENT and LOG take no completion. `AAR_OP_LOG` is one-way and lossy: `buf`, `buf_off` and `buf_len` name one record (at most `AAR_LOG_MAX` bytes, never wrapping) in a shared log buffer that the app side may overwrite once the client has consumed the entry. A post that finds a queue full does not block: the platform counts a full SQ in `AAR_CB_SQ_FULL` and a dropped log record in `AAR_CB_LOG_DROPPED`, and the client counts a full CQ in `AAR_CB_CQ_FULL` and a dropped event in `AAR_CB_EVENTS_DROPPED`. A full SQ is a request or log post that finds no free entry. A message post refused because only the reserve (`AAR_MSG_RESERVE`) is left is not a full SQ and is not counted: the app sees `AAR_E_BUSY`. The runtime input queue retains at least 64 events (8.3); the default CQ also has 64 entries shared with completions and messages.

### 10.5 Visibility of the control structures

On the ZZ9000 the control block and queues come from the ordinary host-visible heap with the normal write-back shareable mapping, which the Zorro master reaches coherently through the ACP. The ARM side orders data before the index with barriers and needs no cache maintenance; the 68k side maps them cache-inhibited. A platform that is not coherent MUST map these structures non-cacheable on the ARM side or use real `publish`/`acquire` internally. The ZZ9000 checked this on the card with a probe of the control structures' own writer pattern (64-entry by 64-byte rings plus a control block, written and read from both ARM cores and read and written cache-inhibited by the Amiga), which ran with zero errors on both sides; a negative control with the shared bit cleared failed at once.

### 10.6 Doorbell

A doorbell is a coalescing hint. The client rings it after posting a completion entry or freeing submission space; the runtime rings it after posting a submission entry. A missed or merged ring MUST NOT lose an entry, because the receiver always re-reads the queue indices. On the ZZ9000 the client-to-ARM ring is a mailbox write that names the handle (one per handle, coalesced in the library). The ARM-to-client direction is a vertical-blank poll, narrowed by event counts.

**Event counts (optional).** A platform MAY publish monotonically increasing 32-bit counts that a client reads as a hint: a global count and group counts (on the ZZ9000, 64 bytes, big-endian, one group per 32 records, 15 groups), each bumped after the platform makes a queue entry or an end visible, with the visible store first. Counts only increase, so the client never clears anything the platform could be setting. A client that sees the global count unchanged MAY skip every armed handle with nothing pending; a client that ignores the counts is still correct, because the queues stay authoritative. On the ZZ9000 the QUERY reply carries the block's board offset (0: none). No Amiga interrupt is defined; on the ZZ9000 the vertical-blank poll with event counts costs one card read per blank when nothing happened.

### 10.7 What a library port implements

[`docs/porting.md`](../docs/porting.md) is the implementer's guide for all of section 10: what a platform provides, the reference loader [`loader/`](../loader), the entry contract, the transport and the conformance run.

The internal transport interface (not part of the standard; named here so implementers can reuse the library core):

| Operation | Purpose |
|---|---|
| `claim(&handle, &control, &queues, &reason)` | admission; returns the handle and the tokens, or a refusal with a reason |
| `release(handle)` | free the record, tables, control block and queues after teardown |
| `load_begin/chunk/end(handle, ...)` | stream the image through a shared buffer, with progress in the control block |
| `start(handle, argv)`, `stop(handle)` | control calls (ZZ9000: mailbox) |
| `status(handle)` | read the control block |
| `queue_post/pop(handle, queue, entry)` | produce and consume entries with the barriers of 10.4 |
| `resolve(token, bytes, &ptr, &flags)` | shared aperture; sets the 68k cache mode |
| `cache_publish/acquire(ptr, len)` | 68k data cache maintenance, or no-op |
| `lease_renew(handle)` | called from the timer or interrupt context |
| `doorbell_ring(handle)`, `doorbell_arm(handler)` | the doorbell of 10.6, where the platform can raise an interrupt or signal |

As a worked example, this is how the ZZ9000 meets each part of the standard (non-normative). Section 13 asks the Emu68/PiStorm and CM5 developers how each row looks on their platform.

| | ZZ9000 |
|---|---|
| Profile | A32 (Cortex-A9, core 1) |
| Host reaches app memory | host-visible heap through a Zorro III window, board offset as token |
| Control block and queues | host-visible heap, ordinary write-back shareable mapping, ACP |
| Coherency | ACP, coherent for the bus. The 68k cache is the client's |
| `aar.library` | thin library over the mailbox claim and doorbell and the queue pair |
| Doorbell | mailbox write to the ARM side; vertical-blank poll back |
| Display | P96 on the card, surface in VRAM |
| Audio | card audio fabric or AHI |
| GL | MiniGL on RasterIX |

The ZZ9000 mailbox opcodes are private to its firmware and library; they are not part of the standard.

## 11. Conformance

### 11.1 What passing means

A platform conforms to a profile at a given API major.minor when:

1. it passes every test of a MUST rule (a rule without a keyword counts as MUST, see Normative words) for that profile and every group it declares;
2. it does not declare a group it does not pass;
3. the suite is run with the platform's own `aar.library`, the unmodified conformance payloads, and results are written in the standard format (`AAR-CONF <id> PASS|FAIL|SKIP <detail>` per test, plus the suite version and the platform's `AARInfo` dump);
4. every SKIP has a reason, and SKIP is allowed only for tests of an undeclared group, tests marked SHOULD, and the MUST audio tests that judge the optional route `PLATFORM` when `AUDIO_OPEN` for that route returns `AAR_E_NOSYS` (the reason starts with "no PLATFORM route", 8.4).

The host runner (SDL) passes the suite for the core group and serves as the reference for app semantics. It does not count as a platform.

### 11.2 The suite

The suite is in aar [`conformance/`](../conformance) (MIT). It tests API 0.5; [`conformance/README.md`](../conformance/README.md) says how to build and run it.

- [`conformance/TESTS.md`](../conformance/TESTS.md) lists every test id, grouped in areas (C-ELF, C-ENTRY, C-TABLE, C-MEM, C-SHARED, C-MSG, C-QUEUE, C-FILE, C-CON, C-TIME, C-LIFE, C-CLIENT, C-DISPLAY, C-INPUT, C-AUDIO, C-GL, C-SERVICES, C-FREE). Each row gives the spec section, MUST, SHOULD or MAY, group (core, display, audio, gl, services, input, or the `zz9000` extension), profile, and who judges it: payload, runner, operator, or none. A row judged by none says why (no observable effect, or nothing in the API to see it with). The rows for concurrent apps (admission, release, CPU share; 5.6) are in C-QUEUE: they use the claim and the queue pair of section 10. C-LIFE holds the rows for one app.
- The payloads are ARM apps (`conf_<area>.elf`) that judge their area from the inside and print `AAR-CONF <id> PASS|FAIL|SKIP <detail>`, then `AAR-CONF-END <area> pass= fail= skip=`. The runner passes facts in argv words (`info:<key>=<n>`, `conf:peer=1`), and peer steps use `CONF-<AREA>-*` message keywords.
- The runner `aar-conf` is an m68k program that uses only `aar.library` and `libaarserve.a`. Its options are `DIR PROFILE TO ONLY ROOT INTERACTIVE`. It dumps `AARInfo` (`AAR-CONF-INFO` lines), runs the payloads, plays the client side, judges the runner tests, and ends with `AAR-CONF-SUMMARY pass= fail= skip= verdict=CONFORMS|DOES-NOT-CONFORM|PARTIAL` (`PARTIAL` for an `ONLY` run).
- `conformance/negative/` holds the corpus: the images (A32 and A64) for C-ELF, generated by `gen_negative.py`, with a `MANIFEST` of the allowed refusals.
- Four tests need a person: C-LIFE.24 (reset, a MUST), C-DISPLAY.17 (picture), C-INPUT.11 (keys and mouse) and C-AUDIO.12 (tone). Without `INTERACTIVE` they are `SKIP manual`. Because C-LIFE.24 is a MUST, a run without an operator cannot say CONFORMS.
- The host runner and qemu run the payloads marked `.host` and `.qemu`, and a `SKIP host:` line is allowed only there. CI builds all payloads for both profiles and runs the runner's m68k build.
- Rules 4 and 5 of 5.5 (stale handles, late writes) can only be provoked through a platform's own administrative call, because the standard has no call that revokes a claim or pauses a lease. A platform that has such calls puts them in its extension table (8.8) and runs the tests under its platform group (the ZZ9000: C-LIFE.22 and C-LIFE.23). A platform without them says in its conformance report that these two rules are not tested. Section 13 asks whether the platforms can offer one common extension for this.

The suite has been run on a ZZ9000 (A32). A64 has no platform yet: its payloads run under qemu in aar CI, and the `aar-conformance-bundle` CI artifact holds the runner, the A32 and A64 payloads and the corpus for a platform developer to run (`PROFILE=a64`).

### 11.3 Questions the suite raised

Writing the suite turned up 66 places where this text was unclear, silent or contradicted itself. They are numbered 1 to 66 at the end of [`conformance/TESTS.md`](../conformance/TESTS.md), each with the reading the test takes, and each is now settled: the text of the section below says what the reading is, and TESTS.md marks the question with the section that settles it. The numbers stay stable. The table lists where each was settled.

| Section | Questions |
|---|---|
| Normative words, 11.1 | 1 |
| 2.2, 2.4 | 39, 61; 39 |
| 3.2 | 3, 33 |
| 3.3, 3.4, 3.5 | 32; 35, 55; 10, 15, 37 |
| 3.7 | 30, 57 |
| 4.1, 4.2 | 5, 26, 34, 40, 55, 60; 29, 60 |
| 4.3, 4.4 | 7; 6, 41 |
| 5.1 to 5.6 | 13, 27; 60; 11; 12; 28, 42, 58; 19, 31, 60 |
| 6 | 47 |
| 7.1, 7.2, 7.3 | 2, 9, 10, 36, 37, 56, 57; 44, 46; 20, 30 |
| 8.1 to 8.6 | 4; 15, 16, 50, 65; 17, 48, 63; 18, 51, 52; 53, 54, 66; 38, 64 |
| 9.2, 9.4, 9.5, 9.6 | 23, 25, 33, 39, 49, 52, 59; 59; 43, 49; 17, 22, 44, 45, 50 |
| 10.2 to 10.4 | 14, 24; 63; 8, 47, 62 |
| 11.2 | 21, 28 |

Questions 7, 28, 38, 49 and 63 also have a proposal for API 0.6 (appendix). Questions 11, 28, 41, 44, 49, 52 and 65 also put a question to the other platforms (section 13). The firmware-internal host tests of the ZZ9000 stay ZZ9000 regression tests. The suite is the black-box check that a second platform runs.

## 12. Licensing and repository

- MIT licenses the specification text, `aar/*.h`, `libaar` sources, `crt0`, linker scripts, the note macros, `aar-check`, the Docker or toolchain recipes, the host runner, the `aar.library` headers, `.fd` files and stubs, the portable client core, `aar_serve.c`, `aar-run`, the reference loader, and the conformance payloads and runner.
- Each platform's implementation keeps its own licence: the ZZ9000 firmware and `zz9k.library` code stay GPL-3.0-or-later. The boundary stays the table (app side) and the library vectors (client side): an app links only MIT `libaar`; a client calls `aar.library` through its vectors and does not link GPL code. No firmware object and no GL front-end object is linked into an app.
- The standard is in a separate repository, `BlitterStudio/aar` (public, MIT), so platform developers who do not want the ZZ9000 firmware tree (GPL, bitstreams, BSP) can adopt the standard. Contents: `include/aar/`, `libaar/`, `loader/`, `client/` (headers, `.fd`, the reference library core and transport interface), `conformance/`, `host-runner/`, `tools/`, `templates/`, `docs/`, `spec/`, `mk/`, `cmake/`, `docker/` and `tests/`. The ZZ9000 firmware repository pins it by commit.

## 13. Open questions for other platform developers

### Emu68 and PiStorm

1. Where does the app run: a separate Cortex-A72 core, or time-sliced on the core that runs the JIT? What isolation (EL0, MMU) is practical? Can the runtime report `AAR_INFOF_ISOLATED`? What is mapped in an app's address space (4.4), which address range is never mapped, and on a platform that cannot isolate, does an access to an unmapped address still trap, and where (5.3)?
2. Is shared memory Amiga RAM, with the token equal to the Amiga address? Does the allocator have a region that is guaranteed below 4 GiB and visible at the same address to the 68k side?
3. Does Emu68 keep guest memory in big-endian byte order in RAM? The standard assumes shared buffers are plain bytes that the app swaps itself (1.3), and the reference tests store their shared counters big-endian so the 68k reads them as native longs. Does the same convention hold?
4. Cache behaviour: is `CacheClearE`/`CacheClearU` a no-op or an instruction-cache flush only? Can `AARSharedMap` always report coherent and uncached? Is there an `mmu.library` at all, or another way to map a range cache-inhibited (the ZZ9000 client refuses to open without it)?
5. Is there a way for a 68k library function to enter ARM-native code, and for native code to `Signal` a 68k task, without a bus transport? Is a 68k stub library with native bodies acceptable?
6. Float and NEON state: the JIT uses ARM vector registers for 68k FPU emulation. Who saves and restores them around an app, and is the zero-FPCR entry contract compatible?
7. Can the Cortex-A72 run AArch32 at EL0 on your platform? If so, should the A32 profile be supported so ZZ9000 apps run unchanged, or is A64 only simpler?
8. Display and audio: which P96 driver and which AHI driver should `aar_serve.c` expect? Is the surface memory the P96 bitmap itself? Does the platform have its own mixer for the audio route `PLATFORM`, or `AHI` only (8.4)? What memory can a client pass to `AARSharedToken`: any Amiga RAM, or only memory the display driver or `aar.library` allocated (9.5)?
9. Lease and dead-client detection currently rely on a vertical-blank server. Is that available with Emu68 timing?

### Raspberry Pi CM5 on a Zorro III card

1. Linux or bare metal? On Linux, is the runtime a daemon and an app a normal user process started with the table pointer in `x0`, so isolation comes from the kernel? What is mapped in an app's address space, and which address range is never mapped (4.4)?
2. Which memory does the Zorro bridge reach: CM5 RAM over PCIe, or memory on the card? What does the bridge guarantee for coherency? `publish`/`acquire` must be real maintenance if CM5 inbound DMA is not coherent. DC IVAC is not available at EL0, so it must be a runtime call (as designed).
3. Size of the Zorro III aperture and what the token encodes. Can `max_image_bytes` and `shared_max_mib` be large, given 16 GiB RAM? What memory can a client pass to `AARSharedToken` (9.5)?
4. Interrupts: which Amiga interrupt line signals the host, and can the library wake the owner immediately?
5. A32 profile: does your kernel run AArch32 at EL0 on the Cortex-A76? If it does not, the plan is A64 only.
6. Boot and update: who loads the runtime, and how does `AARQuery` report that it is up?
7. Does the platform have its own audio mixer for the route `PLATFORM`, or `AHI` only (8.4)?

### Everyone

1. Image packaging: one ELF per profile named by convention, or a small container with both? Is a naming rule enough?
2. Names and prefixes: the standard is `AAR`, the app-side prefixes are `aar_` and `AAR_`, the Amiga library is `aar.library` with calls `AAR*`, and the launcher is `aar-run`. An Aminet search found no existing use of `aar.library`, `armrt`, `arm.library` or `armrun`. Does any of it clash with a name you use or plan?
3. GL: each platform must provide its own implementation. Is the group wanted at all, and which version (GL 1.1 subset, ES 2)?
4. Services group: which algorithms should a second platform plan to provide? Is the whole-image tile buffer of the image sessions (8.6) acceptable for 0.5?
5. Sandboxing expectations: is "trusted app, user mode where possible" acceptable everywhere?
6. How many apps a platform admits (resource-bound with refusal reasons, or a ceiling), whether a weight tag is enough for CPU sharing, and whether more than one app may use display or audio at once.
7. Wall-clock time and network: API 0.5 has neither. Add a client-served wall-clock call (and a network call) through the service queue before 1.0, or leave them for a later minor?
8. Can your platform release a claim from outside the owner and pause lease renewal for a test (5.5, 11.2)? If so, say how; we would then propose one optional extension for both in API 0.6.
9. Should `aar_display_vsync` become a call that waits for the next refresh with a time bound (8.2)? It would need a platform-visible vertical blank.
10. A `console_write` of up to 1024 bytes into an empty buffer returns without waiting for the client (7.2). Is 1024 bytes a number every platform's console buffer can hold?
11. Input rates: the runtime keeps at least 64 events and the client merges mouse motion (8.3, 10.4). Does your platform need event batching, further coalescing or a separate event ring?
12. The A64 baseline assumes no crypto, CRC32, LSE, dot-product or SVE (2.1). Does it match your cores? Can you confirm the A64 encodings given for `AAR_FAULT_ALIGN` (5.3), and for AArch32 under an AArch64 kernel, where the alignment status is 100001?

## Appendix: Planned for API 0.6

These are changes that would alter a header. They are not part of API 0.5 and are listed so that reviewers can comment on them now. Names are proposals.

- A flag in the `flags` argument of `AARSharedMap` that asks for a cacheable map (4.3), for example `AAR_MAPREQ_CACHEABLE`; the library MAY answer `AAR_E_NOSYS`. The 0.5 rule that a non-zero `flags` is `AAR_E_INVAL` then changes for that bit. It lets the suite test the Acquire path on a platform whose maps are normally uncached.
- An optional standard extension of `aar.library` for "release a claim from outside the owner" and "pause the lease" (5.5, 11.2), so that the suite can test rules 4 and 5 of 5.5 on every platform. Section 13 asks whether the platforms can offer it.
- Image stream constants: `AAR_IMAGE_FEED_EOF` (bit 0 of the `flags` of `aar_image_decode`) and `AAR_IMAGE_RESULT_HEADER`, `AAR_IMAGE_RESULT_PARTIAL` and `AAR_IMAGE_RESULT_SCALED` (bits 0 to 2 of the result `flags`) in `aar/app.h` (8.6). It is also open whether `aar_image_result` gains the session state, the tile position (`tile_y`) and the rows decoded (`tile_rows`) so that a tile buffer can be smaller than the image. Constants for the `aar_image_params.flags` bits that the ZZ9000 accepts today are planned as well: bit 0, fit the image to the tile buffer, and bit 1, preserve the aspect ratio when fitting.
- `LONG AARSharedAlloc(AARHandle *, ULONG bytes, ULONG flags, struct AARSharedMap *)` and `AARSharedFree`, the client-side twin of `aar_shared_alloc`, so that a client never needs to guess which memory is in the aperture (9.5). New `.fd` entries and a library minor.
- A platform-written word in the accounting line of the control block (for example `AAR_CB_EVENTS_LOST` at 0x198; blocks are zero-filled, so a reader that does not know the word reads 0) and an `AARStatusInfo.events_lost` field, so that a client can read the events the runtime discarded (8.3).

## Changes

**Review draft 1** (API 0.5): the first public draft. It specifies the app side (profiles, image rules, table, groups), the Amiga client library and the transport contract, and it settles the 66 questions that writing the conformance suite raised (11.3). Open questions for the other platforms are in section 13, and the changes planned for API 0.6 are in the appendix.
