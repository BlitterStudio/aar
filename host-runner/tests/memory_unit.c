/*
 * Unit checks for the host runner's heap and shared buffers. Includes
 * memory.c directly so the block table is observable. libaar's aar.c is
 * linked, so the heap calls are tried through the wrappers too.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/memory.c"
#include "aar_internal.h"

#include <assert.h>

static void test_shared_alignment_and_token(void)
{
    aar_api api;
    aar_shared blk;
    void *p = 0;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    assert(api.shared_alloc(4096u, 4096u, 0u, &blk) == AAR_OK);
    /* malloc does not honour 4096; the returned pointer must. */
    assert(((uintptr_t)blk.ptr & 4095u) == 0u);
    assert((blk.haddr & 4095u) == 0u);
    assert(blk.bytes == 4096u);
    assert((blk.flags & AAR_SHAREDF_COHERENT) != 0u);
    assert(aar_host_shared_resolve(blk.haddr, 0u, 4096u, &p) == AAR_OK);
    assert(p == blk.ptr);
    assert(api.shared_free(blk.ptr) == AAR_OK);
    assert(aar_host_shared_resolve(blk.haddr, 0u, 1u, &p) == AAR_E_INVAL);
    host_memory_shutdown();
}

/* Tokens do not overlap, are reused after a free, and stay below 2^32. */
static void test_tokens(void)
{
    aar_api api;
    aar_shared a, b, c;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    assert(api.shared_alloc(5000u, 0u, 0u, &a) == AAR_OK);
    assert(a.bytes == 8192u);
    assert(api.shared_alloc(1u, 0u, 0u, &b) == AAR_OK);
    assert(b.haddr >= a.haddr + a.bytes || a.haddr >= b.haddr + b.bytes);
    assert(api.shared_free(a.ptr) == AAR_OK);
    assert(api.shared_alloc(8192u, 0u, 0u, &c) == AAR_OK);
    assert(c.haddr == a.haddr); /* the lowest free range is reused */
    assert(api.shared_free(c.ptr) == AAR_OK);
    assert(api.shared_free(b.ptr) == AAR_OK);
    assert(host_shared_used() == 0u);
    host_memory_shutdown();
}

/* Runtime-owned blocks are charged to the limit but are not app blocks. */
static void test_runtime_blocks_are_not_app_blocks(void)
{
    aar_api api;
    aar_shared app[HOST_SHARED_APP_BLOCKS];
    aar_shared run;
    uint32_t i;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    for (i = 0; i < HOST_SHARED_APP_BLOCKS; i++)
        assert(api.shared_alloc(1u, 0u, 0u, &app[i]) == AAR_OK);
    assert(api.shared_alloc(1u, 0u, 0u, &run) == AAR_E_NOMEM);
    assert(host_shared_alloc(100u, 1, &run) == AAR_OK);
    assert(host_shared_used() == (uint64_t)(HOST_SHARED_APP_BLOCKS + 1u) * 4096u);
    /* An app cannot free a runtime block. */
    assert(api.shared_free(run.ptr) == AAR_E_INVAL);
    host_shared_free_runtime(run.ptr);
    for (i = 0; i < HOST_SHARED_APP_BLOCKS; i++)
        assert(api.shared_free(app[i].ptr) == AAR_OK);
    assert(host_shared_used() == 0u);
    host_memory_shutdown();
}

static void test_private_allocator(void)
{
    void *p;

    p = aar_alloc(1u, 0u);
    assert(p != NULL && ((uintptr_t)p & 15u) == 0u);
    aar_free(p);
    p = aar_alloc(7u, 256u);
    assert(p != NULL && ((uintptr_t)p & 255u) == 0u);
    ((uint8_t *)p)[6] = 1u;
    aar_free(p);
    assert(aar_alloc(8u, 24u) == NULL);
    assert(aar_alloc(UINT64_MAX, 8u) == NULL);
    aar_free(NULL);
}

static void test_dynamic_heap_through_table(void)
{
    aar_api api;
    aar_heap_info info;
    void *range = 0;
    void *old_break = 0;
    void *second = 0;
    void *new_break = 0;
    uint64_t range_bytes = 0;
    uint64_t committed;
    uint8_t *grown;
    uint8_t *reported_end;
    int (*saved_shrink)(uint64_t, void **);

    memset(&api, 0, sizeof api);
    api.size = (uint32_t)sizeof api;
    host_memory_init();
    host_memory_bind(&api);
    aar_crt0_bind(&api);

    assert(api.heap_grow != NULL);
    assert(api.heap_shrink != NULL);
    assert(api.heap_info != NULL);

    api.heap_range(&range, &range_bytes);
    assert(range != NULL);
    assert(range_bytes == 0u);
    reported_end = range;
    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == range_bytes);
    assert(info.limit >= (2u << 20));
    assert(info.pool_free == info.limit);
    assert(api.heap_info(NULL) == AAR_E_INVAL);

    assert(api.heap_grow(0u, &old_break) == AAR_E_INVAL);
    assert(api.heap_grow(4096u, &old_break) == AAR_E_INVAL);
    assert(aar_heap_grow(0u, &old_break) == AAR_E_INVAL);

    assert(api.heap_grow(1u << 20, &old_break) == AAR_OK);
    assert(old_break == reported_end);
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == (1u << 20));
    grown = old_break;
    /* New memory is zeroed. */
    assert(grown[0] == 0u && grown[(1u << 20) - 1u] == 0u);
    grown[0] = 0x5a;
    grown[(1u << 20) - 1u] = 0xa5;

    reported_end = (uint8_t *)range + range_bytes;
    assert(aar_heap_grow(1u << 20, &second) == AAR_OK);
    assert(second == reported_end);
    assert(grown[0] == 0x5a);
    assert(grown[(1u << 20) - 1u] == 0xa5);

    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == (2u << 20));
    assert(info.pool_free == info.limit - info.committed);
    while (info.committed + (1u << 20) <= info.limit) {
        assert(api.heap_grow(1u << 20, NULL) == AAR_OK);
        assert(api.heap_info(&info) == AAR_OK);
    }
    committed = info.committed;
    assert(committed == info.limit);
    assert(info.pool_free == 0u);
    assert(api.heap_grow(1u << 20, &old_break) == AAR_E_NOMEM);
    assert(aar_heap_grow(1u << 20, &old_break) == AAR_E_NOMEM);
    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == committed);
    assert(grown[0] == 0x5a);

    assert(aar_heap_shrink(1u << 20, &new_break) == AAR_OK);
    assert(new_break == (uint8_t *)range + committed - (1u << 20));
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == committed - (1u << 20));
    reported_end = (uint8_t *)range + range_bytes;
    assert(api.heap_shrink(0u, &new_break) == AAR_E_INVAL);
    assert(api.heap_shrink(4096u, &new_break) == AAR_E_INVAL);
    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == range_bytes);
    assert(api.heap_shrink(info.committed + (1u << 20), NULL) == AAR_E_INVAL);
    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == range_bytes);
    assert(api.heap_grow(1u << 20, &old_break) == AAR_OK);
    assert(old_break == reported_end);
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == committed);
    /* A granule given back and committed again must come back zeroed. */
    ((uint8_t *)old_break)[0] = 0x77;
    ((uint8_t *)old_break)[(1u << 20) - 1] = 0x77;
    assert(api.heap_shrink(1u << 20, NULL) == AAR_OK);
    assert(api.heap_grow(1u << 20, &old_break) == AAR_OK);
    assert(((uint8_t *)old_break)[0] == 0);
    assert(((uint8_t *)old_break)[(1u << 20) - 1] == 0);

    api.size = (uint32_t)offsetof(aar_api, heap_grow);
    assert(aar_heap_grow(1u << 20, &old_break) == AAR_E_NOSYS);
    assert(aar_heap_shrink(1u << 20, &new_break) == AAR_E_NOSYS);
    api.size = (uint32_t)sizeof api;
    saved_shrink = api.heap_shrink;
    api.heap_shrink = 0;
    assert(aar_heap_shrink(1u << 20, &new_break) == AAR_E_NOSYS);
    api.heap_shrink = saved_shrink;
    assert(api.heap_info(&info) == AAR_OK);
    assert(info.committed == committed);

    host_memory_shutdown();
}

static unsigned low_count;

static void count_low(void)
{
    low_count++;
}

/* The pool is the 8 MiB arena. The low-water mark is 1 MiB of free pool and the
 * high-water mark 2 MiB (spec 4.1): the event comes once when the free pool
 * falls below the low mark, and again only after it has risen above the high
 * mark. */
static void test_low_memory_crossings(void)
{
    aar_api api;
    void *brk = 0;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    host_memory_set_low_notify(count_low);
    low_count = 0;

    assert(api.heap_grow(7u << 20, &brk) == AAR_OK);   /* 1 MiB free: not below */
    assert(low_count == 0u);
    assert(api.heap_grow(1u << 20, &brk) == AAR_OK);   /* empty pool */
    assert(low_count == 1u);
    assert(api.heap_grow(1u << 20, &brk) == AAR_E_NOMEM);
    assert(low_count == 1u);
    assert(api.heap_shrink(1u << 20, &brk) == AAR_OK); /* 1 MiB free: not above high */
    assert(api.heap_grow(1u << 20, &brk) == AAR_OK);
    assert(low_count == 1u);                           /* the same crossing */
    assert(api.heap_shrink(8u << 20, &brk) == AAR_OK); /* everything free */
    assert(api.heap_grow(8u << 20, &brk) == AAR_OK);
    assert(low_count == 2u);
    assert(api.heap_shrink(8u << 20, &brk) == AAR_OK);
    host_memory_set_low_notify(0);
    host_memory_shutdown();
}

int main(void)
{
    test_shared_alignment_and_token();
    test_tokens();
    test_runtime_blocks_are_not_app_blocks();
    test_private_allocator();
    test_dynamic_heap_through_table();
    test_low_memory_crossings();
    return 0;
}
