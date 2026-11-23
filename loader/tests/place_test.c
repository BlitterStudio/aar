/*
 * Host test of the reference loader (loader/aar_load.c) on images built in
 * memory (elfgen.h): what aar_load_plan reports, and what aar_load_place does
 * with the memory it is given: the segments copied, the bss and the gaps zeroed,
 * every RELATIVE relocation (A32 REL and RELA, A64 RELA) applied against the
 * address the app runs at while the writes go through another mapping, and
 * nothing written outside the memory offered, whatever the plan or the image
 * says.
 *
 *   place_test
 *
 * SPDX-License-Identifier: MIT
 */
#include "elfgen.h"
#include "testutil.h"

#define GUARD 64u
#define BOTH (AAR_PROFILE_A32 | AAR_PROFILE_A64)

struct variant {
    const char *name;
    struct gen g;
};

static const struct variant variants[] = {
    { "a32 rel", { .rel = 1 } },
    { "a32 rela", { .rela = 1 } },
    { "a32 rel and rela, a target in the bss", { .rel = 1, .rela = 1, .bss_target = 1 } },
    { "a32 rel, a target in the bss", { .rel = 1, .bss_target = 1 } },
    { "a64 rela", { .a64 = 1 } },
    { "a64 rela, a target in the bss", { .a64 = 1, .bss_target = 1 } },
};

struct run {
    int rc;
    const char *why;
    uint64_t entry;
    uint8_t *raw;
    uint8_t *base;
    uint64_t limit;
    unsigned lead;
    int guard_ok;      /* the bytes around the memory offered are as they were */
    int untouched;     /* the memory offered is as it was */
    int bad_read;
};

/* Runs aar_load_place into a block of `limit` bytes (filled with 0xAA, `lead`
 * bytes after the start of the allocation, so that the block need not be
 * aligned) with GUARD bytes of 0xAA on both sides. */
static void do_place(struct run *r, const struct aar_load_plan *plan,
                     const uint8_t *img, uint64_t len, uint64_t limit,
                     uint64_t app_base, unsigned lead)
{
    struct mem m;
    uint64_t i;

    mem_init(&m, img, len, 0);
    r->limit = limit;
    r->lead = lead;
    r->raw = (uint8_t *)malloc((size_t)limit + lead + GUARD);
    memset(r->raw, 0xAA, (size_t)limit + lead + GUARD);
    r->base = r->raw + lead;
    r->entry = 0x1111111111111111ull;
    r->why = "unset";
    r->rc = aar_load_place(plan, mem_read, &m, r->base, app_base, limit,
                           &r->entry, &r->why);
    r->bad_read = m.bad;
    r->guard_ok = 1;
    for (i = 0; i < lead; i++)
        r->guard_ok &= r->raw[i] == 0xAA;
    for (i = 0; i < GUARD; i++)
        r->guard_ok &= r->base[limit + i] == 0xAA;
    r->untouched = 1;
    for (i = 0; i < limit; i++)
        r->untouched &= r->base[i] == 0xAA;
}

static void free_run(struct run *r)
{
    free(r->raw);
    r->raw = NULL;
}

/* What the placed memory must hold: the file bytes of the two segments at
 * their vaddrs, zero everywhere else, and every slot relocated. */
static uint8_t *expected(const struct gen *g, const struct gen_out *o,
                         const uint8_t *img, uint64_t app_base)
{
    uint8_t *e = (uint8_t *)calloc(1, (size_t)o->span);
    uint32_t i;
    uint64_t doff = o->len - o->data_filesz;

    memcpy(e, img, (size_t)o->text_end);
    memcpy(e + o->data_vaddr, img + doff, (size_t)o->data_filesz);
    for (i = 0; i < o->nslots; i++) {
        const struct gen_slot *s = &o->slot[i];

        if (g->a64)
            gen_put64(e, s->vaddr, s->addend + app_base);
        else
            gen_put32(e, s->vaddr, (uint32_t)(s->addend + app_base));
    }
    return e;
}

static void check_plan(const struct variant *v, const struct gen_out *o,
                       const struct aar_load_plan *plan)
{
    const struct gen *g = &v->g;
    uint32_t tables = g->a64 ? 1u : (uint32_t)((g->rel || !g->rela ? 1 : 0) + (g->rela ? 1 : 0));
    uint32_t i;
    uint64_t total = 0;

    CHECK(plan->profile == (g->a64 ? AAR_PROFILE_A64 : AAR_PROFILE_A32));
    CHECK(plan->length == o->len);
    CHECK(plan->span == o->span);
    CHECK(plan->align == 0x1000u);
    CHECK(plan->entry_offset == o->entry);
    CHECK(plan->nsegments == 2);
    CHECK(plan->seg[0].vaddr == 0 && plan->seg[0].offset == 0);
    CHECK(plan->seg[0].filesz == o->text_end && plan->seg[0].memsz == o->text_end);
    CHECK(plan->seg[0].flags == (AAR_LOAD_R | AAR_LOAD_X));
    CHECK(plan->seg[1].vaddr == o->data_vaddr);
    CHECK(plan->seg[1].offset == o->len - o->data_filesz);
    CHECK(plan->seg[1].filesz == o->data_filesz && plan->seg[1].memsz == o->data_memsz);
    CHECK(plan->seg[1].flags == (AAR_LOAD_R | AAR_LOAD_W));
    CHECK(plan->nrelocs == tables);
    for (i = 0; i < plan->nrelocs && i < AAR_LOAD_MAX_RELOCS; i++) {
        const struct aar_load_reloc *r = &plan->reloc[i];

        CHECK(r->entsize == (g->a64 ? 24u : (r->rela ? 12u : 8u)));
        CHECK(r->size == r->count * r->entsize);
        CHECK(r->offset + r->size <= o->len);
        total += r->count;
    }
    CHECK(total == o->nslots);
    CHECK(plan->note_version == AAR_NOTE_VERSION);
    CHECK(plan->api_major == AAR_API_MAJOR);
    CHECK(plan->api_minor_required == 0);
    CHECK(plan->stack_bytes == 0);
    CHECK(plan->min_heap_mib == 2);
    CHECK(plan->groups_required == 1);
    CHECK(plan->groups_optional == 4);
}

static void check_placed(const struct variant *v, const struct gen_out *o,
                         const struct aar_load_plan *plan, const uint8_t *img,
                         uint64_t app_base, unsigned lead)
{
    struct run r;
    uint8_t *e;

    do_place(&r, plan, img, o->len, o->span, app_base, lead);
    CHECK(r.rc == AAR_OK);
    CHECK(r.why == NULL);
    CHECK(!r.bad_read);
    CHECK(r.guard_ok);
    CHECK(r.entry == app_base + o->entry);
    e = expected(&v->g, o, img, app_base);
    CHECK(memcmp(e, r.base, (size_t)o->span) == 0);
    /* The two checks above say the same as this one in words: the text bytes,
     * the zeroed gap and bss, and the 4 canary bytes next to each A32 slot. */
    CHECK(r.base[o->text_end] == 0 && r.base[o->data_vaddr - 1] == 0);
    CHECK(r.base[o->span - 1] == 0);
    free(e);
    free_run(&r);
}

static void test_variant(const struct variant *v)
{
    struct gen_out o;
    uint8_t b[GEN_CAP];
    uint8_t *img;
    struct aar_load_plan plan;
    struct mem m;
    const char *why = "unset";
    const uint64_t bases32[] = { 0x80000000ull, 0xFFFF0000ull, 0 };
    const uint64_t bases64[] = { 0x123456789000ull, 0xFFFFFFFFFFF00000ull, 0 };
    const uint64_t *bases = v->g.a64 ? bases64 : bases32;
    uint32_t i;
    unsigned long calls;

    printf("  %s\n", v->name);
    gen_image(b, &v->g, &o);
    img = copy_exact(b, o.len);
    mem_init(&m, img, o.len, 512);
    CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_OK);
    CHECK(why == NULL);
    CHECK(!m.bad);
    calls = m.calls;
    check_plan(v, &o, &plan);

    /* Placed at several bases, through a block that is not where the app runs
     * and is not aligned. */
    for (i = 0; i < 3; i++)
        check_placed(v, &o, &plan, img, bases[i], i);

    /* The memory offered: one byte short is refused and nothing is written,
     * exactly enough is accepted. */
    {
        struct run r;

        do_place(&r, &plan, img, o.len, o.span - 1, bases[0], 0);
        CHECK(r.rc == AAR_E_NOMEM);
        CHECK(r.guard_ok && r.untouched);
        free_run(&r);
        do_place(&r, &plan, img, o.len, 0, bases[0], 0);
        CHECK(r.rc == AAR_E_NOMEM);
        CHECK(r.guard_ok && r.untouched);
        free_run(&r);
        do_place(&r, &plan, img, o.len, o.span + 100, bases[0], 0);
        CHECK(r.rc == AAR_OK);
        CHECK(r.guard_ok);
        CHECK(r.base[o.span + 99] == 0xAA); /* beyond the span: not touched */
        free_run(&r);
    }

    /* The address space at the app's base: the image must end within it. */
    {
        struct run r;
        uint64_t top = v->g.a64 ? UINT64_MAX : 0x100000000ull;

        do_place(&r, &plan, img, o.len, o.span, top - o.span, 0);
        CHECK(r.rc == AAR_OK);
        CHECK(r.entry == top - o.span + o.entry);
        free_run(&r);
        do_place(&r, &plan, img, o.len, o.span, top - o.span + 1, 0);
        CHECK(r.rc == AAR_E_INVAL);
        CHECK(r.why != NULL);
        CHECK(r.guard_ok);
        free_run(&r);
    }

    /* Arguments. */
    {
        uint64_t entry = 0;
        uint8_t block[16];

        CHECK(aar_load_place(NULL, mem_read, &m, block, 0, sizeof(block), &entry, NULL) == AAR_E_INVAL);
        CHECK(aar_load_place(&plan, NULL, &m, block, 0, sizeof(block), &entry, NULL) == AAR_E_INVAL);
        CHECK(aar_load_place(&plan, mem_read, &m, NULL, 0, plan.span, &entry, NULL) == AAR_E_INVAL);
        CHECK(aar_load_place(&plan, mem_read, &m, block, 0, plan.span, NULL, NULL) == AAR_E_INVAL);
    }

    /* A plan that aar_load_plan did not make, or an image that is not the one
     * of the plan: refused, and nothing is written outside the offered memory. */
    {
        struct aar_load_plan t;
        struct run r;

#define TAMPER(fix, want)                                                  \
        do {                                                               \
            t = plan;                                                      \
            fix;                                                           \
            do_place(&r, &t, img, o.len, o.span, bases[0], 0);             \
            CHECK(r.rc == (want));                                         \
            CHECK(r.guard_ok);                                             \
            CHECK(!r.bad_read);                                            \
            free_run(&r);                                                  \
        } while (0)
        TAMPER(t.nsegments = 0, AAR_E_INVAL);
        TAMPER(t.nsegments = AAR_LOAD_MAX_SEGMENTS + 1, AAR_E_INVAL);
        TAMPER(t.nrelocs = AAR_LOAD_MAX_RELOCS + 1, AAR_E_INVAL);
        TAMPER(t.profile = 0, AAR_E_INVAL);
        TAMPER(t.seg[1].memsz = t.span, AAR_E_INVAL);          /* past the span */
        TAMPER(t.seg[1].vaddr = t.span, AAR_E_INVAL);
        TAMPER(t.seg[1].vaddr = UINT64_MAX - 4, AAR_E_INVAL);  /* wraps */
        TAMPER(t.seg[1].filesz = t.seg[1].memsz + 1, AAR_E_INVAL);
        TAMPER(t.seg[0].offset = o.len, AAR_E_INVAL);          /* bytes not in the image */
        TAMPER(t.seg[1].filesz = UINT64_MAX, AAR_E_INVAL);
        TAMPER(t.seg[1].offset = UINT64_MAX - 3, AAR_E_INVAL);
        TAMPER(t.seg[1].vaddr = 0x10, AAR_E_INVAL);            /* overlaps the text */
        TAMPER(t.seg[1].flags |= AAR_LOAD_X, AAR_E_INVAL);     /* writable and executable */
        TAMPER(t.entry_offset = t.span, AAR_E_INVAL);
        TAMPER(t.reloc[0].offset = o.len, AAR_E_INVAL);
        TAMPER(t.reloc[0].size = UINT64_MAX, AAR_E_INVAL);
        TAMPER(t.reloc[0].entsize = 16, AAR_E_INVAL);
        TAMPER(t.reloc[0].entsize = 0, AAR_E_INVAL);
#undef TAMPER
    }

    /* An image whose table entries are not what the plan says: the entries are
     * read again, and checked again. Table 0, entry 0. */
    {
        uint8_t *bad = copy_exact(img, o.len);
        const struct aar_load_reloc *tab = &plan.reloc[0];
        uint64_t at = tab->offset;
        uint64_t info = v->g.a64 ? at + 8 : at + 4;
        struct run r;

#define BREAK(stmt)                                                        \
        do {                                                               \
            memcpy(bad, img, (size_t)o.len);                               \
            stmt;                                                          \
            do_place(&r, &plan, bad, o.len, o.span, bases[0], 0);          \
            CHECK(r.rc == AAR_E_INVAL);                                    \
            CHECK(r.why != NULL);                                          \
            CHECK(r.guard_ok);                                             \
            free_run(&r);                                                  \
        } while (0)
        if (v->g.a64) {
            BREAK(gen_put64(bad, at, o.span));                         /* the end of the span */
            BREAK(gen_put64(bad, at, o.span - 4));                     /* half in */
            BREAK(gen_put64(bad, at, 0xFFFFFFFFFFFFFFF8ull));
            BREAK(gen_put64(bad, at, o.data_vaddr + 4));               /* misaligned */
            BREAK(gen_put64(bad, info, 1026));                         /* not RELATIVE */
            BREAK(gen_put64(bad, info, 1027ull | (5ull << 32)));       /* a symbol */
        } else {
            BREAK(gen_put32(bad, at, (uint32_t)o.span));
            BREAK(gen_put32(bad, at, (uint32_t)o.span - 2));
            BREAK(gen_put32(bad, at, 0xFFFFFFFCu));
            BREAK(gen_put32(bad, at, (uint32_t)o.data_vaddr + 2));
            BREAK(gen_put32(bad, info, 22));
            BREAK(gen_put32(bad, info, 23u | (5u << 8)));
        }
#undef BREAK
        free(bad);
    }

    /* A reader that fails: AAR_E_IO from every call of plan and of place. */
    {
        unsigned long k;
        struct mem mk;

        for (k = 1; k <= calls; k++) {
            mem_init(&mk, img, o.len, 512);
            mk.fail_from = k;
            CHECK(aar_load_plan(mem_read, &mk, o.len, BOTH, &plan, &why) == AAR_E_IO);
        }
        mem_init(&mk, img, o.len, 512);
        CHECK(aar_load_plan(mem_read, &mk, o.len, BOTH, &plan, &why) == AAR_OK);
        for (k = 1; k <= 3; k++) {
            uint64_t entry;
            uint8_t *block = (uint8_t *)malloc((size_t)o.span);

            /* Two segments, then the first table. */
            mem_init(&mk, img, o.len, 0);
            mk.fail_from = k;
            CHECK(aar_load_place(&plan, mem_read, &mk, block, 0, o.span, &entry, &why) == AAR_E_IO);
            free(block);
        }
    }

    /* Every truncation of the image is refused, and the reader is only asked
     * for bytes of the image. */
    {
        uint64_t n;
        uint8_t *cut;

        for (n = 0; n < o.len; n++) {
            struct mem mt;

            cut = copy_exact(img, (size_t)n);
            mem_init(&mt, cut, n, 512);
            CHECK(aar_load_plan(mem_read, &mt, n, BOTH, &plan, &why) == AAR_E_INVAL);
            CHECK(!mt.bad);
            free(cut);
        }
    }

    /* Bytes after the image are not part of it. */
    {
        uint8_t *longer = (uint8_t *)malloc((size_t)o.len + 100);
        struct mem ml;
        struct aar_load_plan p2;

        memcpy(longer, img, (size_t)o.len);
        memset(longer + o.len, 0x77, 100);
        mem_init(&ml, longer, o.len + 100, 512);
        CHECK(aar_load_plan(mem_read, &ml, o.len + 100, BOTH, &p2, &why) == AAR_OK);
        CHECK(p2.span == plan.span && p2.entry_offset == plan.entry_offset);
        free(longer);
    }
    free(img);
}

static void test_profiles(void)
{
    uint32_t p;

    for (p = 0; p < 2; p++) {
        struct gen g;
        struct gen_out o;
        uint8_t b[GEN_CAP];
        struct aar_load_plan plan;
        struct mem m;
        const char *why;
        uint32_t own = p ? AAR_PROFILE_A64 : AAR_PROFILE_A32;
        uint32_t other = p ? AAR_PROFILE_A32 : AAR_PROFILE_A64;

        memset(&g, 0, sizeof(g));
        g.a64 = (int)p;
        gen_image(b, &g, &o);
        mem_init(&m, b, o.len, 512);
        CHECK(aar_load_plan(mem_read, &m, o.len, own, &plan, &why) == AAR_OK);
        CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_OK);
        CHECK(aar_load_plan(mem_read, &m, o.len, other, &plan, &why) == AAR_E_PROFILE);
        CHECK(why != NULL);
        CHECK(aar_load_plan(mem_read, &m, o.len, 0, &plan, &why) == AAR_E_PROFILE);
        CHECK(aar_load_plan(mem_read, &m, o.len, 0xFFFFFFFCu, &plan, &why) == AAR_E_PROFILE);
        /* The rules come before the profile. */
        g.no_note_segment = 1;
        gen_image(b, &g, &o);
        mem_init(&m, b, o.len, 512);
        CHECK(aar_load_plan(mem_read, &m, o.len, other, &plan, &why) == AAR_E_INVAL);
        CHECK(aar_load_plan(mem_read, &m, o.len, own, &plan, &why) == AAR_E_INVAL);
        /* Arguments. */
        CHECK(aar_load_plan(NULL, &m, o.len, BOTH, &plan, &why) == AAR_E_INVAL);
        CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, NULL, &why) == AAR_E_INVAL);
        CHECK(aar_load_plan(mem_read, &m, UINT64_MAX, BOTH, &plan, &why) == AAR_E_INVAL);
        CHECK(aar_load_plan(mem_read, &m, 0, BOTH, &plan, NULL) == AAR_E_INVAL);
    }
}

/* Images that are refused or that need more memory than any platform has. */
static void test_images(void)
{
    uint32_t p;

    for (p = 0; p < 2; p++) {
        struct gen g;
        struct gen_out o;
        uint8_t b[GEN_CAP];
        struct aar_load_plan plan;
        struct mem m;
        const char *why;
        uint32_t phsz = p ? 56u : 32u, ehsz = p ? 64u : 52u;
        uint64_t data_memsz_at = ehsz + phsz + (p ? 40u : 20u);
        struct run r;

        memset(&g, 0, sizeof(g));
        g.a64 = (int)p;

        /* Overlapping segments. */
        g.text_extra = 0x5000;
        gen_image(b, &g, &o);
        mem_init(&m, b, o.len, 512);
        CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_E_INVAL);
        CHECK(why != NULL && strstr(why, "overlap") != NULL);
        g.text_extra = 0;
        /* Segments that touch do not overlap: the text ends where the data begins. */
        gen_image(b, &g, &o);
        {
            uint32_t text_memsz = (uint32_t)o.data_vaddr;

            if (p)
                gen_put64(b, ehsz + 40, text_memsz);
            else
                gen_put32(b, ehsz + 20, text_memsz);
            mem_init(&m, b, o.len, 512);
            CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_OK);
            CHECK(plan.span == o.span && plan.seg[0].memsz == text_memsz);
            if (p)
                gen_put64(b, ehsz + 40, text_memsz + 1);
            else
                gen_put32(b, ehsz + 20, text_memsz + 1);
            CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_E_INVAL);
        }

        /* More memory than the platform has: the plan reports it, place refuses. */
        gen_image(b, &g, &o);
        if (p)
            gen_put64(b, data_memsz_at, 0x4000000000ull);
        else
            gen_put32(b, data_memsz_at, 0xE0000000u);
        mem_init(&m, b, o.len, 512);
        CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_OK);
        CHECK(plan.span == o.data_vaddr + (p ? 0x4000000000ull : 0xE0000000u));
        do_place(&r, &plan, b, o.len, 1u << 20, 0, 0);
        CHECK(r.rc == AAR_E_NOMEM);
        CHECK(r.guard_ok && r.untouched);
        free_run(&r);

        /* A segment that ends beyond the 64-bit address space. */
        if (p) {
            gen_image(b, &g, &o);
            gen_put64(b, data_memsz_at, 0xFFFFFFFFFFFFFFF0ull);
            mem_init(&m, b, o.len, 512);
            CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_E_INVAL);
            CHECK(why != NULL);
        } else {
            /* ELF32 sizes are 32 bits: vaddr plus memsz fits 64 bits, the span
             * is beyond 4 GiB, and place refuses it. */
            gen_image(b, &g, &o);
            gen_put32(b, data_memsz_at, 0xFFFFFFF0u);
            mem_init(&m, b, o.len, 512);
            CHECK(aar_load_plan(mem_read, &m, o.len, BOTH, &plan, &why) == AAR_OK);
            CHECK(plan.span > 0x100000000ull);
            do_place(&r, &plan, b, o.len, 1u << 20, 0, 0);
            CHECK(r.rc == AAR_E_NOMEM);
            CHECK(r.guard_ok && r.untouched);
            free_run(&r);
        }
    }
}

static int plan_of(const uint8_t *b, size_t len, struct aar_load_plan *plan, const char **why)
{
    struct mem m;

    mem_init(&m, b, len, 512);
    return aar_load_plan(mem_read, &m, len, BOTH, plan, why);
}

/* The group and heap words of the note are reported, never judged: the platform
 * answers AAR_E_NOGROUP for a required group it lacks, whether the bit is
 * defined, reserved (5 to 27) or experimental (28 to 31), and ignores unknown
 * bits of groups_optional (spec 8.1). min_heap_mib is a 32-bit count with no
 * maximum (spec 4.1). */
static void test_note_words(void)
{
    const uint32_t words[] = { 1u << 5, 1u << 27, 0x0FFFFFE0u, 1u << 28, 1u << 31, 0xFFFFFFFFu };
    const uint32_t heaps[] = { 4095u, 4096u, 0x10000001u, 0xFFFFFFFFu };
    uint32_t p, i;

    for (p = 0; p < 2; p++) {
        struct gen g;
        struct gen_out o;
        uint8_t b[GEN_CAP];
        struct aar_load_plan plan;
        const char *why;
        uint32_t ehsz = p ? 64u : 52u, phsz = p ? 56u : 32u;
        uint32_t note_off = (ehsz + 4u * phsz + 7u) & ~7u;

        memset(&g, 0, sizeof(g));
        g.a64 = (int)p;
        for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
            gen_image(b, &g, &o);
            gen_put32(b, note_off + 44, words[i]);
            why = "unset";
            CHECK(plan_of(b, o.len, &plan, &why) == AAR_OK);
            CHECK(plan.groups_required == words[i]);
            CHECK(plan.groups_optional == 4);
            gen_image(b, &g, &o);
            gen_put32(b, note_off + 48, words[i]);
            CHECK(plan_of(b, o.len, &plan, &why) == AAR_OK);
            CHECK(plan.groups_optional == words[i]);
            CHECK(plan.groups_required == 1);
        }
        for (i = 0; i < sizeof(heaps) / sizeof(heaps[0]); i++) {
            gen_image(b, &g, &o);
            gen_put32(b, note_off + 40, heaps[i]);
            CHECK(plan_of(b, o.len, &plan, &why) == AAR_OK);
            CHECK(plan.min_heap_mib == heaps[i]);
        }
    }
}

/* spec 2.2 rule 4: exactly one PT_DYNAMIC, even when the image has no
 * relocations. Rule 5: every relocation target, with room for the whole word,
 * lies inside the memory (p_memsz, so the bss counts) of a PT_LOAD. */
static void test_dynamic_and_targets(void)
{
    uint32_t p;

    for (p = 0; p < 2; p++) {
        struct gen g;
        struct gen_out o;
        uint8_t b[GEN_CAP];
        struct aar_load_plan plan;
        const char *why;
        uint32_t ehsz = p ? 64u : 52u, phsz = p ? 56u : 32u;
        uint32_t dyn_type_at = ehsz + 2u * phsz;
        uint64_t word = p ? 8u : 4u;
        uint64_t span;

        memset(&g, 0, sizeof(g));
        g.a64 = (int)p;

        /* No PT_DYNAMIC, and two. */
        gen_image(b, &g, &o);
        gen_put32(b, dyn_type_at, 0);
        why = NULL;
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_E_INVAL);
        CHECK(why != NULL && strstr(why, "PT_DYNAMIC") != NULL);
        gen_image(b, &g, &o);
        gen_put32(b, ehsz + 3u * phsz, 2); /* the PT_NOTE entry becomes a second PT_DYNAMIC */
        why = NULL;
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_E_INVAL);
        CHECK(why != NULL && strstr(why, "PT_DYNAMIC") != NULL);

        /* A target in the gap between the segments, past the end of the last one,
         * and in the bss tail. */
        g.first_target = 0x2000;
        gen_image(b, &g, &o);
        why = NULL;
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_E_INVAL);
        CHECK(why != NULL && strstr(why, "outside every PT_LOAD") != NULL);
        g.first_target = o.span;
        gen_image(b, &g, &o);
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_E_INVAL);
        g.first_target = o.span - word;
        gen_image(b, &g, &o);
        why = "unset";
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_OK);
        CHECK(why == NULL);

        /* The last word of the memory is not whole: p_memsz ends two (A32) or
         * four (A64) bytes after an aligned address. */
        memset(&g, 0, sizeof(g));
        g.a64 = (int)p;
        g.bss = p ? 0x7FCu : 0x7FEu;
        gen_image(b, &g, &o);
        span = o.span;
        CHECK(span % word == (p ? 4u : 2u));
        g.first_target = span - (p ? 4u : 2u);
        gen_image(b, &g, &o);
        why = NULL;
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_E_INVAL);
        CHECK(why != NULL && strstr(why, "outside every PT_LOAD") != NULL);
        g.first_target = span - (p ? 12u : 6u);
        gen_image(b, &g, &o);
        CHECK(plan_of(b, o.len, &plan, &why) == AAR_OK);
    }
}

int main(void)
{
    size_t i;

    printf("place_test: pointers are %u bits\n", (unsigned)(8 * sizeof(void *)));
    for (i = 0; i < sizeof(variants) / sizeof(variants[0]); i++)
        test_variant(&variants[i]);
    test_profiles();
    test_images();
    test_note_words();
    test_dynamic_and_targets();
    return finish("place_test");
}
