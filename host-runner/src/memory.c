/*
 * Host stand-ins for the private heap and for shared buffers, plus the
 * libc-flavor private allocator.
 *
 * Private heap: an 8 MiB arena reserved at start. heap_range reports the
 * committed span; heap_grow and heap_shrink commit and release whole
 * AAR_HEAP_GRANULE frames inside it, up to its limit. The span never moves.
 * An app that calls malloc runs on the host's own allocator; the arena is for
 * freestanding code that grows the heap through the table.
 *
 * Shared buffers: ordinary process memory under the default 16 MiB shared
 * limit. A block has a synthetic token (aar_shared.haddr), allocated from a
 * 32-bit space starting at 0x033f0000. The token names the whole block; it is
 * not an address in this process. aar_host_shared_resolve() maps a token back
 * to the memory, as the client side of a real platform does. The host is
 * coherent: publish and acquire only issue a fence.
 *
 * aar_alloc and aar_free are layer 2 of libaar on a target, over newlib's
 * malloc. The host has no syscalls.c, so they live here, over the host malloc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"
#include "aar_host.h"

#include <stdlib.h>
#include <string.h>

struct host_shared {
    void *base;        /* malloc block; ptr is aligned into it */
    void *ptr;
    uint64_t bytes;    /* granted, a multiple of the granule */
    uint32_t token;
    int runtime;       /* owned by the runtime (file map), not an app block */
};

#define HOST_SHARED_SLOTS (HOST_SHARED_APP_BLOCKS + HOST_SHARED_MAP_BLOCKS)

static uint8_t *arena;
static struct host_shared shared[HOST_SHARED_SLOTS];
static uint64_t shared_used;
static uint32_t app_blocks;
static uint64_t heap_committed;

static int whole_granule(uint64_t bytes)
{
    return bytes != 0u && (bytes & (AAR_HEAP_GRANULE - 1u)) == 0u;
}

static int pow2(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

/* Lowest token at or above the base, aligned, that overlaps no live block. */
static int pick_token(uint64_t bytes, uint32_t align, uint32_t *out)
{
    uint64_t cand = HOST_SHARED_TOKEN_BASE;
    uint32_t i;

    for (;;) {
        int clash = 0;

        cand = (cand + align - 1u) & ~(uint64_t)(align - 1u);
        if (cand + bytes > 0x100000000ull)
            return 0;
        for (i = 0; i < HOST_SHARED_SLOTS; i++) {
            if (!shared[i].ptr)
                continue;
            if (cand < (uint64_t)shared[i].token + shared[i].bytes &&
                (uint64_t)shared[i].token < cand + bytes) {
                cand = (uint64_t)shared[i].token + shared[i].bytes;
                clash = 1;
                break;
            }
        }
        if (!clash) {
            *out = (uint32_t)cand;
            return 1;
        }
    }
}

static int alloc_block(uint64_t bytes, uint32_t align, int runtime,
                       aar_shared *out)
{
    uint32_t i;
    uint32_t token;
    uint8_t *raw;
    uintptr_t p;

    if (bytes > HOST_SHARED_LIMIT)
        return AAR_E_NOMEM;
    bytes = (bytes + HOST_SHARED_GRANULE - 1u) & ~(uint64_t)(HOST_SHARED_GRANULE - 1u);
    if (bytes > HOST_SHARED_LIMIT - shared_used)
        return AAR_E_NOMEM;
    if (!runtime && app_blocks >= HOST_SHARED_APP_BLOCKS)
        return AAR_E_NOMEM;
    for (i = 0; i < HOST_SHARED_SLOTS; i++) {
        if (!shared[i].ptr)
            break;
    }
    if (i == HOST_SHARED_SLOTS)
        return AAR_E_NOMEM;
    if (!pick_token(bytes, align, &token))
        return AAR_E_NOMEM;
    /* malloc is not aligned past max_align_t; keep the base pointer so free
     * releases the real block. */
    raw = (uint8_t *)malloc((size_t)bytes + align);
    if (!raw)
        return AAR_E_NOMEM;
    p = ((uintptr_t)raw + align - 1u) & ~(uintptr_t)(align - 1u);
    memset((void *)p, 0, (size_t)bytes);
    shared[i].base = raw;
    shared[i].ptr = (void *)p;
    shared[i].bytes = bytes;
    shared[i].token = token;
    shared[i].runtime = runtime;
    shared_used += bytes;
    if (!runtime)
        app_blocks++;
    out->bytes = bytes;
    out->haddr = token;
    out->flags = AAR_SHAREDF_COHERENT;
    out->ptr = (void *)p;
    return AAR_OK;
}

static void free_slot(uint32_t i)
{
    shared_used -= shared[i].bytes;
    if (!shared[i].runtime)
        app_blocks--;
    free(shared[i].base);
    memset(&shared[i], 0, sizeof shared[i]);
}

int host_shared_alloc(uint64_t bytes, int runtime_owned, aar_shared *out)
{
    if (bytes == 0u)
        return AAR_E_INVAL;
    return alloc_block(bytes, HOST_SHARED_GRANULE, runtime_owned, out);
}

void host_shared_free_runtime(const void *p)
{
    uint32_t i;

    for (i = 0; p && i < HOST_SHARED_SLOTS; i++) {
        if (shared[i].ptr == p && shared[i].runtime) {
            free_slot(i);
            return;
        }
    }
}

uint64_t host_shared_used(void)
{
    return shared_used;
}

int aar_host_shared_resolve(uint32_t haddr, uint64_t offset, uint64_t bytes,
                            void **out)
{
    uint32_t i;

    if (!out)
        return AAR_E_INVAL;
    for (i = 0; i < HOST_SHARED_SLOTS; i++) {
        if (shared[i].ptr && shared[i].token == haddr) {
            if (offset > shared[i].bytes || bytes > shared[i].bytes - offset)
                return AAR_E_INVAL;
            *out = (uint8_t *)shared[i].ptr + offset;
            return AAR_OK;
        }
    }
    return AAR_E_INVAL;
}

static int api_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                            aar_shared *out)
{
    if (flags != 0u || bytes == 0u || !out)
        return AAR_E_INVAL;
    /* The alignment rounds up to a whole granule and must then be a power of
     * two; 0 asks for the granule itself. */
    if (align == 0u) {
        align = HOST_SHARED_GRANULE;
    } else {
        if (align > 0x80000000u - HOST_SHARED_GRANULE)
            return AAR_E_INVAL;
        align = (align + HOST_SHARED_GRANULE - 1u) &
                ~(uint32_t)(HOST_SHARED_GRANULE - 1u);
        if (!pow2(align))
            return AAR_E_INVAL;
    }
    return alloc_block(bytes, align, 0, out);
}

static int api_shared_free(void *p)
{
    uint32_t i;

    if (!p)
        return AAR_E_INVAL;
    for (i = 0; i < HOST_SHARED_SLOTS; i++) {
        if (shared[i].ptr == p && !shared[i].runtime) {
            free_slot(i);
            return AAR_OK;
        }
    }
    return AAR_E_INVAL;
}

/* The host is coherent: both calls are ordering only. */
static int api_publish(const void *p, uint64_t bytes)
{
    (void)p;
    (void)bytes;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return AAR_OK;
}

static int api_acquire(const void *p, uint64_t bytes)
{
    (void)p;
    (void)bytes;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return AAR_OK;
}

static void api_heap_range(void **base, uint64_t *bytes)
{
    if (base)
        *base = arena;
    if (bytes)
        *bytes = arena ? heap_committed : 0u;
}

static int api_heap_grow(uint64_t bytes, void **old_break)
{
    if (!arena)
        return AAR_E_NOMEM;
    if (!whole_granule(bytes))
        return AAR_E_INVAL;
    if (bytes > HOST_ARENA_BYTES - heap_committed)
        return AAR_E_NOMEM;
    if (old_break)
        *old_break = arena + heap_committed;
    /* New memory is zeroed (app.h): a span shrunk earlier still holds the
     * app's old bytes. */
    memset(arena + heap_committed, 0, (size_t)bytes);
    heap_committed += bytes;
    return AAR_OK;
}

static int api_heap_shrink(uint64_t bytes, void **new_break)
{
    if (!whole_granule(bytes) || bytes > heap_committed)
        return AAR_E_INVAL;
    heap_committed -= bytes;
    if (new_break)
        *new_break = arena ? arena + heap_committed : 0;
    return AAR_OK;
}

static int api_heap_info(aar_heap_info *out)
{
    if (!out)
        return AAR_E_INVAL;
    out->committed = heap_committed;
    out->limit = arena ? HOST_ARENA_BYTES : 0u;
    out->pool_free = arena ? HOST_ARENA_BYTES - heap_committed : 0u;
    return AAR_OK;
}

/* Private allocator, libc flavor. The block carries its malloc pointer just
 * below the aligned address, so any power-of-two alignment works. */
void *aar_alloc(uint64_t bytes, uint32_t align)
{
    uint8_t *raw;
    uintptr_t p;

    if (align == 0u)
        align = 16u;
    if (!pow2(align))
        return NULL;
    if (bytes > (uint64_t)SIZE_MAX - align - sizeof(void *))
        return NULL;
    raw = (uint8_t *)malloc((size_t)bytes + align + sizeof(void *));
    if (!raw)
        return NULL;
    p = ((uintptr_t)raw + sizeof(void *) + align - 1u) &
        ~(uintptr_t)(align - 1u);
    ((void **)p)[-1] = raw;
    return (void *)p;
}

void aar_free(void *p)
{
    if (p)
        free(((void **)p)[-1]);
}

void host_memory_init(void)
{
    arena = (uint8_t *)malloc(HOST_ARENA_BYTES);
    shared_used = 0;
    app_blocks = 0;
    heap_committed = 0;
    memset(shared, 0, sizeof shared);
    if (arena)
        memset(arena, 0, HOST_ARENA_BYTES);
}

void host_memory_shutdown(void)
{
    uint32_t i;

    for (i = 0; i < HOST_SHARED_SLOTS; i++) {
        free(shared[i].base);
        memset(&shared[i], 0, sizeof shared[i]);
    }
    free(arena);
    arena = 0;
    shared_used = 0;
    app_blocks = 0;
    heap_committed = 0;
}

void host_memory_bind(aar_api *api)
{
    api->heap_range = api_heap_range;
    api->heap_grow = api_heap_grow;
    api->heap_shrink = api_heap_shrink;
    api->heap_info = api_heap_info;
    api->shared_alloc = api_shared_alloc;
    api->shared_free = api_shared_free;
    api->publish = api_publish;
    api->acquire = api_acquire;
}
