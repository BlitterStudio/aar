/*
 * Shared state for the SDL2 host runner. Not an app header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_HOST_INTERNAL_H
#define AAR_HOST_INTERNAL_H

#include "aar/app.h"
#include "aar/transport.h"

#include <stdint.h>

/* Files and lists: AAR_MIN_FILES of each, ids from AAR_FD_BASE. The count is
 * an int because ids and slot indexes are. */
#define HOST_FD_BASE AAR_FD_BASE
#define HOST_FD_COUNT ((int)AAR_MIN_FILES)
#define HOST_PATH_MAX AAR_PATH_MAX

/* One queue per direction, AAR_QUEUE_DEPTH_DEFAULT slots, one kept free. */
#define HOST_MSG_SLOTS AAR_QUEUE_DEPTH_DEFAULT
#define HOST_MSG_CAP AAR_MSG_CAPACITY
#define HOST_MSG_BYTES AAR_MSG_BYTES

#define HOST_AUDIO_FRAMES 8192u
#define HOST_INPUT_CAP 256u

/* Shared buffers: 4 KiB granule, the default limit, and a table that holds
 * the promised AAR_SHARED_MIN_BLOCKS app blocks plus runtime-owned file maps. */
#define HOST_SHARED_GRANULE 4096u
#define HOST_SHARED_LIMIT ((uint64_t)AAR_SHARED_DEFAULT_MIB << 20)
#define HOST_SHARED_APP_BLOCKS AAR_SHARED_MIN_BLOCKS
#define HOST_SHARED_MAP_BLOCKS 16u
#define HOST_SHARED_TOKEN_BASE 0x033f0000u
#define HOST_MAP_LIMIT (16u << 20)

/* Private heap: an arena reserved up front, committed in AAR_HEAP_GRANULE. */
#define HOST_ARENA_BYTES (8u << 20)

/* What the table's groups word says this runner implements. */
#define HOST_GROUPS (AAR_GROUP_DISPLAY | AAR_GROUP_INPUT | AAR_GROUP_AUDIO | \
                     AAR_GROUP_SERVICES)

struct host_options {
    const char *root;
    int scale;
    int headless;
    int abs_paths;
    uint32_t frames_limit;
    const char *input_path;
    int audio_null;
    uint32_t audio_frames;
    const char *audio_dump;
    const char *msg_script;
    const char *msg_out;
    const char *dump_front;
    int selftest;
};

struct host_options *host_options(void);
int host_sdl_video_ok(void);
int host_stop_requested(void);
void host_request_stop(void);
int host_stop_event_pending(void);

uint64_t host_time_us(void);
uint32_t host_time_ms(void);
void host_pump(void);
void host_sleep_until(uint64_t deadline);
void host_idle_slice(void);

int host_files_init(const char *root, int abs_paths);
void host_files_shutdown(void);
void host_files_bind(aar_api *api);

int host_display_init(void);
void host_display_shutdown(void);
void host_display_bind(aar_api *api);
void host_display_pump(void);
void host_display_dump(const char *path);
int host_display_selftest(void);
uint32_t host_display_flips(void);

int host_input_init(void);
void host_input_shutdown(void);
void host_input_bind(aar_api *api);
void host_input_pump(void);
int host_input_selftest(void);

int host_audio_init(void);
void host_audio_shutdown(void);
void host_audio_bind(aar_api *api);
void host_audio_pump(void);
uint32_t host_audio_frames_consumed(void);

void host_console_bind(aar_api *api);

int host_messages_init(void);
void host_messages_shutdown(void);
void host_messages_bind(aar_api *api);
void host_messages_pump(void);

void host_services_bind(aar_api *api);
int host_services_selftest(void);

void host_memory_init(void);
void host_memory_shutdown(void);
void host_memory_bind(aar_api *api);

/* Shared blocks owned by the runtime itself (file maps): charged to the shared
 * limit but not counted toward the app's blocks. */
int host_shared_alloc(uint64_t bytes, int runtime_owned, aar_shared *out);
void host_shared_free_runtime(const void *p);
uint64_t host_shared_used(void);

#endif
