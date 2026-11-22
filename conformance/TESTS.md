# AAR conformance catalogue

Suite version: 0.1
Specification: AAR API 0.5 (draft), section 11

One row per test id. The runner (`runner/aar-conf.c`) and the payloads
(`payloads/`) print one `AAR-CONF <id> PASS|FAIL|SKIP <detail>` line per id; the
scripts in `tools/` read this file, so keep the table format exactly as it is.

## How to read the tables

Columns, in this order: `id`, `spec` (section of the specification), `level`,
`group`, `profile`, `judged by`, `impl`, `check`.

| Column | Values |
|---|---|
| level | `MUST`: the statement says MUST or MUST NOT, or states a rule without a keyword (a platform behaviour that is not optional). `SHOULD`: the statement says SHOULD. `MAY`: a permission; never tested, listed so that nothing is lost. |
| group | `core`, `display`, `input`, `audio`, `gl`, `services`: the group that must be declared in `AARInfo.groups` for the test to apply. `zz9000`: applies when `AARInfo.platform_id` is `AAR_PLATFORM_ZZ9000` (a platform extension of the standard, section 8.8). |
| profile | `any`, `a32` or `a64`. A test of another profile is not printed and does not count. |
| judged by | `payload`: a conformance app judges it from inside and prints the line (the runner relays it and fails the id if it is missing); the check text says when the runner takes part as a peer. `runner`: the client side judges it, black box, through `aar.library`. `operator`: needs a person or an external event; `SKIP manual` unless the runner is started with `INTERACTIVE`. `none`: not tested; the check text says why. |
| impl | `yes`: the suite prints this id today (the scripts check that it is in the sources). `todo`: not written yet; the runner prints `SKIP not implemented` for it, which keeps a full run from claiming conformance. |

Rules of section 11.1 that the runner applies: a platform conforms when every
`MUST` test of every declared group and of `core` passes; a `SKIP` is allowed
only for a test of an undeclared group or a `SHOULD` test, and carries a reason.
A `SKIP` whose reason starts with `host:` is allowed only in a run on the host
runner or another environment that is not a platform (it never counts toward a
platform's verdict, because a platform's run has a client and a full table).

Spec statements that say MAY, describe internals, or cannot be observed from an
app or a client are listed with judge `none` and a reason, so that every MUST and
SHOULD of sections 2 to 11 has an id or an explicit "not tested".

## C-ELF: image refusal and acceptance (sections 2.2, 2.3, 5.2)

The runner loads the files of `negative/` (and the positive images next to them)
with `AARLoadFile` and checks the result code. Every refusal is `AAR_E_PROFILE`
or `AAR_E_INVAL` unless the check says otherwise. After every refusal the runner
checks that the handle can be closed and that a new claim succeeds.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-ELF.1 | 2.2.1 | MUST | core | any | runner | todo | `e_type` ET_EXEC refused |
| C-ELF.2 | 2.2.2 | MUST | core | any | runner | todo | wrong `EI_CLASS` for the profile refused |
| C-ELF.3 | 2.2.2 | MUST | core | any | runner | todo | `ELFDATA2MSB` refused |
| C-ELF.4 | 2.2.2 | MUST | core | any | runner | todo | wrong `e_machine` refused |
| C-ELF.5 | 2.2.2 | MUST | core | a32 | runner | todo | `EF_ARM_ABI_FLOAT_HARD` clear (soft-float or no float flag) refused |
| C-ELF.6 | 2.2.2 | MUST | core | a32 | runner | todo | EABI version other than 5 refused |
| C-ELF.7 | 2.2.2 | MUST | core | a64 | runner | todo | non-zero `e_flags` refused |
| C-ELF.8 | 2.2.3 | MUST | core | any | runner | todo | image with `PT_INTERP` refused |
| C-ELF.9 | 2.2.3 | MUST | core | any | runner | todo | image with `DT_NEEDED` refused |
| C-ELF.10 | 2.2.3 | MUST | core | any | runner | todo | five `PT_LOAD` segments refused |
| C-ELF.11 | 2.2.3 | MUST | core | any | runner | todo | a segment that is writable and executable refused |
| C-ELF.12 | 2.2.3 | MUST | core | any | runner | todo | `p_memsz` smaller than `p_filesz` refused |
| C-ELF.13 | 2.2.3 | MUST | core | any | runner | todo | overlapping `PT_LOAD` segments refused |
| C-ELF.14 | 2.2.3 | MUST | core | any | runner | todo | a segment larger than any memory the runtime offers refused (`AAR_E_NOMEM` is also accepted) |
| C-ELF.15 | 2.2.4 | MUST | core | any | runner | todo | image with no `PT_DYNAMIC` refused |
| C-ELF.16 | 2.2.4 | MUST | core | any | runner | todo | image with no section headers loads and runs (relocations come from `PT_DYNAMIC`) |
| C-ELF.17 | 2.2.5 | MUST | core | a32 | runner | todo | a `REL` table loads and runs |
| C-ELF.18 | 2.2.5 | MUST | core | a32 | runner | todo | a `RELA` table loads and runs |
| C-ELF.19 | 2.2.5 | MUST | core | any | runner | todo | a relocation type other than `RELATIVE` refused |
| C-ELF.20 | 2.2.5 | MUST | core | any | runner | todo | `DT_RELR` refused |
| C-ELF.21 | 2.2.5 | MUST | core | any | runner | todo | a relocation with symbol index other than 0 refused |
| C-ELF.22 | 2.2.5 | MUST | core | any | runner | todo | a relocation with a misaligned target refused |
| C-ELF.23 | 2.2.5 | MUST | core | any | runner | todo | a relocation target outside the image refused |
| C-ELF.24 | 2.2.6 | MUST | core | any | runner | todo | entry point outside an executable segment refused |
| C-ELF.25 | 2.2.6 | MUST | core | a32 | runner | todo | entry point with bit 0 set refused |
| C-ELF.26 | 2.2.7 | MUST | core | any | runner | todo | image with no AAR note refused |
| C-ELF.27 | 2.2.7 | MUST | core | any | runner | todo | image with two AAR notes refused |
| C-ELF.28 | 2.3 | MUST | core | any | runner | todo | note with a wrong `magic` refused |
| C-ELF.29 | 2.3 | MUST | core | any | runner | todo | `note_version` other than 1 refused |
| C-ELF.30 | 2.3 | MUST | core | any | runner | todo | note `profile` that differs from the ELF class refused with `AAR_E_PROFILE` |
| C-ELF.31 | 2.3 | MUST | core | any | runner | todo | an image of a profile that `AARInfo.profiles` does not list is refused with `AAR_E_PROFILE` (passes with detail `all profiles supported` when the platform lists every defined profile) |
| C-ELF.32 | 2.3 | MUST | core | any | runner | todo | `api_major` other than the platform's refused with `AAR_E_INVAL` |
| C-ELF.33 | 2.3 | MUST | core | any | runner | todo | `api_minor_required` above the platform's minor refused with `AAR_E_INVAL` |
| C-ELF.34 | 2.3 | MUST | core | any | runner | todo | `stack_bytes` below 64 KiB refused |
| C-ELF.35 | 2.3 | MUST | core | any | runner | todo | `stack_bytes` that is not a multiple of 16 refused |
| C-ELF.36 | 2.3 | MUST | core | any | runner | todo | `stack_bytes` 0, 64 KiB and 1 MiB all load and run |
| C-ELF.37 | 2.3 | MUST | core | any | runner | todo | `groups_required` with a reserved experimental bit (bit 28) refused with `AAR_E_NOGROUP` |
| C-ELF.38 | 2.3 | MUST | core | any | runner | todo | a note with wrong name or descriptor padding is not an AAR note: refused |
| C-ELF.39 | 5.2 | MUST | core | any | runner | todo | `AARInfo.max_image_bytes` is at least 16 MiB |
| C-ELF.40 | 4.1 | MUST | core | any | runner | todo | `min_heap_mib` above anything the platform can commit: `AAR_E_NOMEM` from the load call |
| C-ELF.41 | 5.2 | MUST | core | any | runner | todo | every load refusal is one of `AAR_E_PROFILE`, `AAR_E_NOGROUP`, `AAR_E_INVAL`, `AAR_E_NOMEM` (checked over the whole corpus) |
| C-ELF.42 | 2.2 | MUST | core | any | runner | todo | the same image loaded with `AARLoadMemory` gives the same result as `AARLoadFile` (accept and refuse alike) |
| C-ELF.43 | 2.4 | MUST | core | any | none | todo | not tested: `tools/aar-check` is a tool of this repository, tested by `tests/aar_check_test.py` in CI |
| C-ELF.44 | 2.1 | SHOULD | core | any | none | todo | not tested: a launcher picks `.a32` or `.a64` with `AARProbeFile`; probe behaviour is C-CLIENT |

## C-ENTRY: entry contract (section 3.2)

The payload `conf_entry` owns the entry symbol, snapshots every register before it
jumps to `aar_start`, and judges the snapshot. A32 and A64 tests have separate ids
because the registers differ.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-ENTRY.1 | 3.2 | MUST | core | any | payload | yes | the table pointer in `r0`/`x0` is non-NULL, equals `aar_api_ptr` and its `size` covers the core slots |
| C-ENTRY.2 | 3.2 | MUST | core | any | payload | yes | `argc` in `r1`/`w1` and `argv` in `r2`/`x2` equal the arguments the runner started the app with (name, `alpha`, `beta gamma`, `info:` and `conf:` words) |
| C-ENTRY.3 | 3.2 | MUST | core | any | payload | yes | `argv[0]` is the program name: its last path component is the payload's file name or the host executable's name |
| C-ENTRY.4 | 3.2 | MUST | core | any | payload | yes | a second run with 16 arguments of 127 bytes each (runner peer, `AARInfo.max_args` and `max_arg_bytes` permitting) delivers every byte intact |
| C-ENTRY.5 | 3.2 | MUST | core | a32 | payload | yes | `sp` at entry is 8-byte aligned |
| C-ENTRY.6 | 3.2 | MUST | core | a64 | payload | yes | `sp` at entry is 16-byte aligned |
| C-ENTRY.7 | 3.2 | MUST | core | a32 | payload | yes | `lr` is 0 at entry |
| C-ENTRY.8 | 3.2 | MUST | core | a64 | payload | yes | `x30` and `x29` are 0 at entry |
| C-ENTRY.9 | 3.2 | MUST | core | a32 | payload | yes | `r3` to `r12` are 0 at entry |
| C-ENTRY.10 | 3.2 | MUST | core | a64 | payload | yes | `x3` to `x28` are 0 at entry |
| C-ENTRY.11 | 3.2 | MUST | core | a32 | payload | yes | `FPSCR` is 0 at entry |
| C-ENTRY.12 | 3.2 | MUST | core | a64 | payload | yes | `FPCR` and `FPSR` are 0 at entry |
| C-ENTRY.13 | 3.2 | MUST | core | a32 | payload | yes | `d0` to `d31` are 0 at entry |
| C-ENTRY.14 | 3.2 | MUST | core | a64 | payload | yes | `v0` to `v31` are 0 at entry |
| C-ENTRY.15 | 3.2 | MUST | core | any | payload | yes | VFP and NEON (A32) or FP and ASIMD (A64) instructions execute without a fault |
| C-ENTRY.16 | 3.2 | MUST | core | any | payload | yes | the app runs little-endian: a stored 32-bit word reads back low byte first |
| C-ENTRY.17 | 3.2 | MUST | core | any | payload | yes | the image is loaded and relocated: a table of pointers to functions, strings and data works |
| C-ENTRY.18 | 3.2 | MUST | core | any | payload | yes | BSS is zero: a 64 KiB static array is all zero at entry |
| C-ENTRY.19 | 3.2 | MUST | core | any | payload | yes | the `argv` strings are in memory the app can read, each NUL-terminated |
| C-ENTRY.20 | 3.2 | MUST | core | any | payload | yes | constructors run exactly once, before `aar_main` (the runtime does not run them in addition to crt0) |
| C-ENTRY.21 | 3.2 | MUST | core | any | payload | yes | `.preinit_array` entries run before `.init_array` entries, and `aar_api_ptr` is set before either |
| C-ENTRY.22 | 3.2 | MUST | core | any | payload | yes | an `init_array` entry with a priority (`constructor(101)`) runs before one without |
| C-ENTRY.23 | 3.2 | MUST | core | a32 | none | todo | not tested: user mode, A32 state and interrupts enabled are not architecturally readable from user mode [spec question 3]; running at all proves A32 state |
| C-ENTRY.24 | 3.2 | MUST | core | a64 | none | todo | not tested: EL0 and DAIF are not readable from EL0; running at all proves AArch64 |
| C-ENTRY.25 | 3.2 | MUST | core | any | none | todo | not tested: "sp points one past the last usable byte" cannot be observed without a second stack bound; covered by C-ENTRY.5 and C-ENTRY.6 for alignment |
| C-ENTRY.26 | 3.2 | MUST | core | any | none | todo | not tested: returning from the entry point without exiting is undefined, so there is nothing to require |
| C-ENTRY.27 | 3.1 | MUST | core | any | none | todo | not tested: apps must not cache or compare table addresses and must not write the table; an obligation of the app |

## C-TABLE: the table, info, groups, extensions (sections 3.3, 3.4, 3.5, 3.6, 8.1, 8.7, 8.8)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-TABLE.1 | 3.3 | MUST | core | any | payload | todo | the table `size` covers every core slot (`exit` to `msg_wait`) and every core slot is non-NULL |
| C-TABLE.2 | 3.3 | MUST | core | any | payload | todo | `version` names API major `AAR_API_MAJOR` in its high 16 bits and equals the minor `AARInfo.api_minor` in its low 16 bits (the runner passes the client value) |
| C-TABLE.3 | 3.3 | MUST | core | any | payload | todo | the table `groups` word equals `aar_info.groups` and the runner's `AARInfo.groups` |
| C-TABLE.4 | 3.3 | MUST | display | any | payload | todo | with the display group declared the table covers the nine display slots and none is NULL |
| C-TABLE.5 | 3.3 | MUST | audio | any | payload | todo | with the audio group declared the table covers `audio_open` and `audio_close` and they are non-NULL (`audio_stats` may be NULL) |
| C-TABLE.6 | 3.3 | MUST | gl | any | payload | todo | with the GL group declared the table covers and fills `gl_bind`, `gl_swap`, `gl_unbind` |
| C-TABLE.7 | 3.3 | MUST | services | any | payload | todo | with the services group declared the table covers and fills the eight services slots |
| C-TABLE.8 | 3.3 | MUST | core | any | payload | todo | for each group whose bit is clear, the group's wrappers return `AAR_E_NOSYS` (or 0, NULL, nothing) and never fault |
| C-TABLE.9 | 3.3 | MUST | core | any | payload | todo | `aar_map_file` and `aar_unmap_file` and `aar_ext` are either absent or work; an absent one gives `AAR_E_NOSYS` from the wrapper |
| C-TABLE.10 | 3.3 | MUST | core | any | payload | todo | `platform_id` in the table equals `aar_info.platform_id` and the runner's `AARInfo.platform_id` |
| C-TABLE.11 | 3.4 | MUST | core | any | payload | todo | `aar_get_info` with a `size` of 16 changes only the first 16 bytes of the caller's structure |
| C-TABLE.12 | 3.4 | MUST | core | any | payload | todo | `aar_get_info` with a `size` larger than `sizeof(aar_info)` zeroes the bytes past the fields it knows, up to `size`, and writes nothing beyond |
| C-TABLE.13 | 3.4 | MUST | core | any | payload | todo | `aar_info.profile` equals the profile the app was built for |
| C-TABLE.14 | 3.4 | MUST | core | any | payload | todo | `heap_granule` is 1 MiB; `shared_granule` is a power of two of at least 64 |
| C-TABLE.15 | 3.4 | MUST | core | any | payload | todo | `msg_capacity` is at least 32 and equals the runner's `AARInfo.msg_capacity` |
| C-TABLE.16 | 3.4 | MUST | core | any | payload | todo | `stall_limit_ms` is at least 10000 and equals the runner's value; `stop_grace_ms` equals the runner's value |
| C-TABLE.17 | 3.4 | MUST | core | any | payload | todo | `max_files` is at least 16 |
| C-TABLE.18 | 3.4 | MUST | core | any | payload | todo | `shared_limit` is `shared_default_mib` MiB (16 MiB, `AAR_SHARED_DEFAULT_MIB`) when the client set none, and `shared_used` does not exceed it |
| C-TABLE.19 | 3.4 | MUST | core | any | payload | todo | `aar_heap_info` fills `committed`, `limit` and `pool_free`; `committed` does not exceed `limit` |
| C-TABLE.20 | 8.7 | MUST | core | a32 | payload | todo | every `AAR_CPU_A32_*` bit that is set in `cpu_features` is honest: the app executes a VFPv4 fused multiply-add, an integer divide, a half-precision convert as advertised without a fault |
| C-TABLE.21 | 8.7 | MUST | core | a64 | payload | todo | every `AAR_CPU_A64_*` bit that is set is honest: CRC32, LSE atomics, AES, SHA, dot product, FP16 execute without a fault |
| C-TABLE.22 | 8.8 | MUST | core | any | payload | todo | `aar_ext` with an unknown platform id or extension id returns NULL |
| C-TABLE.23 | 8.8 | MUST | zz9000 | any | payload | todo | `aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)` returns a table whose `size` covers `build_id`; `gl_bind_flags` is NULL when the GL group is absent |
| C-TABLE.24 | 3.5 | MUST | core | any | payload | todo | the error codes a call returns are the ones of section 3.5 (checked over every failing call of the payloads: no code outside -1 to -17) |
| C-TABLE.25 | 3.6 | MUST | core | any | none | todo | not tested here: the major and minor check on the client's view is C-CLIENT.3 |
| C-TABLE.26 | 3.3 | MUST | core | any | none | todo | not tested: the slot order, offsets and the 56 slots are pinned by the header's `_Static_assert`s and `tests/abi_check.sh` in CI |
| C-TABLE.27 | 3.3 | MUST | core | any | none | todo | not tested: apps MUST NOT gate on `version`; an obligation of the app |
| C-TABLE.28 | 3.5 | MUST | core | any | none | todo | not tested: the numeric values of the error codes are pinned by the header and `tests/abi_check.sh` |
| C-TABLE.29 | 8.8 | SHOULD | core | any | none | todo | not tested: an app that uses an extension SHOULD say it is not portable; documentation of the app |
| C-TABLE.30 | 8.1 | MUST | core | any | none | todo | not tested: group bit values are constants of the header; bits 28 to 31 are for experiments, no test can tell an experiment from an error [spec question 4] |
| C-TABLE.31 | 3.3 | MUST | core | any | none | todo | not tested: `AAR_API_COVERS` and the wrapper gating belong to libaar, tested by `libaar/tests/runtime_test.sh` (`CORE_ONLY=1`) |

## C-MEM: private memory (section 4.1)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-MEM.1 | 4.1 | MUST | core | any | payload | todo | `heap_range` returns a base and a length; the length equals `heap_info.committed` |
| C-MEM.2 | 4.1 | MUST | core | any | payload | todo | `heap_grow` of one granule returns `AAR_OK` and an old break equal to the previous end (base plus length) |
| C-MEM.3 | 4.1 | MUST | core | any | payload | todo | successive grows are contiguous: each old break equals the previous end |
| C-MEM.4 | 4.1 | MUST | core | any | payload | todo | new heap memory is zeroed, also after a shrink and a grow of the same range |
| C-MEM.5 | 4.1 | MUST | core | any | payload | todo | existing pointers never move: data written before a grow is intact after it |
| C-MEM.6 | 4.1 | MUST | core | any | payload | todo | `heap_shrink` returns whole granules at the top: the new break is the old end minus the request and `heap_range` shrinks by it |
| C-MEM.7 | 4.1 | MUST | core | any | payload | todo | a grow or shrink that is not a whole number of granules is `AAR_E_INVAL` and changes nothing |
| C-MEM.8 | 4.1 | MUST | core | any | payload | todo | grow until failure: the failure is `AAR_E_NOMEM`, the committed size never exceeds `heap_info.limit` |
| C-MEM.9 | 4.1 | MUST | core | any | payload | todo | variant `caps` (runner sets `AAR_TAG_PRIVATE_MIB`): `heap_info.limit` is the smaller of the platform's cap and the client's value, and a grow past it is `AAR_E_NOMEM` |
| C-MEM.10 | 4.1 | MUST | core | any | payload | todo | `min_heap_mib` of the note is committed at load: the heap is at least that long at entry (payload note: 2 MiB) |
| C-MEM.11 | 4.1 | MUST | core | any | payload | todo | `heap_info.pool_free` falls when the heap grows and rises when it shrinks |
| C-MEM.12 | 4.1 | MUST | core | any | payload | todo | exhausting the pool produces one `AAR_EV_LOW_MEMORY` event per low-water crossing; its fields other than `type` are 0; no second event arrives until the pool has risen above the high-water mark [spec question 5] |
| C-MEM.13 | 4.4 | MUST | core | any | payload | todo | when `AAR_INFOF_ISOLATED` is set, a call given a bad pointer returns `AAR_E_FAULT` and the app keeps running (passes with detail `not isolated` when the flag is clear) |
| C-MEM.14 | 4.4 | MUST | core | any | none | todo | not tested: "the platform MUST document" the behaviour of a bad pointer on a non-isolated runtime; documentation |
| C-MEM.15 | 4.1 | MUST | core | any | none | todo | not tested: the heap starts at the end of the image and apps must not assume an address; platform internals and an obligation of the app |
| C-MEM.16 | 4.4 | MUST | core | any | none | todo | not tested: apps cannot reach firmware, other apps or host memory except through granted buffers; an isolation proof, not black-box [spec question 6] |

## C-SHARED: shared buffers and visibility (sections 4.2, 4.3, 9.5, 10.3, 10.5)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-SHARED.1 | 4.2 | MUST | core | any | payload | todo | `shared_alloc` with `flags` other than 0 is `AAR_E_INVAL` |
| C-SHARED.2 | 4.2 | MUST | core | any | payload | todo | a granted block has `bytes` rounded up to `shared_granule`, `ptr` aligned to the requested alignment (rounded up to `shared_granule`) and writable over all `bytes`; `flags` holds only `AAR_SHAREDF_COHERENT` |
| C-SHARED.3 | 4.2 | MUST | core | any | payload | todo | `shared_free` of an interior pointer, of NULL, of a block already freed is `AAR_E_INVAL`; of the base of a live block is `AAR_OK` |
| C-SHARED.4 | 4.2 | MUST | core | any | payload | todo | at least 64 live blocks (`AAR_SHARED_MIN_BLOCKS`) can be held at once |
| C-SHARED.5 | 4.2 | MUST | core | any | payload | todo | 64 live blocks are still possible after the runtime has made its own service buffers (console writes, a file read) |
| C-SHARED.6 | 4.2 | MUST | core | any | payload | todo | allocate until failure: the failure is `AAR_E_NOMEM` and the total granted does not exceed `shared_limit` |
| C-SHARED.7 | 4.2 | MUST | core | any | payload | todo | `shared_used` rises by at least the granted bytes with an allocation and falls back with the free |
| C-SHARED.8 | 4.2 | MUST | core | any | payload | todo | variant `caps` (runner sets `AAR_TAG_SHARED_MIB`): `shared_limit` is that value and allocation past it, service buffers included, fails |
| C-SHARED.9 | 4.2 | MUST | core | any | payload | todo | token round trip (runner peer): the app writes a pattern, publishes, sends `haddr` and length in a message; the client maps it with `AARSharedMap` and reads the same bytes |
| C-SHARED.10 | 4.3 | MUST | core | any | payload | todo | the client writes a different pattern, publishes and posts a message; the app acquires and reads it intact (runner peer) |
| C-SHARED.11 | 4.3 | MUST | core | any | payload | todo | counter and pattern test (runner peer): the app writes an increasing counter and a pattern into a shared buffer while the client reads it in a loop with the map the library gave and `AARSharedAcquire` before each pass; zero stale and zero torn reads |
| C-SHARED.12 | 4.3 | MUST | core | any | payload | todo | the same test with a client map that is deliberately cacheable plus `AARSharedAcquire` (needs a way to ask for a cacheable map [spec question 7]) |
| C-SHARED.13 | 4.3 | MUST | core | any | payload | todo | `aar_publish` and `aar_acquire` over a range inside a live block return `AAR_OK`; they return at once when the runtime is coherent |
| C-SHARED.14 | 4.3 | MUST | core | any | payload | todo | granule ownership (runner peer): app and client write alternate adjacent granules between hand-offs and neither sees the other's bytes in its own granule |
| C-SHARED.15 | 9.5 | MUST | core | any | runner | todo | `AARSharedMap` of a range that leaves the block or the platform's aperture is `AAR_E_INVAL`; an unknown token is `AAR_E_INVAL` |
| C-SHARED.16 | 9.5 | SHOULD | core | any | runner | todo | `AARSharedMap` after the app ended and its claim is gone is `AAR_E_STALE` (where the platform can tell) |
| C-SHARED.17 | 9.5 | MUST | core | any | runner | todo | `AARSharedMap.flags` holds only `AAR_MAPF_UNCACHED` and `AAR_MAPF_COHERENT`; `granule` is a power of two; `base` is non-NULL |
| C-SHARED.18 | 9.5 | MUST | core | any | runner | todo | `AARSharedMap` with a non-zero `flags` argument is `AAR_E_INVAL` |
| C-SHARED.19 | 9.5 | MUST | core | any | runner | todo | `AARSharedUnmap` of a live map returns `AAR_OK`; the map is valid until then, until `AARClose` or the end of the app |
| C-SHARED.20 | 9.5 | MUST | core | any | runner | todo | `AARSharedPublish` and `AARSharedAcquire` on a valid range return `AAR_OK` and round to the map's granule; they are no-ops on an uncached or coherent map |
| C-SHARED.21 | 9.5 | MUST | core | any | none | todo | not tested: after an end state the client MUST unmap and MUST NOT write through a map; an obligation of the client (the runner follows it) |
| C-SHARED.22 | 9.5 | SHOULD | core | any | none | todo | not tested: the library SHOULD revoke maps where the platform allows; no observable difference without writing through a stale map, see C-LIFE.23 |
| C-SHARED.23 | 4.3 | MUST | core | any | none | todo | not tested: after writing data for the client the app MUST call `aar_publish` before sending the message; an obligation of the app, exercised by C-SHARED.9 to C-SHARED.12 |
| C-SHARED.24 | 10.3 | MUST | core | any | none | todo | not tested: control block layout, one writer per word, 64-byte lines, big-endian words and magic last are not visible to a client of `aar.library`; covered by `tests/transport_model_test.c` and the platform's own tests |
| C-SHARED.25 | 10.5 | MUST | core | any | none | todo | not tested: non-coherent platforms map control structures non-cacheable or use publish and acquire internally; visible only as stale or torn reads, which C-SHARED.11 and C-SHARED.12 would find |
| C-SHARED.26 | 4.3 | MUST | display | any | none | todo | not tested here: "flip publishes the surface" is C-DISPLAY.12 |
| C-SHARED.27 | 4.2 | MUST | core | any | runner | todo | a request the platform could never satisfy fails the load: a shared limit above `AARInfo.shared_max_mib` (and at most 4095) gives `AAR_E_NOMEM` from `AARLoadFile`, or `AAR_E_INVAL` or `AAR_E_NOSYS` from `AARSetTagList` where that is the call's documented answer [spec question 29] |

## C-MSG: messages and waiting (sections 6, 9.4, 9.6, 10.4)

The runner is the peer for all of these: it posts, collects and holds back
messages as the payload asks for.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-MSG.1 | 6 | MUST | core | any | payload | todo | 32 bytes in, the same 32 bytes out, in both directions (all 256 byte values, no word swapped) |
| C-MSG.2 | 6 | MUST | core | any | payload | todo | app to client: `msg_capacity` numbered messages arrive in order, none lost or repeated |
| C-MSG.3 | 6 | MUST | core | any | payload | todo | client to app: `msg_capacity` numbered messages arrive in order through `msg_poll`, none lost or repeated |
| C-MSG.4 | 6 | MUST | core | any | payload | todo | with the client not draining, `msg_send` returns `AAR_E_BUSY` after at least `msg_capacity` messages and never blocks (each call returns within 100 ms); then the client drains and gets the accepted messages intact and in order |
| C-MSG.5 | 6 | MUST | core | any | runner | todo | with the app not polling, `AARSendMessage` returns `AAR_E_BUSY` after at least `msg_capacity` messages; the app then polls them all intact and in order |
| C-MSG.6 | 6 | MUST | core | any | payload | todo | `msg_poll` returns 0 when empty, 1 with a copy when a message is queued |
| C-MSG.7 | 6 | MUST | core | any | runner | todo | `AARGetMessage` returns `AAR_E_BUSY` when no message from the app is queued |
| C-MSG.8 | 6 | MUST | core | any | payload | todo | `msg_wait` returns `AAR_OK` with the message still queued (a following `msg_poll` returns it) |
| C-MSG.9 | 6 | MUST | core | any | payload | todo | `msg_wait` with nothing queued returns `AAR_E_TIMEDOUT` after at least the timeout |
| C-MSG.10 | 6 | MUST | core | any | payload | todo | `msg_wait(0)` only tests: returns at once, `AAR_OK` with a message queued, `AAR_E_TIMEDOUT` without |
| C-MSG.11 | 6 | MUST | core | any | payload | todo | a stop request ends `msg_wait` with `AAR_E_IO` (runner peer stops the app while it waits) |
| C-MSG.12 | 6 | MUST | core | any | payload | todo | a message queued before the stop is reported first: `msg_wait` returns `AAR_OK` for it, then `AAR_E_IO` |
| C-MSG.13 | 5.4 | MUST | core | any | payload | todo | an app parked in `msg_wait` for 12 seconds is not ended by the watchdog |
| C-MSG.14 | 6 | MUST | core | any | runner | todo | messages the app sent remain readable by the client after the app exited, until `AARClose` (up to `msg_capacity`) |
| C-MSG.15 | 10.4 | MUST | core | any | runner | todo | `AARInfo.msg_capacity` equals `queue_depth` minus 1 (`AAR_MSG_RESERVE`), and a platform with the default depth reports 63 |
| C-MSG.16 | 10.4 | MUST | core | any | payload | todo | a request behind a message that the full client inbox holds back waits until the client drains: a console write completes after the client resumes draining (runner peer holds draining for 300 ms) |
| C-MSG.17 | 9.6 | MUST | core | any | payload | todo | a burst of messages in both directions with a slow receiver loses none and reorders none (the `msgflood` pattern, 2000 messages) |
| C-MSG.18 | 9.6 | MUST | core | any | none | todo | not tested: app service calls park the app and become requests, payloads are shared buffers named by token; internal, shown indirectly by C-FILE and C-CON |
| C-MSG.19 | 6 | MUST | core | any | none | todo | not tested: a runtime without wait support "may implement it as sleep-and-poll"; permitted, indistinguishable |
| C-MSG.20 | 6 | MAY | core | any | none | todo | not tested: message content convention is recommended, not required |
| C-MSG.21 | 10.4 | MUST | core | any | none | todo | not tested: the app is single-threaded, at most one service request parks it; not observable from outside |

## C-QUEUE: queue pair, request storage, many apps (sections 7.1, 9.6, 10.2, 10.4, 10.6, 5.6)

The queue rules that need a manual service loop use `AARServiceNext` and
`AARServiceReply` directly; the runner then plays a client that withholds, delays
or misaddresses completions.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-QUEUE.1 | 10.4 | MUST | core | any | runner | todo | `AARInfo.queue_depth` is a power of two from 64 to 4096 |
| C-QUEUE.2 | 9.6 | MUST | core | any | runner | todo | `AARServiceNext` returns `AAR_OK` with a request, or `AAR_E_BUSY` when none is waiting |
| C-QUEUE.3 | 9.6 | MUST | core | any | runner | todo | a reply with a wrong `id` does not resume the parked app or release its storage; the app continues only after the right reply |
| C-QUEUE.4 | 9.6 | MUST | core | any | runner | todo | a reply with a wrong `op`, and a duplicate reply, do the same as C-QUEUE.3 |
| C-QUEUE.5 | 9.6 | MUST | core | any | runner | todo | `AARServiceReply` resumes the app only if it is parked for that request: a reply to a non-parking request (console write) leaves a later parked request parked |
| C-QUEUE.6 | 9.6 | MUST | core | any | payload | todo | a request whose client withholds the reply past the 30 s file timeout returns `AAR_E_TIMEDOUT` and a timeout is not a completion: the next call that needs the storage returns `AAR_E_BUSY` until the late completion is consumed (runner peer) |
| C-QUEUE.7 | 9.6 | MUST | core | any | payload | todo | after the late completion of C-QUEUE.6 is consumed, the app can use the storage again |
| C-QUEUE.8 | 10.4 | MUST | core | any | runner | todo | after `AAR_E_BUSY` for a full SQ the status counter `sq_full` has increased |
| C-QUEUE.9 | 10.4 | MUST | core | any | payload | todo | `aar_log` flood with the client not serving never blocks the app; the status counter `log_dropped` increases when records were lost |
| C-QUEUE.10 | 10.6 | MUST | core | any | payload | todo | a missed or merged doorbell does not lose an entry: 5000 console requests with the client serving only on a 50 ms timer (no wakeups) complete with every byte present |
| C-QUEUE.11 | 10.4 | MUST | core | any | runner | todo | an unsolicited entry (event or log) that arrives while a request is pending does not complete or disturb that request |
| C-QUEUE.12 | 5.6 | MUST | core | any | runner | todo | open claims until `AAR_E_NORESOURCE` (or until `max_apps`): the refusal carries a reason from 1 to 4 (`AAR_REASON_*`); a platform with a ceiling reports `max_apps` |
| C-QUEUE.13 | 5.6 | MUST | core | any | runner | todo | a refused open leaves nothing allocated: after 100 further refused opens and closing every claim, the same number of claims can be opened again |
| C-QUEUE.14 | 5.6 | MUST | core | any | runner | todo | releasing every claim returns every resource: open to the limit, close all, open to the limit again, the same count |
| C-QUEUE.15 | 5.6 | SHOULD | core | any | runner | todo | with two compute-bound apps of weights 1 and 3 and `AAR_TAG_WEIGHT` accepted, the work done is within 30 percent of 1 to 3 (`AAR_E_NOSYS` for the tag skips with that reason) |
| C-QUEUE.16 | 5.6 | MUST | core | any | runner | todo | an app parked in a wait uses no CPU: a parked app beside a compute-bound app leaves the compute-bound app at least 90 percent of the work it does alone |
| C-QUEUE.17 | 5.6 | MUST | core | any | none | todo | not tested: "keeps its own service work ahead of apps" is a platform scheduling policy |
| C-QUEUE.18 | 10.4 | MUST | core | any | none | todo | not tested: entry layout, big-endian fields, slot clearing, reserved bytes, op codes in the entry, default pair geometry (`0x3000` bytes), release and acquire barriers and the head and tail writer rules are not visible to an `aar.library` client; covered by `tests/transport_model_test.c` and the platform's tests |
| C-QUEUE.19 | 10.4 | MUST | core | any | none | todo | not tested: an index count above `depth` marks the block corrupt (`AAR_E_IO`); needs write access to the shared control block, which the client API does not give [spec question 8] |
| C-QUEUE.20 | 10.4 | MUST | core | any | none | todo | not tested: `AAR_OP_NONE` is never valid, `AAR_OP_EVENT` is CQ only, `AAR_OP_LOG` is SQ only; transport internals |
| C-QUEUE.21 | 10.6 | MUST | core | any | none | todo | not tested: who rings the doorbell after which post is internal; the observable half is C-QUEUE.10 |
| C-QUEUE.22 | 10.6 | MAY | core | any | none | todo | not tested: publishing event counts is a permission; a client that ignores the counts is still correct |
| C-QUEUE.23 | 10.1 | MUST | core | any | none | todo | not tested: shared memory, doorbells and a client mapping are the platform's means; their effect is every other test |
| C-QUEUE.24 | 10.7 | MUST | core | any | none | todo | not tested: the internal transport interface is not part of the standard |

## C-FILE: files (section 7.1)

The runner prepares a fixture tree under its root before the payload starts
(`conf-fixtures/`: a text file with known bytes, a 1 MiB pattern file, a file with
a known date, a directory with five files, a chain of directories for the longest
path) and checks the files the payload leaves behind.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-FILE.1 | 7.1 | MUST | core | any | payload | todo | a path of 255 bytes opens; the first file id is at least `AAR_FD_BASE` (3) and below 64 |
| C-FILE.2 | 7.1 | MUST | core | any | payload | todo | a path of 256 bytes is refused with a negative code [spec question 9] |
| C-FILE.3 | 7.1 | MUST | core | any | payload | todo | a relative path resolves under the client's root: a file the app creates is found there by the runner, and a fixture the runner placed there reads back |
| C-FILE.4 | 7.1 | SHOULD | core | any | payload | todo | `..` and a leading `/` are refused with `AAR_E_ACCES` by the recommended default policy (`aar_serve`) |
| C-FILE.5 | 7.1 | SHOULD | core | any | payload | todo | a path with a volume or assign prefix is refused with `AAR_E_ACCES` unless `AAR_SERVEF_ABS_PATHS` is set; with the flag set a prefixed path to the root's own volume opens |
| C-FILE.6 | 7.1 | MUST | core | any | payload | todo | once the client has asked the app to stop, a create, a truncate and a write are refused (negative code) while a read still works (runner peer stops the app) |
| C-FILE.7 | 7.1 | MUST | core | any | payload | todo | 16 files and, separately, 16 directory lists can be open at once; closing the lists leaves the files open |
| C-FILE.8 | 7.1 | MUST | core | any | payload | todo | a bad id (-1, 0, 1, 2, 999, a closed id) is `AAR_E_BADF` from close, read, write, seek and list next |
| C-FILE.9 | 7.1 | MUST | core | any | payload | todo | `AAR_FILE_READ` opens an existing file; `AAR_FILE_WRITE` writes; a write through a read-only id and a read through a write-only id fail with a negative code [spec question 10] |
| C-FILE.10 | 7.1 | MUST | core | any | payload | todo | `CREATE` and `TRUNCATE` together create a missing file and empty an existing one |
| C-FILE.11 | 7.1 | MUST | core | any | payload | todo | `CREATE` alone keeps the contents of an existing file |
| C-FILE.12 | 7.1 | MUST | core | any | payload | todo | `TRUNCATE` alone empties an existing file and does not create a missing one (`AAR_E_NOENT`) |
| C-FILE.13 | 7.1 | MUST | core | any | payload | todo | `CREATE` with `EXCL`: `AAR_E_EXIST` for an existing file, which stays untouched; creates a missing file |
| C-FILE.14 | 7.1 | MUST | core | any | payload | todo | flags 0 (neither read nor write) is `AAR_E_INVAL` |
| C-FILE.15 | 7.1 | MUST | core | any | payload | todo | `CREATE` without `WRITE` and `TRUNCATE` without `WRITE` are `AAR_E_INVAL` |
| C-FILE.16 | 7.1 | MUST | core | any | payload | todo | `EXCL` without `CREATE` is `AAR_E_INVAL` |
| C-FILE.17 | 7.1 | MUST | core | any | payload | todo | an unknown flag bit is `AAR_E_INVAL` |
| C-FILE.18 | 7.1 | MUST | core | any | payload | todo | a missing file without `CREATE` is `AAR_E_NOENT` |
| C-FILE.19 | 7.1 | MUST | core | any | payload | todo | seek with `AAR_SEEK_SET`, `CUR` and `END` returns the new position; a position before the start is `AAR_E_INVAL`; a seek past the end then a read returns `AAR_OK` and 0 bytes |
| C-FILE.20 | 7.1 | MUST | core | any | payload | todo | `file_read` sets `*out_n` to the bytes obtained; a read of more than the file holds returns what is left; a read at the end returns `AAR_OK` and 0 |
| C-FILE.21 | 7.1 | MUST | core | any | payload | todo | one `file_read` of the 1 MiB fixture returns all of it (chunking is the platform's business, a short read means end of file) |
| C-FILE.22 | 7.1 | MUST | core | any | payload | todo | one `file_write` of 1 MiB returns a count of 1 MiB and the runner finds the exact bytes in the file |
| C-FILE.23 | 7.1 | MUST | core | any | payload | todo | `file_stat` gives size and mode (1 file, 2 directory) of the fixtures; a missing path is `AAR_E_NOENT` |
| C-FILE.24 | 7.1 | MUST | core | any | payload | todo | `mtime` of the dated fixture equals its date in Unix seconds, or is 0 when the filesystem has none |
| C-FILE.25 | 7.1 | MUST | core | any | payload | todo | a directory list returns every entry of the fixture directory once, names of at most 255 bytes, mode per entry, then `AAR_E_NOENT`; a missing directory is `AAR_E_NOENT` |
| C-FILE.26 | 7.1 | MUST | core | any | payload | todo | a call waits at most 30 seconds for the client and then returns `AAR_E_TIMEDOUT` (runner peer withholds the reply; the test takes 31 seconds) |
| C-FILE.27 | 7.1 | MUST | core | any | payload | todo | after a timeout the file id is not reused for the rest of the app's life: ids the runtime hands out later differ from the timed-out one (runner peer reports the ids it saw) |
| C-FILE.28 | 7.1 | MUST | core | any | payload | todo | a stop request ends a file wait with `AAR_E_IO` (runner peer withholds the reply, then stops the app) |
| C-FILE.29 | 7.1 | MUST | core | any | payload | todo | `aar_map_file`, when present, returns the exact contents of the 1 MiB fixture and the buffer stays valid until `aar_unmap_file`; `AAR_E_NOSYS` is allowed |
| C-FILE.30 | 7.1 | MUST | zz9000 | any | payload | todo | `aar_map_file` is present, `shared_used` rises by the file size while mapped, and a file over 16 MiB is `AAR_E_NOSPC` |
| C-FILE.31 | 7.1 | MUST | core | any | runner | todo | after the app exited with files and lists still open, the runner can delete and rename those files (the client released every lock and handle) |
| C-FILE.32 | 7.1 | MAY | core | any | none | todo | not tested: a platform MAY return `AAR_E_NOSYS` from `aar_map_file` (accepted by C-FILE.29) |
| C-FILE.33 | 7.1 | MUST | core | any | none | todo | not tested: AmigaDOS has no atomic exclusive create; the check-then-create in the serving loop is only exclusive among the apps one client serves; C-FILE.13 tests what the standard promises |
| C-FILE.34 | 7.1 | MUST | zz9000 | any | none | todo | not tested: the mapping of `READ`, `WRITE`, `CREATE` and `TRUNCATE` onto `MODE_OLDFILE`, `MODE_READWRITE`, `MODE_NEWFILE` is an implementation detail of the ZZ9000 client; its effects are C-FILE.9 to C-FILE.13 |
| C-FILE.35 | 3.8 | MUST | core | any | none | todo | not tested: `fopen`, `O_CREAT`, `O_TRUNC`, `O_EXCL`, `O_APPEND` and `EOVERFLOW` in the libc glue belong to libaar, tested by `libaar/tests/runtime_test.sh` |
| C-FILE.36 | 7.1 | SHOULD | core | any | none | todo | not tested: "a short write means the client stopped early; the caller may retry" is advice to the caller; the stop case is C-FILE.6 |

## C-CON: console and log (section 7.2, 10.4)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-CON.1 | 7.2 | MUST | core | any | payload | todo | `console_write` of 200 KiB returns `AAR_OK` with `*n` equal to the length and the runner's capture holds every byte in order (pattern checked) |
| C-CON.2 | 7.2 | MUST | core | any | payload | todo | with the client's output held back for 500 ms the write parks and then completes without losing a byte |
| C-CON.3 | 7.2 | MUST | core | any | payload | todo | `console_read` parks until the client has input and returns the bytes the runner feeds (runner peer feeds after 300 ms) |
| C-CON.4 | 7.2 | MUST | core | any | payload | todo | `console_read` returns `AAR_OK` with `*n == 0` at the end of input |
| C-CON.5 | 7.2 | MUST | core | any | payload | todo | a stop request ends a console wait with `AAR_E_IO` (runner peer holds the input and stops the app) |
| C-CON.6 | 3.8 | MUST | core | any | payload | todo | libc flavour: `write(1, ...)` and `write(2, ...)` reach the console, `read(0, ...)` reads its input, `isatty(0)`, `isatty(1)`, `isatty(2)` return 0 (host build: `SKIP host:`) |
| C-CON.7 | 7.2 | MUST | core | any | payload | todo | an `aar_log` record reaches the client's log hook intact and does not appear in the console stream |
| C-CON.8 | 7.2 | MUST | core | any | payload | todo | an `aar_log` record of 2000 bytes arrives truncated to at most `AAR_LOG_MAX` (1024) bytes, the first bytes intact, in one piece |
| C-CON.9 | 10.4 | MUST | core | any | payload | todo | `aar_log` records over 1024 bytes are never wrapped or split: every record the client receives is a prefix of a record the app sent |
| C-CON.10 | 7.2 | MAY | core | any | none | todo | not tested: the platform MAY rate-limit `aar_log` (ZZ9000: 32 per second to the client, 8 per second to its UART) |
| C-CON.11 | 7.2 | MUST | core | any | none | todo | not tested: the client decides what the console is (Shell streams or a window); Ctrl-\ behaviour is the client's |
| C-CON.12 | 7.2 | MUST | core | any | none | todo | not tested: `aar_log` is "lossy, separate from stdout, for the platform's debug output"; the platform's UART path is not visible to a client |

## C-TIME: monotonic time and sleep (section 7.3)

The pattern area of this suite: complete, payload `conf_time`, runs on the host
runner and under qemu user mode.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-TIME.1 | 7.3 | MUST | core | any | payload | yes | `aar_time_us` never goes backwards over 200000 calls and over a 20 ms sleep |
| C-TIME.2 | 7.3 | SHOULD | core | any | payload | yes | the smallest step between two different readings is 1 ms or finer (checked over 50 ms) |
| C-TIME.3 | 7.3 | MUST | core | any | payload | yes | `aar_sleep_us(us)` parks for at least `us`, measured by `aar_time_us`, for 1 ms, 10 ms and 50 ms |
| C-TIME.4 | 7.3 | SHOULD | core | any | payload | yes | a sleep takes no more than `us` plus four sleep granules (at least 1 ms each) plus 10 ms |
| C-TIME.5 | 3.7 | MUST | core | any | payload | yes | a stop request ends a long sleep early (runner peer stops the app 100 ms into a 10 s sleep); `aar_should_stop` is 1 afterwards. Host build: `SKIP host:` |
| C-TIME.6 | 7.3 | MUST | core | any | none | todo | not tested: "from an epoch the platform chooses (not wall-clock)" cannot be told from a wall clock that happens to start at zero |
| C-TIME.7 | 7.3 | MUST | core | any | none | todo | not tested: no wall-clock call is defined in API 0.5; an absence |
| C-TIME.8 | 5.4 | MUST | core | any | none | todo | not tested here: `time_us` and `sleep_us` as watchdog activity or declared waits are C-LIFE.15 and C-LIFE.16 |

## C-LIFE: lifecycle, faults, watchdog, leases (sections 5.1 to 5.5)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-LIFE.1 | 5.1 | MUST | core | any | runner | yes | after `AAROpen` the state is `AAR_STATE_CLAIMED` and the generation is non-zero |
| C-LIFE.2 | 5.1 | MUST | core | any | runner | yes | after a payload returned from `aar_main` the state is `AAR_STATE_IDLE`, the end reason `AAR_END_EXIT` and the fault class `AAR_FAULT_NONE`, and the status stays readable until `AARClose` |
| C-LIFE.3 | 5.2 | MUST | core | any | runner | yes | the exit code the client reads equals the value `aar_main` returned (0 when every payload test passed, 10 otherwise, as the payload's end line says) |
| C-LIFE.4 | 5.2 | MUST | core | any | runner | todo | exit codes 0, 5, 10, 20 and 255 returned by `aar_main` and passed to `aar_exit` arrive unchanged in `AARStatus` |
| C-LIFE.5 | 5.1 | MUST | core | any | runner | todo | state while running is `RUNNING`; `AARStop` moves it to `STOPPING` and then `IDLE` |
| C-LIFE.6 | 5.2 | MUST | core | any | runner | todo | a stop sets `aar_should_stop` and queues `AAR_EV_STOP`; an app that leaves within the grace period ends with `AAR_END_EXIT` and its own exit code |
| C-LIFE.7 | 5.2 | MUST | core | any | runner | todo | an app that ignores a stop is ended by force after `stop_grace_ms`: end reason `AAR_END_FORCED`, no earlier than the grace period and no later than the grace period plus 2 s |
| C-LIFE.8 | 5.3 | MUST | core | any | runner | todo | a data abort in the app (write to an unmapped address) ends only that app: a second app that waits in `msg_wait` keeps running and exits normally afterwards (needs two claims) |
| C-LIFE.9 | 5.3 | MUST | core | any | runner | todo | data abort: end reason `AAR_END_USER_FAULT`, fault class `AAR_FAULT_DATA`, `fault_addr` equals the address the app touched, `fault_pc` equals the address of the faulting instruction |
| C-LIFE.10 | 5.3 | MUST | core | any | runner | todo | instruction fetch from an unmapped address: class `AAR_FAULT_FETCH`, `fault_pc` equals the target |
| C-LIFE.11 | 5.3 | MUST | core | any | runner | todo | an undefined instruction: class `AAR_FAULT_ILLEGAL`, `fault_pc` equals the instruction |
| C-LIFE.12 | 5.3 | MUST | core | any | runner | todo | an alignment fault (a misaligned exclusive access): class `AAR_FAULT_ALIGN` [spec question 11] |
| C-LIFE.13 | 5.3 | MUST | core | any | runner | todo | `fault_sp` and `fault_lr` equal the values the app had at the faulting instruction (the app reports them in a message first); addresses are app virtual addresses: `fault_pc` minus `load_base` is the offset in the image |
| C-LIFE.14 | 5.3 | MUST | core | any | runner | todo | `fault_syndrome` is non-zero for a data abort; `fault_pstate` shows user mode (A32 low five bits 0x10, A64 EL0); the profile reported in the status equals the image's |
| C-LIFE.15 | 5.4 | MUST | core | any | runner | todo | an app that computes for 10 seconds without a table call is not stopped (`stall_limit_ms` is at least 10000) |
| C-LIFE.16 | 5.4 | MUST | core | any | runner | todo | an app parked in `sleep_us` for 12 seconds is not stopped (a parked call does not count toward the stall limit) |
| C-LIFE.17 | 5.4 | MUST | core | any | runner | todo | each of `yield`, `should_stop`, `time_us`, `msg_wait` with a timeout, a file call and a console call, issued every 4 seconds by an otherwise silent app for 11 seconds, keeps it alive |
| C-LIFE.18 | 5.4 | SHOULD | core | any | runner | todo | an app that computes for 90 percent of the advertised `stall_limit_ms` without a call (at most 60 s) is not stopped |
| C-LIFE.19 | 5.5 | MUST | core | any | runner | todo | the client task blocking for 15 seconds without calling the library does not stop the app (the lease renews from interrupt or timer context) |
| C-LIFE.20 | 5.5 | MUST | core | any | runner | todo | a client task that exits without `AARClose` (a child process) releases its handle: the app it started stops running (its heartbeat file stops) within 10 s and a new claim succeeds |
| C-LIFE.21 | 5.5 | MUST | core | any | runner | todo | a client task that closes its last `aar.library` base without `AARClose` releases its handles and stops the app |
| C-LIFE.22 | 5.5 | MUST | zz9000 | any | runner | todo | after the claim is revoked (`aar-run RELEASE` through `AARExt`), calls with the old handle return `AAR_E_STALE` and change nothing |
| C-LIFE.23 | 5.5 | MUST | zz9000 | any | runner | todo | late writes: after revocation, a second app allocates the freed shared memory and fills it with a pattern while the old client writes garbage through its stale map; the pattern is intact |
| C-LIFE.24 | 5.5 | MUST | core | any | operator | todo | reset: with apps live, the operator resets the platform; afterwards a new run of the suite starts and passes and no claim of the old apps remains (`AAR_END_PLATFORM_RESET` is seen by the clients that survive) |
| C-LIFE.25 | 5.2 | MUST | core | any | runner | todo | relaunch: the same payload started 20 times one after the other behaves identically each time (same output, same exit code) |
| C-LIFE.26 | 5.1 | MUST | core | any | runner | todo | `AARClose` of a running app stops it, releases the claim and the signal and returns `AAR_OK`; the app's heartbeat stops |
| C-LIFE.27 | 5.3 | MUST | core | any | none | todo | not tested: a fault in runtime code ends every app and restarts the runtime (`AAR_END_RUNTIME_RESTART`); needs fault injection into the platform |
| C-LIFE.28 | 5.4 | MUST | core | any | none | todo | not tested: `AAR_END_STUCK_CALL` needs a table call that runs for 10 seconds inside the runtime, which no honest call does; the 10 s floor is [spec question 12] |
| C-LIFE.29 | 5.4 | MAY | core | any | none | todo | not tested: the runtime MAY stop an app that goes longer than the stall limit with `AAR_END_STALL` |
| C-LIFE.30 | 5.5 | MUST | core | any | none | todo | not tested: lease period values and the vertical-blank server are platform specifics; the outcome is C-LIFE.19 to C-LIFE.21 |
| C-LIFE.31 | 5.5 | MUST | core | any | none | todo | not tested: the owner-reap method (task address, name pointer, node type, CLI number, signal bit) is an implementation; the outcome is C-LIFE.20 |
| C-LIFE.32 | 5.1 | MUST | core | any | none | todo | not tested: whether state `FAULTED` persists after teardown is open in the spec [spec question 13]; C-LIFE.9 accepts `IDLE` or `FAULTED` with the end reason `USER_FAULT` |
| C-LIFE.33 | 5.2 | MUST | core | any | none | todo | not tested: incremental load, no step stalling other work for more than about a millisecond; a scheduling latency measurement of the platform, done by `aarlibtest latency` |
| C-LIFE.34 | 5.3 | MUST | core | any | none | todo | not tested: end reasons `LEASE`, `PLATFORM_RESET`, `QUARANTINED` as codes need the platform to produce them; seen in C-LIFE.22 and C-LIFE.24 where a platform can |
| C-LIFE.35 | 10.2 | MUST | core | any | none | todo | not tested: "everything reserved is released on every failure path" is C-FREE.7 |

## C-CLIENT: aar.library as a client sees it (sections 3.6, 5.6, 9, 10.2)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-CLIENT.1 | 9.1 | MUST | core | any | runner | yes | `aar.library` opens with version `AAR_LIB_VERSION` and its version equals the API major |
| C-CLIENT.2 | 9.2 | MUST | core | any | runner | yes | `AARQuery` needs no handle; with `size` 16 it changes only 16 bytes; with a larger `size` it zeroes the rest and writes nothing beyond |
| C-CLIENT.3 | 3.6 | MUST | core | any | runner | yes | `AARInfo.api_major` equals the suite's major and `api_minor` is at least the suite's minor |
| C-CLIENT.4 | 9.2 | MUST | core | any | runner | yes | `profiles` is non-zero, holds only defined bits and contains the profile under test |
| C-CLIENT.5 | 8.1 | MUST | core | any | runner | yes | `groups` holds only defined groups and experimental bits (28 to 31) |
| C-CLIENT.6 | 5.2 | MUST | core | any | runner | yes | `max_image_bytes` is at least 16 MiB, `max_args` at least 16 and `max_arg_bytes` at least 128 (127 bytes plus the NUL) |
| C-CLIENT.7 | 9.2 | MUST | core | any | runner | yes | `shared_granule` is a power of two of at least 64; `msg_capacity` is at least 32; `stall_limit_ms` is at least 10000; `shared_default_mib` is 16; `shared_max_mib` is at least `shared_default_mib` |
| C-CLIENT.8 | 10.4 | MUST | core | any | runner | yes | `queue_depth` is a power of two from 64 to 4096 and `msg_capacity` equals `queue_depth` minus 1 |
| C-CLIENT.9 | 9.2 | MUST | core | any | runner | todo | `AARProbeFile` and `AARProbeMemory` return the note fields of a conformance payload (profile, major, minor, stack, heap, groups) without loading it and without a handle |
| C-CLIENT.10 | 9.2 | MUST | core | any | runner | todo | `AARProbeFile` of a file that is not an ELF, of a truncated ELF and of a missing file fails with a code of section 3.5 and changes nothing |
| C-CLIENT.11 | 9.2 | MUST | core | any | runner | todo | `AAROpen` with a NULL notify port and a NULL `reason` succeeds; two claims have different handles |
| C-CLIENT.12 | 9.2 | MUST | core | any | runner | todo | `AARSetTagList` before the load accepts `AAR_TAG_PRIVATE_MIB`, `AAR_TAG_SHARED_MIB`, `AAR_TAG_AUDIO_ROUTE`, `AAR_TAG_LEASE_EXTENDED`, `AAR_TAG_CLIENT_CAPS`; `AAR_TAG_WEIGHT` gives `AAR_OK` or `AAR_E_NOSYS` |
| C-CLIENT.13 | 9.2 | MUST | core | any | runner | todo | `AARSetTagList` after the load began returns `AAR_E_BUSY`; a limit above 4095 MiB is `AAR_E_INVAL` |
| C-CLIENT.14 | 9.2 | MUST | core | any | runner | todo | `AAR_TAG_WEIGHT` above `AARInfo.weight_max` is `AAR_E_INVAL` where weighting exists; `weight_max` 0 means `AAR_E_NOSYS` for the tag |
| C-CLIENT.15 | 9.2 | MUST | core | any | runner | todo | `AARLoadMemory` of a payload and `AARLoadFile` of the same file produce the same run (= C-ELF.42 for accepted images) |
| C-CLIENT.16 | 9.2 | MUST | core | any | runner | todo | `AARStart` with `max_args` strings of `max_arg_bytes` each is accepted; the first string is the program name |
| C-CLIENT.17 | 9.2 | MUST | core | any | runner | todo | `AARStatus` with `size` 16 fills state, generation and end reason only; `generation` is non-zero; `load_base` is non-zero after the load; `profile` equals the image's |
| C-CLIENT.18 | 9.2 | MUST | core | any | runner | todo | `AARMessageSignal` returns a non-zero mask, the same mask for two handles of one task, and 0 for an invalid handle |
| C-CLIENT.19 | 9.4 | MUST | core | any | runner | todo | the signal is set when the app sends a message, when it exits and when it queues a service request |
| C-CLIENT.20 | 9.4 | SHOULD | core | any | runner | todo | the owner wakes within 250 ms of an app message (measured over 20 messages) |
| C-CLIENT.21 | 9.4 | MUST | core | any | runner | todo | a notify port receives one `AAREvent` at a time: the next is not posted until the previous is taken; its `handle` names the handle; the message needs no reply |
| C-CLIENT.22 | 9.4 | MUST | core | any | runner | todo | event types coalesce in the order `MESSAGE`, then `COMPLETE` or `FAULT` when the app has ended, else `REQUEST` |
| C-CLIENT.23 | 9.4 | MUST | core | any | runner | todo | a signal bit is allocated at a task's first open and freed at its last close: the number of free signal bits is the same before the first open and after the last close, with three handles in between |
| C-CLIENT.24 | 9.3 | MUST | core | any | runner | todo | a call with a handle from a task that does not own it returns `AAR_E_OWNER` (status, stop, send, close; the handle stays usable by its owner) |
| C-CLIENT.25 | 9.3 | MUST | core | any | runner | todo | one task can hold at least 20 handles when admission allows (the per-task signal bit is shared) |
| C-CLIENT.26 | 10.2 | MUST | core | any | runner | todo | a NULL handle is refused with `AAR_E_INVAL` and changes nothing |
| C-CLIENT.27 | 9.2 | MUST | core | any | runner | todo | `AARExt` needs no handle and returns NULL for an unknown platform id or extension id |
| C-CLIENT.28 | 9.2 | MUST | zz9000 | any | runner | todo | `AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT)` returns a size-first table |
| C-CLIENT.29 | 9.2 | MUST | core | any | runner | todo | `AARSharedToken` returns `AAR_OK` with a token or `AAR_E_NOSYS`; no other code for a valid buffer |
| C-CLIENT.30 | 9.6 | MUST | core | any | runner | todo | a serving loop that serves only some groups (`aar_serve_config.groups`) completes a request of an unserved group with `AAR_E_NOSYS` (the display open of a payload) |
| C-CLIENT.31 | 9.6 | MUST | core | any | runner | todo | `aar_serve` serves every request op of the groups it serves (files, console, log, messages, display, audio, input); the suite's payloads complete through it |
| C-CLIENT.32 | 9.1 | MUST | core | any | none | todo | not tested: calls return `LONG` in `d0`; seen in every call the runner makes |
| C-CLIENT.33 | 9.2 | SHOULD | core | any | none | todo | not tested: where section 9.2 and the `.fd` and `.h` files differ, the files are right; documentation rule |
| C-CLIENT.34 | 9.7 | MUST | core | any | none | todo | not tested: structure sizes (136, 164, 32, 128, 48) are checked by the header's `_Static_assert`s and `client/tests/m68k_check.sh` |
| C-CLIENT.35 | 9.1 | MUST | core | any | none | todo | not tested: `aar.library` MUST open on every platform that implements the standard; shown by every run of the suite |
| C-CLIENT.36 | 9.4 | SHOULD | core | any | none | todo | not tested: the library SHOULD wake the owner from the doorbell, or from a timer or vertical-blank poll; the observable half is C-CLIENT.20 |
| C-CLIENT.37 | 10.2 | MUST | core | any | none | todo | not tested: handle 0 invalid, generation 0 never issued, generation wrap at 65535 reuses; handle layout is transport internal [spec question 14] |
| C-CLIENT.38 | 10.6 | MAY | core | any | none | todo | not tested: a client MAY skip armed handles when the global event count is unchanged |

## C-DISPLAY: display group (section 8.2)

The runner supplies its own display table to `aar_serve` (pixel memory it names
with `AARSharedToken`, where the platform allows) and reads the presented frames
from it; with a real screen the picture is judged by the operator.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-DISPLAY.1 | 8.2 | MUST | display | any | payload | todo | `display_open` of 320x200, 640x480 and 4096x1 in RGB565 and BGRA8888 succeeds with width, height and format as asked, `pitch` at least width times bytes per pixel, `reserved` 0, `caps` only defined bits |
| C-DISPLAY.2 | 8.2 | MUST | display | any | payload | todo | `layout` is consistent with the format (RGB565: layouts 1 to 4; BGRA8888: 5 or 6; CLUT8: 7) and `back` is non-NULL and writable; `front` is NULL or writable |
| C-DISPLAY.3 | 8.2 | MUST | display | any | payload | todo | width 0 or height 0 is refused with a negative code; an unknown format is refused with `AAR_E_INVAL` or `AAR_E_NOSYS` [spec question 15] |
| C-DISPLAY.4 | 8.2 | MUST | display | any | payload | todo | one display per app: a second `display_open` while one is open fails with a negative code |
| C-DISPLAY.5 | 8.2 | MUST | display | any | payload | todo | `display_close` returns `AAR_OK`; a second close and a flip after close are negative; a new open works again |
| C-DISPLAY.6 | 8.2 | MUST | display | any | payload | todo | `flip` of the open id returns `AAR_OK`; a flip of any other id is `AAR_E_INVAL` |
| C-DISPLAY.7 | 8.2 | MUST | display | any | payload | todo | write `back`, `flip`, `refresh`: after the refresh `back` can be written again and `front` (when non-NULL) holds the frame just presented [spec question 16] |
| C-DISPLAY.8 | 8.2 | MUST | display | any | payload | todo | `AAR_DISPCAP_ASYNC` granted: `flip_async` then `flip_idle` reaches 1 within 1 s; a second async flip while one is pending is `AAR_E_BUSY` or completes (no frame is lost); without the cap `flip_async` is negative |
| C-DISPLAY.9 | 8.2 | MUST | display | any | payload | todo | `AAR_DISPLAY_LATEST` requested: the open succeeds with or without `AAR_DISPCAP_LATEST`; without the cap `display_latest_dropped` returns 0 |
| C-DISPLAY.10 | 8.2 | MUST | display | any | payload | todo | with `AAR_DISPCAP_LATEST` granted, 100 quick flips leave `display_latest_dropped` non-decreasing and not above 99 |
| C-DISPLAY.11 | 8.2 | MUST | display | any | payload | todo | `vsync` returns `AAR_OK`; ten vsyncs take at least 100 ms and at most 2 s |
| C-DISPLAY.12 | 8.2 | MUST | display | any | payload | todo | flip publishes the surface (runner peer): the frame the app flips without calling `aar_publish` is read by the client's display table byte for byte |
| C-DISPLAY.13 | 8.2 | MUST | display | any | payload | todo | pixel layout (runner peer): a four-colour test frame stored by the reported `layout` decodes in the client's table to the four colours, for each format the platform grants |
| C-DISPLAY.14 | 8.2 | MUST | display | any | payload | todo | `pitch` larger than the row bytes is honoured: rows stored by `pitch` arrive in the right rows (a surface whose pitch equals the row bytes is also accepted) |
| C-DISPLAY.15 | 8.2 | MUST | display | any | payload | todo | CLUT8 with `AAR_DISPCAP_CLUT8` granted: `set_palette` of 256 colours returns `AAR_OK`; first plus count above 256 is `AAR_E_INVAL`; the client's table received the colours |
| C-DISPLAY.16 | 8.2 | MUST | display | any | payload | todo | `set_palette` on a surface without the CLUT8 cap is negative |
| C-DISPLAY.17 | 8.2 | SHOULD | display | any | operator | todo | the picture: the operator sees the test pattern the payload shows in the colours named on the console |
| C-DISPLAY.18 | 8.2 | MAY | display | any | none | todo | not tested: a platform MAY allow more than 4096 in width or height |
| C-DISPLAY.19 | 9.6 | MUST | display | any | none | todo | not tested: a platform that owns the display buffers may complete a flip itself, so no flip entry is promised; no test may assume one (C-DISPLAY.6 to C-DISPLAY.12 allow both) |
| C-DISPLAY.20 | 8.2 | MUST | display | any | none | todo | not tested: the app MUST write `back`, flip, then `refresh` before writing again; an obligation of the app |

## C-INPUT: events and input (sections 8.3, 9.6, 10.4)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-INPUT.1 | 8.3 | MUST | core | any | payload | todo | `event_poll` never blocks: 1000 calls with nothing pending return `AAR_OK` with 0 events within 100 ms |
| C-INPUT.2 | 8.3 | MUST | core | any | payload | todo | a stop (`AARStop` and an `AAR_EV_STOP` posted with `AARPostEvent`) reaches `event_poll` as type 5 even without the input group |
| C-INPUT.3 | 8.3 | MUST | input | any | payload | todo | key down and key up events posted with `AARPostEvent` arrive in order with `code`, `buttons` and `time_ms` intact (types 1 and 2) |
| C-INPUT.4 | 8.3 | MUST | input | any | payload | todo | mouse events arrive in order with `dx`, `dy` and button bits intact (type 3) |
| C-INPUT.5 | 8.3 | MUST | input | any | payload | todo | joystick events arrive with axes in -1, 0, 1 and fire in bit 0 (type 4) |
| C-INPUT.6 | 8.3 | MUST | input | any | payload | todo | Ctrl-D, Ctrl-E and Ctrl-F signalled to the runner task arrive as `AAR_EV_BREAK` with codes 4, 5 and 6 (the serving loop's break mask) |
| C-INPUT.7 | 8.3 | MUST | input | any | payload | todo | `AAR_EV_RESIZE` (6) is never delivered during the whole input test |
| C-INPUT.8 | 8.3 | MUST | input | any | payload | todo | the runtime retains at least 64 events: 64 key events posted while the app does not poll all arrive in order |
| C-INPUT.9 | 8.3 | MUST | input | any | runner | todo | with the queue overfull the client drops the new events, never the old ones: the app finds the first events in order, and `AARStatusInfo.events_dropped` equals the number posted minus the number received |
| C-INPUT.10 | 9.6 | MUST | input | any | payload | todo | mouse motion posted while the queue is full: the sum of `dx` received plus `events_dropped` equals the sum posted (merged or counted as dropped, never lost silently) [spec question 17] |
| C-INPUT.11 | 8.3 | SHOULD | input | any | operator | todo | real keyboard, mouse and joystick events reach the app with the up bit removed from keys: the operator presses keys and moves the mouse and confirms the codes the payload prints |
| C-INPUT.12 | 8.3 | MAY | input | any | none | todo | not tested: the client MAY coalesce mouse motion when the queue is full (C-INPUT.10 allows both) |
| C-INPUT.13 | 8.3 | MUST | input | any | none | todo | not tested: "the runtime keeps at least 64 events" beyond the completion queue's own 64 entries is internal; the outcome is C-INPUT.8 |
| C-INPUT.14 | 8.3 | MUST | core | any | none | todo | not tested: an idle app uses `msg_wait`, `sleep_us` or `yield` and not a spin; advice to apps |

## C-AUDIO: audio group (sections 8.4, 9.6)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-AUDIO.1 | 8.4 | MUST | audio | any | payload | todo | `audio_open(22050, 2)` succeeds; the grant has `channels` 2, `rate` one of 22050, 44100, 48000, `frames` a power of two, `flags` 0, `samples`, `read_frame` and `write_frame` non-NULL |
| C-AUDIO.2 | 8.4 | MUST | audio | any | payload | todo | the ring header is where the standard puts it: `read_frame` is `write_frame` plus 0x40 bytes, `samples` is `write_frame` plus 0x80 bytes, the little-endian words at +4 (rate), +8 (channels), +0xC (format `AAR_AUDIO_FORMAT_S16LE`) agree with the grant [spec question 18] |
| C-AUDIO.3 | 8.4 | MUST | audio | any | payload | todo | `audio_open` of 44100 and 48000 stereo returns `AAR_OK` with a supported pair or a refusal of `AAR_E_INVAL`; a granted pair is written into `out` |
| C-AUDIO.4 | 8.4 | MUST | audio | any | payload | todo | an unsupported rate (8000, 96000) or channel count (1, 0, 6) is `AAR_E_INVAL`; `channels` is always 2 in a grant |
| C-AUDIO.5 | 8.4 | MUST | audio | any | payload | todo | the app writes 1 second of samples ahead of `*read_frame` (never more than `frames` ahead); the runtime advances `*read_frame` at the granted rate within 25 percent over 1 second, and `read_frame` never passes `write_frame` (route PLATFORM) |
| C-AUDIO.6 | 8.4 | MUST | audio | any | payload | todo | when the app stops writing, underruns are counted: `audio_get_stats` (when present) shows more than before and equals the ring's word at +0x44 |
| C-AUDIO.7 | 8.4 | MUST | audio | any | payload | todo | `audio_get_stats` honours `size` (8 fills `size` and `underruns` only); a NULL slot returns `AAR_E_NOSYS` |
| C-AUDIO.8 | 8.4 | MUST | audio | any | payload | todo | `audio_close` detaches the stream, returns `AAR_OK`, a new open works, a second close is negative |
| C-AUDIO.9 | 8.4 | SHOULD | audio | any | payload | todo | a second `audio_open` in the same app is `AAR_E_BUSY`; opens in more apps than the platform has streams are `AAR_E_BUSY` (the runner opens apps until the first refusal) |
| C-AUDIO.10 | 9.6 | MUST | audio | any | runner | todo | route `AHI` (`AAR_TAG_AUDIO_ROUTE`): `AARAudioRead` with `frames` 0 returns the frames available |
| C-AUDIO.11 | 9.6 | MUST | audio | any | runner | todo | route `AHI`: the PCM `AARAudioRead` returns is exactly what the app wrote, in order (a ramp of 48000 frames) |
| C-AUDIO.12 | 8.4 | SHOULD | audio | any | operator | todo | sound: the operator hears the 440 Hz tone the payload plays for two seconds and confirms |
| C-AUDIO.13 | 9.6 | MUST | audio | any | none | todo | not tested: a failed `AUDIO_CLOSE` retains the ring while a consumer can access it; needs failure injection |
| C-AUDIO.14 | 8.4 | MUST | audio | any | none | todo | not tested: each ring index has its own cache line and one writer, indices are free-running `uint32_t` counters that wrap (a wrap needs 2^32 frames, about 54 hours) |
| C-AUDIO.15 | 8.4 | MUST | audio | any | none | todo | not tested: the ring is little-endian and the library swaps the words it reads; the effect is C-AUDIO.11 |

## C-GL: GL group (section 8.5)

The payload is `templates/mgl_conformance.c` adapted to print these ids; its
matrix cases map onto C-GL.6.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-GL.1 | 8.5 | MUST | gl | any | payload | todo | `aar_gl_bind` of an open surface returns a table with `size` and `version` set, `version` `AAR_GL_VERSION_V0` or `AAR_GL_VERSION_ARRAYS` |
| C-GL.2 | 8.5 | MUST | gl | any | payload | todo | every slot of the table that `size` covers up to `Build2DMipmaps` is non-NULL; with `AAR_GL_VERSION_ARRAYS` the array entries are non-NULL too; fields past `Build2DMipmaps` are used only when `AAR_GL_COVERS` says so |
| C-GL.3 | 8.5 | MUST | gl | any | payload | todo | `aar_gl_bind` with an id that is not an open surface returns NULL |
| C-GL.4 | 8.5 | MUST | gl | any | payload | todo | `aar_gl_unbind` releases the context; a new bind works |
| C-GL.5 | 8.5 | MUST | gl | any | payload | todo | `aar_gl_swap` flushes, copies and flips: a cleared colour is read by the client's display table after the swap (runner peer) |
| C-GL.6 | 8.5 | MUST | gl | any | payload | todo | the `mgl_conformance` matrix cases pass (each case gets its own detail in the line) |
| C-GL.7 | 8.5 | MUST | gl | any | payload | todo | `Perspective`, `Ortho`, `Frustum` and `DepthRange` take doubles and `gluPerspective` takes four doubles: a scene drawn with them has the expected pixels |
| C-GL.8 | 8.5 | MUST | gl | any | payload | todo | on an isolated runtime a client-array draw with a bad pointer returns `AAR_E_FAULT` and sets `GL_INVALID_VALUE` (passes with detail `not isolated` otherwise) |
| C-GL.9 | 8.5 | MUST | gl | any | none | todo | not tested: integer parameters are `int32_t` and `uint32_t`; the matrix and header checks cover the widths |
| C-GL.10 | 8.5 | MUST | gl | any | none | todo | not tested: debug bind flags are not part of the standard (ZZ9000 extension, 8.8); tested only through C-TABLE.23 |
| C-GL.11 | 8.5 | MUST | gl | any | none | todo | not tested: the `gl*` wrappers forward to the table that `aar_gl_bind` returned (libaar, 3.8) |

## C-SERVICES: services group (section 8.6)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-SERVICES.1 | 8.6 | MUST | services | any | payload | todo | `aar_sha256` gives the known digests of the empty message, `abc`, a 1 MiB pattern, and an unaligned buffer of 1000 bytes |
| C-SERVICES.2 | 8.6 | MUST | services | any | payload | todo | `aar_decompress` for each of the ten algorithms either returns `AAR_E_NOSYS` or the exact bytes of a known stream; `bytes_consumed` and `bytes_written` are right |
| C-SERVICES.3 | 8.6 | MUST | services | any | payload | todo | `AAR_COMPRESS_NONE` and an unknown algorithm number are refused (`AAR_E_INVAL`) |
| C-SERVICES.4 | 8.6 | MUST | services | any | payload | todo | a destination that is too small gives an error and writes nothing beyond `dst_bytes` (guard bytes intact); a truncated stream gives an error |
| C-SERVICES.5 | 8.6 | MUST | services | any | payload | todo | PNG decode through an image session is exact (BGRA8888 tiles of a known image); a codec that is not implemented gives `AAR_E_NOSYS` from `image_open` |
| C-SERVICES.6 | 8.6 | MUST | services | any | payload | todo | JPEG and WebP decode of known images stay within 3 per channel of the reference pixels (decoders differ in rounding) |
| C-SERVICES.7 | 8.6 | MUST | services | any | payload | todo | FLAC decode is exact; MP3 and Vorbis decode stay within a root-mean-square error of 64 of the reference PCM; a codec that is not implemented gives `AAR_E_NOSYS` |
| C-SERVICES.8 | 8.6 | MUST | services | any | payload | todo | at least four image sessions and four audio sessions can be open at once (`AAR_MIN_DECODE_SESSIONS`); the first refusal beyond is `AAR_E_NOMEM` |
| C-SERVICES.9 | 8.6 | MUST | services | any | payload | todo | session calls with an unknown session id are `AAR_E_INVAL`; a closed session id is refused |
| C-SERVICES.10 | 8.6 | MUST | services | any | none | todo | not tested: byte counts of whole buffers are `uint64_t`; a test would need buffers of 4 GiB or more |
| C-SERVICES.11 | 8.6 | MAY | services | any | none | todo | not tested: a platform MAY implement any subset of algorithms and codecs (C-SERVICES.2, 5 and 7 accept `AAR_E_NOSYS`) |

## C-FREE: freestanding build, leaks, release (sections 3.8, 5.6, 8.6, 10.2)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-FREE.1 | 3.8 | MUST | core | any | payload | todo | the freestanding payload `conf_free` (built with `AAR_FREESTANDING=1`, linked without libc) runs and hashes a 3 MiB buffer to the known FNV-1a value using only `heap_range` and `heap_grow` |
| C-FREE.2 | 3.8 | MUST | core | any | payload | todo | in the freestanding flavour the weak `memcpy`, `memmove`, `memset` and `memcmp` that the compiler calls work (a struct copy, an overlapping move, a large fill, a compare) |
| C-FREE.3 | 3.8 | MUST | core | any | none | todo | not tested: the freestanding link contains no archive but `libaar.a` and `libgcc.a`; checked at build time by `tools/aar-check --no-libc` and `make check-freestanding-guard` |
| C-FREE.4 | 5.6 | MUST | core | any | runner | todo | heap: a payload that takes the largest heap it can get and exits without freeing runs 20 times; the last run gets as much as the first |
| C-FREE.5 | 5.6 | MUST | core | any | runner | todo | shared memory: the same for shared blocks (with console and file service buffers in use) |
| C-FREE.6 | 5.6 | MUST | core | any | runner | todo | claims: 200 rounds of open, load, start, exit, close never end in `AAR_E_NORESOURCE` or `AAR_E_NOMEM` |
| C-FREE.7 | 10.2 | MUST | core | any | runner | todo | after the whole negative corpus of C-ELF (every refusal path), the number of claims, the heap and the shared memory a payload can get are the same as before |
| C-FREE.8 | 8.6 | MUST | services | any | runner | todo | a payload that exits with decode sessions open, 20 times: the last run can still open four sessions of each kind |
| C-FREE.9 | 4.2 | MUST | display | any | runner | todo | a payload that exits with the display open, 20 times: each run opens it again |
| C-FREE.10 | 8.4 | MUST | audio | any | runner | todo | a payload that exits with the audio stream open, 20 times: each run opens it again |
| C-FREE.11 | 7.1 | MUST | core | any | none | todo | not tested here: the release of files and lists that an exiting app left open is C-FILE.31 |

## Spec questions for review

Statements that are ambiguous, contradict another statement, or cannot be tested
as written. Each has a number that the tables refer to. Slice 13 decides them.

1. **Verdict for statements without a keyword** (11.1). Sections 4 to 10 state
   many rules in plain present tense ("`aar_time_us` returns monotonic
   microseconds", "File ids are small integers, at least 3"). The suite treats
   them as MUST because 11.1 says "every MUST test"; the spec should say that
   rules without a keyword are normative, or mark the ones that are not.
2. **The recommended path policy** (7.1) is a recommendation of the client
   (`aar-run`), not of the runtime, yet it is the only observable behaviour of
   the path rules. C-FILE.4 and C-FILE.5 are SHOULD and test `aar_serve`'s
   defaults; the spec should say whether a conforming client must follow them.
   Also unstated: the error for a path longer than 255 bytes (C-FILE.2) and for
   a read through a write-only id (C-FILE.9).
3. **Entry mode** (3.2). "user, A32 state, little-endian, interrupts enabled" and
   "EL0, AArch64" cannot be read from user mode: `MRS CPSR` in user mode returns
   only the APSR bits, and `CurrentEL` and `DAIF` trap at EL0. Either name an
   observable consequence or drop the sentence from the test surface.
4. **Group bits** (8.1). Bits 5 to 27 are neither defined nor reserved. Say
   whether a platform may set them (this suite cannot tell experiments, bits 28
   to 31, from a wrong declaration).
5. **LOW_MEMORY** (4.1) has no thresholds ("once per low-water crossing", "high-water
   mark"). A test has to exhaust the pool, which the host runner cannot do
   (heap arena fixed at 8 MiB with no trigger), so C-MEM.12 cannot pass there
   (the host build prints `SKIP host:`). State the watermarks as a fraction of
   the pool or give apps a way to provoke the event.
6. **Isolation** (4.4): "apps cannot reach firmware, other apps or the host's
   memory" is a security property; no black-box test is possible. Keep it as
   documentation or define what a test should show.
7. **Deliberately cacheable client map** (4.3, 11.2). `AARSharedMap` cache-inhibits
   "when it can", and no flag asks for a cacheable map, so the second half of
   the C-SHARED stale-read test (map cacheable plus `AARSharedAcquire`) has no
   API. Add an `AARSharedMap` flag (the `flags` argument is reserved) or an
   `AARSharedAcquire` obligation test on platforms that cannot do otherwise.
8. **Corrupt queue** (10.4): "a count above `depth` means the block is corrupt
   (`AAR_E_IO`)" needs write access to the control block; the client API offers
   none. The status flag `AAR_SERVE_R_QUEUE_CORRUPT` exists, but no way to cause
   it.
9. **Overlong path** (7.1): "at most 255 bytes plus NUL" but no error code for 256
   bytes (`AAR_E_INVAL`, `AAR_E_NOENT` and `AAR_E_ACCES` are all plausible).
   Also: a 255-byte path on AmigaDOS needs components of up to 107 characters
   (FFS), which fails on OFS volumes; say what the fixture may assume.
10. **File access modes** (7.1): no error code is given for a write through an id
    opened `READ` only, or a read through one opened `WRITE` only.
11. **Alignment faults** (5.3): the class `AAR_FAULT_ALIGN` is listed but an
    alignment fault is only produced for some instructions (exclusive and
    multiple loads, device memory); say which fault the standard expects the
    runtime to classify as `ALIGN` and whether unaligned plain loads (usually
    legal) are faults at all.
12. **Stuck call** (5.4): "a single call that runs longer than the runtime's
    stuck-call limit (at least 10 s)" cannot be made by a conforming app, since
    calls park in declared waits that do not count; C-LIFE.28 is not tested.
13. **State after a fault** (5.1): "whether state `FAULTED` persists after
    teardown is to be pinned down before 1.0". C-LIFE.9 accepts both.
14. **Generation wrap** (10.2, open): a platform may tolerate a stale handle that
    names a live app after 65535 reuses; the suite cannot test either answer.
15. **Unknown display format** (8.2): `AAR_E_INVAL` (unknown argument) or `AAR_E_NOSYS`
    ("format not implemented", the CLUT8 case)? C-DISPLAY.3 accepts both.
16. **What `front` holds after `flip` and `refresh`** (8.2): the text does not say
    that `front` shows the frame just presented; C-DISPLAY.7 assumes it.
17. **Mouse coalescing** (8.3 against 9.6): 8.3 says the client MAY coalesce when
    the queue is full; 9.6 says `AARPostEvent` merges mouse motion while the queue
    is full (no MAY). C-INPUT.10 accepts both through the sum rule; choose one.
18. **Audio ring format value** (8.4): `AAR_AUDIO_FORMAT_S16LE` has no stated
    number in the text; C-AUDIO.2 uses the header's value. Also, the `aar_audio`
    pointers are in the app's address space, so the "plus 0x40" and "plus 0x80"
    offsets assume the three pointers point into one block; say that.
19. **Level of the weighted share** (5.6): "SHOULD give runnable apps CPU in
    proportion to their weight" has no tolerance; C-QUEUE.15 uses 30 percent.
    "MUST NOT let an app that is parked consume CPU" is testable only as a
    throughput bound (C-QUEUE.16, 90 percent).
20. **Resolution and granularity** (7.3): "resolution SHOULD be 1 ms or finer" is
    testable; "actual granularity is `sleep_granule_us`" has no upper bound on
    oversleep, so C-TIME.4 uses four granules plus 10 ms, a judgement call.
21. **Where the many-apps tests belong** (11.2): the table lists admission,
    release and CPU shares under C-QUEUE, but sections 5.6 and 10.2 are lifecycle
    and client rules. The suite keeps the spec's placement (C-QUEUE.12 to
    C-QUEUE.16).
22. **Unserved ops** (9.6): "requests of an unserved op are completed with
    `AAR_E_NOSYS` by the library" does not say how the library knows an op is
    unserved when the client simply never replies. C-CLIENT.30 tests the
    `aar_serve_config.groups` mechanism instead.
23. **Probe versus load** (9.2): `AARProbeFile` "does not judge the image against
    the platform"; no statement says what it returns for a note that is present
    but invalid (bad magic): the suite expects an error code, C-CLIENT.10.
24. **Handle and NULL** (10.2): "handle 0 is invalid" is stated for the transport
    value; the client API takes a pointer. C-CLIENT.26 assumes NULL gives
    `AAR_E_INVAL`.
25. **AARSharedToken** (9.2): "the token of a client buffer for the app" has no app
    side use defined (an app cannot map client memory), so the call is only
    checked for its return code (C-CLIENT.29).
26. **Zero-length grow** (4.1): `heap_grow(0)` is "a whole number of granules";
    the suite does not test it (C-MEM.7 uses non-multiples only).
27. **State right after a load** (5.1 table): LOADING to STARTING on "load
    complete, `AARStart`" does not say whether the state after `AARLoadFile`
    returns is `LOADING` or `STARTING`; C-LIFE.1 tests only `CLAIMED` after open
    and the end states.
28. **Revoking a claim** (5.5, 11.2): the lease, stale-handle and late-write rules
    can only be provoked through a platform's own administrative call (the
    ZZ9000 `AARExt` release and lease pause), because the standard has no
    call that revokes a claim or pauses the lease. C-LIFE.22 and C-LIFE.23 are
    therefore group `zz9000`; for another platform the rules stay untested
    until the standard offers a way, or the platform brings its own tests.
29. **Unsatisfiable limits** (4.2): "a request the platform could never satisfy
    fails the load (`AAR_E_NOMEM`)" does not say when a limit is unsatisfiable
    (`shared_max_mib` is the nearest figure) or whether `AARSetTagList` may
    refuse it first (9.2 gives `AAR_E_INVAL` above 4095 MiB only).
