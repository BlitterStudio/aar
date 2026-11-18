/*
 * aar_* wrappers. Every runtime call goes through the table in aar_api_ptr. A
 * slot is read only when the table's size covers it, and a NULL slot means
 * the runtime does not implement the call. The wrappers then return
 * AAR_E_NOSYS (or 0, NULL, nothing) as aar/app.h documents.
 *
 * This file is part of layer 1 and has no libc dependency: it is linked into
 * both the libc and the freestanding flavour.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar_internal.h"

const aar_api *aar_api_ptr;
static const aar_gl *bound_gl;

/* aar_info.flags & AAR_INFOF_SHARED_COHERENT, read once. */
#define COHERENCE_UNKNOWN 0u
#define COHERENCE_NO      1u
#define COHERENCE_YES     2u
static uint32_t shared_coherence;

void aar_crt0_bind(const aar_api *api)
{
    aar_api_ptr = api;
    bound_gl = 0;
    shared_coherence = COHERENCE_UNKNOWN;
}

const aar_gl *aar_gl_bound(void)
{
    return bound_gl;
}

void aar_exit(int code)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, exit) && api->exit)
        api->exit(code);
    for (;;)
        ;
}

void aar_yield(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, yield) && api->yield)
        api->yield();
}

int aar_should_stop(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, should_stop) && api->should_stop)
        return api->should_stop();
    return AAR_E_NOSYS;
}

uint64_t aar_time_us(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, time_us) && api->time_us)
        return api->time_us();
    return 0;
}

void aar_sleep_us(uint32_t us)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, sleep_us) && api->sleep_us)
        api->sleep_us(us);
}

int aar_get_info(aar_info *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, info) && api->info)
        return api->info(out);
    return AAR_E_NOSYS;
}

void aar_heap_range(void **base, uint64_t *bytes)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, heap_range) && api->heap_range) {
        api->heap_range(base, bytes);
        return;
    }
    if (base)
        *base = 0;
    if (bytes)
        *bytes = 0;
}

int aar_heap_grow(uint64_t bytes, void **old_break)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, heap_grow) && api->heap_grow)
        return api->heap_grow(bytes, old_break);
    return AAR_E_NOSYS;
}

int aar_heap_shrink(uint64_t bytes, void **new_break)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, heap_shrink) && api->heap_shrink)
        return api->heap_shrink(bytes, new_break);
    return AAR_E_NOSYS;
}

int aar_get_heap_info(aar_heap_info *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, heap_info) && api->heap_info)
        return api->heap_info(out);
    return AAR_E_NOSYS;
}

int aar_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                     aar_shared *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, shared_alloc) && api->shared_alloc)
        return api->shared_alloc(bytes, align, flags, out);
    return AAR_E_NOSYS;
}

int aar_shared_free(void *p)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, shared_free) && api->shared_free)
        return api->shared_free(p);
    return AAR_E_NOSYS;
}

/* Whether the runtime keeps the app and the host coherent without cache
 * maintenance (AAR_INFOF_SHARED_COHERENT). That cannot change while the app
 * runs, so it is asked once. A runtime without the info call, or one that
 * fails it, counts as not coherent: the table call is then always made. */
static int shared_is_coherent(void)
{
    if (shared_coherence == COHERENCE_UNKNOWN) {
        aar_info info;

        shared_coherence = COHERENCE_NO;
        info.size = (uint32_t)sizeof(info);
        info.flags = 0;
        if (aar_get_info(&info) == AAR_OK &&
            (info.flags & AAR_INFOF_SHARED_COHERENT) != 0u)
            shared_coherence = COHERENCE_YES;
    }
    return shared_coherence == COHERENCE_YES;
}

/* Where the runtime is coherent the call is a barrier and nothing else. */
int aar_publish(const void *p, uint64_t bytes)
{
    const aar_api *api = aar_api_ptr;

    if (shared_is_coherent()) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        return AAR_OK;
    }
    if (AAR_API_COVERS(api, publish) && api->publish)
        return api->publish(p, bytes);
    return AAR_E_NOSYS;
}

int aar_acquire(const void *p, uint64_t bytes)
{
    const aar_api *api = aar_api_ptr;

    if (shared_is_coherent()) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return AAR_OK;
    }
    if (AAR_API_COVERS(api, acquire) && api->acquire)
        return api->acquire(p, bytes);
    return AAR_E_NOSYS;
}

void aar_log(const char *msg, uint32_t len)
{
    const aar_api *api = aar_api_ptr;

    if (!msg)
        return;

    if (AAR_API_COVERS(api, log) && api->log)
        api->log(msg, len);
}

int aar_file_open(const char *path, uint32_t flags)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_open) && api->file_open)
        return api->file_open(path, flags);
    return AAR_E_NOSYS;
}

int aar_file_close(int fd)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_close) && api->file_close)
        return api->file_close(fd);
    return AAR_E_NOSYS;
}

int aar_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_read) && api->file_read)
        return api->file_read(fd, buf, len, out_n);
    return AAR_E_NOSYS;
}

int aar_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_write) && api->file_write)
        return api->file_write(fd, buf, len, out_n);
    return AAR_E_NOSYS;
}

int aar_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_seek) && api->file_seek)
        return api->file_seek(fd, off, whence, out_pos);
    return AAR_E_NOSYS;
}

int aar_file_stat(const char *path, aar_stat *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_stat) && api->file_stat)
        return api->file_stat(path, out);
    return AAR_E_NOSYS;
}

int aar_file_list_open(const char *path)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_list_open) && api->file_list_open)
        return api->file_list_open(path);
    return AAR_E_NOSYS;
}

int aar_file_list_next(int list, aar_dirent *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_list_next) && api->file_list_next)
        return api->file_list_next(list, out);
    return AAR_E_NOSYS;
}

int aar_file_list_close(int list)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, file_list_close) && api->file_list_close)
        return api->file_list_close(list);
    return AAR_E_NOSYS;
}

int aar_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, console_write) && api->console_write)
        return api->console_write(buf, len, out_n);
    return AAR_E_NOSYS;
}

int aar_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, console_read) && api->console_read)
        return api->console_read(buf, len, out_n);
    return AAR_E_NOSYS;
}

int aar_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, event_poll) && api->event_poll)
        return api->event_poll(out, max, out_n);
    return AAR_E_NOSYS;
}

int aar_msg_send(const void *msg)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, msg_send) && api->msg_send)
        return api->msg_send(msg);
    return AAR_E_NOSYS;
}

int aar_msg_poll(void *msg)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, msg_poll) && api->msg_poll)
        return api->msg_poll(msg);
    return AAR_E_NOSYS;
}

int aar_msg_wait(uint32_t timeout_us)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, msg_wait) && api->msg_wait)
        return api->msg_wait(timeout_us);
    return AAR_E_NOSYS;
}

const void *aar_ext(uint32_t platform_id, uint32_t ext_id)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, ext) && api->ext)
        return api->ext(platform_id, ext_id);
    return 0;
}

int aar_map_file(const char *path, const void **data, uint64_t *bytes)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, map_file) && api->map_file)
        return api->map_file(path, data, bytes);
    return AAR_E_NOSYS;
}

void aar_unmap_file(const void *data)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, unmap_file) && api->unmap_file)
        api->unmap_file(data);
}

int aar_display_open(uint32_t w, uint32_t h, uint32_t format, uint32_t flags,
                     aar_surface *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_open) && api->display_open)
        return api->display_open(w, h, format, flags, out);
    return AAR_E_NOSYS;
}

int aar_display_flip(uint32_t id)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_flip) && api->display_flip)
        return api->display_flip(id);
    return AAR_E_NOSYS;
}

int aar_display_flip_async(uint32_t id)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_flip_async) && api->display_flip_async)
        return api->display_flip_async(id);
    return AAR_E_NOSYS;
}

int aar_display_flip_idle(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_flip_idle) && api->display_flip_idle)
        return api->display_flip_idle();
    return AAR_E_NOSYS;
}

int aar_display_refresh(aar_surface *s)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_refresh) && api->display_refresh)
        return api->display_refresh(s);
    return AAR_E_NOSYS;
}

int aar_display_vsync(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_vsync) && api->display_vsync)
        return api->display_vsync();
    return AAR_E_NOSYS;
}

int aar_display_set_palette(uint32_t id, uint32_t first, uint32_t count,
                            const uint32_t *rgb)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_set_palette) && api->display_set_palette)
        return api->display_set_palette(id, first, count, rgb);
    return AAR_E_NOSYS;
}

int aar_display_close(uint32_t id)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_close) && api->display_close)
        return api->display_close(id);
    return AAR_E_NOSYS;
}

uint32_t aar_display_latest_dropped(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, display_latest_dropped) &&
        api->display_latest_dropped)
        return api->display_latest_dropped();
    return 0;
}

int aar_audio_open(uint32_t rate, uint32_t channels, aar_audio *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_open) && api->audio_open)
        return api->audio_open(rate, channels, out);
    return AAR_E_NOSYS;
}

int aar_audio_close(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_close) && api->audio_close)
        return api->audio_close();
    return AAR_E_NOSYS;
}

int aar_audio_get_stats(aar_audio_stats *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_stats) && api->audio_stats)
        return api->audio_stats(out);
    return AAR_E_NOSYS;
}

const aar_gl *aar_gl_bind(uint32_t surface_id)
{
    const aar_api *api = aar_api_ptr;

    bound_gl = 0;
    if (AAR_API_COVERS(api, gl_bind) && api->gl_bind)
        bound_gl = api->gl_bind(surface_id);
    return bound_gl;
}

void aar_gl_swap(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, gl_swap) && api->gl_swap)
        api->gl_swap();
}

void aar_gl_unbind(void)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, gl_unbind) && api->gl_unbind)
        api->gl_unbind();
    bound_gl = 0;
}

int aar_sha256(const void *data, uint64_t bytes, uint8_t digest[32])
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, sha256) && api->sha256)
        return api->sha256(data, bytes, digest);
    return AAR_E_NOSYS;
}

int aar_decompress(uint32_t algorithm, uint32_t flags, const void *src,
                   uint64_t src_bytes, void *dst, uint64_t dst_bytes,
                   aar_service_result *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, decompress) && api->decompress)
        return api->decompress(algorithm, flags, src, src_bytes, dst,
                               dst_bytes, out);
    return AAR_E_NOSYS;
}

int aar_image_open(const aar_image_params *params, uint32_t *session,
                   aar_image_result *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, image_open) && api->image_open)
        return api->image_open(params, session, out);
    return AAR_E_NOSYS;
}

int aar_image_decode(uint32_t session, const void *src, uint32_t src_bytes,
                     uint32_t flags, aar_image_result *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, image_decode) && api->image_decode)
        return api->image_decode(session, src, src_bytes, flags, out);
    return AAR_E_NOSYS;
}

int aar_image_close(uint32_t session)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, image_close) && api->image_close)
        return api->image_close(session);
    return AAR_E_NOSYS;
}

int aar_audio_decode_open(uint32_t codec, uint32_t *session)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_decode_open) && api->audio_decode_open)
        return api->audio_decode_open(codec, session);
    return AAR_E_NOSYS;
}

int aar_audio_decode(uint32_t session, const void *src, uint32_t src_bytes,
                     uint32_t flags, int16_t *pcm, uint32_t pcm_frames,
                     aar_audio_decode_result *out)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_decode) && api->audio_decode)
        return api->audio_decode(session, src, src_bytes, flags, pcm,
                                 pcm_frames, out);
    return AAR_E_NOSYS;
}

int aar_audio_decode_close(uint32_t session)
{
    const aar_api *api = aar_api_ptr;

    if (AAR_API_COVERS(api, audio_decode_close) && api->audio_decode_close)
        return api->audio_decode_close(session);
    return AAR_E_NOSYS;
}
