/*
 * Unit checks for the freestanding flavour of libaar on the host. aar.c and
 * gl.c are compiled with AAR_FREESTANDING set, so the test also proves that
 * the wrappers need no libc, and freestanding.c is included under other names
 * so its mem* routines can be compared with the host ones. The wrappers are
 * driven through a fake table: absent and NULL slots, the size gate, the
 * coherent short cut of aar_publish and aar_acquire, and the GL table gate.
 *
 * SPDX-License-Identifier: MIT
 */
#define AAR_FREESTANDING 1
#define memcpy aarfs_memcpy
#define memmove aarfs_memmove
#define memset aarfs_memset
#define memcmp aarfs_memcmp
#include "../../libaar/src/freestanding.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#include "../../libaar/src/aar.c"
#include "../../libaar/src/gl.c"

#include <assert.h>
#include <string.h>

static int info_calls;
static int publish_calls;
static int acquire_calls;
static uint32_t info_flags;
static int clear_calls;
static uint32_t clear_mask;

static int fake_info(aar_info *out)
{
    info_calls++;
    out->flags = info_flags;
    return AAR_OK;
}

static int fake_publish(const void *p, uint64_t bytes)
{
    (void)p;
    (void)bytes;
    publish_calls++;
    return AAR_OK;
}

static int fake_acquire(const void *p, uint64_t bytes)
{
    (void)p;
    (void)bytes;
    acquire_calls++;
    return AAR_OK;
}

static int fake_msg_poll(void *msg)
{
    (void)msg;
    return 1;
}

static int fake_msg_wait(uint32_t timeout_us)
{
    (void)timeout_us;
    return AAR_OK;
}

static void fake_clear(uint32_t mask)
{
    clear_calls++;
    clear_mask = mask;
}

static aar_gl fake_gl;

static const aar_gl *fake_gl_bind(uint32_t surface_id)
{
    return surface_id == 1u ? &fake_gl : 0;
}

static void fake_gl_unbind(void)
{
}

static void reset(aar_api *api)
{
    memset(api, 0, sizeof *api);
    api->size = (uint32_t)sizeof *api;
    api->version = AAR_API_VERSION;
    info_calls = publish_calls = acquire_calls = 0;
    info_flags = 0;
    clear_calls = 0;
    clear_mask = 0;
    aar_crt0_bind(api);
}

static void test_missing_slots(void)
{
    aar_api api;
    aar_info info;
    void *base = (void *)1;
    uint64_t bytes = 1;
    uint8_t digest[32];

    reset(&api);
    /* A NULL slot is "not implemented": integers say NOSYS, others do nothing. */
    assert(aar_time_us() == 0u);
    assert(aar_should_stop() == AAR_E_NOSYS);
    assert(aar_get_info(&info) == AAR_E_NOSYS);
    assert(aar_get_heap_info(0) == AAR_E_NOSYS);
    assert(aar_heap_grow(1u << 20, 0) == AAR_E_NOSYS);
    assert(aar_file_open("x", AAR_FILE_READ) == AAR_E_NOSYS);
    assert(aar_msg_wait(0u) == AAR_E_NOSYS);
    assert(aar_map_file("x", 0, 0) == AAR_E_NOSYS);
    assert(aar_sha256(digest, 0u, digest) == AAR_E_NOSYS);
    assert(aar_ext(AAR_PLATFORM_ZZ9000, 1u) == 0);
    assert(aar_display_latest_dropped() == 0u);
    aar_heap_range(&base, &bytes);
    assert(base == 0 && bytes == 0u);
    aar_yield();
    aar_sleep_us(1u);
    aar_unmap_file(0);
    aar_log("x", 1u);

    /* No table at all behaves the same. */
    aar_crt0_bind(0);
    assert(aar_time_us() == 0u);
    assert(aar_msg_poll(digest) == AAR_E_NOSYS);
}

static void test_size_gate(void)
{
    aar_api api;
    uint8_t msg[AAR_MSG_BYTES];

    reset(&api);
    api.msg_poll = fake_msg_poll;
    api.msg_wait = fake_msg_wait;
    assert(aar_msg_poll(msg) == 1);
    assert(aar_msg_wait(0u) == AAR_OK);
    /* A table that ends before msg_wait has no such slot, set or not. */
    api.size = (uint32_t)offsetof(aar_api, msg_wait);
    assert(aar_msg_poll(msg) == 1);
    assert(aar_msg_wait(0u) == AAR_E_NOSYS);
    /* The version word is not the gate. */
    api.size = (uint32_t)sizeof api;
    api.version = 0u;
    assert(aar_msg_wait(0u) == AAR_OK);
}

static void test_publish_and_acquire(void)
{
    aar_api api;
    uint8_t byte = 0;

    /* A coherent runtime: a fence only, and info is asked once. */
    reset(&api);
    api.info = fake_info;
    api.publish = fake_publish;
    api.acquire = fake_acquire;
    info_flags = AAR_INFOF_SHARED_COHERENT;
    assert(aar_publish(&byte, 1u) == AAR_OK);
    assert(aar_acquire(&byte, 1u) == AAR_OK);
    assert(aar_publish(&byte, 1u) == AAR_OK);
    assert(publish_calls == 0 && acquire_calls == 0 && info_calls == 1);

    /* Not coherent: the table call is made every time. */
    reset(&api);
    api.info = fake_info;
    api.publish = fake_publish;
    api.acquire = fake_acquire;
    assert(aar_publish(&byte, 1u) == AAR_OK);
    assert(aar_publish(&byte, 1u) == AAR_OK);
    assert(aar_acquire(&byte, 1u) == AAR_OK);
    assert(publish_calls == 2 && acquire_calls == 1 && info_calls == 1);

    /* No info slot counts as not coherent; no publish slot is NOSYS. */
    reset(&api);
    assert(aar_publish(&byte, 1u) == AAR_E_NOSYS);
    api.publish = fake_publish;
    assert(aar_publish(&byte, 1u) == AAR_OK && publish_calls == 1);
    assert(aar_acquire(&byte, 1u) == AAR_E_NOSYS);
}

static void test_gl_gate(void)
{
    aar_api api;
    const aar_gl *gl;

    reset(&api);
    glClear(1u);
    assert(clear_calls == 0);
    assert(aar_gl_bind(1u) == 0);

    api.gl_bind = fake_gl_bind;
    api.gl_unbind = fake_gl_unbind;
    memset(&fake_gl, 0, sizeof fake_gl);
    fake_gl.size = (uint32_t)sizeof fake_gl;
    fake_gl.version = AAR_GL_VERSION_ARRAYS;
    fake_gl.Clear = fake_clear;
    assert(aar_gl_bind(2u) == 0);
    gl = aar_gl_bind(1u);
    assert(gl == &fake_gl);
    glClear(0x4000u);
    assert(clear_calls == 1 && clear_mask == 0x4000u);

    /* A slot past the table's size is a no-op, and so is a NULL one. */
    fake_gl.size = (uint32_t)offsetof(aar_gl, Clear);
    glClear(1u);
    assert(clear_calls == 1);
    fake_gl.size = (uint32_t)sizeof fake_gl;
    fake_gl.Clear = 0;
    glClear(1u);
    assert(clear_calls == 1);
    assert(glGetError() == 0u);

    /* Unbinding drops the table. */
    fake_gl.Clear = fake_clear;
    aar_gl_unbind();
    glClear(1u);
    assert(clear_calls == 1);
}

static aar_ext_zz9000 fake_ext;
static uint32_t ext_surface;
static uint32_t ext_flags;
static int ext_bind_calls;

static const aar_gl *fake_gl_bind_flags(uint32_t surface_id, uint32_t flags)
{
    ext_bind_calls++;
    ext_surface = surface_id;
    ext_flags = flags;
    return surface_id == 1u ? &fake_gl : 0;
}

static const void *fake_ext_lookup(uint32_t platform_id, uint32_t ext_id)
{
    return platform_id == AAR_PLATFORM_ZZ9000 && ext_id == AAR_EXT_ZZ9000_MAIN ?
           (const void *)&fake_ext : 0;
}

/* aar_zz9000_gl_bind_flags adopts the table of the extension's slot, so the
 * gl* wrappers reach it, and aar_gl_unbind drops it again. A bare call of
 * the slot gives the app a table but not the wrappers. */
static void test_zz9000_gl_bind_flags(void)
{
    aar_api api;

    reset(&api);
    memset(&fake_gl, 0, sizeof fake_gl);
    fake_gl.size = (uint32_t)sizeof fake_gl;
    fake_gl.version = AAR_GL_VERSION_ARRAYS;
    fake_gl.Clear = fake_clear;
    memset(&fake_ext, 0, sizeof fake_ext);
    fake_ext.size = (uint32_t)sizeof fake_ext;
    fake_ext.gl_bind_flags = fake_gl_bind_flags;
    ext_bind_calls = 0;
    api.gl_unbind = fake_gl_unbind;

    /* No ext slot: nothing to look up, so nothing to call. */
    assert(aar_zz9000_gl_bind_flags(1u, 0u) == 0);
    api.ext = fake_ext_lookup;
    assert(ext_bind_calls == 0);

    /* The slot alone does not reach the wrappers. */
    assert(fake_ext.gl_bind_flags(1u, 0u) == &fake_gl);
    glClear(1u);
    assert(clear_calls == 0);
    ext_bind_calls = 0;

    /* The wrapper passes surface and flags on and adopts the table. */
    assert(aar_zz9000_gl_bind_flags(1u, AAR_ZZ9000_GL_BIND_SYNC_PRESENT) ==
           &fake_gl);
    assert(ext_bind_calls == 1 && ext_surface == 1u &&
           ext_flags == AAR_ZZ9000_GL_BIND_SYNC_PRESENT);
    glClear(0x4000u);
    assert(clear_calls == 1 && clear_mask == 0x4000u);

    /* aar_gl_unbind drops it. */
    aar_gl_unbind();
    glClear(1u);
    assert(clear_calls == 1);

    /* A failed bind returns NULL and leaves no stale table behind. */
    assert(aar_zz9000_gl_bind_flags(1u, 0u) == &fake_gl);
    assert(aar_zz9000_gl_bind_flags(2u, 0u) == 0);
    glClear(1u);
    assert(clear_calls == 1);

    /* A table that ends before the slot, or has it NULL, binds nothing. */
    assert(aar_zz9000_gl_bind_flags(1u, 0u) == &fake_gl);
    fake_ext.size = (uint32_t)offsetof(aar_ext_zz9000, gl_bind_flags);
    ext_bind_calls = 0;
    assert(aar_zz9000_gl_bind_flags(1u, 0u) == 0 && ext_bind_calls == 0);
    glClear(1u);
    assert(clear_calls == 1);
    fake_ext.size = (uint32_t)sizeof fake_ext;
    fake_ext.gl_bind_flags = 0;
    assert(aar_zz9000_gl_bind_flags(1u, 0u) == 0);

    /* The plain aar_gl_bind keeps working next to it. */
    api.gl_bind = fake_gl_bind;
    assert(aar_gl_bind(1u) == &fake_gl);
    glClear(1u);
    assert(clear_calls == 2);
    aar_gl_unbind();
}

enum { BUF = 96 };

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; ++i)
        p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 3));
}

static void test_mem_routines(void)
{
    uint32_t backing_a[BUF / 4 + 4], backing_b[BUF / 4 + 4];
    uint8_t *a = (uint8_t *)backing_a;
    uint8_t *b = (uint8_t *)backing_b;
    uint8_t ref[BUF + 16];
    size_t off_d, off_s, n;

    for (off_d = 0; off_d < 4; ++off_d) {
        for (off_s = 0; off_s < 4; ++off_s) {
            for (n = 0; n <= 40; ++n) {
                fill(a, sizeof backing_a, 1);
                fill(b, sizeof backing_b, 2);
                memcpy(ref, a, sizeof backing_a);
                memcpy(ref + off_d, b + off_s, n);
                assert(aarfs_memcpy(a + off_d, b + off_s, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);

                /* Overlapping moves in both directions. */
                fill(a, sizeof backing_a, 3);
                memcpy(ref, a, sizeof backing_a);
                memmove(ref + off_d + 4, ref + off_s, n);
                assert(aarfs_memmove(a + off_d + 4, a + off_s, n) ==
                       a + off_d + 4);
                assert(memcmp(a, ref, sizeof backing_a) == 0);
                fill(a, sizeof backing_a, 4);
                memcpy(ref, a, sizeof backing_a);
                memmove(ref + off_d, ref + off_s + 4, n);
                assert(aarfs_memmove(a + off_d, a + off_s + 4, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);

                fill(a, sizeof backing_a, 5);
                memcpy(ref, a, sizeof backing_a);
                memset(ref + off_d, 0xa5, n);
                assert(aarfs_memset(a + off_d, 0x1a5, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);
            }
        }
    }

    fill(a, 32, 6);
    memcpy(b, a, 32);
    assert(aarfs_memcmp(a, b, 32) == 0);
    assert(aarfs_memcmp(a, b, 0) == 0);
    b[17] = (uint8_t)(a[17] + 1u);
    assert(aarfs_memcmp(a, b, 32) < 0);
    assert(aarfs_memcmp(b, a, 32) > 0);
    assert(aarfs_memcmp(a, b, 17) == 0);
    a[3] = 0xff;
    b[3] = 0x01;
    assert(aarfs_memcmp(a, b, 32) > 0);
}

int main(void)
{
    test_missing_slots();
    test_size_gate();
    test_publish_and_acquire();
    test_gl_gate();
    test_zz9000_gl_bind_flags();
    test_mem_routines();
    return 0;
}
