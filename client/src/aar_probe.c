/*
 * The image probe. See aar_probe.h. The checks are the image rules of the AAR
 * standard in loader/aar_image.c, applied as the loader applies them: the
 * section headers are neither required nor read, so an image without them is
 * accepted. The probe reports the note in the structure of aar.library and does
 * not load anything. client/tests/probe_corpus.py runs it over the corpus of
 * tests/aar_check_test.py and over mutations of it, against the rules of
 * tools/aar-check that do not read section headers.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar_probe.h"
#include "../../loader/aar_image.h"

/* The caller's 32-bit reader or a block of memory, behind aar_image's reader. */
struct source {
    aar_probe_read_fn fn;
    void *user;
    const uint8_t *mem;
};

static int source_read(void *user, uint64_t offset, void *dst, uint32_t count)
{
    struct source *s = (struct source *)user;

    if (!s->fn) {
        memcpy(dst, s->mem + (size_t)offset, count);
        return AAR_OK;
    }
    /* aar_image only asks for bytes inside the length, below 4 GiB here. */
    return s->fn(s->user, (uint32_t)offset, dst, count);
}

/* Runs every check and, on success, copies the note into the caller's
 * structure, bounded by its size. */
static int run(struct source *src, uint32_t length, AARImageInfo *info,
               const char **why)
{
    struct aar_image_info image;
    AARImageInfo full;
    ULONG keep;
    int rc;

    if (why)
        *why = NULL;
    if (!info || info->size < 4)
        return AAR_E_INVAL;
    rc = aar_image_check(source_read, src, length, 0, &image, why);
    if (rc != AAR_OK) {
        /* The loader's rules, one code: every refusal is AAR_E_INVAL, including
         * a note whose profile word is not the ELF class (the loader says
         * AAR_E_PROFILE there); a failing reader is AAR_E_IO. The platform's
         * profiles are not consulted, so the probe never says AAR_E_PROFILE. */
        return rc == AAR_E_IO ? AAR_E_IO : AAR_E_INVAL;
    }
    memset(&full, 0, sizeof(full));
    full.size = sizeof(full);
    full.profile = image.profile;
    full.api_major = image.api_major;
    full.api_minor_required = image.api_minor_required;
    full.stack_bytes = image.stack_bytes;
    full.min_heap_mib = image.min_heap_mib;
    full.groups_required = image.groups_required;
    full.groups_optional = image.groups_optional;
    full.note_version = image.note_version;
    full.image_bytes = (ULONG)length;
    keep = info->size;
    memcpy(info, &full, keep < sizeof(full) ? keep : sizeof(full));
    info->size = keep;
    return AAR_OK;
}

int aar_probe_source(aar_probe_read_fn read, void *user, uint32_t length,
                     AARImageInfo *info, const char **why)
{
    struct source src;

    if (!read) {
        if (why)
            *why = NULL;
        return AAR_E_INVAL;
    }
    src.fn = read;
    src.user = user;
    src.mem = NULL;
    return run(&src, length, info, why);
}

int aar_probe_memory_why(const void *data, uint32_t length, AARImageInfo *info,
                         const char **why)
{
    struct source src;

    if (!data && length) {
        if (why)
            *why = NULL;
        return AAR_E_INVAL;
    }
    src.fn = NULL;
    src.user = NULL;
    src.mem = (const uint8_t *)data;
    return run(&src, length, info, why);
}

int aar_probe_memory(const void *data, uint32_t length, AARImageInfo *info)
{
    return aar_probe_memory_why(data, length, info, NULL);
}
