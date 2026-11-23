/*
 * Shared by the loader host tests: check counting, a reader over memory that
 * checks how it is used, and file helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_LOADER_TEST_UTIL_H
#define AAR_LOADER_TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_load.h"

static unsigned long checks;
static unsigned long failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

/* A reader over `len` bytes at `data`. It records a read outside the image or
 * of an unreasonable size (`bad`), counts its calls, and can fail from the
 * call number `fail_from` on (0: never). Reads of plan are at most 512 bytes:
 * `max_count` is that for plan, 0 for place. */
struct mem {
    const uint8_t *data;
    uint64_t len;
    unsigned long calls;
    unsigned long fail_from;
    uint32_t max_count;
    int bad;
};

static inline int mem_read(void *user, uint64_t offset, void *dst, uint32_t count)
{
    struct mem *m = (struct mem *)user;

    m->calls++;
    if (m->fail_from && m->calls >= m->fail_from)
        return AAR_E_IO;
    if (count == 0 || offset > m->len || count > m->len - offset ||
        (m->max_count && count > m->max_count)) {
        m->bad = 1;
        return AAR_E_IO;
    }
    memcpy(dst, m->data + offset, count);
    return AAR_OK;
}

static inline void mem_init(struct mem *m, const uint8_t *data, uint64_t len, uint32_t max_count)
{
    memset(m, 0, sizeof(*m));
    m->data = data;
    m->len = len;
    m->max_count = max_count;
}

/* The whole file in a heap block of exactly its size (so that a read past the
 * end is an error for the address sanitizer). NULL when it cannot be read. */
static inline uint8_t *read_whole(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    data = (uint8_t *)malloc(size ? (size_t)size : 1);
    if (!data || fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *len = (size_t)size;
    return data;
}

/* A copy of `data` in a heap block of exactly `len` bytes. */
static inline uint8_t *copy_exact(const uint8_t *data, size_t len)
{
    uint8_t *p = (uint8_t *)malloc(len ? len : 1);

    if (p && len)
        memcpy(p, data, len);
    return p;
}

static inline int finish(const char *name)
{
    if (failures) {
        printf("%s: %lu of %lu checks FAILED\n", name, failures, checks);
        return 1;
    }
    printf("%s: %lu checks passed\n", name, checks);
    return 0;
}

#endif /* AAR_LOADER_TEST_UTIL_H */
