/*
 * Host test of the reference loader against tools/aar-check.
 *
 *   corpus_test CORPUS
 *
 * CORPUS is written by loader/tests/loader_corpus.py: images with the decisions
 * of aar-check (the strict ones) and of the rules of the spec without the
 * section headers (what the loader decides), and for an accepted image what it
 * says. The test checks that
 *   - aar_image_check with AAR_IMAGE_SECTIONS decides as aar-check does, with the
 *     same note,
 *   - aar_image_check without it and aar_load_plan decide as the spec rules do,
 *     with the same profile, note, segments, span, entry and relocation tables,
 *     and with AAR_E_PROFILE exactly for the platform that lacks the profile,
 *   - every accepted image that fits in 1 MiB places, and one that does not is
 *     refused with AAR_E_NOMEM,
 *   - no read goes outside the image or asks for more than 512 bytes of headers.
 * Build it with -fsanitize=address,undefined as well: every image of the
 * mutation part of the corpus is a damaged header or table, and the image is in
 * a heap block of exactly its size.
 *
 * SPDX-License-Identifier: MIT
 */
#include "testutil.h"

#define BOTH (AAR_PROFILE_A32 | AAR_PROFILE_A64)
#define LIMIT (1u << 20)
#define NFIELDS 13

struct record {
    char name[160];
    int strict, spec, overflow;
    uint64_t f[NFIELDS]; /* profile, api major, minor, stack, heap, groups required,
                          * optional, note version, segments with memory, span,
                          * entry, relocation entries, relocation tables */
    uint64_t len;
    uint8_t *data;
};

static uint64_t get_le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    while (n--)
        v = (v << 8) | p[n];
    return v;
}

static int code_of(int rc)
{
    return rc == AAR_OK ? 0 : rc == AAR_E_INVAL ? 1 : rc == AAR_E_PROFILE ? 2 : -1;
}

static void check_note(const struct record *r, const struct aar_image_info *i)
{
    CHECK(i->profile == r->f[0]);
    CHECK(i->api_major == r->f[1]);
    CHECK(i->api_minor_required == r->f[2]);
    CHECK(i->stack_bytes == r->f[3]);
    CHECK(i->min_heap_mib == r->f[4]);
    CHECK(i->groups_required == r->f[5]);
    CHECK(i->groups_optional == r->f[6]);
    CHECK(i->note_version == r->f[7]);
}

static void run_image(const struct record *r, unsigned long *accepted)
{
    struct aar_image_info info;
    struct aar_load_plan plan;
    struct mem m;
    const char *why = "unset";
    int rc, want, i;
    uint64_t entries = 0;

    /* aar-check's rules, section headers included. */
    mem_init(&m, r->data, r->len, 512);
    rc = aar_image_check(mem_read, &m, r->len, AAR_IMAGE_SECTIONS, &info, &why);
    CHECK(code_of(rc) == r->strict);
    CHECK(!m.bad);
    CHECK((rc == AAR_OK) == (why == NULL));
    if (rc == AAR_OK) {
        CHECK(r->spec == 0);
        check_note(r, &info);
    }
    if (code_of(rc) != r->strict)
        printf("  case: %s (module %d, aar-check %d)\n", r->name, code_of(rc), r->strict);

    /* The spec's rules. */
    mem_init(&m, r->data, r->len, 512);
    rc = aar_image_check(mem_read, &m, r->len, 0, &info, &why);
    CHECK(code_of(rc) == r->spec);
    CHECK(!m.bad);
    CHECK((rc == AAR_OK) == (why == NULL));
    if (code_of(rc) != r->spec)
        printf("  case: %s (module %d, spec oracle %d)\n", r->name, code_of(rc), r->spec);
    if (rc == AAR_OK) {
        uint64_t mem_segments = 0;
        uint64_t span = 0;

        check_note(r, &info);
        for (i = 0; i < (int)info.nloads; i++) {
            if (info.load[i].memsz) {
                mem_segments++;
                if (info.load[i].vaddr + info.load[i].memsz > span)
                    span = info.load[i].vaddr + info.load[i].memsz;
            }
        }
        for (i = 0; i < (int)info.ntables; i++)
            entries += info.table[i].count;
        CHECK(mem_segments == r->f[8]);
        if (!r->overflow)
            CHECK(span == r->f[9]);
        CHECK(info.entry == r->f[10]);
        CHECK(entries == r->f[11]);
        CHECK(info.ntables == r->f[12]);
    }

    /* The plan, for a platform with both profiles and for one with the other. */
    want = r->spec == 0 && r->overflow ? 1 : r->spec;
    mem_init(&m, r->data, r->len, 512);
    rc = aar_load_plan(mem_read, &m, r->len, BOTH, &plan, &why);
    CHECK(code_of(rc) == want);
    CHECK(!m.bad);
    CHECK((rc == AAR_OK) == (why == NULL));
    if (rc == AAR_OK) {
        uint32_t other = plan.profile == AAR_PROFILE_A32 ? AAR_PROFILE_A64 : AAR_PROFILE_A32;
        struct aar_load_plan p2;
        uint8_t *block = (uint8_t *)malloc(LIMIT);
        uint64_t entry = 0;
        const uint64_t base = plan.profile == AAR_PROFILE_A64 ? 0x100000000ull : 0x10000000ull;
        int prc;

        (*accepted)++;
        CHECK(plan.profile == r->f[0]);
        CHECK(plan.nsegments == r->f[8]);
        CHECK(plan.span == r->f[9]);
        CHECK(plan.entry_offset == r->f[10]);
        CHECK(plan.stack_bytes == r->f[3] && plan.min_heap_mib == r->f[4]);
        CHECK(plan.groups_required == r->f[5] && plan.groups_optional == r->f[6]);
        entries = 0;
        for (i = 0; i < (int)plan.nrelocs; i++)
            entries += plan.reloc[i].count;
        CHECK(entries == r->f[11] && plan.nrelocs == r->f[12]);

        CHECK(aar_load_plan(mem_read, &m, r->len, plan.profile, &p2, &why) == AAR_OK);
        CHECK(aar_load_plan(mem_read, &m, r->len, other, &p2, &why) == AAR_E_PROFILE);
        CHECK(aar_load_plan(mem_read, &m, r->len, 0, &p2, &why) == AAR_E_PROFILE);

        mem_init(&m, r->data, r->len, 0);
        prc = aar_load_place(&plan, mem_read, &m, block, base, LIMIT, &entry, &why);
        CHECK(!m.bad);
        if (plan.span > LIMIT) {
            CHECK(prc == AAR_E_NOMEM);
        } else {
            CHECK(prc == AAR_OK);
            CHECK(entry == base + plan.entry_offset);
        }
        if (prc != (plan.span > LIMIT ? AAR_E_NOMEM : AAR_OK))
            printf("  case: %s (place %d, span %llu)\n", r->name, prc,
                   (unsigned long long)plan.span);
        free(block);
    } else if (rc == AAR_E_PROFILE || rc == AAR_E_INVAL) {
        /* A refusal does not depend on the platform's profiles. */
        CHECK(aar_load_plan(mem_read, &m, r->len, AAR_PROFILE_A32, &plan, &why) == rc);
    }
}

static int read_corpus(const char *path, struct record **out, uint32_t *count)
{
    FILE *fp = fopen(path, "rb");
    uint8_t b[8];
    uint32_t n, i, k;
    struct record *v;

    if (!fp || fread(b, 1, 4, fp) != 4)
        return -1;
    n = (uint32_t)get_le(b, 4);
    v = (struct record *)calloc(n, sizeof(*v));
    if (!v)
        return -1;
    for (i = 0; i < n; i++) {
        uint8_t h[3];
        uint32_t namelen, len;

        if (fread(b, 1, 2, fp) != 2)
            return -1;
        namelen = (uint32_t)get_le(b, 2);
        if (namelen >= sizeof(v[i].name) || fread(v[i].name, 1, namelen, fp) != namelen)
            return -1;
        if (fread(h, 1, 3, fp) != 3)
            return -1;
        v[i].strict = h[0];
        v[i].spec = h[1];
        v[i].overflow = h[2];
        for (k = 0; k < NFIELDS; k++) {
            if (fread(b, 1, 8, fp) != 8)
                return -1;
            v[i].f[k] = get_le(b, 8);
        }
        if (fread(b, 1, 4, fp) != 4)
            return -1;
        len = (uint32_t)get_le(b, 4);
        v[i].len = len;
        v[i].data = (uint8_t *)malloc(len ? len : 1);
        if (!v[i].data || fread(v[i].data, 1, len, fp) != len)
            return -1;
    }
    fclose(fp);
    *out = v;
    *count = n;
    return 0;
}

int main(int argc, char **argv)
{
    struct record *v;
    uint32_t n, i;
    unsigned long accepted = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: corpus_test CORPUS\n");
        return 2;
    }
    if (read_corpus(argv[1], &v, &n) != 0) {
        fprintf(stderr, "corpus_test: cannot read %s\n", argv[1]);
        return 2;
    }
    for (i = 0; i < n; i++)
        run_image(&v[i], &accepted);
    for (i = 0; i < n; i++)
        free(v[i].data);
    free(v);
    printf("corpus_test: %u images, %lu accepted by the loader\n", n, accepted);
    return finish("corpus_test");
}
