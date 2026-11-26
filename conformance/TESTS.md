# AAR conformance catalogue

Suite version: 0.2
Specification: AAR API 0.6 (draft), section 11

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
only for a test of an undeclared group or a `SHOULD` test, and has a reason.
One more skip is allowed: a `MUST` row whose check text says `SKIP no PLATFORM route`
(C-AUDIO.1 to C-AUDIO.9) may skip with a reason that starts with those words, because
the route is optional (spec 8.4).
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
| C-ELF.1 | 2.2.1 | MUST | core | any | runner | yes | `e_type` ET_EXEC refused |
| C-ELF.2 | 2.2.2 | MUST | core | any | runner | yes | wrong `EI_CLASS` for the profile refused |
| C-ELF.3 | 2.2.2 | MUST | core | any | runner | yes | `ELFDATA2MSB` refused |
| C-ELF.4 | 2.2.2 | MUST | core | any | runner | yes | wrong `e_machine` refused |
| C-ELF.5 | 2.2.2 | MUST | core | a32 | runner | yes | `EF_ARM_ABI_FLOAT_HARD` clear (soft-float or no float flag) refused |
| C-ELF.6 | 2.2.2 | MUST | core | a32 | runner | yes | EABI version other than 5 refused |
| C-ELF.7 | 2.2.2 | MUST | core | a64 | runner | yes | non-zero `e_flags` refused |
| C-ELF.8 | 2.2.3 | MUST | core | any | runner | yes | image with `PT_INTERP` refused |
| C-ELF.9 | 2.2.3 | MUST | core | any | runner | yes | image with `DT_NEEDED` refused |
| C-ELF.10 | 2.2.3 | MUST | core | any | runner | yes | five `PT_LOAD` segments refused |
| C-ELF.11 | 2.2.3 | MUST | core | any | runner | yes | a segment that is writable and executable refused |
| C-ELF.12 | 2.2.3 | MUST | core | any | runner | yes | `p_memsz` smaller than `p_filesz` refused |
| C-ELF.13 | 2.2.3 | MUST | core | any | runner | yes | overlapping `PT_LOAD` segments refused |
| C-ELF.14 | 2.2.3 | MUST | core | any | runner | yes | a segment larger than any memory the runtime offers refused (`AAR_E_NOMEM` is also accepted) |
| C-ELF.15 | 2.2.4 | MUST | core | any | runner | yes | image with no `PT_DYNAMIC` is refused with `AAR_E_INVAL` [spec question 61] |
| C-ELF.16 | 2.2.4 | MUST | core | any | runner | yes | image with no section headers loads and runs (relocations come from `PT_DYNAMIC`), and `AARProbeFile` accepts it [spec question 39] |
| C-ELF.17 | 2.2.5 | MUST | core | a32 | runner | yes | a `REL` table loads and runs |
| C-ELF.18 | 2.2.5 | MUST | core | a32 | runner | yes | a `RELA` table loads and runs |
| C-ELF.19 | 2.2.5 | MUST | core | any | runner | yes | a relocation type other than `RELATIVE` refused |
| C-ELF.20 | 2.2.5 | MUST | core | any | runner | yes | `DT_RELR` refused |
| C-ELF.21 | 2.2.5 | MUST | core | any | runner | yes | a relocation with symbol index other than 0 refused |
| C-ELF.22 | 2.2.5 | MUST | core | any | runner | yes | a relocation with a misaligned target refused |
| C-ELF.23 | 2.2.5 | MUST | core | any | runner | yes | a relocation target outside the memory (`p_memsz`) of every `PT_LOAD` is refused: in a gap between segments, at the end of the memory, or without room for a full word; a target in the bss tail loads and runs [spec question 61] |
| C-ELF.24 | 2.2.6 | MUST | core | any | runner | yes | entry point outside an executable segment refused |
| C-ELF.25 | 2.2.6 | MUST | core | a32 | runner | yes | entry point with bit 0 set refused |
| C-ELF.26 | 2.2.7 | MUST | core | any | runner | yes | image with no AAR note refused |
| C-ELF.27 | 2.2.7 | MUST | core | any | runner | yes | image with two AAR notes refused |
| C-ELF.28 | 2.3 | MUST | core | any | runner | yes | note with a wrong `magic` refused |
| C-ELF.29 | 2.3 | MUST | core | any | runner | yes | `note_version` other than 1 refused |
| C-ELF.30 | 2.3 | MUST | core | any | runner | yes | note `profile` that differs from the ELF class refused with `AAR_E_PROFILE` |
| C-ELF.31 | 2.3 | MUST | core | any | runner | yes | an image of a profile that `AARInfo.profiles` does not list is refused with `AAR_E_PROFILE` or `AAR_E_INVAL` (2.2 allows both for the class; passes with detail `all profiles supported` when the platform lists every defined profile) [spec question 61] |
| C-ELF.32 | 2.3 | MUST | core | any | runner | yes | `api_major` other than the platform's refused with `AAR_E_INVAL` |
| C-ELF.33 | 2.3 | MUST | core | any | runner | yes | `api_minor_required` above the platform's minor, or below `AAR_API_MINOR_OLDEST`, refused with `AAR_E_INVAL` |
| C-ELF.34 | 2.3 | MUST | core | any | runner | yes | `stack_bytes` below 64 KiB refused |
| C-ELF.35 | 2.3 | MUST | core | any | runner | yes | `stack_bytes` that is not a multiple of 16 refused |
| C-ELF.36 | 2.3 | MUST | core | any | runner | yes | `stack_bytes` 0, 64 KiB and 1 MiB all load and run |
| C-ELF.37 | 2.3 | MUST | core | any | runner | yes | `groups_required` with a bit that no platform sets (reserved bit 5 or 27, experimental bit 28 or 31) refused with `AAR_E_NOGROUP`; unknown bits of `groups_optional` are ignored: the image loads and runs [spec question 4] |
| C-ELF.38 | 2.3 | MUST | core | any | runner | yes | a note with wrong name or descriptor padding is not an AAR note: refused |
| C-ELF.39 | 5.2 | MUST | core | any | runner | yes | `AARInfo.max_image_bytes` is at least 16 MiB |
| C-ELF.40 | 4.1 | MUST | core | any | runner | yes | `min_heap_mib` above anything the platform can commit (0xFFFFFFFF, and 0x10000001, which wraps to 1 MiB in 32-bit bytes) is refused. The load call returns `AAR_E_NOMEM`, or `AAR_E_NORESOURCE` with a reason in `AARStatusInfo.reason` [spec questions 40 and 60] |
| C-ELF.41 | 5.2 | MUST | core | any | runner | yes | every load refusal is one of `AAR_E_PROFILE`, `AAR_E_NOGROUP`, `AAR_E_INVAL`, `AAR_E_NOMEM` (or `AAR_E_NORESOURCE` with a reason, spec question 60); checked over every image in the corpus |
| C-ELF.42 | 2.2 | MUST | core | any | runner | yes | the same image loaded with `AARLoadMemory` gives the same result as `AARLoadFile` (accept and refuse alike) |
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
| C-ENTRY.4 | 3.2 | MUST | core | any | payload | yes | a second run (variant `long`) with 16 arguments of 127 bytes each, `argv[0]` included (a 127-byte path that ends in the file name), delivers every byte intact. A platform whose `AARInfo.max_args` or `max_arg_bytes` is below 16 and 128 fails here (`argv[0]` is one of the 16 and the NUL is one of the 128, spec 3.2) |
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
| C-ENTRY.23 | 3.2 | MUST | core | a32 | runner | yes | when `AAR_INFOF_ISOLATED` is set, an app that executes a privileged instruction (`mcr p15, 0, r0, c1, c0, 0`, the variant `fault-privileged` of `conf_life`) ends with `AAR_END_USER_FAULT` and fault class `AAR_FAULT_ILLEGAL` or `AAR_FAULT_OTHER`; passes with detail `not isolated` when the flag is clear; running at all proves A32 state |
| C-ENTRY.24 | 3.2 | MUST | core | a64 | runner | yes | when `AAR_INFOF_ISOLATED` is set, an app that executes a privileged instruction (`msr vbar_el1, x0`, the variant `fault-privileged` of `conf_life`) ends with `AAR_END_USER_FAULT` and fault class `AAR_FAULT_ILLEGAL` or `AAR_FAULT_OTHER` (a trapped system register access may be classed either way); passes with detail `not isolated` when the flag is clear; running at all proves AArch64 state |
| C-ENTRY.25 | 3.2 | MUST | core | any | none | todo | not tested: "sp points one past the last usable byte" cannot be observed without a second stack bound; covered by C-ENTRY.5 and C-ENTRY.6 for alignment |
| C-ENTRY.26 | 3.2 | MUST | core | any | none | todo | not tested: returning from the entry point without exiting is undefined, so there is nothing to require |
| C-ENTRY.27 | 3.1 | MUST | core | any | none | todo | not tested: apps must not cache or compare table addresses and must not write the table; an obligation of the app |

## C-TABLE: the table, info, groups, extensions (sections 3.3, 3.4, 3.5, 3.6, 8.1, 8.7, 8.8)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-TABLE.1 | 3.3 | MUST | core | any | payload | yes | the table `size` covers every core slot (`exit` to `clip_write`) and every core slot is non-NULL |
| C-TABLE.2 | 3.3 | MUST | core | any | payload | yes | `version` names API major `AAR_API_MAJOR` in its high 16 bits and equals the minor `AARInfo.api_minor` in its low 16 bits (the runner passes the client value) |
| C-TABLE.3 | 3.3 | MUST | core | any | payload | yes | the table `groups` word equals `aar_info.groups` and the runner's `AARInfo.groups`, and `aar_info.flags` equals `AARInfo.flags` (spec 3.4) |
| C-TABLE.4 | 3.3 | MUST | display | any | payload | yes | with the display group declared the table covers the sixteen display slots and none is NULL |
| C-TABLE.5 | 3.3 | MUST | audio | any | payload | yes | with the audio group declared the table covers `audio_open` and `audio_close` and they are non-NULL (`audio_stats` may be NULL) |
| C-TABLE.6 | 3.3 | MUST | gl | any | payload | yes | with the GL group declared the table covers and fills `gl_bind`, `gl_swap`, `gl_unbind` |
| C-TABLE.7 | 3.3 | MUST | services | any | payload | yes | with the services group declared the table covers and fills the eight services slots |
| C-TABLE.8 | 3.3 | MUST | core | any | payload | yes | for each group whose bit is clear, every slot of the group that `size` covers is NULL (a slot that is present fails the row and is not called), and the group's wrappers return `AAR_E_NOSYS` (or 0, NULL, nothing) and never fault (spec 3.3 rule 2) |
| C-TABLE.9 | 3.3 | MUST | core | any | payload | yes | `aar_map_file`, `aar_unmap_file` and `aar_ext` are either absent (the wrapper gives `AAR_E_NOSYS`, `ext` gives NULL) or work: a 16-byte file the payload writes maps with the right bytes, and `AAR_E_NOSYS` from a present `map_file` is accepted (7.1: a platform MAY) |
| C-TABLE.10 | 3.3 | MUST | core | any | payload | yes | `platform_id` in the table equals `aar_info.platform_id` and the runner's `AARInfo.platform_id`, and `aar_info.shared_granule` equals `AARInfo.shared_granule` (spec 3.4) |
| C-TABLE.11 | 3.4 | MUST | core | any | payload | yes | `aar_get_info` with a `size` of 16 changes only the first 16 bytes of the caller's structure |
| C-TABLE.12 | 3.4 | MUST | core | any | payload | yes | `aar_get_info` with a `size` larger than `sizeof(aar_info)` zeroes the bytes past the fields it knows, up to `size`, and writes nothing beyond |
| C-TABLE.13 | 3.4 | MUST | core | any | payload | yes | `aar_info.profile` equals the profile the app was built for and is a member of the client's `AARInfo.profiles` (the runner passes the mask in the `info:aarinfo` word; without it the membership is not checked) |
| C-TABLE.14 | 3.4 | MUST | core | any | payload | yes | `heap_granule` is 1 MiB; `shared_granule` is a power of two of at least 64 |
| C-TABLE.15 | 3.4 | MUST | core | any | payload | yes | `msg_capacity` is at least 32 and equals the runner's `AARInfo.msg_capacity` (spec 3.4) |
| C-TABLE.16 | 3.4 | MUST | core | any | payload | yes | `stall_limit_ms` is at least 10000 and equals the runner's value; `stop_grace_ms` equals the runner's value (an app and its client describe one platform, spec 3.4) |
| C-TABLE.17 | 3.4 | MUST | core | any | payload | yes | `max_files` is at least 16 |
| C-TABLE.18 | 3.4 | MUST | core | any | payload | yes | `shared_limit` is `shared_default_mib` MiB (16 MiB, `AAR_SHARED_DEFAULT_MIB`) when the client set none, and `shared_used` does not exceed it |
| C-TABLE.19 | 3.4 | MUST | core | any | payload | yes | `aar_heap_info` fills `committed`, `limit` and `pool_free`; `committed` does not exceed `limit` |
| C-TABLE.20 | 8.7 | MUST | core | a32 | payload | yes | every `AAR_CPU_A32_*` bit set in `cpu_features` identifies a supported feature: the app executes a VFPv4 fused multiply-add, an integer divide, a half-precision convert as advertised without a fault |
| C-TABLE.21 | 8.7 | MUST | core | a64 | payload | yes | every `AAR_CPU_A64_*` bit set identifies a supported feature: CRC32, LSE atomics, AES, SHA, dot product, FP16 execute without a fault |
| C-TABLE.22 | 8.8 | MUST | core | any | payload | yes | `aar_ext` with an unknown platform id or extension id returns NULL |
| C-TABLE.23 | 8.8 | MUST | zz9000 | any | payload | yes | `aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)` returns a table whose `size` covers `build_id`; `gl_bind_flags` is NULL when the GL group is absent |
| C-TABLE.24 | 3.5 | MUST | core | any | payload | yes | the error codes a call returns are the ones of section 3.5: over a battery of failing calls the payload makes (files, heap, shared buffers, groups, absent slots), no code is outside -1 to -17 |
| C-TABLE.25 | 3.6 | MUST | core | any | none | todo | not tested here: the major and minor check on the client's view is C-CLIENT.3 |
| C-TABLE.26 | 3.3 | MUST | core | any | none | todo | not tested: the slot order, offsets and the 77 slots (42 core, `ext`, `map_file`, `unmap_file`, the optional `file_get_comment` and `file_set_comment`, the groups) are pinned by the header's `_Static_assert`s and `tests/abi_check.sh` in CI. The two comment slots are optional and may be NULL: C-TABLE.1 covers the 42 core slots only, and C-FILE.53 to C-FILE.55 exercise the comment wrappers |
| C-TABLE.27 | 3.3 | MUST | core | any | none | todo | not tested: apps MUST NOT gate on `version`; an obligation of the app |
| C-TABLE.28 | 3.5 | MUST | core | any | none | todo | not tested: the numeric values of the error codes are pinned by the header and `tests/abi_check.sh` |
| C-TABLE.29 | 8.8 | SHOULD | core | any | none | todo | not tested: an app that uses an extension SHOULD say it is not portable; documentation of the app |
| C-TABLE.30 | 8.1 | MUST | core | any | payload | yes | neither the table `groups` word nor `aar_info.groups` has a bit from 5 to 27 set (reserved for later versions of the standard, spec 8.1); bits 28 to 31 are for experiments and are allowed; the client's `AARInfo.groups` is checked by C-CLIENT.5 |
| C-TABLE.31 | 3.3 | MUST | core | any | none | todo | not tested: `AAR_API_COVERS` and the wrapper gating belong to libaar, tested by `libaar/tests/runtime_test.sh` (`CORE_ONLY=1`) |

## C-MEM: private memory (section 4.1)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-MEM.1 | 4.1 | MUST | core | any | payload | yes | `heap_range` returns a base and a length, and the length is strictly below `heap_info.committed`, which counts the image and the stack as well as the heap (spec 3.4, 4.1); an equal value fails when the runner is the client, and in a fixture that is not a platform (host runner, qemu loader) it ends as `SKIP host:` when `committed` counts the heap only |
| C-MEM.2 | 4.1 | MUST | core | any | payload | yes | `heap_grow` of one granule returns `AAR_OK` and an old break equal to the previous end (base plus length); `heap_range` grows by it and `heap_info.committed` rises by the same bytes |
| C-MEM.3 | 4.1 | MUST | core | any | payload | yes | successive grows are contiguous: each old break equals the previous end |
| C-MEM.4 | 4.1 | MUST | core | any | payload | yes | new heap memory is zeroed, also after a shrink and a grow of the same range |
| C-MEM.5 | 4.1 | MUST | core | any | payload | yes | existing pointers never move: data written before a grow is intact after it |
| C-MEM.6 | 4.1 | MUST | core | any | payload | yes | `heap_shrink` returns whole granules at the top: the new break is the old end minus the request, `heap_range` shrinks by it and `heap_info.committed` falls by the same bytes |
| C-MEM.7 | 4.1 | MUST | core | any | payload | yes | a grow or shrink of zero bytes, or of a size that is not a whole number of granules, is `AAR_E_INVAL` and changes nothing |
| C-MEM.8 | 4.1 | MUST | core | any | payload | yes | grow until failure: the failure is `AAR_E_NOMEM`, the committed size never exceeds `heap_info.limit` |
| C-MEM.9 | 4.1 | MUST | core | any | payload | yes | variant `caps` (runner sets `AAR_TAG_PRIVATE_MIB` to 4): `heap_info.limit` is the smaller of the platform's cap (the limit `main` reported) and the client's value, a grow past it is `AAR_E_NOMEM` and the heap never grows past it (the platform's cap need not equal the pool, spec 4.1) |
| C-MEM.10 | 4.1 | MUST | core | any | payload | yes | `min_heap_mib` of the note is committed at load: the heap is at least that long at entry (payload note: 2 MiB) |
| C-MEM.11 | 4.1 | MUST | core | any | payload | yes | `heap_info.pool_free` falls when the heap grows and rises when it shrinks |
| C-MEM.12 | 4.1 | MUST | core | any | payload | yes | variant `lowmem`: exhausting the pool twice (the second time after releasing it) produces one `AAR_EV_LOW_MEMORY` event per low-water crossing, none while the pool stays exhausted. Its fields other than `type` are 0. When `heap_info.limit` is below `committed + pool_free` at entry the cap stops the loop before the pool, and the row passes with detail `cap below pool, not provoked` (spec 4.1) |
| C-MEM.13 | 4.4 | MUST | core | any | payload | yes | when `AAR_INFOF_ISOLATED` is set, a call given a bad pointer returns `AAR_E_FAULT` and the app keeps running (passes with detail `not isolated` when the flag is clear) |
| C-MEM.14 | 4.4 | MUST | core | any | none | todo | not tested: "the platform MUST document" the behaviour of a bad pointer on a non-isolated runtime; documentation |
| C-MEM.15 | 4.1 | MUST | core | any | none | todo | not tested: the heap starts at the end of the image and apps must not assume an address; platform internals and an obligation of the app |
| C-MEM.16 | 4.4 | MUST | core | any | none | todo | not tested: apps cannot reach firmware, other apps or host memory except through granted buffers; an isolation proof that no black-box test can give (spec 4.4). C-MEM.13 tests what a client or an app can see, the bad pointer |

## C-SHARED: shared buffers and visibility (sections 4.2, 4.3, 9.5, 10.3, 10.5)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-SHARED.1 | 4.2 | MUST | core | any | payload | yes | `shared_alloc` with `flags` other than 0 is `AAR_E_INVAL` |
| C-SHARED.2 | 4.2 | MUST | core | any | payload | yes | a granted block has `bytes` rounded up to `shared_granule`, `ptr` aligned to the requested alignment (rounded up to `shared_granule`) and writable over all `bytes`; `flags` holds only `AAR_SHAREDF_COHERENT` |
| C-SHARED.3 | 4.2 | MUST | core | any | payload | yes | `shared_free` of an interior pointer, of NULL, of a block already freed is `AAR_E_INVAL`; of the base of a live block is `AAR_OK` |
| C-SHARED.4 | 4.2 | MUST | core | any | payload | yes | at least 64 live blocks (`AAR_SHARED_MIN_BLOCKS`) can be held at once |
| C-SHARED.5 | 4.2 | MUST | core | any | payload | yes | 64 live blocks are still possible after the runtime has made its own service buffers (console writes, a file read) |
| C-SHARED.6 | 4.2 | MUST | core | any | payload | yes | allocate until failure: the failure is `AAR_E_NOMEM` and the total granted does not exceed `shared_limit` |
| C-SHARED.7 | 4.2 | MUST | core | any | payload | yes | `shared_used` rises by at least the granted bytes with an allocation and falls back with the free |
| C-SHARED.8 | 4.2 | MUST | core | any | payload | yes | variant `caps` (runner sets `AAR_TAG_SHARED_MIB`): `shared_limit` is that value and allocation past it, service buffers included, fails |
| C-SHARED.9 | 4.2 | MUST | core | any | payload | yes | token round trip (runner peer): the app writes a pattern, publishes, sends `haddr` and length in a message; the client maps it with `AARSharedMap` and reads the same bytes |
| C-SHARED.10 | 4.3 | MUST | core | any | payload | yes | the client writes a different pattern, publishes and posts a message; the app acquires and reads it intact (runner peer) |
| C-SHARED.11 | 4.3 | MUST | core | any | payload | yes | counter and pattern test (runner peer): the app writes an increasing counter and a pattern into a shared buffer while the client reads it in a loop with the map the library gave and `AARSharedAcquire` before each pass; zero stale and zero torn reads |
| C-SHARED.12 | 4.3 | MUST | core | any | none | todo | not tested: the same test with a client map that is deliberately cacheable plus `AARSharedAcquire` needs a way to ask for a cacheable map, which the API does not have (a flag for it is a candidate for API 0.6). C-SHARED.11 runs with the map the library gives and calls `AARSharedAcquire` before each pass, which covers the Acquire path wherever the map is not uncached (spec 4.3) |
| C-SHARED.13 | 4.3 | MUST | core | any | payload | yes | `aar_publish` and `aar_acquire` over a range inside a live block return `AAR_OK`; they return at once when the runtime is coherent |
| C-SHARED.14 | 4.3 | MUST | core | any | payload | yes | granule ownership (runner peer): app and client write alternate adjacent granules between hand-offs and neither sees the other's bytes in its own granule |
| C-SHARED.15 | 9.5 | MUST | core | any | runner | yes | `AARSharedMap` of a range that leaves the platform's aperture is `AAR_E_INVAL`; an unknown token is `AAR_E_INVAL` (judged while the runner serves the app's `CONF-SHARED-MAP` message, with the app's block live; the block is not checked, only the aperture, spec 9.5) |
| C-SHARED.16 | 9.5 | SHOULD | core | any | runner | yes | `AARSharedMap` after the app ended and its claim is gone is `AAR_E_STALE` (where the platform can tell; the claim is gone after `AARClose`; `AAR_E_INVAL` or `AAR_E_NOSYS` skips) |
| C-SHARED.17 | 9.5 | MUST | core | any | runner | yes | `AARSharedMap.flags` holds only `AAR_MAPF_UNCACHED` and `AAR_MAPF_COHERENT`; `granule` is a power of two; `base` is non-NULL |
| C-SHARED.18 | 9.5 | MUST | core | any | runner | yes | `AARSharedMap` with a non-zero `flags` argument is `AAR_E_INVAL` |
| C-SHARED.19 | 9.5 | MUST | core | any | runner | yes | `AARSharedUnmap` of a live map returns `AAR_OK`; the map is valid until then, until `AARClose` or the end of the app |
| C-SHARED.20 | 9.5 | MUST | core | any | runner | yes | `AARSharedPublish` and `AARSharedAcquire` on a valid range return `AAR_OK` and round to the map's granule (ranges of one byte at the start and at the end of the map are given); they are no-ops on an uncached or coherent map |
| C-SHARED.21 | 9.5 | MUST | core | any | none | todo | not tested: after an end state the client MUST unmap and MUST NOT write through a map; an obligation of the client (the runner follows it) |
| C-SHARED.22 | 9.5 | SHOULD | core | any | none | todo | not tested: the library SHOULD revoke maps where the platform allows; no observable difference without writing through a stale map, see C-LIFE.23 |
| C-SHARED.23 | 4.3 | MUST | core | any | none | todo | not tested: after writing data for the client the app MUST call `aar_publish` before sending the message; an obligation of the app, exercised by C-SHARED.9 to C-SHARED.12 |
| C-SHARED.24 | 10.3 | MUST | core | any | none | todo | not tested: control block layout, one writer per word, 64-byte lines, big-endian words and magic last are not visible to a client of `aar.library`; covered by `tests/transport_model_test.c` and the platform's own tests |
| C-SHARED.25 | 10.5 | MUST | core | any | none | todo | not tested: non-coherent platforms map control structures non-cacheable or use publish and acquire internally; visible only as stale or torn reads, which C-SHARED.11 and C-SHARED.12 would find |
| C-SHARED.26 | 4.3 | MUST | display | any | none | todo | not tested here: "flip publishes the surface" is C-DISPLAY.12 |
| C-SHARED.27 | 4.2 | MUST | core | any | runner | yes | a limit the platform cannot grant when the load starts fails the load: a shared limit above `AARInfo.shared_max_mib` (and at most 4095) gives `AAR_E_NOMEM` or `AAR_E_NORESOURCE` from `AARLoadFile`, or `AAR_E_INVAL` or `AAR_E_NOSYS` from `AARSetTagList` where that is the call's documented answer; passes when `shared_max_mib` is 4095 (spec 4.2) |

## C-MSG: messages and waiting (sections 6, 9.4, 9.6, 10.4)

The runner is the peer for all of these: it posts, collects and holds back
messages as the payload asks for.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-MSG.1 | 6 | MUST | core | any | payload | yes | 32 bytes in, the same 32 bytes out, in both directions (all 256 byte values, no word swapped) |
| C-MSG.2 | 6 | MUST | core | any | payload | yes | app to client: `msg_capacity` numbered messages arrive in order, none lost or repeated |
| C-MSG.3 | 6 | MUST | core | any | payload | yes | client to app: `msg_capacity` numbered messages arrive in order through `msg_poll`, none lost or repeated |
| C-MSG.4 | 6 | MUST | core | any | payload | yes | with the client not draining, `msg_send` returns `AAR_E_BUSY` after at least `msg_capacity` messages and never blocks (each call returns within 100 ms); then the client drains and gets the accepted messages intact and in order (`msg_capacity` is a minimum: the client library's own inbox adds to it, spec 6) |
| C-MSG.5 | 6 | MUST | core | any | runner | yes | with the app not polling, `AARSendMessage` returns `AAR_E_BUSY` after at least `msg_capacity` messages; the app then polls them all intact and in order |
| C-MSG.6 | 6 | MUST | core | any | payload | yes | `msg_poll` returns 0 when empty, 1 with a copy when a message is queued |
| C-MSG.7 | 6 | MUST | core | any | runner | yes | `AARGetMessage` returns `AAR_E_BUSY` when no message from the app is queued |
| C-MSG.8 | 6 | MUST | core | any | payload | yes | `msg_wait` returns `AAR_OK` with the message still queued (a following `msg_poll` returns it) |
| C-MSG.9 | 6 | MUST | core | any | payload | yes | `msg_wait` with nothing queued returns `AAR_E_TIMEDOUT` after at least the timeout |
| C-MSG.10 | 6 | MUST | core | any | payload | yes | `msg_wait(0)` only tests: returns at once, `AAR_OK` with a message queued, `AAR_E_TIMEDOUT` without |
| C-MSG.11 | 6 | MUST | core | any | payload | yes | a stop request ends `msg_wait` with `AAR_E_IO` (runner peer stops the app while it waits) |
| C-MSG.12 | 6 | MUST | core | any | payload | yes | a message queued before the stop is reported first: `msg_wait` returns `AAR_OK` for it, then `AAR_E_IO` |
| C-MSG.13 | 5.4 | MUST | core | any | payload | yes | an app parked in `msg_wait` for 12 seconds is not ended by the watchdog |
| C-MSG.14 | 6 | MUST | core | any | runner | yes | messages the app sent remain readable by the client after the app exited, until `AARClose` (up to `msg_capacity`) |
| C-MSG.15 | 10.4 | MUST | core | any | runner | yes | `AARInfo.msg_capacity` equals `queue_depth` minus 1 (`AAR_MSG_RESERVE`), and a platform with the default depth reports 63 |
| C-MSG.16 | 10.4 | MUST | core | any | payload | yes | a request behind a message that the full client inbox holds back waits until the client drains: the app sends messages until it is refused, then a console write completes after the client resumes draining (runner peer holds draining for 300 ms; spec 10.4) |
| C-MSG.17 | 9.6 | MUST | core | any | payload | yes | a burst of messages in both directions with a slow receiver loses none and reorders none (the `msgflood` pattern, 2000 messages) |
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
| C-QUEUE.1 | 10.4 | MUST | core | any | runner | yes | `AARInfo.queue_depth` is a power of two from 64 to 4096 |
| C-QUEUE.2 | 9.6 | MUST | core | any | runner | yes | `AARServiceNext` returns `AAR_OK` with a request, or `AAR_E_BUSY` when none is waiting |
| C-QUEUE.3 | 9.6 | MUST | core | any | runner | yes | a reply with a wrong `id` returns `AAR_E_INVAL`, does not resume the parked app and does not release its storage; the app continues only after the right reply (spec 9.6) |
| C-QUEUE.4 | 9.6 | MUST | core | any | runner | yes | a reply with a wrong `op`, and a duplicate reply, do the same as C-QUEUE.3: the refused reply returns `AAR_E_INVAL` and the app stays parked (spec 9.6) |
| C-QUEUE.5 | 9.6 | MUST | core | any | runner | yes | `AARServiceReply` resumes the app only if it is parked for that request. A console write of 16 bytes does not park the app (it is one of the three requests that return at once, spec 9.6; a platform that parks the app there fails the row), and the reply to it leaves a later parked request parked |
| C-QUEUE.6 | 9.6 | MUST | core | any | payload | yes | a request whose client withholds the reply past the 30 s file timeout returns `AAR_E_TIMEDOUT` and a timeout is not a completion: the next call that needs the storage returns `AAR_E_BUSY` until the late completion is consumed (runner peer) |
| C-QUEUE.7 | 9.6 | MUST | core | any | payload | yes | after the late completion of C-QUEUE.6 is consumed, the app can use the storage again |
| C-QUEUE.8 | 10.4 | MUST | core | any | runner | yes | after `AAR_E_BUSY` for a full SQ the status counter `sq_full` has increased (the SQ is filled to its last entry with log records after the messages are refused: a refused message post does not count, spec 10.4) |
| C-QUEUE.9 | 10.4 | MUST | core | any | payload | yes | `aar_log` flood with the client not serving never blocks the app; the status counter `log_dropped` increases when records were lost; a record that is discarded for any reason, a rate limit included, counts (spec 7.2) |
| C-QUEUE.10 | 10.6 | MUST | core | any | payload | yes | a missed or merged doorbell does not lose an entry: 5000 console requests with the client serving only on a 50 ms timer (no wakeups) complete with every byte present |
| C-QUEUE.11 | 10.4 | MUST | core | any | runner | yes | an unsolicited entry (event or log) that arrives while a request is pending does not complete or disturb that request |
| C-QUEUE.12 | 5.6 | MUST | core | any | runner | yes | open claims until `AAR_E_NORESOURCE`: when `AARInfo.max_apps` is 0 the first refusal has a reason from 1 to 3 and no ceiling is asserted (no refusal in 256 opens also passes); when it is above 0 the refusal comes at exactly `max_apps` claims with reason `AAR_REASON_LIMIT` (spec 5.6) |
| C-QUEUE.13 | 5.6 | MUST | core | any | runner | yes | a refused open leaves nothing allocated: after 100 further refused opens and closing every claim, the same number of claims can be opened again |
| C-QUEUE.14 | 5.6 | MUST | core | any | runner | yes | releasing every claim returns every resource: open to the limit, close all, open to the limit again, the same count |
| C-QUEUE.15 | 5.6 | SHOULD | core | any | runner | yes | with two compute-bound apps of weights 1 and 3 and `AAR_TAG_WEIGHT` accepted, the work done is within 30 percent of 1 to 3 (`AAR_E_NOSYS` for the tag skips with that reason); the standard states no tolerance; this is how the suite measures (spec 5.6) |
| C-QUEUE.16 | 5.6 | MUST | core | any | runner | yes | an app parked in a wait uses no CPU: a parked app beside a compute-bound app leaves the compute-bound app at least 90 percent of the work it does alone (the observable meaning of "parked consumes no CPU", spec 5.6) |
| C-QUEUE.17 | 5.6 | MUST | core | any | none | todo | not tested: "keeps its own service work ahead of apps" is a platform scheduling policy |
| C-QUEUE.18 | 10.4 | MUST | core | any | none | todo | not tested: entry layout, big-endian fields, slot clearing, reserved bytes, op codes in the entry, default pair geometry (`0x3000` bytes), release and acquire barriers and the head and tail writer rules are not visible to an `aar.library` client; covered by `tests/transport_model_test.c` and the platform's tests |
| C-QUEUE.19 | 10.4 | MUST | core | any | none | todo | not tested on a platform: a conforming writer never produces a count above `depth`; every reader applies the rule and it is covered by `tests/transport_model_test.c` and the client unit tests (spec 10.4) |
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
| C-FILE.1 | 7.1 | MUST | core | any | payload | yes | a path of 255 bytes opens; the first file id is at least `AAR_FD_BASE` (3) and below 64. The fixture chain has components of 30 characters, which an OFS volume can hold: 255 bytes is the limit of the app interface, and a client's filesystem may accept less (spec 7.1) |
| C-FILE.2 | 7.1 | MUST | core | any | payload | yes | a path of 256 bytes, which exists, is refused with `AAR_E_INVAL` (spec 7.1) |
| C-FILE.3 | 7.1 | MUST | core | any | payload | yes | a relative path reaches the client unchanged and is served under the root the client chose: a file the app creates is found there by the runner, and a fixture the runner placed there reads back |
| C-FILE.4 | 7.1 | SHOULD | core | any | payload | yes | `..` and a leading `/` are refused with `AAR_E_ACCES` by the recommended default policy (tests the reference serving loop `aar_serve`, not the platform: the policy is the client's, spec 7.1) |
| C-FILE.5 | 7.1 | SHOULD | core | any | payload | yes | a path with a volume or assign prefix is refused with `AAR_E_ACCES` unless `AAR_SERVEF_ABS_PATHS` is set; with the flag set a prefixed path to the root's own volume opens (tests the reference serving loop `aar_serve`, not the platform: the policy is the client's, spec 7.1) |
| C-FILE.6 | 7.1 | MUST | core | any | payload | yes | once the client has asked the app to stop, a create, a truncate and a write after the stop give a negative code and the runner finds, when the app has ended, no new file, the kept file unchanged and the written file empty. A read after the stop either returns data or a negative code, and neither is judged (spec 3.7, 7.1; runner peer stops the app) |
| C-FILE.7 | 7.1 | MUST | core | any | payload | yes | 16 files and, separately, 16 directory lists can be open at once; closing the lists leaves the files open |
| C-FILE.8 | 7.1 | MUST | core | any | payload | yes | a bad id (-1, 0, 1, 2, 999, a closed id) is `AAR_E_BADF` from close, read, write, seek and list next |
| C-FILE.9 | 7.1 | MUST | core | any | payload | yes | `AAR_FILE_READ` opens an existing file; `AAR_FILE_WRITE` writes in place; the use of an id for the wrong direction is C-FILE.37 |
| C-FILE.10 | 7.1 | MUST | core | any | payload | yes | `CREATE` and `TRUNCATE` together create a missing file and empty an existing one |
| C-FILE.11 | 7.1 | MUST | core | any | payload | yes | `CREATE` alone keeps the contents of an existing file |
| C-FILE.12 | 7.1 | MUST | core | any | payload | yes | `TRUNCATE` alone empties an existing file and does not create a missing one (`AAR_E_NOENT`) |
| C-FILE.13 | 7.1 | MUST | core | any | payload | yes | `CREATE` with `EXCL`: `AAR_E_EXIST` for an existing file, which stays untouched; creates a missing file |
| C-FILE.14 | 7.1 | MUST | core | any | payload | yes | flags 0 (neither read nor write) is `AAR_E_INVAL` |
| C-FILE.15 | 7.1 | MUST | core | any | payload | yes | `CREATE` without `WRITE` and `TRUNCATE` without `WRITE` are `AAR_E_INVAL` |
| C-FILE.16 | 7.1 | MUST | core | any | payload | yes | `EXCL` without `CREATE` is `AAR_E_INVAL` |
| C-FILE.17 | 7.1 | MUST | core | any | payload | yes | an unknown flag bit is `AAR_E_INVAL` |
| C-FILE.18 | 7.1 | MUST | core | any | payload | yes | a missing file without `CREATE` is `AAR_E_NOENT` |
| C-FILE.19 | 7.1 | MUST | core | any | payload | yes | seek with `AAR_SEEK_SET`, `CUR` and `END` returns the new position; a position before the start is `AAR_E_INVAL`; a seek past the end is either accepted (then a read there returns `AAR_OK` and 0 bytes) or `AAR_E_INVAL`. A platform may do either (spec 7.1) |
| C-FILE.20 | 7.1 | MUST | core | any | payload | yes | `file_read` sets `*out_n` to the bytes obtained; a read of more than the file holds returns what is left; a read at the end returns `AAR_OK` and 0 |
| C-FILE.21 | 7.1 | MUST | core | any | payload | yes | one `file_read` of the 1 MiB fixture returns all of it (chunking is the platform's business; a short read means end of file) |
| C-FILE.22 | 7.1 | MUST | core | any | payload | yes | one `file_write` of 1 MiB returns a count of 1 MiB and the runner finds the exact bytes in the file |
| C-FILE.23 | 7.1 | MUST | core | any | payload | yes | `file_stat` gives size and mode (1 file, 2 directory) of the fixtures; a missing path is `AAR_E_NOENT` |
| C-FILE.24 | 7.1 | MUST | core | any | payload | yes | `mtime` of the dated fixture equals its date in Unix seconds, or is 0 when the filesystem has none. The Amiga runner sets the fixture using local clock fields for that UTC instant |
| C-FILE.25 | 7.1 | MUST | core | any | payload | yes | a directory list returns every entry of the fixture directory once, names of at most 255 bytes, mode per entry, then `AAR_E_NOENT`; a missing directory is `AAR_E_NOENT` |
| C-FILE.26 | 7.1 | MUST | core | any | runner | yes | a request waits for the client and then returns `AAR_E_TIMEDOUT` after 29 to 36 seconds as the app measures it, per request (spec 7.1: a shorter limit does not conform). The runner serves the app by hand and withholds the reply; the test takes 31 seconds |
| C-FILE.27 | 7.1 | MUST | core | any | runner | yes | after a timeout the file id is not reused for the rest of the app's life: 20 later opens, after the runner completed the late request, all succeed and none is given the timed-out id (the runner reads the id from the request) |
| C-FILE.28 | 7.1 | MUST | core | any | runner | yes | a stop request ends a file wait with `AAR_E_IO` (the runner withholds the reply, waits one second, then stops the app; the call ends with that code after at least 0.5 s) |
| C-FILE.29 | 7.1 | MUST | core | any | payload | yes | `aar_map_file`, when present, returns the exact contents of the 1 MiB fixture and the buffer stays valid until `aar_unmap_file`; `AAR_E_NOSYS` is allowed |
| C-FILE.30 | 7.1 | MUST | zz9000 | any | payload | yes | `aar_map_file` is present, `shared_used` rises by the file size while mapped, and a file over 16 MiB is `AAR_E_NOSPC` |
| C-FILE.31 | 7.1 | MUST | core | any | runner | yes | after the app exited with files and lists still open, the runner can delete and rename those files (the client released every lock and handle) |
| C-FILE.32 | 7.1 | MAY | core | any | none | todo | not tested: a platform MAY return `AAR_E_NOSYS` from `aar_map_file` (accepted by C-FILE.29) |
| C-FILE.33 | 7.1 | MUST | core | any | none | todo | not tested: AmigaDOS has no atomic exclusive create; the check-then-create in the serving loop is only exclusive among the apps one client serves; C-FILE.13 tests what the standard promises |
| C-FILE.34 | 7.1 | MUST | zz9000 | any | none | todo | not tested: mapping `READ`, `WRITE`, `CREATE` and `TRUNCATE` onto AmigaDOS open modes is an implementation detail of the reference serving loop; its effects are C-FILE.9 to C-FILE.13 |
| C-FILE.35 | 3.8 | MUST | core | any | none | todo | not tested: `fopen`, `O_CREAT`, `O_TRUNC`, `O_EXCL`, `O_APPEND`, `EOVERFLOW`, and the libc functions over the calls of API 0.6 (`unlink`, `remove`, `rename`, `mkdir`, `rmdir`, `ftruncate`, `truncate`, `chmod`, `utime`, `utimes`, the permission bits of `stat`, that `fstat` gives the descriptor's size and no path data, `getenv`, `setenv`, `time`, `gettimeofday`, `clock`) belong to libaar, tested by `libaar/tests/runtime_test.sh` |
| C-FILE.36 | 7.1 | SHOULD | core | any | none | todo | not tested: "a short write means the client stopped early; the caller may retry" is advice to the caller; the stop case is C-FILE.6 |
| C-FILE.37 | 7.1 | MUST | core | any | payload | yes | a write through an id opened `AAR_FILE_READ` and a read through an id opened `AAR_FILE_WRITE` are `AAR_E_BADF`, and the file's bytes are unchanged afterwards (the payload reads the file back; spec 7.1, 3.5) |
| C-FILE.38 | 7.1 | MUST | core | any | payload | yes | `file_delete` removes a file and an empty directory (a later stat is `AAR_E_NOENT`); a missing path is `AAR_E_NOENT`; a directory that is not empty is `AAR_E_EXIST` and it and its file stay. `AAR_E_NOSYS` from a client that does not serve the call is accepted by this row and by C-FILE.39 to C-FILE.47, and C-FILE.48 judges it |
| C-FILE.39 | 7.1 | MUST | core | any | payload | yes | a file with `AAR_PROT_DELETE` set by `file_set_protect` is refused by `file_delete` with `AAR_E_ACCES` and stays; with the bit cleared it is deleted. If the client cannot represent DELETE on its own, `file_set_protect` may answer `AAR_E_NOSYS` and the file stays |
| C-FILE.40 | 7.1 | MUST | core | any | payload | yes | `file_delete` of a file that the app has open for writing is `AAR_E_BUSY` and the file stays (host build: `SKIP host:`, because a POSIX host deletes open files) |
| C-FILE.41 | 7.1 | MUST | core | any | payload | yes | `file_rename` moves a file (the old name is `AAR_E_NOENT`, the new name has the 100 bytes) and a directory; with `AAR_RENAME_REPLACE` an existing target is replaced by the source's bytes |
| C-FILE.42 | 7.1 | MUST | core | any | payload | yes | `file_rename` onto an existing file without `AAR_RENAME_REPLACE` is `AAR_E_EXIST` and changes neither file; a missing source is `AAR_E_NOENT` with or without the flag; a flag bit other than `AAR_RENAME_REPLACE` (bits 1 and 31) is `AAR_E_INVAL` and moves nothing |
| C-FILE.43 | 7.1 | MUST | core | any | payload | yes | `dir_create` makes a directory (stat mode 2) in which a file can be made; an existing directory or a file with that name is `AAR_E_EXIST`; a missing parent is `AAR_E_NOENT` and is not made |
| C-FILE.44 | 7.1 | MUST | core | any | payload | yes | `file_set_date` to 1000000000 and to 1500000000 gives that `mtime` in stat, for a file and for a directory; a date before the filesystem's epoch (0) is `AAR_OK` or `AAR_E_INVAL`; a missing path is `AAR_E_NOENT` |
| C-FILE.45 | 7.1 | MUST | core | any | payload | yes | stat reports `protect` within bits 0 to 7; `AAR_PROT_WRITE` and `AAR_PROT_DELETE` (bits 0 to 3 deny) are reported back and cleared by 0; `AAR_PROT_ARCHIVE` and `AAR_PROT_SCRIPT` are reported back or refused with `AAR_E_NOSYS` without changing the mask; a bit above bit 7 is `AAR_E_INVAL`; a missing path is `AAR_E_NOENT` |
| C-FILE.46 | 7.1 | MUST | core | any | payload | yes | `file_truncate` of an id opened for writing shortens the file, extends it with zero bytes (checked by reading it back) and truncates to 0; an id opened with `AAR_FILE_READ` only, -1 and 999 are `AAR_E_BADF` |
| C-FILE.47 | 7.1 | MUST | core | any | payload | yes | `volume_info` of a path in the root: `block_bytes` and `total_bytes` above 0, `free_bytes` not above `total_bytes`, only `AAR_VOLF_READONLY` in `flags`, `reserved` 0, the bytes past the structure inside `size` zeroed, and with a `size` of 8 nothing past byte 8 is written; a missing path is `AAR_E_NOENT` |
| C-FILE.48 | 7.1 | SHOULD | core | any | payload | yes | the client serves the seven file calls of API 0.6: none of `file_delete`, `file_rename`, `dir_create`, `file_set_date`, `file_set_protect`, `file_truncate` and `volume_info` answers `AAR_E_NOSYS` (spec 7.1 allows it for a client that does not serve them, so this is a SHOULD) |
| C-FILE.49 | 7.1 | MUST | core | any | payload | yes | after the client asked the app to stop, `file_delete`, `file_rename`, `dir_create`, `file_set_date`, `file_set_protect`, `file_truncate` and `file_set_comment` give negative codes and the runner finds, when the app has ended, the file still there, the old name still the only name, no new directory, the date and the protection unchanged, the length still 5 and the comment of the commented file still empty or the one set before the stop (a client without comments keeps none; the same judgement as C-FILE.6). Without a peer: `SKIP host:` |
| C-FILE.50 | 7.1 | MUST | core | any | none | todo | not tested: a `file_rename` across volumes is `AAR_E_INVAL` and `AAR_VOLF_READONLY` marks a volume that cannot be written; both need a second or a write-protected volume, which a conformance run does not have |
| C-FILE.51 | 7.1 | MUST | core | any | none | todo | not tested: `AAR_RENAME_REPLACE` deletes the target and then renames, which is not atomic on AmigaDOS (spec 7.1); a failure between the two steps is not something an app can provoke |
| C-FILE.52 | 7.1 | MUST | core | any | payload | yes | `file_rename` of a path onto itself is `AAR_OK` and changes nothing, with `AAR_RENAME_REPLACE` and without it: the file keeps its bytes, and the same holds for a second spelling of the same path (the name in another letter case, which AmigaDOS compares without case) when the client resolves both to one object; a missing path is still `AAR_E_NOENT` [spec question 67] |
| C-FILE.53 | 7.1 | MUST | core | any | payload | yes | file comments of a regular 100 byte scratch file (a directory is not tried): a file with no comment reads as `AAR_OK` with length 0, and capacity 0 takes a NULL buffer; 8 raw bytes with values above 0x7F (and 0x01) read back byte for byte with nothing written past them and no terminator; capacity 0 and capacity 3 for those 8 bytes are `AAR_E_NOSPC` with the full length 8 in `*out_n`, capacity 3 copying the first 3 bytes only, capacity 8 is `AAR_OK`; 79 bytes (`AAR_FILE_COMMENT_MAX`) are accepted and read back; 80 bytes and an embedded NUL are `AAR_E_INVAL`, a NULL buffer with a non-zero length is negative, and each refused set leaves the 79 bytes; a new comment replaces the old one; 0 bytes, from a buffer or from NULL, clears it (read back as length 0); the file's length stays 100; a missing path is `AAR_E_NOENT` for both calls; `file_get_comment` without `out_n` is negative. A client may answer `AAR_E_NOSYS` for a call it does not serve (optional route, spec 7.1): the row then passes with a note that says so, C-FILE.54 judges the support, and the reads are checked without the writes (the empty comment, and a set that answers `AAR_E_NOSYS` leaves it empty) or the writes without the reads (the refusals and the replace and clear steps without read-back). No suite skip applies |
| C-FILE.54 | 7.1 | SHOULD | core | any | payload | yes | the client serves the two comment calls of API 0.6: neither `file_get_comment` nor `file_set_comment` answers `AAR_E_NOSYS` for a missing path (optional route, so this is a SHOULD). Without a peer: `SKIP host:` |
| C-FILE.55 | 7.1 | MUST | core | any | payload | yes | refusals that hold with or without comment support: `file_set_comment` of 80 bytes, with an embedded NUL, and of 1 byte from NULL, `file_get_comment` without `out_n`, and either call with a path of 256 bytes give negative codes; a `file_get_comment` that answers `AAR_E_NOSYS` writes neither the buffer nor `*out_n`, otherwise a missing path is `AAR_E_NOENT`; a `file_set_comment` of a missing path is `AAR_E_NOSYS` or `AAR_E_NOENT` and makes no object. It runs on the host runner, which serves no comments, and shows the unsupported answer there |

## C-CON: console and log (section 7.2, 10.4)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-CON.1 | 7.2 | MUST | core | any | payload | yes | `console_write` of 200 KiB returns `AAR_OK` with `*n` equal to the length and the runner's capture holds every byte in order (pattern checked) |
| C-CON.2 | 7.2 | MUST | core | any | payload | yes | with the client's output held back for 500 ms the write parks and then completes without losing a byte |
| C-CON.3 | 7.2 | MUST | core | any | payload | yes | `console_read` parks until the client has input and returns the bytes the runner feeds (runner peer feeds after 300 ms) |
| C-CON.4 | 7.2 | MUST | core | any | payload | yes | `console_read` returns `AAR_OK` with `*n == 0` at the end of input |
| C-CON.5 | 7.2 | MUST | core | any | payload | yes | a stop request ends a console wait with `AAR_E_IO` (runner peer holds the input and stops the app) |
| C-CON.6 | 3.8 | MUST | core | any | payload | yes | libc flavour: `write(1, ...)` and `write(2, ...)` reach the console, `read(0, ...)` reads its input, `isatty(0)`, `isatty(1)`, `isatty(2)` return 0 (host build: `SKIP host:`) |
| C-CON.7 | 7.2 | MUST | core | any | payload | yes | an `aar_log` record reaches the client's log hook intact and does not appear in the console stream |
| C-CON.8 | 7.2 | MUST | core | any | payload | yes | an `aar_log` record of 2000 bytes arrives truncated to at most `AAR_LOG_MAX` (1024) bytes, the first bytes intact, in one piece |
| C-CON.9 | 10.4 | MUST | core | any | payload | yes | `aar_log` records over 1024 bytes are never wrapped or split: every record the client receives is a prefix of a record the app sent |
| C-CON.10 | 7.2 | MAY | core | any | none | todo | not tested: the platform MAY rate-limit `aar_log` (ZZ9000: 32 per second to the client, 8 per second to its UART) |
| C-CON.11 | 7.2 | MUST | core | any | none | todo | not tested: the client decides what the console is (Shell streams or a window); Ctrl-\ behaviour is the client's |
| C-CON.12 | 7.2 | MUST | core | any | none | todo | not tested: `aar_log` is "lossy, separate from stdout, for the platform's debug output"; the platform's UART path is not visible to a client |

## C-TIME: monotonic time and sleep (section 7.3)

The pattern area of this suite. It is complete, its payload is `conf_time`, and it
runs on the host runner and under qemu user mode.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-TIME.1 | 7.3 | MUST | core | any | payload | yes | `aar_time_us` never goes backwards over 200000 calls and over a 20 ms sleep |
| C-TIME.2 | 7.3 | SHOULD | core | any | payload | yes | the smallest step between two different readings is 1 ms or finer (checked over 50 ms) |
| C-TIME.3 | 7.3 | MUST | core | any | payload | yes | when no stop is pending, `aar_sleep_us(us)` parks for at least `us`, measured by `aar_time_us`, for 1 ms, 10 ms and 50 ms |
| C-TIME.4 | 7.3 | SHOULD | core | any | payload | yes | an app alone: a sleep takes no more than `us` plus four sleep granules (at least 1 ms each) plus 10 ms |
| C-TIME.5 | 3.7 | MUST | core | any | payload | yes | a stop request ends a long sleep early: the payload asks the runner (peer) to stop it, then sleeps 10 s; the sleep must end well before 9 s and `aar_should_stop` must be 1 afterwards. An app that does not leave within `stop_grace_ms` is ended by force and cannot report, so the runner fails the id with end reason `FORCED` (spec 3.7, 7.3). Host build: `SKIP host:` |
| C-TIME.6 | 7.3 | MUST | core | any | none | todo | not tested: "from an epoch the platform chooses (not wall-clock)" cannot be told from a wall clock that happens to start at zero |
| C-TIME.7 | 7.3 | MUST | core | any | none | todo | not tested: `aar_time_us` is not wall-clock time; the wall clock is `aar_time_unix_us`, judged by C-TIME.10 and C-TIME.11 |
| C-TIME.8 | 5.4 | MUST | core | any | none | todo | not tested here: `time_us` and `sleep_us` as watchdog activity or declared waits are C-LIFE.15 and C-LIFE.16 |
| C-TIME.9 | 3.7 | MUST | core | any | payload | yes | a sleep entered with a stop request already pending returns at once: right after C-TIME.5 (`aar_should_stop()` is 1) `aar_sleep_us` of 10 s returns within 1 s (spec 3.7, 7.3). Host build: `SKIP host:` |
| C-TIME.10 | 7.3 | MUST | core | any | payload | yes | `aar_time_unix_us` returns a time that is not before 1978-01-01 (the client adds 2922 days to `DateStamp`, spec 7.3). `AAR_E_NOSYS` from a client that does not serve the call is accepted, and C-HOST.15 judges it |
| C-TIME.11 | 7.3 | MUST | core | any | payload | yes | wall-clock time moves with the monotonic clock: across a 200 ms sleep `aar_time_unix_us` and `aar_time_us` differ in elapsed time by less than 150 ms (a `DateStamp` tick is 20 ms; a runtime may answer from a base plus elapsed time) |
| C-TIME.12 | 7.3 | MAY | core | any | none | todo | not tested: a runtime MAY fetch the time once and answer from that base plus `aar_time_us`; the value can step when the user sets the clock |
| C-TIME.13 | 3.8 | MUST | core | any | none | todo | not tested: `time`, `gettimeofday`, `clock` and `times` in the libc glue belong to libaar, tested by `libaar/tests/runtime_test.sh` (C-FILE.35) |

## C-HOST: host environment: locale, environment variables, clipboard (section 7.4)

The client serves these core calls (spec 7.4, 9.6). A client that does not serve one answers
`AAR_E_NOSYS` and may refuse by policy with `AAR_E_ACCES`. For valid clipboard requests in
C-HOST.8 to C-HOST.12, the payload accepts these results independently for each read and write
and notes what was unavailable. C-HOST.15 judges `AAR_E_NOSYS`. The runner prepares the
environment variables and the clipboard unit that `payloads/conf_host.h` lists, and is the peer
of C-HOST.3, C-HOST.7, C-HOST.9, C-HOST.13 and C-HOST.14 (host build: `SKIP host:`; the qemu run
has the variables of C-HOST.4 to C-HOST.6 and no runner). The suite does not touch clipboard unit
0: the scratch units are 200 to 202.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-HOST.1 | 7.4 | MUST | core | any | payload | yes | `aar_get_locale_info` fills the record: `language`, `country`, `decimal_point` and `group_separator` are NUL-terminated inside their arrays, `language` has no `.language` suffix, `flags` holds only `AAR_LOCALEF_OFFSET_KNOWN`, `utc_offset_min` is within -1440 to 1440 and `reserved` is 0 |
| C-HOST.2 | 3.4 | MUST | core | any | payload | yes | the `size` rule for `aar_get_locale_info`: with a `size` larger than the structure the bytes past it, up to `size`, are zeroed and nothing beyond is written; with a `size` of 8 nothing past byte 8 changes (the rule of C-TABLE.11 and C-TABLE.12) |
| C-HOST.3 | 7.4 | MUST | core | any | payload | yes | the record agrees with the client's locale (runner peer): `utc_offset_min` is `loc_GMTOffset` negated, `language` is `loc_LanguageName` without `.language`, `country` is the non-zero bytes of `loc_CountryCode`, most significant first (compared when the client reports a country), `decimal_point` and `group_separator` are the locale's. The runner makes the expected separators from the native bytes of `loc_DecimalPoint` and `loc_GroupSeparator` as the client does: each byte is an ISO-8859-1 character written as UTF-8 (`A0` is `C2 A0`), into the 3 bytes of the field, whole characters only. The runner answers 2 and the row passes with a note when it cannot open `locale.library` [spec question 68] |
| C-HOST.4 | 7.4 | MUST | core | any | payload | yes | `aar_env_get` of a global variable of 11 bytes: `AAR_OK` with `*n` 11 and the value NUL-terminated; a `bytes` of 0 and of 11 are `AAR_E_NOSPC` with `*n` 11; a `bytes` of 12 fits |
| C-HOST.5 | 7.4 | MUST | core | any | payload | yes | a value of 300 bytes: a 64-byte buffer is `AAR_E_NOSPC` with `*n` 300, a buffer of exactly 300 is `AAR_E_NOSPC`, a buffer of 301 holds all 300 bytes, which are the stored ones |
| C-HOST.6 | 7.4 | MUST | core | any | payload | yes | an unset name is `AAR_E_NOENT`; a name of 0 bytes and a name of 256 bytes are `AAR_E_INVAL`; a name of 255 bytes is not (it is `AAR_E_NOENT`) |
| C-HOST.7 | 7.4 | MUST | core | any | payload | yes | a variable that is local to another process (the runner's, `GVF_LOCAL_ONLY`) is not in `ENV:` and is `AAR_E_NOENT` (the client reads global variables only, spec 7.4) |
| C-HOST.8 | 7.4 | MUST | core | any | payload | yes | `aar_clip_write` of text to unit 200 and `aar_clip_read` give it back: `*n` is the text size, a `bytes` of 0 is `AAR_E_NOSPC` with `*n` set, a short buffer gets `min(bytes, *n)` bytes and `AAR_E_NOSPC`, and a second write replaces the first |
| C-HOST.9 | 7.4 | MUST | core | any | payload | yes | text conversion (host build: `SKIP host:`, because the loader has no character set): a character in ISO-8859-1 (U+00E9) round-trips and one that no Amiga character set has (U+4E2D) is written as `?` |
| C-HOST.10 | 7.4 | MUST | core | any | payload | yes | `AAR_CLIP_IFF` passes one IFF FORM through unchanged, and a read of `AAR_CLIP_TEXT` from a unit that holds a FORM that is not text is `AAR_E_NOENT` |
| C-HOST.11 | 7.4 | MUST | core | any | payload | yes | a clip of 70000 bytes, more than one staging buffer of the runtime, is written and read back exactly; the TEXT and IFF FORM round trips are checked independently |
| C-HOST.12 | 7.4 | MUST | core | any | payload | yes | a unit of 256 or more (`AAR_CLIP_UNITS`) and a type of 0 or 3 are `AAR_E_INVAL` for `aar_clip_read` and `aar_clip_write`. Each call is judged alone: one that answers `AAR_E_NOSYS` or `AAR_E_ACCES` to the probe (unit 256) is noted and not checked further, the other is still held to `AAR_E_INVAL`. A client that serves neither passes with both noted |
| C-HOST.13 | 7.4 | MUST | core | any | payload | yes | what the runner put on clipboard unit 201 as an IFF FTXT in ISO-8859-1 reads as UTF-8 (runner peer) |
| C-HOST.14 | 7.4 | MUST | core | any | payload | yes | what the app writes to unit 202 is an IFF FTXT on the clipboard, with the text converted to ISO-8859-1 and `?` for U+4E2D (runner peer reads `clipboard.device`) |
| C-HOST.15 | 7.4 | SHOULD | core | any | payload | yes | the client serves the calls of section 7.4 and 7.3: none of `aar_get_locale_info`, `aar_env_get`, `aar_clip_read` and `aar_time_unix_us` answers `AAR_E_NOSYS` |
| C-HOST.16 | 7.4 | MAY | core | any | none | todo | not tested: a client MAY refuse any of these calls by policy with `AAR_E_ACCES` |
| C-HOST.17 | 7.4 | MUST | core | any | none | todo | not tested: an empty clipboard is `AAR_E_NOENT`, and unit 0 is the primary clipboard; a test would change or depend on the user's clipboard, so the suite uses units 200 to 202 and gets `AAR_E_NOENT` from a type mismatch (C-HOST.10) |

## C-LIFE: lifecycle, faults, watchdog, leases (sections 5.1 to 5.5)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-LIFE.1 | 5.1 | MUST | core | any | runner | yes | after `AAROpen` the state is `AAR_STATE_CLAIMED` and the generation is non-zero |
| C-LIFE.2 | 5.1 | MUST | core | any | runner | yes | after a payload returned from `aar_main` the state is `AAR_STATE_IDLE`, the end reason `AAR_END_EXIT` and the fault class `AAR_FAULT_NONE`, and the status stays readable until `AARClose` |
| C-LIFE.3 | 5.2 | MUST | core | any | runner | yes | the exit code the client reads equals the value `aar_main` returned (0 when every payload test passed, 10 otherwise, as the payload's end line says) |
| C-LIFE.4 | 5.2 | MUST | core | any | runner | yes | exit codes 0, 5, 10, 20 and 255 returned by `aar_main` and passed to `aar_exit` arrive unchanged in `AARStatus` |
| C-LIFE.5 | 5.1 | MUST | core | any | runner | yes | state while running is `RUNNING`; `AARStop` moves it to `STOPPING` and then `IDLE` (the app takes 200 ms to clean up, so `STOPPING` can be seen) |
| C-LIFE.6 | 5.2 | MUST | core | any | runner | yes | a stop sets `aar_should_stop` and queues `AAR_EV_STOP`; an app that leaves within the grace period ends with `AAR_END_EXIT` and its own exit code |
| C-LIFE.7 | 5.2 | MUST | core | any | runner | yes | an app that ignores a stop is ended by force after `stop_grace_ms`: end reason `AAR_END_FORCED`, no earlier than the grace period and no later than the grace period plus 2 s (the clock has a 20 ms step, so the lower bound allows 60 ms of slack) |
| C-LIFE.8 | 5.3 | MUST | core | any | runner | yes | a data abort in the app (write to an unmapped address) ends only that app: a second app that waits in `msg_wait` keeps running and exits normally afterwards (needs two claims) |
| C-LIFE.9 | 5.3 | MUST | core | any | runner | yes | data abort: end reason `AAR_END_USER_FAULT`, fault class `AAR_FAULT_DATA`, `fault_addr` equals the address the app touched, `fault_pc` equals the address of the faulting instruction. The app picks an address outside its image, heap and stack and reports it first (spec 5.3). The address is `0xDEAD0000` by default, or `0xDEAD00000000` on A64, and is moved to another address of a short list when it lies within 16 MiB of one of them |
| C-LIFE.10 | 5.3 | MUST | core | any | runner | yes | instruction fetch from an unmapped address: class `AAR_FAULT_FETCH`, `fault_pc` equals the target |
| C-LIFE.11 | 5.3 | MUST | core | any | runner | yes | an undefined instruction: class `AAR_FAULT_ILLEGAL`, `fault_pc` equals the instruction |
| C-LIFE.12 | 5.3 | MUST | core | any | runner | yes | an alignment fault (a misaligned exclusive access): class `AAR_FAULT_ALIGN` (an exclusive access that is not aligned to its size always faults; whether a plain unaligned access faults is the platform's choice and is not tested, spec 5.3) |
| C-LIFE.13 | 5.3 | MUST | core | any | runner | yes | `fault_sp` and `fault_lr` equal the values the app had at the faulting instruction (the app reports them in a message first); addresses are app virtual addresses: `fault_pc` minus `load_base` is the offset in the image (the app reports the address of `aar_start`, offset 0 of the image) |
| C-LIFE.14 | 5.3 | MUST | core | any | runner | yes | `fault_syndrome` is non-zero for a data abort; `fault_pstate` shows user mode (A32 low five bits 0x10, A64 EL0); the profile reported in the status equals the image's |
| C-LIFE.15 | 5.4 | MUST | core | any | runner | yes | an app that computes for 10 seconds without a table call is not stopped (`stall_limit_ms` is at least 10000): it computes for 9.8 s and has to measure at least 9 s of silence |
| C-LIFE.16 | 5.4 | MUST | core | any | runner | yes | an app parked in `sleep_us` for 12 seconds is not stopped (a parked call does not count toward the stall limit) |
| C-LIFE.17 | 5.4 | MUST | core | any | runner | yes | each of `yield`, `should_stop`, `time_us`, `msg_wait` with a timeout, a file call and a console call, issued every 4 seconds by an otherwise silent app for 11 seconds, keeps it alive (for each kind, three rounds of 4 s of silence and one call take 12 s) |
| C-LIFE.18 | 5.4 | SHOULD | core | any | runner | yes | an app that computes for 90 percent of the advertised `stall_limit_ms` without a call (at most 60 s) is not stopped (it has to measure at least 85 percent) |
| C-LIFE.19 | 5.5 | MUST | core | any | runner | yes | the client task blocking for 15 seconds without calling the library does not stop the app (the lease renews from interrupt or timer context) |
| C-LIFE.20 | 5.5 | MUST | core | any | runner | yes | a client task that exits without `AARClose` (a child process) releases its handle: within 10 s a call on that handle returns `AAR_E_STALE` and no longer `AAR_E_OWNER`, and a new claim succeeds (the base API cannot show that the app stopped; the ZZ9000 claim list does, C-LIFE.37; spec 5.5) |
| C-LIFE.21 | 5.5 | MUST | core | any | runner | yes | a client task that closes its last `aar.library` base without `AARClose` releases its handles and stops the app: a call on the handle returns `AAR_E_STALE` and a new claim succeeds (spec 5.5) |
| C-LIFE.22 | 5.5 | MUST | zz9000 | any | runner | yes | after the claim is revoked (`aar-run RELEASE` through `AARExt`), calls with the old handle return `AAR_E_STALE` and change nothing (`AARStop` is the first call: it reaches the platform; `AARStatus` and message calls before that first answer may succeed, spec 5.5) |
| C-LIFE.23 | 5.5 | MUST | zz9000 | any | runner | yes | late writes: after the claim is revoked (the client's lease paused through `AARExt` until the platform ends it), a second app allocates shared memory and fills it with a pattern while the old client writes garbage through its stale map; the pattern is intact |
| C-LIFE.24 | 5.5 | MUST | core | any | operator | yes | start two apps with `aar-run` (for example a long-running payload of the suite) and reset the machine while they run. After the boot the suite starts and passes. `aar-run STATUS` or the platform's own listing shows no claim of the old apps. A client that survived the reset (a platform that resets only the ARM side) saw `AAR_END_PLATFORM_RESET` |
| C-LIFE.25 | 5.2 | MUST | core | any | runner | yes | relaunch: the same payload started 20 times one after the other behaves identically each time (same output, same exit code) |
| C-LIFE.26 | 5.1 | MUST | core | any | runner | yes | `AARClose` of a running app stops it, releases the claim and returns `AAR_OK`; the app's heartbeat stops (seen as messages before the close, and as a stale handle and a working new claim after it; the signal bit is C-CLIENT.23) |
| C-LIFE.27 | 5.3 | MUST | core | any | none | todo | not tested: a fault in runtime code ends every app and restarts the runtime (`AAR_END_RUNTIME_RESTART`); needs fault injection into the platform |
| C-LIFE.28 | 5.4 | MAY | core | any | none | todo | not tested: an app cannot reach the stuck-call limit (every long call is a declared wait); a runtime MAY end an app whose call stays inside the runtime's own code with `AAR_END_STUCK_CALL` (spec 5.4) |
| C-LIFE.29 | 5.4 | MAY | core | any | none | todo | not tested: the runtime MAY stop an app that goes longer than the stall limit with `AAR_END_STALL` |
| C-LIFE.30 | 5.5 | MUST | core | any | none | todo | not tested: lease period values and the vertical-blank server are platform specifics; the outcome is C-LIFE.19 to C-LIFE.21 |
| C-LIFE.31 | 5.5 | MUST | core | any | none | todo | not tested: the owner-reap method (task address, name pointer, node type, CLI number, signal bit) is an implementation; the outcome is C-LIFE.20 |
| C-LIFE.32 | 5.1 | MUST | core | any | none | todo | not tested: after a fault the state is `IDLE` or `FAULTED` by design, and a client treats both as ended (spec 5.1); C-LIFE.9 judges `end_reason`, which says why the app ended |
| C-LIFE.33 | 5.2 | MUST | core | any | none | todo | not tested: incremental load, no step stalling other work for more than about a millisecond; a scheduling latency measurement of the platform, done by `aarlibtest latency` |
| C-LIFE.34 | 5.3 | MUST | core | any | none | todo | not tested: end reasons `LEASE`, `PLATFORM_RESET`, `QUARANTINED` as codes need the platform to produce them; seen in C-LIFE.22 and C-LIFE.24 where a platform can |
| C-LIFE.35 | 10.2 | MUST | core | any | none | todo | not tested: "everything reserved is released on every failure path" is C-FREE.7 |
| C-LIFE.36 | 5.1 | MUST | core | any | runner | yes | after `AARLoadFile` returned `AAR_OK` and before `AARStart`, `AARStatus` shows `AAR_STATE_LOADING`; after `AARStart` the state is `AAR_STATE_STARTING` or `AAR_STATE_RUNNING` (spec 5.1: the load does not change the state, and `STARTING` lasts only until the entry) |
| C-LIFE.37 | 5.5 | MUST | zz9000 | any | runner | yes | the card's view of the release of a dead client's claim (`AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT)`): after the owner task ended without `AARClose`, the claim list no longer holds the old handle word and `AARInfo.platform[AAR_ZZ9000_INFO_RECORDS_FREE]` is back to its value before the open (spec 5.5 rule 3) |

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
| C-CLIENT.9 | 9.2 | MUST | core | any | runner | yes | `AARProbeFile` and `AARProbeMemory` return the note fields of a conformance image (`negative/<profile>/probe-note.aarimg`: profile, major, minor, stack 128 KiB, heap 3 MiB, groups) without loading it and without a handle |
| C-CLIENT.10 | 9.2 | MUST | core | any | runner | yes | `AARProbeFile` and `AARProbeMemory` of a file that is not an ELF and of a truncated ELF fail with `AAR_E_INVAL`, `AARProbeFile` of a missing file fails with `AAR_E_NOENT`, and the caller's structure is unchanged (spec 9.2) |
| C-CLIENT.11 | 9.2 | MUST | core | any | runner | yes | `AAROpen` with a NULL notify port and a NULL `reason` succeeds; two claims have different handles |
| C-CLIENT.12 | 9.2 | MUST | core | any | runner | yes | `AARSetTagList` before the load accepts `AAR_TAG_PRIVATE_MIB`, `AAR_TAG_SHARED_MIB`, `AAR_TAG_AUDIO_ROUTE`, `AAR_TAG_LEASE_EXTENDED`, `AAR_TAG_CLIENT_CAPS`; `AAR_TAG_WEIGHT` gives `AAR_OK` or `AAR_E_NOSYS` |
| C-CLIENT.13 | 9.2 | MUST | core | any | runner | yes | `AARSetTagList` after the load began returns `AAR_E_BUSY`; a limit above 4095 MiB is `AAR_E_INVAL` |
| C-CLIENT.14 | 9.2 | MUST | core | any | runner | yes | `AAR_TAG_WEIGHT` above `AARInfo.weight_max` is `AAR_E_INVAL` where weighting exists; `weight_max` 0 means `AAR_E_NOSYS` for the tag |
| C-CLIENT.15 | 9.2 | MUST | core | any | runner | yes | `AARLoadMemory` of an image and `AARLoadFile` of the same file produce the same run (= C-ELF.42 for accepted images) |
| C-CLIENT.16 | 9.2 | MUST | core | any | runner | yes | `AARStart` with `max_args` strings of `max_arg_bytes` each is accepted; the first string is the program name |
| C-CLIENT.17 | 9.2 | MUST | core | any | runner | yes | `AARStatus` with `size` 16 fills state, generation and end reason only; `generation` is non-zero; `load_base` is non-zero after the load; `profile` equals the image's |
| C-CLIENT.18 | 9.2 | MUST | core | any | runner | yes | `AARMessageSignal` returns a non-zero mask, the same mask for two handles of one task, and 0 for an invalid handle (NULL and a closed handle) |
| C-CLIENT.19 | 9.4 | MUST | core | any | runner | yes | the signal is set when the app sends a message, when it exits and when it queues a service request |
| C-CLIENT.20 | 9.4 | SHOULD | core | any | runner | yes | the owner wakes within 250 ms of an app message (measured over 20 messages: the runner pings the app, which answers at once; the clock has a 20 ms step, so 290 ms are allowed) |
| C-CLIENT.21 | 9.4 | MUST | core | any | runner | yes | a notify port receives one `AAREvent` at a time: the next is not posted until the previous is taken; its `handle` names the handle; the message needs no reply |
| C-CLIENT.22 | 9.4 | MUST | core | any | runner | yes | event types coalesce in the order `MESSAGE`, then `COMPLETE` or `FAULT` when the app has ended, else `REQUEST`. While a message waits every event is `MESSAGE`, also after the end (the status it carries shows the end); with nothing waiting a request gives `REQUEST`, an exit `COMPLETE` and a fault `FAULT` |
| C-CLIENT.23 | 9.4 | MUST | core | any | runner | yes | a signal bit is allocated at a task's first open and freed when the task closes its last `aar.library` base: in a child task that opens the library itself the number of free signal bits drops by one at the first `AAROpen` (three handles), and is back after `CloseLibrary` (`AARClose` frees the claim only, spec 9.4) |
| C-CLIENT.24 | 9.3 | MUST | core | any | runner | yes | a call with a handle from a task that does not own it returns `AAR_E_OWNER` (status, stop, send, close; the handle stays usable by its owner) |
| C-CLIENT.25 | 9.3 | MUST | core | any | runner | yes | one task can hold at least 20 handles when admission allows (the per-task signal bit is shared); a platform that refuses earlier with `AAR_E_NORESOURCE` passes when that is its `max_apps` |
| C-CLIENT.26 | 10.2 | MUST | core | any | runner | yes | a NULL handle is refused with `AAR_E_STALE` by each of 13 calls (`AARClose`, `AARStatus`, `AARStop`, `AARStart`, `AARLoadFile`, `AARLoadMemory`, `AARSetTagList`, `AARSendMessage`, `AARGetMessage`, `AARSharedMap`, `AARSharedUnmap`, `AARServiceNext`, `AARServiceReply`; the runner prints the 13 codes) and nothing is written into the caller's structures (spec 10.2: the client API takes a pointer, and `AAR_E_INVAL` for the handle word 0 belongs to the transport) |
| C-CLIENT.27 | 9.2 | MUST | core | any | runner | yes | `AARExt` needs no handle and returns NULL for an unknown platform id or extension id |
| C-CLIENT.28 | 9.2 | MUST | zz9000 | any | runner | yes | `AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT)` returns a size-first table |
| C-CLIENT.29 | 9.2 | MUST | core | any | runner | yes | `AARSharedToken` returns `AAR_OK` with a token or `AAR_E_NOSYS`; no other code for a valid buffer (the buffer is one the app shared and the library mapped; `AAR_OK` gives the token of that buffer, which is what the client names its own memory with in a reply, spec 9.2) |
| C-CLIENT.30 | 9.6 | MUST | core | any | runner | yes | a serving loop that serves only some groups (`aar_serve_config.groups`) completes a request of an unserved group with `AAR_E_NOSYS` (the display open of a payload; passes with detail `no display group declared` when the platform has none). The reference serving loop answers; the library does not know which ops a client serves (spec 9.6) |
| C-CLIENT.31 | 9.6 | MUST | core | any | runner | yes | `aar_serve` serves every request op of the groups it serves: files, directory lists, console, log and messages here (a payload, then the file under the root and the console text); display, audio and input by C-DISPLAY, C-AUDIO and C-INPUT |
| C-CLIENT.32 | 9.1 | MUST | core | any | none | todo | not tested: calls return `LONG` in `d0`; seen in every call the runner makes |
| C-CLIENT.33 | 9.2 | SHOULD | core | any | none | todo | not tested: where section 9.2 and the `.fd` and `.h` files differ, the files are right; documentation rule |
| C-CLIENT.34 | 9.7 | MUST | core | any | none | todo | not tested: structure sizes (160, 188, 32, 128, 48) are checked by the header's `_Static_assert`s and `client/tests/m68k_check.sh` |
| C-CLIENT.35 | 9.1 | MUST | core | any | none | todo | not tested: `aar.library` MUST open on every platform that implements the standard; shown by every run of the suite |
| C-CLIENT.36 | 9.4 | SHOULD | core | any | none | todo | not tested: the library SHOULD wake the owner from the doorbell, or from a timer or vertical-blank poll; the observable half is C-CLIENT.20 |
| C-CLIENT.37 | 10.2 | MUST | core | any | none | todo | not tested: handle 0 invalid, generation 0 never issued, generation wrap at 65535 reuses (a platform need not retire the index at the wrap, spec 10.2); handle layout is transport internal |
| C-CLIENT.38 | 10.6 | MAY | core | any | none | todo | not tested: a client MAY skip armed handles when the global event count is unchanged |

## C-DISPLAY: display group (section 8.2)

The runner supplies its own display table to `aar_serve` (pixel memory it names
with `AARSharedToken`, where the platform allows) and reads the presented frames
from it; with a real screen the picture is judged by the operator.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-DISPLAY.1 | 8.2 | MUST | display | any | payload | yes | `display_open` of 320x200, 640x480 and 4096x1 in RGB565 and BGRA8888 succeeds with width, height and format as asked, `pitch` at least width times bytes per pixel, `reserved` 0, `caps` only defined bits |
| C-DISPLAY.2 | 8.2 | MUST | display | any | payload | yes | `layout` is consistent with the format (RGB565: layouts 1 to 4; BGRA8888: 5 or 6; CLUT8: 7) and `back` is non-NULL and writable; `front` is NULL or writable |
| C-DISPLAY.3 | 8.2 | MUST | display | any | payload | yes | width 0 or height 0 is refused with a negative code; an unknown format (0, 4, 99) is refused with `AAR_E_INVAL` (a defined format the platform does not provide is `AAR_E_NOSYS`, spec 8.2) |
| C-DISPLAY.4 | 8.2 | MUST | display | any | payload | yes | one display per app: a second `display_open` while one is open fails with a negative code |
| C-DISPLAY.5 | 8.2 | MUST | display | any | payload | yes | `display_close` returns `AAR_OK`; a second close and a flip after close are negative; a new open works again |
| C-DISPLAY.6 | 8.2 | MUST | display | any | payload | yes | `flip` of the open id returns `AAR_OK`; a flip of any other id is `AAR_E_INVAL` |
| C-DISPLAY.7 | 8.2 | MUST | display | any | payload | yes | write `back`, `flip`, `refresh`: after the refresh `back` can be written again and `front` (when non-NULL) holds the frame just presented. The test opens without `AAR_DISPLAY_LATEST`; with `AAR_DISPCAP_LATEST` granted `front` is not promised to hold any frame (spec 8.2) |
| C-DISPLAY.8 | 8.2 | MUST | display | any | payload | yes | `AAR_DISPCAP_ASYNC` granted: `flip_async` then `flip_idle` reaches 1 within 1 s; a second async flip while one is pending is `AAR_E_BUSY` or completes (no frame is lost); without the cap `flip_async` is negative |
| C-DISPLAY.9 | 8.2 | MUST | display | any | payload | yes | `AAR_DISPLAY_LATEST` requested: the open succeeds with or without `AAR_DISPCAP_LATEST`; without the cap `display_latest_dropped` returns 0 |
| C-DISPLAY.10 | 8.2 | MUST | display | any | payload | yes | with `AAR_DISPCAP_LATEST` granted, 100 quick flips leave `display_latest_dropped` non-decreasing and not above 99 |
| C-DISPLAY.11 | 8.2 | MUST | display | any | payload | yes | `vsync` returns `AAR_OK`; ten vsyncs take at most 2 s (no floor: the call is a pacing hint, a platform may wait for the refresh or not, spec 8.2) |
| C-DISPLAY.12 | 8.2 | MUST | display | any | payload | yes | flip publishes the surface (runner peer): the frame the app flips without calling `aar_publish` is read by the client's display table byte for byte (spec 8.2; the runner takes pixel memory the platform can name with `AARSharedToken`, spec 9.5) |
| C-DISPLAY.13 | 8.2 | MUST | display | any | payload | yes | pixel layout (runner peer): a four-colour test frame stored by the reported `layout` decodes in the client's table to the four colours, for each format the platform grants |
| C-DISPLAY.14 | 8.2 | MUST | display | any | payload | yes | `pitch` larger than the row bytes is honoured: rows stored by `pitch` arrive in the right rows (a surface whose pitch equals the row bytes is also accepted; the copy of a single buffer keeps pitch and layout, spec 8.2) |
| C-DISPLAY.15 | 8.2 | MUST | display | any | payload | yes | CLUT8 with `AAR_DISPCAP_CLUT8` granted: `set_palette` of 256 colours returns `AAR_OK`; first plus count above 256 is `AAR_E_INVAL`; the client's table received the colours |
| C-DISPLAY.16 | 8.2 | MUST | display | any | payload | yes | `set_palette` on a surface without the CLUT8 cap is negative |
| C-DISPLAY.17 | 8.2 | SHOULD | display | any | operator | yes | the picture: the operator sees the test pattern the payload shows in the colours named on the console |
| C-DISPLAY.18 | 8.2 | MAY | display | any | none | todo | not tested: a platform MAY allow more than 4096 in width or height |
| C-DISPLAY.19 | 9.6 | MUST | display | any | none | todo | not tested: a platform that owns the display buffers may complete a flip itself, so no flip entry is promised; no test may assume one (C-DISPLAY.6 to C-DISPLAY.12 allow both) |
| C-DISPLAY.20 | 8.2 | MUST | display | any | none | todo | not tested: the app MUST write `back`, flip, then `refresh` before writing again; an obligation of the app |
| C-DISPLAY.21 | 8.2 | MUST | display | any | payload | yes | variant window: `display_get_info` of the open display reports the surface's `width` and `height`, `flags` of only the three defined bits with `AAR_DISPINFO_WINDOW` and `AAR_DISPINFO_SCALED` agreeing with the caps, a screen size above 0 and `reserved` 0; the bytes past the structure inside `size` are zeroed and with a `size` of 8 nothing past byte 8 changes; an id that is not open, and the id after `display_close`, are `AAR_E_INVAL`. `AAR_E_NOSYS` from a client that does not serve the call is accepted by this row and by C-DISPLAY.22 to C-DISPLAY.26, and C-DISPLAY.29 judges it |
| C-DISPLAY.22 | 8.2 | MUST | display | any | payload | yes | variant window: `display_modes` needs no open display; every record written has a width and height of at least 1, formats that are a non-empty set of `1u << AAR_FMT_*`, only `AAR_MODEF_PREFERRED` in `flags`, `reserved` 0, at most one preferred mode; `AAR_E_NOSPC` exactly when `*n` exceeds `max`; with `max` 0 the count is the same and the call is `AAR_E_NOSPC` unless there are no modes |
| C-DISPLAY.23 | 8.2 | MUST | display | any | payload | yes | variant window: `display_set_title` takes a title of 15 bytes, UTF-8 text and a title of 255 bytes, and refuses one of 256 bytes with `AAR_E_INVAL` |
| C-DISPLAY.24 | 8.2 | MUST | display | any | payload | yes | variant window: `display_pointer` takes `HIDDEN`, `BUSY`, `DEFAULT` with no image, and `CUSTOM` with a 16x16 and a 64x64 image; `CUSTOM` with no image, a width or height of 0 or 65, a hot spot outside the image (`hot_x` 16, `hot_y` -1) and a mode of 4 or 2^32-1 are `AAR_E_INVAL` |
| C-DISPLAY.25 | 8.2 | MUST | display | any | payload | yes | variant window: `display_resize` on a display opened without `AAR_DISPLAY_RESIZABLE` is `AAR_E_NOSYS`; where `AAR_DISPCAP_RESIZE` is granted, a width or height of 0 or 2^32-1 is `AAR_E_INVAL`, a resize to 200x120 refreshes the surface (that size, same id and format, a writable `back`), a flip then works and `display_get_info` reports the new size |
| C-DISPLAY.26 | 8.2 | MUST | display | any | payload | yes | variant window: `display_wait_vblank` with no display open is `AAR_E_INVAL` (or `AAR_E_NOSYS`), with a timeout of 0 the same, with 500 ms it returns `AAR_OK` within 600 ms (or `AAR_E_NOSYS`: the client cannot see a vertical blank), and with 1 us it returns `AAR_OK`, `AAR_E_TIMEDOUT` or `AAR_E_NOSYS` within 200 ms |
| C-DISPLAY.27 | 8.2 | MUST | display | any | payload | yes | `display_last_present_us` is 0 before the first flip; after a flip of the surface (opened with `AAR_DISPLAY_VSYNC`) it is between the time before and the time after the call, and after a second flip it is not smaller and not in the future |
| C-DISPLAY.28 | 8.2 | MUST | display | any | payload | yes | variant window: `display_open` with each request of API 0.6 alone and with all of them (`AAR_DISPLAY_WINDOW`, `_RESIZABLE`, `_POINTER_ABS`, `_TEXT_INPUT`, `_CLOSE_REQUEST`, `_SCALE`) succeeds; the caps granted are only those that ask for, `caps` has no undefined bit, `AAR_DISPCAP_RESIZE` comes only with `AAR_DISPCAP_WINDOW`, and `AAR_DISPLAY_RESIZABLE` alone does not get `AAR_DISPCAP_RESIZE` |
| C-DISPLAY.29 | 8.2 | SHOULD | display | any | payload | yes | variant window: the client serves `display_info`, `display_modes`, `display_set_title`, `display_pointer` and `display_wait_vblank` without requiring a cap. The wait returns `AAR_OK` or `AAR_E_TIMEDOUT`; the result records the window or screen fallback and whether a blank arrived. The spec permits `AAR_E_NOSYS` when a call cannot be served, so this is a SHOULD |
| C-DISPLAY.30 | 8.2 | SHOULD | display | any | operator | yes | the title and the pointer: a window titled "AAR conformance: title and pointer" shows a gradient for 12 seconds, and the pointer over it is the normal one, then hidden, then the busy pointer, then a red square with a black frame, three seconds each (variant pointer, the client's own display) |
| C-DISPLAY.31 | 8.2 | SHOULD | display | any | operator | yes | resizing by hand: the window titled "AAR conformance: resize me" opens with `AAR_DISPLAY_RESIZABLE`; the operator drags its corner a few times, the app answers each `AAR_EV_RESIZE` with `display_resize` and redraws, and the gradient fills the new size. The yes needs a resize the app answered (variant resize; the display must grant `AAR_DISPCAP_RESIZE`) |
| C-DISPLAY.32 | 8.2 | MUST | display | any | none | todo | not tested: a scaled surface keeps its aspect ratio (`AAR_DISPLAY_SCALE`) and a close request replaces the stop that the close gadget sends (`AAR_DISPLAY_CLOSE_REQUEST`); both need a person and the platform's windows, and C-DISPLAY.28 and C-INPUT.17 cover the caps and the events |
| C-DISPLAY.33 | 8.2 | MUST | display | any | none | todo | not tested: `display_resize` is `AAR_E_BUSY` while a flip is pending; a client that completes a flip at once never shows the state, and the runner's display table is not granted `AAR_DISPCAP_RESIZE` |

## C-INPUT: events and input (sections 8.3, 9.6, 10.4)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-INPUT.1 | 8.3 | MUST | core | any | payload | yes | `event_poll` never blocks: 1000 calls with nothing pending return `AAR_OK` with 0 events within 100 ms |
| C-INPUT.2 | 8.3 | MUST | core | any | payload | yes | a stop (`AARStop` and an `AAR_EV_STOP` posted with `AARPostEvent`) reaches `event_poll` as type 5 even without the input group |
| C-INPUT.3 | 8.3 | MUST | input | any | payload | yes | key down and key up events posted with `AARPostEvent` arrive in order with `code`, `buttons` and `time_ms` intact (types 1 and 2; the library does not change a posted event, spec 8.3) |
| C-INPUT.4 | 8.3 | MUST | input | any | payload | yes | mouse events arrive in order with `dx`, `dy` and button bits intact (type 3) |
| C-INPUT.5 | 8.3 | MUST | input | any | payload | yes | joystick events arrive with axes in -1, 0, 1 and fire in bit 0 (type 4) |
| C-INPUT.6 | 8.3 | MUST | input | any | payload | yes | Ctrl-D, Ctrl-E and Ctrl-F signalled to the runner task arrive as `AAR_EV_BREAK` with codes 4, 5 and 6 (the serving loop's break mask) |
| C-INPUT.7 | 8.3 | MUST | input | any | payload | yes | `AAR_EV_RESIZE` (6) is never delivered during the input test |
| C-INPUT.8 | 8.3 | MUST | input | any | payload | yes | the runtime retains at least 64 events: 64 key events posted while the app does not poll all arrive in order |
| C-INPUT.9 | 8.3 | MUST | input | any | runner | yes | with the queue overfull the client drops the new events, never the old ones: the app finds the first (at least 64) events in order, and `AARStatusInfo.events_dropped` equals the number of posts the client was refused (events that the runtime discards beyond its own 64 are not counted by it, spec 8.3) |
| C-INPUT.10 | 9.6 | MUST | input | any | payload | yes | mouse motion posted while the queue is full: the sum of `dx` received plus `events_dropped` equals the sum posted (merged or counted as dropped, never lost silently; the reference library merges, spec 8.3) |
| C-INPUT.11 | 8.3 | SHOULD | input | any | operator | yes | real keyboard, mouse and joystick events reach the app with the up bit removed from keys. The operator presses keys and moves the mouse and confirms the codes the payload prints |
| C-INPUT.12 | 8.3 | MAY | input | any | none | todo | not tested: the client MAY coalesce mouse motion when the queue is full (C-INPUT.10 allows both) |
| C-INPUT.13 | 8.3 | MUST | input | any | none | todo | not tested: "the runtime keeps at least 64 events" beyond the completion queue's own 64 entries is internal; the outcome is C-INPUT.8 |
| C-INPUT.14 | 8.3 | MUST | core | any | none | todo | not tested: an idle app uses `msg_wait`, `sleep_us` or `yield` and not a spin; advice to apps |
| C-INPUT.15 | 8.3 | MUST | input | any | payload | yes | the app requests text and close events when a display is available. The events posted with `AARPostEvent` arrive in order and unchanged: `AAR_EV_TEXT` (U+00E9 and qualifier) and `AAR_EV_CLOSE_REQUEST` only with their granted caps; `AAR_EV_WHEEL` (`dx` 2, `dy` -3) and `AAR_EV_ACTIVE` (code 1 and 0) without a cap. The app distinguishes the posts from native events by their `time_ms` |
| C-INPUT.16 | 8.3 | MUST | input | any | payload | yes | absolute mouse events while the queue is full (runner peer): after 100 key events the runner posts 20 events with `AAR_MOUSE_ABSOLUTE` in `buttons` and positions (10 i, 5 i) with no retry; every absolute event the app gets is one of those positions with the flag still set (the merge keeps the latest position and never sums), and the last one is the last post the client accepted (spec 8.3) |
| C-INPUT.17 | 8.3 | SHOULD | input | any | operator | yes | real devices follow the rules of spec 8.3: the app opens a window with `AAR_DISPLAY_POINTER_ABS` and `AAR_DISPLAY_TEXT_INPUT` for 20 seconds, the operator presses each mouse button, turns the wheel and types; a mouse event has code 0 to 3, a text event a code of 1 to 65535, a focus event code 0 or 1, and when wheel events arrive the wheel's raw keys 0x7A to 0x7D do not also arrive as keys. The yes needs a mouse button, wheel or text event as evidence |
| C-INPUT.18 | 8.3 | MUST | core | any | none | todo | not tested: "an app MUST ignore an event type it does not know" is an obligation of the app; the payloads skip types they did not ask for |
| C-INPUT.19 | 8.3 | MUST | input | any | none | todo | not tested: that `AAR_EV_TEXT`, `AAR_EV_CLOSE_REQUEST` and the absolute position are sent only with the caps that ask for them depends on the client's input source; C-INPUT.17 shows the real events and C-DISPLAY.28 the caps |

## C-AUDIO: audio group (sections 8.4, 9.6)

Rows C-AUDIO.1 to C-AUDIO.9 run on the route `PLATFORM`, which is optional (spec 8.4). When
`aar_audio_open` for it returns `AAR_E_NOSYS`, each of them ends in `SKIP` with a reason that
starts with `no PLATFORM route`; the runner and `tools/check_results.py` accept that reason for
these rows although they are MUST rows (and for no other row). C-AUDIO.10 and C-AUDIO.11 run on
the route `AHI`, which a platform with the audio group provides.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-AUDIO.1 | 8.4 | MUST | audio | any | payload | yes | `audio_open(22050, 2)` succeeds; the grant has `channels` 2, `rate` one of 22050, 44100, 48000, `frames` a power of two, `flags` 0, `samples`, `read_frame` and `write_frame` non-NULL; `SKIP no PLATFORM route` when `audio_open` for the route PLATFORM is refused with `AAR_E_NOSYS` (the route is optional, spec 8.4; C-AUDIO.10 and C-AUDIO.11 run the stream test on `AHI`) |
| C-AUDIO.2 | 8.4 | MUST | audio | any | payload | yes | the ring header is where the standard puts it: `read_frame` is `write_frame` plus 0x40 bytes, `samples` is `write_frame` plus 0x80 bytes (the three pointers share one mapping of the ring), the little-endian words at +4 (rate), +8 (channels), +0xC (format `AAR_AUDIO_FORMAT_S16LE`, value 1) agree with the grant (spec 8.4); `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.3 | 8.4 | MUST | audio | any | payload | yes | `audio_open` of 44100 and 48000 stereo returns `AAR_OK` with a supported pair or a refusal of `AAR_E_INVAL`; a granted pair is written into `out`; `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.4 | 8.4 | MUST | audio | any | payload | yes | an unsupported rate (8000, 96000) or channel count (1, 0, 6) is `AAR_E_INVAL`; `channels` is always 2 in a grant; `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.5 | 8.4 | MUST | audio | any | payload | yes | the app writes 1 second of samples ahead of `*read_frame` (never more than `frames` ahead); the runtime advances `*read_frame` at the granted rate within 25 percent over 1 second, and `read_frame` never passes `write_frame` (route PLATFORM); `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.6 | 8.4 | MUST | audio | any | payload | yes | when the app stops writing, underruns are counted, once per starved read or period (the unit is the consumer's, spec 8.4): `audio_get_stats` (when present) shows more than before and equals the ring's word at +0x44; `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.7 | 8.4 | MUST | audio | any | payload | yes | `audio_get_stats` honours `size` (8 fills `size` and `underruns` only); a NULL slot returns `AAR_E_NOSYS`; `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.8 | 8.4 | MUST | audio | any | payload | yes | `audio_close` detaches the stream, returns `AAR_OK`, a new open works, a second close is negative; `SKIP no PLATFORM route` as for C-AUDIO.1 |
| C-AUDIO.9 | 8.4 | SHOULD | audio | any | payload | yes | a second `audio_open` in the same app is `AAR_E_BUSY`; opens in more apps than the platform has streams are `AAR_E_BUSY` (the runner opens apps until the first refusal); `SKIP no PLATFORM route` when `audio_open` for the route is refused with `AAR_E_NOSYS` |
| C-AUDIO.10 | 9.6 | MUST | audio | any | runner | yes | route `AHI` (`AAR_TAG_AUDIO_ROUTE`; a platform with the audio group provides it, spec 8.4): `AARAudioRead` with `frames` 0 returns the frames available |
| C-AUDIO.11 | 9.6 | MUST | audio | any | runner | yes | route `AHI`: the PCM `AARAudioRead` returns is exactly what the app wrote, in order (a ramp of 48000 frames) |
| C-AUDIO.12 | 8.4 | SHOULD | audio | any | operator | yes | sound: the operator hears the 440 Hz tone the payload plays for two seconds and confirms |
| C-AUDIO.13 | 9.6 | MUST | audio | any | none | todo | not tested: a failed `AUDIO_CLOSE` retains the ring while a consumer can access it; needs failure injection |
| C-AUDIO.14 | 8.4 | MUST | audio | any | none | todo | not tested: each ring index has its own cache line and one writer, indices are free-running `uint32_t` counters that wrap (a wrap needs 2^32 frames, about 54 hours) |
| C-AUDIO.15 | 8.4 | MUST | audio | any | none | todo | not tested: the ring is little-endian and the library swaps the words it reads; the effect is C-AUDIO.11 |

## C-GL: GL group (section 8.5)

The payload is `templates/mgl_conformance.c` adapted to print these ids; its
matrix cases map onto C-GL.6.

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-GL.1 | 8.5 | MUST | gl | any | payload | yes | `aar_gl_bind` of an open surface returns a table with `size` and `version` set, `version` `AAR_GL_VERSION_V0`, `AAR_GL_VERSION_ARRAYS` or `AAR_GL_VERSION_11` |
| C-GL.2 | 8.5 | MUST | gl | any | payload | yes | every slot of the table that `size` covers up to `Build2DMipmaps` is non-NULL; with `AAR_GL_VERSION_ARRAYS` or later the array entries are non-NULL too; fields past `Build2DMipmaps` are used only when `AAR_GL_COVERS` says so |
| C-GL.3 | 8.5 | MUST | gl | any | payload | yes | `aar_gl_bind` with an id that is not an open surface returns NULL |
| C-GL.4 | 8.5 | MUST | gl | any | payload | yes | `aar_gl_unbind` releases the context; a new bind works |
| C-GL.5 | 8.5 | MUST | gl | any | payload | yes | `aar_gl_swap` flushes and gives the frame to the platform to copy and flip: a cleared colour is read by the client's display table after the swap, or after up to two further swaps of the same picture (at most two swapped frames are in flight, spec 8.5; runner peer) |
| C-GL.6 | 8.5 | MUST | gl | any | payload | yes | the `mgl_conformance` matrix cases pass (the line names the first failing case, with its environment, format, draw path, texel, colour and the actual and expected pixel, and counts the failures) |
| C-GL.7 | 8.5 | MUST | gl | any | payload | yes | `Perspective`, `Ortho`, `Frustum` and `DepthRange` take doubles and `gluPerspective` takes four doubles: a scene drawn with them has the expected pixels (the depth-tested scene needs the depth buffer of at least 16 bits that spec 8.5 promises) |
| C-GL.8 | 8.5 | MUST | gl | any | payload | yes | on an isolated runtime a client-array draw with a bad pointer or range sets `GL_INVALID_VALUE` for the next `glGetError` and does not fault or stop the app (passes with detail `not isolated` otherwise). That nothing is drawn is not checked, and the `gl*` entries return nothing for it (spec 8.5) |
| C-GL.9 | 8.5 | MUST | gl | any | none | todo | not tested: integer parameters are `int32_t` and `uint32_t`; the matrix and header checks cover the widths |
| C-GL.10 | 8.5 | MUST | gl | any | none | todo | not tested: debug bind flags are not part of the standard (ZZ9000 extension, 8.8); tested only through C-TABLE.23 |
| C-GL.11 | 8.5 | MUST | gl | any | none | todo | not tested: the `gl*` wrappers forward to the table that `aar_gl_bind` returned (libaar, 3.8) |
| C-GL.12 | 8.5 | MUST | gl | any | payload | yes | the table's `version` is at most `AAR_GL_VERSION_11`; from `AAR_GL_VERSION_ARRAYS` on `size` covers `UnlockArrays`; with `AAR_GL_VERSION_11` it covers `PopAttrib` and is at least `sizeof(aar_gl)` (123 slots), and with a lower version it does not cover `ClearDepth` |
| C-GL.13 | 8.5 | MUST | gl | any | payload | yes | valid calls through the `AAR_GL_VERSION_11` wrappers leave `glGetError` at `GL_NO_ERROR`. Independent calls exercise the wrapper even if the slot is absent. List allocation requires deletion support; list creation requires a generated name and both `NewList` and `EndList`; list execution requires successful creation and `CallLists` also requires a known list base. Attribute stacks are tested only with both `PushAttrib` and `PopAttrib`, and a pop follows only a successful push. Unavailable combinations are omitted. Copy-to-texture calls are not exercised |
| C-GL.14 | 8.5 | MUST | gl | any | payload | yes | `glGetIntegerv(GL_STENCIL_BITS)` returns 0 to 32 with no GL error, and the stencil calls with no stencil buffer (0 bits) leave no error (spec 8.5: the stencil buffer is not promised) |
| C-GL.15 | 8.5 | MUST | gl | any | payload | yes | where the relevant slots are present: `glClearDepth(0.25)` reads back through `glGetDoublev(GL_DEPTH_CLEAR_VALUE)`, `glColorMask(1,0,1,0)` through `glGetBooleanv(GL_COLOR_WRITEMASK)`, `glLineWidth(2)` and `glPointSize(3)` through `glGetFloatv`, and `glIsTexture` is true for a bound name and false after `glDeleteTextures`. With `GenLists` and `DeleteLists`, `glGenLists(2)` returns a non-zero first name |
| C-GL.16 | 8.5 | MUST | gl | any | none | todo | not tested: what the lighting, fog, display-list, clip-plane, stencil and copy-texture entries draw depends on the renderer, which keeps the group a GL 1.1 subset (spec 8.5); C-GL.13 to C-GL.15 judge the calls, not the pixels |

## C-SERVICES: services group (section 8.6)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-SERVICES.1 | 8.6 | MUST | services | any | payload | yes | `aar_sha256` gives the known digests of the empty message, `abc`, a 1 MiB pattern, and an unaligned buffer of 1000 bytes; `AAR_E_NOSYS` from the first call passes with the detail `SHA-256 not provided` (a subset is allowed, C-SERVICES.11), and any other answer must be the right digest |
| C-SERVICES.2 | 8.6 | MUST | services | any | payload | yes | `aar_decompress` for each of the ten algorithms, with the formats and framings of spec 8.6 (LZMA2 with its property byte, LHA without its header), either returns `AAR_E_NOSYS` or the exact bytes of a known stream; `bytes_written` is the plain length and `bytes_consumed` the stream length (for an LHA stream `bytes_consumed` is at most the stream length and is not judged beyond that) |
| C-SERVICES.3 | 8.6 | MUST | services | any | payload | yes | `AAR_COMPRESS_NONE` and unknown algorithm numbers (11, 99, 0xFFFFFFFF) are refused with `AAR_E_INVAL`; `AAR_E_NOSYS` is also accepted, as the call description of `aar_decompress` allows |
| C-SERVICES.4 | 8.6 | MUST | services | any | payload | yes | for each implemented algorithm whose stream ends itself (not LHA, which has no end marker, spec 8.6): a destination that is too small gives an error and writes nothing beyond `dst_bytes` (guard bytes intact); a truncated stream gives an error |
| C-SERVICES.5 | 8.6 | MUST | services | any | payload | yes | PNG decode through an image session is exact (a 24x16 image in one BGRA8888 tile, fed whole, then drained with the end-of-input flag 1, spec 8.6, image sessions); a codec that is not implemented gives `AAR_E_NOSYS` from `image_open` |
| C-SERVICES.6 | 8.6 | MUST | services | any | payload | yes | JPEG decode of a known image stays within 6 per channel and WebP decode within 3 of the reference pixels (spec 8.6: a lossy decoder may use a faster IDCT or a cheaper chroma upsampling). A fast integer IDCT is off by up to 4 on the vector, and the JPEG vector is 4:4:4, so chroma upsampling is not exercised |
| C-SERVICES.7 | 8.6 | MUST | services | any | payload | yes | FLAC decode is exact; MP3 and Vorbis decode stay within a root-mean-square error of 64 of the reference PCM; a codec that is not implemented gives `AAR_E_NOSYS` |
| C-SERVICES.8 | 8.6 | MUST | services | any | payload | yes | at least four image sessions and four audio sessions can be open at once (`AAR_MIN_DECODE_SESSIONS`), with the first implemented codec of each kind; the first refusal beyond is `AAR_E_NOMEM` |
| C-SERVICES.9 | 8.6 | MUST | services | any | payload | yes | session calls with an unknown session id are `AAR_E_INVAL` (`AAR_E_NOSYS` where the kind has no codec); a closed session id is refused |
| C-SERVICES.10 | 8.6 | MUST | services | any | none | todo | not tested: byte counts of whole buffers are `uint64_t`; a test would need buffers of 4 GiB or more |
| C-SERVICES.11 | 8.6 | MAY | services | any | none | todo | not tested: a platform MAY implement any subset of algorithms and codecs (C-SERVICES.1, 2, 5 and 7 accept `AAR_E_NOSYS`) |

## C-FREE: freestanding build, leaks, release (sections 3.8, 5.6, 8.6, 10.2)

| id | spec | level | group | profile | judged by | impl | check |
|---|---|---|---|---|---|---|---|
| C-FREE.1 | 3.8 | MUST | core | any | payload | yes | the freestanding payload `conf_free` (built with `AAR_FREESTANDING=1`, linked without libc) runs and hashes a 3 MiB buffer to the known FNV-1a value using only `heap_range` and `heap_grow` |
| C-FREE.2 | 3.8 | MUST | core | any | payload | yes | in the freestanding flavour the weak `memcpy`, `memmove`, `memset` and `memcmp` that the compiler calls work (a struct copy, an overlapping move, a large fill, a compare) |
| C-FREE.3 | 3.8 | MUST | core | any | none | todo | not tested: the freestanding link contains no archive but `libaar.a` and `libgcc.a`; checked at build time by `tools/aar-check --no-libc` and `make check-freestanding-guard` |
| C-FREE.4 | 5.6 | MUST | core | any | runner | yes | heap: a payload that takes the largest heap it can get and exits without freeing runs 20 times; every run gets as much as the first, within one granule of rounding |
| C-FREE.5 | 5.6 | MUST | core | any | runner | yes | shared memory: the same for shared blocks (with console and file service buffers in use, a file left open), within 1 MiB |
| C-FREE.6 | 5.6 | MUST | core | any | runner | yes | claims: 200 rounds of open, load, start, exit, close never end in `AAR_E_NORESOURCE` or `AAR_E_NOMEM` |
| C-FREE.7 | 10.2 | MUST | core | any | runner | yes | after load refusals on every path (images the runner makes from `conf_free.elf` by damaging the header, the note and the segments, and every file under `negative/` next to the payload directories when there is one), the number of claims that can be open, the heap and the shared memory a payload can get are the same as before |
| C-FREE.8 | 8.6 | MUST | services | any | runner | yes | a payload that exits with four image and four audio decode sessions open, 20 times: every run opens four of each kind that has a codec; a kind is unavailable only when every codec answers `AAR_E_NOSYS` (a kind that opened sessions in an earlier run and now refuses, or opens fewer, or answers another code, fails with the run and the code) |
| C-FREE.9 | 4.2 | MUST | display | any | runner | yes | a payload that exits with the display open, 20 times: each run opens it again |
| C-FREE.10 | 8.4 | MUST | audio | any | runner | yes | a payload that exits with the audio stream open, 20 times: each run opens it again |
| C-FREE.11 | 7.1 | MUST | core | any | none | todo | not tested here: the release of files and lists that an exiting app left open is C-FILE.31 |

## Spec questions for review

Statements that are ambiguous, contradict another statement, or cannot be tested
as written. Each has a number that the tables refer to. Each is marked `settled`, with the
section of the specification that decides it, or `open` where it was put to the other platform
developers.

1. **Verdict for statements without a keyword** (11.1). Sections 4 to 10 state
   many rules in plain present tense ("`aar_time_us` returns monotonic
   microseconds", "File ids are small integers, at least 3"). The suite treats
   them as MUST because 11.1 says "every MUST test"; the spec should say that
   rules without a keyword are normative, or mark the ones that are not.
   settled: spec 11.1 (a statement of behaviour without a keyword is a requirement as if it said MUST; explanatory text is marked non-normative)
2. **The recommended path policy** (7.1) is a recommendation of the client
   (`aar-run`), not of the runtime, yet it is the only observable behaviour of
   the path rules. C-FILE.4 and C-FILE.5 are SHOULD and test `aar_serve`'s
   defaults; the spec should say whether a conforming client must follow them.
   Also unstated: the error for a path longer than 255 bytes (C-FILE.2) and for
   a read through a write-only id (C-FILE.9).
   settled: spec 7.1 (the path policy is the client's; the default is a SHOULD for a client that serves apps it does not trust)
3. **Entry mode** (3.2). "user, A32 state, little-endian, interrupts enabled" and
   "EL0, AArch64" cannot be read from user mode: `MRS CPSR` in user mode returns
   only the APSR bits, and `CurrentEL` and `DAIF` trap at EL0. Either name an
   observable consequence or drop the sentence from the test surface.
   settled: spec 3.2 (the mode shows in two effects: the profile's instruction state, and a fault on a privileged instruction where the runtime isolates; C-ENTRY.23 and C-ENTRY.24)
4. **Group bits** (8.1). Bits 5 to 27 were neither defined nor reserved.
   settled: spec 8.1 (bits 5 to 27 are reserved for later versions and no platform sets one, bits 28 to 31 are for experiments of a platform; a required group the platform lacks is `AAR_E_NOGROUP` whether the bit is defined, reserved or experimental, and unknown bits of `groups_optional` are ignored: C-ELF.37)
5. **LOW_MEMORY** (4.1) has no thresholds ("once per low-water crossing", "high-water
   mark"). A test has to exhaust the pool, which the host runner cannot do
   (heap arena fixed at 8 MiB with no trigger), so C-MEM.12 cannot pass there
   (the host build prints `SKIP host:`). State the watermarks as a fraction of
   the pool or give apps a way to provoke the event.
   settled: spec 4.1 (the marks are the platform's own with 0 < low-water < high-water, and a cap below the pool is not provoked)
6. **Isolation** (4.4): "apps cannot reach firmware, other apps or the host's
   memory" is a security property; no black-box test is possible. Keep it as
   documentation or define what a test should show.
   settled: spec 4.4 (isolation is not tested as a whole; the suite tests the bad pointer and the fault report)
7. **Deliberately cacheable client map** (4.3, 11.2). `AARSharedMap` cache-inhibits
   "when it can", and no flag asks for a cacheable map, so the second half of
   the C-SHARED stale-read test (map cacheable plus `AARSharedAcquire`) has no
   API. Add an `AARSharedMap` flag (the `flags` argument is reserved) or an
   `AARSharedAcquire` obligation test on platforms that cannot do otherwise.
   settled: spec 4.3 (the test runs with the map the library gives and calls `AARSharedAcquire` before each pass; a flag for a cacheable map is an API 0.6 candidate)
8. **Corrupt queue** (10.4): "a count above `depth` means the block is corrupt
   (`AAR_E_IO`)" needs write access to the control block; the client API offers
   none. The status flag `AAR_SERVE_R_QUEUE_CORRUPT` exists, but there is no way to
   cause it.
   settled: spec 10.4 (every reader applies the rule; it is checked against the reference model, not by the suite)
9. **Overlong path** (7.1): "at most 255 bytes plus NUL" but no error code for 256
   bytes (`AAR_E_INVAL`, `AAR_E_NOENT` and `AAR_E_ACCES` are all plausible).
   Also, a 255-byte path on AmigaDOS needs components of up to 107 characters
   (FFS), which fails on OFS volumes; say what the fixture may assume.
   settled: spec 7.1 (an empty or overlong path is `AAR_E_INVAL`; a client's filesystem may accept less)
10. **File access modes** (7.1): no error code is given for a write through an id
    opened `READ` only, or a read through one opened `WRITE` only.
    settled: spec 7.1, 3.5 (the wrong direction of a file id is `AAR_E_BADF`, see question 37)
11. **Alignment faults** (5.3): the class `AAR_FAULT_ALIGN` is listed but an
    alignment fault is only produced for some instructions (exclusive and
    multiple loads, device memory); say which fault the standard expects the
    runtime to classify as `ALIGN` and whether unaligned plain loads (usually
    legal) are faults at all.
    settled: spec 5.3 (an unaligned exclusive access is classed `ALIGN`; a plain unaligned access is the platform's choice)
12. **Stuck call** (5.4): "a single call that runs longer than the runtime's
    stuck-call limit (at least 10 s)" cannot be made by a conforming app, since
    calls park in declared waits that do not count; C-LIFE.28 is not tested.
    settled: spec 5.4 (the stuck-call limit is a MAY; C-LIFE.28 is a MAY row)
13. **State after a fault** (5.1): "whether state `FAULTED` persists after
    teardown is to be pinned down before 1.0". C-LIFE.9 accepts both.
    settled: spec 5.1 (after a fault the state is `IDLE` or `FAULTED`, and a client treats both as ended)
14. **Generation wrap** (10.2, open): a platform may tolerate a stale handle that
    names a live app after 65535 reuses; the suite cannot test either answer.
    settled: spec 10.2 (a platform need not retire the index at the wrap)
15. **Unknown display format** (8.2): `AAR_E_INVAL` (unknown argument) or `AAR_E_NOSYS`
    ("format not implemented", the CLUT8 case)? C-DISPLAY.3 accepts both.
    settled: spec 8.2, 3.5 (an unknown format is `AAR_E_INVAL`, a defined format the platform does not provide is `AAR_E_NOSYS`)
16. **What `front` holds after `flip` and `refresh`** (8.2): the text does not say
    that `front` shows the frame just presented; C-DISPLAY.7 assumes it.
    settled: spec 8.2 (after a completed flip and a refresh `front` is NULL or the frame just presented; not promised with LATEST)
17. **Mouse coalescing** (8.3 against 9.6): 8.3 says the client MAY coalesce when
    the queue is full; 9.6 says `AARPostEvent` merges mouse motion while the queue
    is full (no MAY). C-INPUT.10 accepts both through the sum rule; choose one.
    settled: spec 8.3, 9.6 (motion is never lost without a count: it is merged or counted)
18. **Audio ring format value** (8.4): `AAR_AUDIO_FORMAT_S16LE` has no stated
    number in the text; C-AUDIO.2 uses the header's value. Also, the `aar_audio`
    pointers are in the app's address space, so the "plus 0x40" and "plus 0x80"
    offsets assume the three pointers point into one block; say that.
    settled: spec 8.4 (the format value is 1 and the three pointers share one mapping of the ring)
19. **Level of the weighted share** (5.6): "SHOULD give runnable apps CPU in
    proportion to their weight" has no tolerance; C-QUEUE.15 uses 30 percent.
    "MUST NOT let an app that is parked consume CPU" is testable only as a
    throughput bound (C-QUEUE.16, 90 percent).
    settled: spec 5.6 (the standard states no tolerance; the suite's 30 percent and 90 percent are stated as how it measures)
20. **Resolution and granularity** (7.3): "resolution SHOULD be 1 ms or finer" is
    testable; "actual granularity is `sleep_granule_us`" has no upper bound on
    oversleep, so C-TIME.4 uses four granules plus 10 ms, a judgement call.
    settled: spec 7.3 (an app alone SHOULD sleep no longer than `us` plus four granules plus 10 ms)
21. **Where the many-apps tests belong** (11.2): the table lists admission,
    release and CPU shares under C-QUEUE, but sections 5.6 and 10.2 are lifecycle
    and client rules. The suite keeps the spec's placement (C-QUEUE.12 to
    C-QUEUE.16).
    settled: spec 11.2 (the rows stay in C-QUEUE and the placement is explained)
22. **Unserved ops** (9.6): "requests of an unserved op are completed with
    `AAR_E_NOSYS` by the library" does not say how the library knows an op is
    unserved when the client never replies. C-CLIENT.30 tests the
    `aar_serve_config.groups` mechanism instead.
    settled: spec 9.6 (the client's serving loop answers unserved ops with `AAR_E_NOSYS`, not the library)
23. **Probe versus load** (9.2): `AARProbeFile` "does not judge the image against
    the platform"; no statement says what it returns for a note that is present
    but invalid (bad magic): the suite expects an error code, C-CLIENT.10.
    settled: spec 9.2 (`AAR_E_NOENT` and `AAR_E_IO` for the file errors, `AAR_E_INVAL` for anything that is not a valid image, never `AAR_E_PROFILE`)
24. **Handle and NULL** (10.2): "handle 0 is invalid" is stated for the transport
    value; the client API takes a pointer. C-CLIENT.26 assumes NULL gives
    `AAR_E_INVAL`.
    settled: spec 10.2 (a NULL pointer, or a pointer that is not a live handle, is `AAR_E_STALE`)
25. **AARSharedToken** (9.2): "the token of a client buffer for the app" has no app
    side use defined (an app cannot map client memory), so the call is only
    checked for its return code (C-CLIENT.29).
    settled: spec 9.2 (the token names the client's own memory to the app in a reply, such as the surfaces of a display open)
26. **Zero-length grow** (4.1): `heap_grow(0)` is "a whole number of granules";
    the suite does not test it (C-MEM.7 uses non-multiples only).
    settled: spec 4.1 (a zero-length grow or shrink is `AAR_E_INVAL`)
27. **State right after a load** (5.1 table): LOADING to STARTING on "load
    complete, `AARStart`" does not say whether the state after `AARLoadFile`
    returns is `LOADING` or `STARTING`; C-LIFE.1 tests only `CLAIMED` after open
    and the end states.
    settled: spec 5.1 (the load calls leave the state `LOADING` until `AARStart` is accepted)
28. **Revoking a claim** (5.5, 11.2): stale-handle and late-write tests need a
    platform administrative call that revokes a claim or pauses its lease.
    C-LIFE.22 and C-LIFE.23 use the ZZ9000 client extension of spec 8.8 and
    9.2. Spec 11.2 describes how another platform uses its administrative
    calls or reports that those rules are untested.
    settled: spec 5.5, 11.2 (rules 4 and 5 are tested through a platform's own administrative calls; asked in section 13)
29. **Unsatisfiable limits** (4.2): "a request the platform could never satisfy
    fails the load (`AAR_E_NOMEM`)" does not say when a limit is unsatisfiable
    (`shared_max_mib` is the nearest figure) or whether `AARSetTagList` may
    refuse it first (9.2 gives `AAR_E_INVAL` above 4095 MiB only).
    settled: spec 4.2 (`shared_max_mib` is an upper bound; a limit the platform cannot grant fails the load)
30. **Sleep against stop** (3.7, 7.3): 3.7 lists sleep among the calls that park and
    return "on stop", while 7.3 says `aar_sleep_us(us)` "parks for at least
    `us`". The two cannot both hold when a stop arrives during a sleep. C-TIME.5
    follows 3.7 (a stop ends the sleep early, which is also how the host runner
    behaves) and C-TIME.3 follows 7.3 (without a stop). State the exception in 7.3,
    and say whether `aar_sleep_us` has to look at the stop request when it is
    entered with one already pending.
    settled: spec 3.7, 7.3 (a sleep ends early only for a stop, also one that is pending at entry)
31. **`max_apps` of 0** (5.6, 9.2): the question was whether a process-record
    table sized from memory at start counts as a fixed app ceiling.
    C-QUEUE.12 opens claims until the first refusal and checks either
    resource-limited admission or the platform's reported ceiling.
    settled: spec 5.6 (0 means bounded only by resources, a record table sized from memory included; a fixed ceiling is reported and refused with `AAR_REASON_LIMIT`)
32. **Slots of a group that is not declared** (3.3 rule 2): the text says that a set
    group bit means every slot the table covers is non-NULL, not the converse. A
    platform that leaves a slot non-NULL while the bit is clear is not said to be
    wrong, and the wrapper would then run the real call. C-TABLE.8 calls the wrappers
    of an undeclared group only over absent or NULL slots (`AAR_E_NOSYS`) and does
    not call a present one. Say whether a clear bit requires absent or NULL slots.
    settled: spec 3.3 (a clear group bit means every slot of that group is absent or NULL)
33. **Argument limits** (3.2, 9.2): "at least 16 arguments of up to 127 bytes each"
    does not say whether `argv[0]` is one of the 16, and `AARInfo.max_args` and
    `max_arg_bytes` (the latter with the NUL, 128) do not say it either. C-ENTRY.4
    passes 16 strings including `argv[0]`, each of 127 bytes, which is the strictest
    reading; a platform that counts `argv[0]` apart fails it. State which.
    settled: spec 3.2, 9.2 (`argv[0]` is one of the 16 arguments and the NUL is one of the 128 bytes)
34. **The platform's own cap and the pool** (4.1): the limit is "the smaller of the
    platform's cap and the client's `AAR_TAG_PRIVATE_MIB`", but nothing says what the
    platform's cap is, or whether it equals the pool the app can exhaust. C-MEM.9
    compares the capped limit with the limit the same platform reports without a
    cap; C-MEM.12 assumes that without a cap the limit is the pool, so that a
    `heap_grow` loop reaches the low-water mark.
    settled: spec 4.1 (the platform's cap need not equal the pool; `pool_free` is a separate bound)
35. **App and client report the same platform** (3.4, 9.2): `msg_capacity`,
    `stall_limit_ms`, `stop_grace_ms`, the groups and the platform id appear in
    `aar_info` (the app) and in `AARInfo` (the client), and nothing says they must be
    equal. C-TABLE.3, .10, .15 and .16 require it. Say so, or say which may differ.
    settled: spec 3.4 (`platform_id`, `groups`, `flags`, `shared_granule`, `msg_capacity`, `stall_limit_ms` and `stop_grace_ms` are equal in `aar_info` and `AARInfo`)
36. **How long a file wait lasts** (7.1): "a call waits at most 30 seconds for the
    client (`AAR_E_TIMEDOUT`)". "At most" gives only an upper bound, and no
    tolerance is stated for the timer that measures it. C-FILE.26 accepts
    29 to 36 seconds as the app measures it; the spec should say whether a
    shorter wait (a runtime that gives up after 10 s) conforms and how exact
    the 30 seconds is.
    settled: spec 7.1 (a request waits 30 seconds, measured 29 to 36 as the app sees it; a shorter limit does not conform)
37. **Reading through a write-only file id, writing through a read-only one**
    (7.1): the spec says nothing about either. The host runner reads through a
    write-only id. The ZZ9000 client opens `AAR_FILE_WRITE` with `MODE_READWRITE`,
    which cannot refuse a read. In the first card run a write through an id opened
    `AAR_FILE_READ` took bytes, because `aar_serve` does not keep the open mode
    of an id. C-FILE.9 tests neither. Say whether a runtime has to refuse them
    and, if so, with which code (question 10).
    settled: spec 7.1, 3.5 (a file id is used only for the direction it was opened for, else `AAR_E_BADF`: C-FILE.37)
38. **Stream and session details of the services group** (8.6). The text did not
    give these details:
    - the framing of an LZMA2 stream (one byte of properties before the raw stream)
    - the LHA streams: no end marker, so the destination size is the decode length
      and a destination that is too small ends the decode without an error;
      `bytes_consumed` is not defined
    - whether a destination larger than the output is allowed for the other
      algorithms
    - the image-session protocol: what the `aar_image_decode` `flags` mean (bit 0
      ends the input), when a tile is complete, how a drain with no new input is
      made, what `bytes_written` counts, and the result flags

    C-SERVICES.2 to C-SERVICES.5 follow the readings now written in 8.6; a platform
    with another framing fails them.
    settled: spec 8.6 (the formats, framings and session rules are pinned; the open points of the session protocol go to API 0.6)
39. **What the probe refuses** (9.2, 2.2): `AARProbeFile` "reads the note without
    loading", and said nothing about an image that breaks a rule of 2.2 or 2.3. The
    reference library applied every rule of `tools/aar-check`, which also demands
    section headers, while 2.2.4 says section headers MAY be absent.
    settled: spec 2.2, 2.4, 9.2 (the probe applies the image rules of 2.2 and 2.3 as the loader does, with or without section headers, and does not judge the image against the platform; `tools/aar-check` stays a stricter lint for build output; C-ELF.16 requires the probe to accept the image without section headers)
40. **An unsatisfiable heap** (4.1, 2.3): `min_heap_mib` "committed at load" had no
    limit in the text, and a platform that converts MiB to bytes in 32 bits wraps
    at 4096.
    settled: spec 4.1 (the word is a 32-bit count with no maximum, and a runtime compares it without a conversion that can wrap; C-ELF.40 uses `heap-max` (0xFFFFFFFF) and `heap-wrap` (0x10000001, which is 1 MiB in 32-bit bytes), both refused with `AAR_E_NOMEM` or `AAR_E_NORESOURCE`)
41. **An address that no app can map** (5.3): the fault tests need an address that is
    unmapped in every app's address space. The standard does not say what the
    address space of an app holds beyond the image, heap and stack. C-LIFE.9 uses
    `0xDEAD0000` (A32) and `0xDEAD00000000` (A64); a platform that maps it fails
    the test through no fault of its own. Say which part of the space is never
    mapped.
    settled: spec 4.4 (an app's address space holds its image, heap, stack and the buffers it owns, and where `AAR_INFOF_ISOLATED` is set every other address is unmapped; the test picks an address outside them at run time; asked in section 13 what each platform maps)
42. **Seeing a dead client's release** (5.5): the client API has no call that shows
    from outside that the runtime stopped an app whose owner is gone. C-LIFE.20 and
    C-LIFE.21 show that the handle is released (a call returns `AAR_E_STALE`, not
    `AAR_E_OWNER`) and that a new claim works, and cannot show that the app stopped
    running; a platform with a ceiling (`max_apps`) shows the slot coming back.
    settled: spec 5.5 (the base API shows a stale handle and a free claim; a platform MAY offer a listing, the ZZ9000 does: C-LIFE.37)
43. **Range check of AARSharedMap** (9.5): the library "MUST check the range against
    the platform's shared aperture", which says nothing about the block the token
    names: a range that leaves the app's block but stays inside the aperture is
    accepted, and C-SHARED.15 cannot test it. The test uses tokens outside the
    aperture (0 and 0xFFFFFFF0) and a range of 2 GiB and of 4 GiB minus 16 bytes,
    and assumes the aperture is smaller than that. Say whether the block is part of
    the check.
    settled: spec 9.5 (the library checks the aperture only; the client MUST NOT write outside its block)
44. **Which requests park the app** (9.6, 10.4): the text calls console writes, palette
    updates and asynchronous flips "non-parking" without saying that no other op
    is, or that a console write never parks. C-QUEUE.5 needs a request that leaves
    the app running; if the platform parks the app in `console_write`, the test
    passes with the reason in its detail and the rule is not exercised. Say which
    ops never park.
    settled: spec 9.6, 7.2 (three requests return at once and every other op parks; a console write of 16 bytes does not park)
45. **Reply to a request that is not outstanding** (9.6): the text says wrong-id, wrong-op
    and duplicate replies "cannot release another request's storage" and that
    `AARServiceReply` resumes the app only if it is parked for that request, but
    gives no code for the refused reply (docs/aar-client-api.md says `AAR_E_INVAL`).
    C-QUEUE.3 and C-QUEUE.4 record the code and judge only that the app stays
    parked.
    settled: spec 9.6 (a reply that matches no outstanding request is `AAR_E_INVAL`; the late completion after a timeout is accepted)
46. **Log drops under a rate limit** (7.2, 10.4): a platform MAY rate-limit `aar_log`
    (see C-CON.10 for the ZZ9000 limits), and the platform counts a
    dropped record in `log_dropped`. It is not said whether a record that the rate
    limit discards is a dropped record. C-QUEUE.9 requires `log_dropped` above 0
    whenever fewer records than were written reached the client.
    settled: spec 7.2 (a record discarded before the client receives it, for any reason, counts in `log_dropped`)
47. **The client's inbox** (6, 10.4): C-MSG.16 needs the client's inbox to be full while
    the next message and a request behind it wait in the submission queue. The text
    says a full inbox leaves the next message in the queue and holds up the request
    behind it, but not how big the inbox is (`msg_capacity`?), so the number of
    messages `aar_msg_send` accepts before `AAR_E_BUSY` is only bounded below (C-MSG.4
    and C-MSG.5 test "at least `msg_capacity`").
    settled: spec 6, 10.4 (`msg_capacity` is a minimum; the client library's inbox adds to it)
48. **Posted events** (8.3, 9.6): C-INPUT.3 assumes that `AARPostEvent` passes `time_ms`
    and `code` of a mouse event to the app unchanged ("client timestamp" may mean
    that the library stamps it), and that an `AAR_EV_STOP` posted with `AARPostEvent`
    is an event only, not a stop request. Say what the library may change.
    settled: spec 8.3 (a posted event arrives as given, `time_ms` is the client's, a posted `AAR_EV_STOP` is an event only)
49. **Client pixel memory** (9.2, 8.2): `AARSharedToken` names memory "in the platform's
    shared aperture", and a client cannot obtain such memory portably (the ZZ9000
    aperture is the card's own memory). The C-DISPLAY and C-GL tests that read the
    frame (C-DISPLAY.12 to C-DISPLAY.15, C-GL.5) give the platform a display table with
    memory from `AllocVec` and, when the platform refuses it, from a Picasso96 bitmap;
    a platform whose aperture is neither cannot be tested. The standard should give a
    client a way to allocate shared memory (the app side has `aar_shared_alloc`).
    settled: spec 9.2, 9.5 (`AARSharedToken` accepts the platform's aperture only and the platform documents which memory is in it; an allocator is an API 0.6 candidate)
50. **Copy pitch in single-buffer mode** (8.2, 9.6): when the client's table has one buffer,
    "the platform copies each frame"; it is not said that the copy keeps the pitch of
    the open reply and the layout. C-DISPLAY.12 and C-DISPLAY.14 assume it.
    settled: spec 8.2, 9.6 (the copy of a single buffer keeps pitch and layout)
51. **Underrun counting** (8.4): "the runtime counts underruns" does not say per frame, per
    period or per starved callback. C-AUDIO.6 requires only that the count rises while
    the app writes nothing, and that the stats call and the ring word agree.
    settled: spec 8.4 (the consumer counts one underrun per starved read or period; the unit is its own)
52. **AHI route is not optional?** (8.4, 9.2): `AAR_TAG_AUDIO_ROUTE` is "the client's
    decision" and `AARSetTagList` may answer `AAR_E_NOSYS` for an option the platform
    cannot support. It is not said whether a platform with the audio group must support the
    route `AHI`. C-AUDIO.10 and C-AUDIO.11 fail when it does not.
    settled: spec 8.4, 9.2 (a platform with the audio group supports the route `AHI`; `PLATFORM` is optional and may be refused with `AAR_E_NOSYS`)
53. **Depth buffer in the GL group** (8.5): the group is "GL 1.1 entry points" but the text
    does not promise a depth buffer. C-GL.7 draws a depth-tested scene for `DepthRange`;
    a platform without depth testing fails that row.
    settled: spec 8.5 (the context has a depth buffer of at least 16 bits)
54. **Bad pointer in a `void` GL call** (8.5): "return `AAR_E_FAULT` plus `GL_INVALID_VALUE`" is
    not observable through a `void` `gl*` wrapper; C-GL.8 checks `glGetError` only.
    settled: spec 8.5 (a bad range draws nothing, sets `GL_INVALID_VALUE` and does not fault the app)
55. **What `heap_info.committed` counts** (3.4, 4.1): "private memory currently
    committed" and the limit that caps it are about private memory, which 4.1 says is
    "its image, stack and a heap", while `heap_range` reports only the heap. The ZZ9000
    reports all private memory (4 MiB for an app with 3 MiB of heap, image and a 1 MiB
    stack frame). C-MEM.1 requires only that the heap is not larger than `committed`;
    C-MEM.2, .6 and .8 judge `committed` by how it changes. Say which figure it is.
    settled: spec 3.4, 4.1 (`committed` counts image, stack and heap, so the heap is smaller than `committed`)
56. **Seek past the end of a file** (7.1): `aar_file_seek` has no rule for a
    position beyond the end. POSIX allows it; AmigaDOS refuses it on a handle
    that cannot grow the file (the ZZ9000 client answers `AAR_E_INVAL` for a
    `AAR_FILE_READ` id). C-FILE.19 accepts `AAR_E_INVAL`, or success followed by
    a read of 0 bytes. Say which is meant.
    settled: spec 7.1 (a seek past the end MAY be refused with `AAR_E_INVAL` or accepted; before the start is `AAR_E_INVAL`)
57. **Calls after a stop request** (3.7, 7.1): 3.7 says a stop ends parked calls
    with `AAR_E_IO`, and 7.1 says the client refuses requests that create,
    truncate or write once it has asked the app to stop. On the ZZ9000 a call
    that the client does not answer within a few polls is ended by the stop and
    abandoned. The next call on that entry is `AAR_E_BUSY` until the client's
    late completion arrives. So after a stop the app cannot tell a refusal from a
    cancelled wait, and a read need not work. C-FILE.6 asks only for negative
    codes from the mutating calls and judges what the client did from the files.
    Say whether a call after a stop request is expected to complete when the
    client answers at once.
    settled: spec 3.7, 7.1 (a call after a stop MAY complete or end with `AAR_E_IO`; a change the client refused never takes effect)
58. **Revoked handle and calls that read memory** (5.5, 9.3): "a request whose generation no
    longer names a live app fails with `AAR_E_STALE`". The library learns that a claim
    was revoked only from a platform call that answers stale (stop, load, map, start);
    `AARStatus`, `AARGetMessage` and `AARSendMessage` read or write the shared
    memory and cannot know. C-LIFE.22 makes `AARStop` the first call after the
    revocation and requires `AAR_E_STALE` from every call after it. Say whether the
    library has to check the claim (the handle word of the control block) on every call.
    settled: spec 5.5 (requests that reach the platform fail with `AAR_E_STALE`; calls that only touch shared memory MAY succeed until the library has seen that answer)
59. **When the signal bit is freed** (9.2, 9.3, 9.4): `AARClose` "releases the claim and
    the signal", 9.4 says the bit is freed "at its last close", and 9.3 says closing the
    last base closes the handles. The reference library frees the bit when the task
    closes its last `aar.library` base, not at its last `AARClose`. C-CLIENT.23 counts
    in a child task and accepts either point after the last `AARClose`, and requires the
    bit back after `CloseLibrary`.
    settled: spec 9.2, 9.4 (the signal bit is freed at the last `CloseLibrary`, not at the last `AARClose`)
60. **Refusing a load for lack of memory** (4.1, 5.2, 5.6, 9.2): 5.2 lists `AAR_E_NOMEM`
    for a load, 5.6 and the client API give `AAR_E_NORESOURCE` with an `AAR_REASON_*`
    for a platform that lacks the memory. The ZZ9000 answers `AAR_E_NORESOURCE` with
    `AAR_REASON_MEMORY` to a heap it can never commit. C-ELF.40 and C-ELF.41 accept
    both. Say whether a request that can never be satisfied is `AAR_E_NOMEM` and a
    shortage of the moment `AAR_E_NORESOURCE`.
    settled: spec 4.1, 4.2, 5.2, 5.6 (a refusal for lack of memory is `AAR_E_NORESOURCE` with a reason or `AAR_E_NOMEM`; a client treats both alike)
61. **Rules the loader is not told to enforce** (2.2): the spec did not say that an
    image without `PT_DYNAMIC` or a relocation whose target lies outside every
    `PT_LOAD` is refused, and for a class or machine that the platform does not run it
    allowed `AAR_E_PROFILE` or `AAR_E_INVAL`.
    settled: spec 2.2 (an image has exactly one `PT_DYNAMIC`, every relocation target lies inside `p_memsz` of a `PT_LOAD` with room for a full word, and both are refused with `AAR_E_INVAL`; C-ELF.15 and C-ELF.23 accept a refusal only; C-ELF.31 accepts `AAR_E_PROFILE` or `AAR_E_INVAL`, as 2.2 says)
62. **SQ full and the message reserve** (10.3, 10.4): `sq_full` counts "a post that finds the SQ
    full", but a message post is refused one entry early (the reserve for a request, 6), when
    the queue is not yet full. The ZZ9000 does not count that refusal. C-QUEUE.8 fills the
    last entry with log records and counts the next post as the full queue; the text should
    say whether a refused message post is a full SQ.
    settled: spec 10.4 (`sq_full` counts a request or log post that finds no free entry; a refused message post is not counted)
63. **Events the runtime drops** (8.3, 9.6, 10.3): the client counts an event it could not post
    (`events_dropped`), and "the runtime keeps at least 64 events". It is not said what
    happens to an event the runtime takes out of the CQ when its own queue is full, nor who
    counts it. The ZZ9000 drops it without a count the client can read (the word is
    written by the client), so a client never sees more than 64 events kept, with no refusal
    and no counter, however many it posts. C-INPUT.9 takes the guarantees that are stated
    (the first 64 kept in order, the client's own refusals counted); C-INPUT.10 keeps the
    sum rule of 9.6 ("never lost silently") and fails on such a platform.
    settled: spec 8.3, 10.3 (the runtime MAY discard events beyond 64, mouse motion excepted, and does not count them; a runtime counter is an API 0.6 candidate)
64. **Tolerance of lossy image decode** (8.6): the text gives none and does not fix the
    IDCT or the chroma upsampling. The reference pixels are those of libjpeg's accurate
    integer IDCT and libwebp; the ZZ9000 uses the fast integer IDCT (`JDCT_IFAST`), which
    is off by 4 on the 16x16 test image. C-SERVICES.6 accepts 6 for JPEG and 3 for WebP.
    Say which error a conforming decoder may have, or require the accurate IDCT.
    settled: spec 8.6 (lossy decoders are not bit exact; the suite accepts 6 per channel for JPEG and 3 for WebP)
65. **Does `aar_display_vsync` wait?** (8.2): the spec lists the call and says nothing of its
    timing; docs/aar-api.md says it "requests synchronisation with the display refresh" and
    that the ZZ9000 "yields once; it does not park for the next raster line". The test asks
    for `AAR_OK` and for no hang, not for the time of a refresh. If the standard means that
    the call returns at the next refresh, a floor can be added and the ZZ9000 fails it.
    settled: spec 8.2 (`aar_display_vsync` is a pacing hint)
66. **When is a swapped frame visible?** (8.5): "`aar_gl_swap()` flushes, copies and flips" and
    docs/aar-api.md says it "parks as `aar_display_flip` does" and, for the ZZ9000, "drains
    RasterIX and copies the context target". The ZZ9000 queues the copy and the flip and
    does the copy during the next swap (or at once only with its debug flag `SYNC_PRESENT`):
    a client that reads the frame right after the swap sees the one before. C-GL.5 accepts
    a frame that is visible after up to four further swaps of the same picture; the text
    should say whether a swap may return before its frame is presented.
    settled: spec 8.5 (a swap may return before its frame is visible; at most two swapped frames are in flight)
67. **A rename onto the same name** (7.1). `file_rename(a, a, AAR_RENAME_REPLACE)` "deletes the target
    first", and the target is the source. A client that does that destroys the file and then fails.
    POSIX makes it a no-op, and C-FILE.52 tests it, with a second spelling of the same path where the
    client can express one.
    settled: spec 7.1 (a source and a target that are the same object leave it unchanged and the call is `AAR_OK`, with or without the flag; the client compares objects, not strings)
68. **What the locale record reports** (7.4). `country` was "the country code the client reports
    (`loc_CountryCode` as text)" and `loc_CountryCode` is a `ULONG`. `language` is compared with
    `loc_LanguageName` of the default locale, which may be empty on a system without locale
    preferences, and an empty name then has to match an empty answer. C-HOST.3 compares the UTC offset
    (negated), the language, the country where the client reports one, the decimal point and the group
    separator.
    settled: spec 7.4 (`country` is the non-zero bytes of `loc_CountryCode`, most significant first, as text)
