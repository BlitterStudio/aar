/*
 * ZZ9000 platform extension table, returned by
 *
 *     aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)
 *
 * Only the ZZ9000 runtime provides it. An app that uses it is not portable.
 * It holds what is specific to the ZZ9000 and that the standard does not
 * keep: the GL bind flags (use aar_zz9000_gl_bind_flags) and the two source
 * revision strings.
 *
 * The table is size-first and append-only. Two uint32_t words come first so
 * the first pointer is at offset 8 on both profiles. Test fields with
 * AAR_EXT_ZZ9000_COVERS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_EXT_ZZ9000_H
#define AAR_EXT_ZZ9000_H

#include <stddef.h>
#include <stdint.h>
#include "app.h"
#include "gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AAR_PLATFORM_ZZ9000 (aar/app.h) is aar_api.platform_id on this platform and
 * the first argument of aar_ext(). */

/* Second argument of aar_ext(). */
#define AAR_EXT_ZZ9000_MAIN 1u

/* gl_bind_flags() requests. All four are specific to the ZZ9000 renderer. */
#define AAR_ZZ9000_GL_BIND_DIRECT_PRESENT 1u
/* Debug: drain and copy every frame before aar_gl_swap returns. */
#define AAR_ZZ9000_GL_BIND_SYNC_PRESENT 2u
/* Debug: keep the RasterIX command feed synchronous for A/B comparison. */
#define AAR_ZZ9000_GL_BIND_SYNC_RIX_FEED 4u
/* Debug: keep the asynchronous ring but push command words inline. */
#define AAR_ZZ9000_GL_BIND_INLINE_RIX_FEED 8u

typedef struct aar_ext_zz9000 {
    uint32_t size;     /* sizeof of the table the runtime built */
    uint32_t reserved; /* 0 */
    /* Same as aar_gl_bind() plus AAR_ZZ9000_GL_BIND_* flags. NULL when the
     * GL group is absent. Call it through aar_zz9000_gl_bind_flags(), which
     * also hands the table to the gl* wrappers. */
    const aar_gl *(*gl_bind_flags)(uint32_t surface_id, uint32_t flags);
    /* Resident firmware source revision, NUL-terminated, or NULL. */
    const char *build_id;
    /* GL module source revision, NUL-terminated, or NULL. */
    const char *gl_build_id;
} aar_ext_zz9000;

#define AAR_EXT_ZZ9000_COVERS(ext, field) \
    ((ext) != 0 && (uint32_t)(ext)->size >= \
        (uint32_t)(offsetof(aar_ext_zz9000, field) + sizeof((ext)->field)))

#if defined(__cplusplus)
static_assert(offsetof(aar_ext_zz9000, gl_bind_flags) == 8,
              "ZZ9000 extension first slot at offset 8");
static_assert(sizeof(aar_ext_zz9000) == 8 + 3 * sizeof(void *),
              "ZZ9000 extension has no padding");
#else
_Static_assert(offsetof(aar_ext_zz9000, gl_bind_flags) == 8,
               "ZZ9000 extension first slot at offset 8");
_Static_assert(sizeof(aar_ext_zz9000) == 8 + 3 * sizeof(void *),
               "ZZ9000 extension has no padding");
#endif

/* The wrapper to use instead of calling the gl_bind_flags slot yourself.
 * aar_gl_bind() records the table it returns, and the gl* wrappers forward
 * to that record; a table returned by a bare call of the slot is never
 * recorded, so the wrappers would do nothing. This looks up the extension,
 * calls gl_bind_flags(surface_id, flags) and records the result the same way,
 * so the gl* wrappers and aar_gl_unbind() work as after aar_gl_bind().
 * Returns NULL, and records nothing, when the extension, its slot or the GL
 * group is absent or the bind fails. */
const aar_gl *aar_zz9000_gl_bind_flags(uint32_t surface_id, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
