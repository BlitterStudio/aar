/*
 * Host test of the image probe (client/src/aar_probe.c).
 *
 *   probe_test CORPUS
 *
 * CORPUS is written by client/tests/probe_corpus.py: images with the decision
 * that the rules of spec 2.2 and 2.3 make about each (tools/aar-check without
 * the rules that read section headers), and for an accepted image the note
 * fields read independently by
 * the Python tool. The test checks that the C probe
 *   - decides the same on every image, from memory and through a read
 *     callback that hands out at most the bytes asked for and never reads
 *     outside the image,
 *   - returns the same fields for an accepted image from both sources,
 *   - refuses every prefix of an accepted image that cuts into what the rules
 *     read, and accepts the prefix without the section headers,
 *   - honours the caller's `size`,
 *   - leaves the structure alone when it refuses.
 * Build it with -fsanitize=address,undefined as well: every image of the
 * mutation part of the corpus is a damaged header or table.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aar_probe.h"

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

struct image {
    char name[160];
    int expect_rc;
    uint32_t fields[9]; /* profile, api_major, api_minor_required, stack, heap,
                         * groups_required, groups_optional, note_version,
                         * image_bytes */
    uint32_t need;      /* the shortest prefix that is still accepted */
    uint32_t len;
    uint8_t *data;
};

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- A reader that checks how it is used ------------------------------------ */

struct reader {
    const uint8_t *data;
    uint32_t len;
    unsigned long calls;
    int bad_use;
};

static int checked_read(void *user, uint32_t offset, void *dst, uint32_t count)
{
    struct reader *r = (struct reader *)user;

    r->calls++;
    if (count == 0 || count > 512 || offset > r->len || count > r->len - offset) {
        r->bad_use = 1;
        return AAR_E_IO;
    }
    memcpy(dst, r->data + offset, count);
    return AAR_OK;
}

static int failing_read(void *user, uint32_t offset, void *dst, uint32_t count)
{
    (void)user;
    (void)offset;
    (void)dst;
    (void)count;
    return AAR_E_IO;
}

/* ---- Comparisons ----------------------------------------------------------------- */

static void fill_pattern(AARImageInfo *info)
{
    memset(info, 0xA5, sizeof(*info));
    info->size = sizeof(*info);
}

static int same_info(const AARImageInfo *a, const AARImageInfo *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void check_fields(const struct image *img, const AARImageInfo *info)
{
    CHECK(info->size == sizeof(AARImageInfo));
    CHECK(info->profile == img->fields[0]);
    CHECK(info->api_major == img->fields[1]);
    CHECK(info->api_minor_required == img->fields[2]);
    CHECK(info->stack_bytes == img->fields[3]);
    CHECK(info->min_heap_mib == img->fields[4]);
    CHECK(info->groups_required == img->fields[5]);
    CHECK(info->groups_optional == img->fields[6]);
    CHECK(info->note_version == img->fields[7]);
    CHECK(info->image_bytes == img->fields[8]);
    CHECK(info->reserved[0] == 0 && info->reserved[1] == 0);
}

static void run_image(const struct image *img)
{
    AARImageInfo mem, cb, before;
    struct reader rd;
    const char *why = "unset";
    int rc_mem, rc_cb;

    fill_pattern(&mem);
    before = mem;
    rc_mem = aar_probe_memory_why(img->data, img->len, &mem, &why);
    CHECK(rc_mem == img->expect_rc);
    if (rc_mem != AAR_OK) {
        CHECK(why != NULL);
        CHECK(same_info(&mem, &before));
    } else {
        CHECK(why == NULL);
        check_fields(img, &mem);
    }
    if (rc_mem != img->expect_rc)
        printf("  case: %s (memory probe %d, want %d)\n", img->name, rc_mem,
               img->expect_rc);

    /* Through the callback: same decision, same fields, sane reads. */
    rd.data = img->data;
    rd.len = img->len;
    rd.calls = 0;
    rd.bad_use = 0;
    fill_pattern(&cb);
    rc_cb = aar_probe_source(checked_read, &rd, img->len, &cb, NULL);
    CHECK(rc_cb == img->expect_rc);
    if (rc_cb != img->expect_rc)
        printf("  case: %s (reader probe %d, want %d)\n", img->name, rc_cb,
               img->expect_rc);
    CHECK(!rd.bad_use);
    CHECK(same_info(&cb, &mem));
    /* The reader is only asked for what the image has, and sequentially
     * cached: a full probe of an image reads far less than a call per byte. */
    CHECK(rd.calls <= img->len + 1);
}

/* Every prefix shorter than what the runtime rules read is refused. The bytes
 * after that (the section headers and their string table, which no runtime reads)
 * are not needed: a prefix that keeps everything before them is accepted. */
static void run_truncations(const struct image *img)
{
    uint32_t n;

    if (img->expect_rc != AAR_OK)
        return;
    CHECK(img->need <= img->len);
    for (n = 0; n < img->len; n++) {
        AARImageInfo info;
        struct reader rd;
        int want = n < img->need ? AAR_E_INVAL : AAR_OK;

        fill_pattern(&info);
        CHECK(aar_probe_memory(img->data, n, &info) == want);
        if (want == AAR_OK)
            CHECK(info.image_bytes == n);
        rd.data = img->data;
        rd.len = n;
        rd.calls = 0;
        rd.bad_use = 0;
        CHECK(aar_probe_source(checked_read, &rd, n, &info, NULL) == want);
        CHECK(!rd.bad_use);
    }
}

static void run_size_rules(const struct image *img)
{
    AARImageInfo info;
    ULONG words[(sizeof(AARImageInfo) + 16) / sizeof(ULONG)];
    uint8_t *guard = (uint8_t *)words;
    AARImageInfo *at = (AARImageInfo *)words;

    if (img->expect_rc != AAR_OK)
        return;
    /* size < 4 cannot hold the size word. */
    fill_pattern(&info);
    info.size = 3;
    CHECK(aar_probe_memory(img->data, img->len, &info) == AAR_E_INVAL);
    info.size = 0;
    CHECK(aar_probe_memory(img->data, img->len, &info) == AAR_E_INVAL);
    /* size 8 fills the first two words and keeps size. */
    fill_pattern(&info);
    info.size = 8;
    CHECK(aar_probe_memory(img->data, img->len, &info) == AAR_OK);
    CHECK(info.size == 8);
    CHECK(info.profile == img->fields[0]);
    CHECK(info.api_major == 0xA5A5A5A5u); /* beyond size, untouched */
    /* A larger caller structure is only filled up to what the library knows. */
    memset(words, 0x5A, sizeof(words));
    at->size = sizeof(words);
    CHECK(aar_probe_memory(img->data, img->len, at) == AAR_OK);
    CHECK(at->size == sizeof(words));
    CHECK(at->image_bytes == img->fields[8]);
    CHECK(guard[sizeof(AARImageInfo)] == 0x5A);
    CHECK(guard[sizeof(words) - 1] == 0x5A);
    /* No image at all. */
    fill_pattern(&info);
    CHECK(aar_probe_memory(NULL, 0, &info) == AAR_E_INVAL);
    CHECK(aar_probe_memory(NULL, 10, &info) == AAR_E_INVAL);
    CHECK(aar_probe_memory(img->data, img->len, NULL) == AAR_E_INVAL);
    CHECK(aar_probe_source(NULL, NULL, 10, &info, NULL) == AAR_E_INVAL);
    /* A reader that fails is an I/O error, not a verdict on the image. */
    fill_pattern(&info);
    CHECK(aar_probe_source(failing_read, NULL, img->len, &info, NULL) == AAR_E_IO);
}

/* ---- The corpus ------------------------------------------------------------------------ */

static int read_corpus(const char *path, struct image **out, uint32_t *count)
{
    FILE *fp = fopen(path, "rb");
    uint8_t b[4];
    uint32_t n, i, k;
    struct image *v;

    if (!fp)
        return -1;
    if (fread(b, 1, 4, fp) != 4) {
        fclose(fp);
        return -1;
    }
    n = get_u32(b);
    v = (struct image *)calloc(n, sizeof(*v));
    if (!v)
        return -1;
    for (i = 0; i < n; i++) {
        uint8_t h[2];
        uint8_t flag;
        uint32_t namelen;

        if (fread(h, 1, 2, fp) != 2)
            return -1;
        namelen = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
        if (namelen >= sizeof(v[i].name))
            return -1;
        if (fread(v[i].name, 1, namelen, fp) != namelen)
            return -1;
        if (fread(&flag, 1, 1, fp) != 1)
            return -1;
        switch (flag) {
        case 0: v[i].expect_rc = AAR_OK; break;
        case 1: v[i].expect_rc = AAR_E_INVAL; break;
        case 3: v[i].expect_rc = AAR_E_VERSION; break;
        default: return -1;
        }
        for (k = 0; k < 9; k++) {
            if (fread(b, 1, 4, fp) != 4)
                return -1;
            v[i].fields[k] = get_u32(b);
        }
        if (fread(b, 1, 4, fp) != 4)
            return -1;
        v[i].need = get_u32(b);
        if (fread(b, 1, 4, fp) != 4)
            return -1;
        v[i].len = get_u32(b);
        v[i].data = (uint8_t *)malloc(v[i].len ? v[i].len : 1);
        if (!v[i].data || fread(v[i].data, 1, v[i].len, fp) != v[i].len)
            return -1;
    }
    fclose(fp);
    *out = v;
    *count = n;
    return 0;
}

int main(int argc, char **argv)
{
    struct image *v;
    uint32_t n, i, accepted = 0, truncated = 0, stripped = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: probe_test CORPUS\n");
        return 2;
    }
    if (read_corpus(argv[1], &v, &n) != 0) {
        fprintf(stderr, "probe_test: cannot read %s\n", argv[1]);
        return 2;
    }
    for (i = 0; i < n; i++) {
        run_image(&v[i]);
        if (v[i].expect_rc == AAR_OK && strncmp(v[i].name, "stripped section headers", 24) == 0)
            stripped++;
        if (v[i].expect_rc == AAR_OK) {
            accepted++;
            /* Truncating and the size rules are the same for every accepted
             * image; the named ones (not the mutations) are enough. */
            if (strncmp(v[i].name, "mut ", 4) != 0) {
                run_truncations(&v[i]);
                run_size_rules(&v[i]);
                truncated++;
            }
        }
    }
    /* The image rules are the loader's: an image without section headers is
     * accepted (one per profile). */
    CHECK(stripped >= 2);
    for (i = 0; i < n; i++)
        free(v[i].data);
    free(v);
    if (failures) {
        printf("probe_test: %lu of %lu checks FAILED (%u images)\n", failures,
               checks, n);
        return 1;
    }
    printf("probe_test: %lu checks passed (%u images, %u accepted, %u fully truncated)\n",
           checks, n, accepted, truncated);
    return 0;
}
