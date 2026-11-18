<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# zzapp 0.4 to AAR 0.5

`include/aar/*.h` replaces `include/zzapp.h` and `include/zzapp_note.h`.
Nothing of the old API shipped, so there is no compatibility layer. This
page maps every call, constant group and type of `zzapp.h` 0.4, and the
launcher wire layouts of `zzapp_proto.h`, to what replaces them, or says why
it is dropped.

Naming: `zz_` becomes `aar_`, `ZZ_` and `ZZAPP_` become `AAR_`. Two wrappers
change more than the prefix, because `aar_info` and `aar_heap_info` are
structure names: `zz_mem_info` is split into `aar_get_info` and
`aar_get_heap_info`, and the optional audio counters are `aar_audio_get_stats`.

## Table slots

| zzapp_api (0.4) | aar_api (0.5) |
|---|---|
| `size`, `version` | `size`, `version` (same meaning; gate on `size` and `groups`, not `version`) |
| `flags` (runtime private) | dropped; replaced by `groups` |
| `reserved0` | `platform_id` |
| `exit`, `yield`, `should_stop`, `time_us`, `sleep_us` | same names, core |
| `alloc(bytes, align, flags)` | dropped from the table. Private memory is `heap_range` / `heap_grow` / `heap_shrink`; the libc-flavor `aar_alloc(uint64_t bytes, uint32_t align)` is libaar layer 2 (not declared when `AAR_FREESTANDING`). The host-visible case is `shared_alloc` |
| `free` | `aar_free` for private blocks (layer 2), `shared_free` for shared ones |
| `heap_range(void **, uint32_t *)` | `heap_range(void **, uint64_t *)` |
| `log` | `log`, core |
| `file_open` ... `file_list_close` (9 calls) | same names, core. `aar_stat.mtime` is 64-bit |
| `display_open`, `display_flip`, `display_vsync`, `display_close` | same names, group `AAR_GROUP_DISPLAY` |
| `display_flip_async`, `display_flip_idle`, `display_refresh`, `display_set_palette` | same names, display group |
| `display_mailbox_replaced` | `display_latest_dropped` |
| `input_poll` | `event_poll`, core. `AAR_EV_STOP` and `AAR_EV_LOW_MEMORY` are always delivered; the input events need `AAR_GROUP_INPUT` |
| `audio_open`, `audio_close` | same names, group `AAR_GROUP_AUDIO`; rate and channels are negotiated (22050, 44100, 48000 Hz stereo) |
| (none) | `audio_stats`, optional, audio group (spec 8.4) |
| `gl_bind`, `gl_swap`, `gl_unbind` | same names, group `AAR_GROUP_GL` |
| `gl_bind_flags` | dropped from the table; `aar_ext_zz9000.gl_bind_flags` (`aar/ext_zz9000.h`) |
| `build_id` | dropped; `aar_ext_zz9000.build_id` |
| `host_address(const void *, uint32_t *)` | dropped. `shared_alloc` returns the token as `aar_shared.haddr` |
| `mem_info` | dropped. `info` (`aar_info`, limits and `shared_used`) plus `heap_info`. The legacy `arena_*` fields are gone |
| `map_file(path, const void **, uint32_t *)` | `map_file(..., uint64_t *)`, optional |
| `unmap_file` | `unmap_file`, optional |
| `sha256`, `decompress`, `image_open`, `image_decode`, `image_close`, `audio_decode_open`, `audio_decode`, `audio_decode_close` | same names, group `AAR_GROUP_SERVICES`. Whole-buffer byte counts are `uint64_t`; per-call chunk sizes stay `uint32_t` |
| `console_write`, `console_read` | same names, core |
| `msg_send`, `msg_poll`, `msg_wait` | same names, core |
| `heap_grow`, `heap_shrink` | same names, core, `uint64_t bytes` |
| `heap_info` | `heap_info`, core, 64-bit fields |
| (none) | `info`, `shared_alloc`, `shared_free`, `publish`, `acquire`, `ext` (new) |

Slot order is the new one in `aar/app.h`: core in spec order, then `ext`,
`map_file`, `unmap_file`, then display, audio, gl and services. The GL table
is `aar_gl` in `aar/gl.h`; the `gl*` wrappers and `gluPerspective`,
`gluBuild2DMipmaps` keep their names.

## Wrappers and entry points

| zzapp | AAR |
|---|---|
| `zz_*` for every table call above | `aar_*` for the same call |
| `zz_alloc` | `aar_alloc` (private) or `aar_shared_alloc` (`ZZ_ALLOC_HOST_VISIBLE`) |
| `zz_free` | `aar_free` or `aar_shared_free` |
| `zz_host_address` | dropped, see above |
| `zz_mem_info` | `aar_get_info`, `aar_get_heap_info` |
| `zz_gl_bind_flags` | `aar_ext_zz9000.gl_bind_flags` |
| `zz_display_mailbox_replaced` | `aar_display_latest_dropped` |
| `zzapp_api_ptr`, `zz_main` | `aar_api_ptr`, `aar_main` |
| `ZZ_API_COVERS`, `ZZ_GL_COVERS` | `AAR_API_COVERS`, `AAR_GL_COVERS` (new: `AAR_API_AT_LEAST`, `AAR_EXT_ZZ9000_COVERS`) |
| (none) | `aar_bswap16`, `aar_bswap32`, `aar_bswap64` |

## Constants

| zzapp | AAR |
|---|---|
| `ZZAPP_API_MAJOR`, `_MINOR`, `_VERSION` (0.4) | `AAR_API_MAJOR`, `_MINOR`, `_VERSION` (0.5) |
| `ZZ_OK`, `ZZ_EIO` .. `ZZ_EBADF` (0 to -12) | `AAR_OK`, `AAR_E_IO` .. `AAR_E_BADF`, same values |
| `ZZAPP_ERR_STALE` (launcher status 5) | `AAR_E_STALE` (-16); new codes `AAR_E_PROFILE` -13, `AAR_E_NOGROUP` -14, `AAR_E_NORESOURCE` -15, `AAR_E_OWNER` -17 |
| `ZZ_FMT_RGB565`, `_BGRA8888`, `_CLUT8` | `AAR_FMT_*` |
| `ZZ_LAYOUT_*` (7 values) | `AAR_LAYOUT_*` |
| `ZZ_EV_KEY_DOWN` .. `ZZ_EV_LOW_MEMORY` | `AAR_EV_*` |
| `ZZ_BREAK_CTRL_D/E/F` | `AAR_BREAK_CTRL_D/E/F` |
| `ZZ_ALLOC_UNCACHED` | dropped: it had no effect |
| `ZZ_ALLOC_HOST_VISIBLE` | dropped as a flag: `aar_shared_alloc` is the host-visible allocator. Its `flags` argument must be 0 |
| `ZZ_COMPRESS_NONE` .. `ZZ_COMPRESS_LH7` | `AAR_COMPRESS_*`, same numbers |
| `ZZ_FILE_READ`, `_WRITE`; `ZZ_SEEK_SET`, `_CUR`, `_END` | `AAR_FILE_*`, `AAR_SEEK_*`; new `AAR_MODE_FILE`, `AAR_MODE_DIR` (were `ZZAPP_MODE_*` in the proto header) |
| `ZZ_FILE_CREATE` (create or replace) | `AAR_FILE_CREATE \| AAR_FILE_TRUNCATE`. `AAR_FILE_CREATE` alone now creates a missing file and keeps an existing one. New: `AAR_FILE_TRUNCATE` (empty an existing file) and `AAR_FILE_EXCL` (with `AAR_FILE_CREATE`, fail with `AAR_E_EXIST` when the file exists) |
| `ZZ_DISPLAY_VSYNC` | `AAR_DISPLAY_VSYNC` |
| `ZZ_DISPLAY_MAILBOX` | `AAR_DISPLAY_LATEST`; `AAR_DISPCAP_LATEST` in `aar_surface.caps` says it is in effect |
| `ZZ_IMAGE_JPEG`, `_PNG`, `_WEBP` | `AAR_IMAGE_*` |
| `ZZ_AUDIO_MP3`, `_FLAC`, `_VORBIS`; `ZZ_AUDIO_DECODE_EOF`, `_DRAIN` | `AAR_AUDIO_*`, `AAR_AUDIO_DECODE_*` |
| `ZZ_GL_BIND_DIRECT_PRESENT`, `_SYNC_PRESENT`, `_SYNC_RIX_FEED`, `_INLINE_RIX_FEED` | `AAR_ZZ9000_GL_BIND_*` in `aar/ext_zz9000.h` |
| `ZZAPP_GL_VERSION_ARRAYS` | `AAR_GL_VERSION_ARRAYS` (and `AAR_GL_VERSION_V0`); stored in `aar_gl.version` |
| `ZZAPP_NOTE_MAGIC` (bytes `AZPP`), `ZZAPP_NOTE_TYPE` | `AAR_NOTE_MAGIC` (`AARN`), `AAR_NOTE_TYPE`, `AAR_NOTE_VERSION` |
| `ZZAPP_NOTE_FLAG_DISPLAY`, `_AUDIO`, `_GL` | `AAR_GROUP_DISPLAY`, `_AUDIO`, `_GL` in the note's `groups_required` and `groups_optional` words |
| `ZZAPP_NOTE_FLAG_DYNAMIC_HEAP` | dropped: the heap is always dynamic and the fixed 96 MiB region is gone |
| `ZZAPP_NOTE(flags, stack)` | `AAR_NOTE(stack, min_heap_mib, groups_required, groups_optional)` |
| `ZZAPP_NOTE_MIN_HEAP(flags, stack, mib)` | `AAR_NOTE`; `AAR_NOTE_EX` adds an explicit `api_minor_required` |
| (none) | `AAR_PROFILE_A32`, `AAR_PROFILE_A64`, `AAR_PLATFORM_*`, `AAR_GROUP_*`, `AAR_INFOF_*`, `AAR_CPU_*`, `AAR_SHAREDF_*`, `AAR_DISPCAP_*`, `AAR_REASON_*`, `AAR_STATE_*`, `AAR_FAULT_*`, `AAR_END_*` |

## Types

| zzapp | AAR |
|---|---|
| `zzapp_note` (24 bytes, flags word) | the 40-byte note descriptor of spec 2.3; no C structure, `AAR_NOTE` emits it |
| `zzapp_surface` | `aar_surface`: `card_front` and `card_back` dropped; `layout` stays; `caps` added; the two pointers moved after the integers |
| `zzapp_event` | `aar_event`, unchanged layout (16 bytes) |
| `zzapp_stat` | `aar_stat`: `mtime` is `int64_t`, so the structure is 24 bytes |
| `zzapp_dirent` | `aar_dirent`, unchanged |
| `zzapp_audio` | `aar_audio`: the three pointers moved after the integers; `flags` added |
| `zzapp_mem_info` | dropped; `aar_info` and `aar_heap_info` |
| `zzapp_heap_info` | `aar_heap_info`, `uint64_t` fields |
| `zzapp_service_result` | `aar_service_result`, `uint64_t` byte counts |
| `zzapp_image_open` | `aar_image_params` (renamed: `aar_image_open` is the call); the pointer moved last |
| `zzapp_image_result`, `zzapp_audio_decode_result` | `aar_image_result`, `aar_audio_decode_result`, unchanged |
| `zzapp_gl` | `aar_gl`: `version` word added after `size`; `build_id` dropped (see `aar_ext_zz9000`) |
| `zzapp_api` | `aar_api`: header is `size`, `version`, `groups`, `platform_id` |
| (none) | `aar_info`, `aar_shared`, `aar_audio_stats`, `aar_ext_zz9000` |

## Launcher wire layouts (`zzapp_proto.h`) to the transport

The 592 KiB per-app window and every fixed offset in it are retired. The
control block and queue pair of `aar/transport.h` replace them. The mailbox
opcodes (`ZZAPP_OP_*`, `ZZAPP_CAP_*`, the QUERY, ALLOC, LOAD, START, STOP,
STATUS, DISPLAY_BIND, LIST and RELEASE payloads) are not part of the app
standard. They stay private to the firmware and its library and keep their
`ZZAPP_` names; the slice that builds the new transport reworks them.

| zzapp_proto.h | transport.h |
|---|---|
| `ZZAPP_LIFE_*` lifecycle block (magic, state, generation, heartbeat, stop, fault, exit, pc/sp/lr/far/fsr/cpsr, flags, progress) | control block: `AAR_CB_MAGIC`, `STATE`, `END_REASON`, `EXIT_CODE`, `STOP_REQUEST`, `LEASE_COUNTER`, `FAULT_*`, `LOAD_BASE_*`, `FLAGS`, `PROGRESS_*` |
| `ZZAPP_LIFE_GENERATION_OFF`, `ZZAPP_SEL_GENERATION`, `ZZAPP_SEL_SLOT` | the 16-bit generation of the handle (`aar_handle_*`); slot numbers are gone |
| `ZZAPP_LIFE_INPUT_DROPS_OFF`, `ZZAPP_LIFE_LOG_DROPS_OFF` | `AAR_CB_EVENTS_DROPPED`, `AAR_CB_LOG_DROPPED` |
| `ZZAPP_LIFE_FLIP_*`, `_WIDTH` .. `_CARD_SPARE`, `_PALETTE_*`, `_MAILBOX_REPLACED` | dropped from shared state: flips, binds and palettes are `AAR_OP_DISPLAY_*` requests and completions; the latest-mode drop count is `AAR_R_DISPLAY_FLIP_DROPPED` |
| `ZZAPP_LIFE_AUDIO_*` words | `AAR_OP_AUDIO_OPEN` arguments and results, and the header of the audio ring buffer (`AAR_AUDIO_RING_*`) |
| `ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF` | dropped; the app sees `AAR_EV_LOW_MEMORY` |
| `ZZAPP_ST_*` (state) | `AAR_STATE_*` (state 4, paused, is not defined) |
| `ZZAPP_FAULT_*` | `AAR_FAULT_*` class plus the raw syndrome; the watchdog and forced-stop causes are `AAR_END_*` |
| `ZZAPP_END_*` | `AAR_END_*` (`AMIGA_RESET` is `PLATFORM_RESET`, `CORE_RESTART` is `RUNTIME_RESTART`) |
| `ZZAPP_BLOCK_*` | dropped (a debugging aid of the list reply) |
| `ZZAPP_F_*` flags | `AAR_CBF_DISPLAY`, `_AUDIO`, `_GL`, `_CONSOLE`; launcher capability bits become `aar_surface.caps` and the group bits |
| `ZZAPP_INPUT_*` ring (256 events) | unsolicited `AAR_OP_EVENT` entries in the completion queue |
| `ZZAPP_LOG_*` ring | `AAR_OP_LOG` entries naming a record in a shared log buffer |
| `ZZAPP_FILE_*`, `zzapp_file_wire`, `ZZAPP_FILE_OP_*` | `AAR_OP_FILE_*` and `AAR_OP_LIST_*` requests with `AAR_A_FILE_*` and `AAR_R_FILE_*` word indices; paths travel in a shared buffer |
| `ZZAPP_FILE_READ/WRITE/CREATE`, `ZZAPP_SEEK_*`, `ZZAPP_MODE_*`, `ZZAPP_PATH_MAX`, `ZZAPP_FD_BASE` | `AAR_FILE_*`, `AAR_SEEK_*`, `AAR_MODE_*`, `AAR_PATH_MAX`, `AAR_FD_BASE` |
| `ZZAPP_DATA_*`, `ZZAPP_STAGING_*`, `ZZAPP_CHUNK_*` | dropped: file payloads are shared buffers named by `buf`, `buf_off`, `buf_len` |
| `ZZAPP_CONSOLE_*` rings | `AAR_OP_CONSOLE_WRITE`, `AAR_OP_CONSOLE_READ`; the bytes sit in a console buffer named by the entry |
| `ZZAPP_AUDIO_*` ring and header | `AAR_AUDIO_RING_*`, `AAR_AUDIO_ROUTE_*` (`ROUTE_CARD` is `ROUTE_PLATFORM`), `AAR_AUDIO_FORMAT_S16LE` |
| `ZZAPP_MSG_H2A_*`, `ZZAPP_MSG_A2H_*`, `ZZAPP_MSG_CAP` (64), `zzapp_wire_msg` | `AAR_OP_MSG` entries (32 bytes at `AAR_ENTRY_MSG`), `AAR_MSG_BYTES`, `AAR_MSG_CAPACITY` (63) |
| `ZZAPP_PALETTE_*` | `AAR_OP_DISPLAY_PALETTE`, colors in a shared buffer |
| `ZZAPP_ARGV_*` (argv area) | `AAR_MIN_ARGS`, `AAR_MIN_ARG_BYTES`; the strings are delivered by the load and start calls, not by a fixed area |
| `ZZAPP_WINDOW_BYTES` and every `*_OFF` | dropped: no per-app window |
| `zzapp_put_be*`, `zzapp_get_be*` | `aar_put_be16/32/64`, `aar_get_be16/32/64` |
| `ZZAPP_HOST_VISIBLE_DEFAULT_BYTES` (16 MiB) | `AAR_SHARED_DEFAULT_MIB`; limits are `AAR_CB_SHARED_MIB`, `AAR_CB_PRIVATE_MIB` |
