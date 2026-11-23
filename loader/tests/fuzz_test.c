/*
 * A bounded mutation fuzz run of the reference loader: valid images (the ones
 * elfgen.h builds, and the files named on the command line, for example the
 * accepted images of the conformance corpus) with a few bytes damaged, or cut
 * short, run through aar_load_plan and aar_load_place, and through aar_load_place
 * with a damaged plan. No crash, and no read outside the image or write outside
 * the memory offered, which a heap block of exactly the right size gives the
 * address sanitizer; the test checks what it can see itself:
 *   - the codes are the ones the API names,
 *   - the reader is never asked for a byte outside the image,
 *   - a plan the loader made places, in memory of exactly its span,
 *   - memory one byte short is refused, with nothing written,
 *   - a damaged plan is placed or refused, never worse.
 *
 *   fuzz_test [IMAGE...]
 *
 * AAR_FUZZ_ITER sets the iterations per image (default 20000), AAR_FUZZ_SEED the
 * seed. The run is deterministic for a seed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "elfgen.h"
#include "testutil.h"

#define BOTH (AAR_PROFILE_A32 | AAR_PROFILE_A64)
#define LIMIT (1u << 20)

static uint64_t rng_state;

static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint32_t below(uint32_t n)
{
    return (uint32_t)(rnd() % n);
}

static const uint64_t interesting[] = {
    0, 1, 2, 3, 4, 7, 8, 0xFF, 0x100, 0xFFF, 0x1000, 0x4000, 0xFFFF, 0x10000,
    0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull,
    0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFF0ull,
    0xFFFFFFFFFFFFFFFFull,
};

static uint64_t pick_value(uint64_t len)
{
    switch (below(5)) {
    case 0: return rnd();
    case 1: return len + below(16) - 8;
    default: return interesting[below(sizeof(interesting) / sizeof(interesting[0]))];
    }
}

static void mutate(uint8_t *b, size_t len)
{
    unsigned n = 1 + below(4);

    while (n--) {
        size_t at = below((uint32_t)len);

        switch (below(5)) {
        case 0:
            b[at] = (uint8_t)rnd();
            break;
        case 1:
            b[at] ^= (uint8_t)(1u << below(8));
            break;
        case 2: { /* a 16, 32 or 64-bit field */
            unsigned w = 2u << below(3);
            uint64_t v = pick_value(len);
            unsigned k;

            for (k = 0; k < w && at + k < len; k++)
                b[at + k] = (uint8_t)(v >> (8 * k));
            break;
        }
        case 3: { /* a field near the start: the headers */
            size_t head = len < 256 ? len : 256;
            uint64_t v = pick_value(len);
            unsigned k;

            at = below((uint32_t)head);
            for (k = 0; k < 4 && at + k < len; k++)
                b[at + k] = (uint8_t)(v >> (8 * k));
            break;
        }
        default:
            b[at] = 0xFF;
            break;
        }
    }
}

static unsigned long images, planned, placed_ok, refused_n, nomem, plans_damaged, io_checks;

static void run_one(const uint8_t *img, size_t len)
{
    struct aar_load_plan plan;
    struct aar_image_info info;
    struct mem m;
    const char *why = "unset";
    int rc;

    images++;
    mem_init(&m, img, len, 512);
    rc = aar_load_plan(mem_read, &m, len, BOTH, &plan, &why);
    CHECK(rc == AAR_OK || rc == AAR_E_INVAL || rc == AAR_E_PROFILE);
    CHECK(!m.bad);
    CHECK((rc == AAR_OK) == (why == NULL));
    /* The strict rules are the spec's rules and more. */
    mem_init(&m, img, len, 512);
    if (aar_image_check(mem_read, &m, len, AAR_IMAGE_SECTIONS, &info, &why) == AAR_OK)
        CHECK(rc == AAR_OK);
    CHECK(!m.bad);
    if (rc != AAR_OK) {
        refused_n++;
        return;
    }
    planned++;
    {
        const uint64_t base = plan.profile == AAR_PROFILE_A64 ? 0x100000000ull : 0x10000000ull;
        uint64_t entry = 0;
        uint8_t *block;
        uint64_t size = plan.span <= LIMIT ? plan.span : LIMIT;
        int prc;

        block = (uint8_t *)malloc((size_t)size);
        mem_init(&m, img, len, 0);
        prc = aar_load_place(&plan, mem_read, &m, block, base, size, &entry, &why);
        CHECK(!m.bad);
        if (plan.span <= LIMIT) {
            CHECK(prc == AAR_OK);
            placed_ok++;
        } else {
            CHECK(prc == AAR_E_NOMEM);
            nomem++;
        }
        free(block);

        /* One byte short: refused, nothing written (a block of 0xAA stays). */
        if (plan.span > 1 && plan.span <= LIMIT) {
            uint64_t k;

            block = (uint8_t *)malloc((size_t)plan.span - 1);
            memset(block, 0xAA, (size_t)plan.span - 1);
            mem_init(&m, img, len, 0);
            CHECK(aar_load_place(&plan, mem_read, &m, block, base, plan.span - 1, &entry, &why) == AAR_E_NOMEM);
            for (k = 0; k + 1 < plan.span; k++)
                if (block[k] != 0xAA) {
                    CHECK(block[k] == 0xAA);
                    break;
                }
            free(block);
        }
    }
    /* A damaged plan, in memory of a random size. */
    if (below(4) == 0) {
        struct aar_load_plan bad = plan;
        uint8_t *bytes = (uint8_t *)&bad;
        unsigned n = 1 + below(3);
        uint64_t size = below(4) == 0 ? plan.span : below(8192);
        uint8_t *block;
        uint64_t entry = 0;
        int prc;

        while (n--) {
            size_t at = (size_t)below((uint32_t)sizeof(bad)) & ~(size_t)3;
            uint64_t v = pick_value(len);
            unsigned k;

            for (k = 0; k < 8 && at + k < sizeof(bad); k++)
                bytes[at + k] = (uint8_t)(v >> (8 * k));
        }
        /* The length is the caller's statement of how many bytes the reader has. */
        bad.length = plan.length;
        if (size > LIMIT)
            size = LIMIT;
        block = (uint8_t *)malloc((size_t)size ? (size_t)size : 1);
        mem_init(&m, img, len, 0);
        prc = aar_load_place(&bad, mem_read, &m, block, below(2) ? rnd() : 0x10000000u,
                             size, &entry, &why);
        CHECK(!m.bad);
        CHECK(prc == AAR_OK || prc == AAR_E_INVAL || prc == AAR_E_NOMEM);
        free(block);
        plans_damaged++;
    }
    /* A reader that fails part-way. */
    if (below(16) == 0) {
        uint64_t entry;
        uint8_t *block = (uint8_t *)malloc(plan.span <= LIMIT ? (size_t)plan.span : 1);

        mem_init(&m, img, len, 0);
        m.fail_from = 1 + below(4);
        if (plan.span <= LIMIT) {
            int prc = aar_load_place(&plan, mem_read, &m, block, 0x10000000ull, plan.span, &entry, &why);

            CHECK(prc == AAR_OK || prc == AAR_E_IO);
        }
        free(block);
        io_checks++;
    }
}

static void fuzz(const char *label, const uint8_t *seed, size_t len, unsigned long iter)
{
    unsigned long i;

    printf("  %s (%zu bytes)\n", label, len);
    for (i = 0; i < iter; i++) {
        uint8_t *buf;
        size_t n = len;

        if (below(8) == 0)
            n = below((uint32_t)len + 1); /* cut short */
        buf = (uint8_t *)malloc(n ? n : 1);
        memcpy(buf, seed, n);
        if (n)
            mutate(buf, n);
        run_one(buf, n);
        free(buf);
    }
}

int main(int argc, char **argv)
{
    const char *env;
    unsigned long iter = 20000;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint8_t b[GEN_CAP];
    struct gen_out o;
    int i;
    static const struct gen gens[] = {
        { .rel = 1 }, { .rela = 1 }, { .rel = 1, .rela = 1, .bss_target = 1 },
        { .a64 = 1 }, { .a64 = 1, .bss_target = 1 },
    };

    if ((env = getenv("AAR_FUZZ_ITER")) != NULL)
        iter = strtoul(env, NULL, 0);
    if ((env = getenv("AAR_FUZZ_SEED")) != NULL)
        seed = strtoull(env, NULL, 0);
    rng_state = seed ? seed : 1;
    printf("fuzz_test: seed %#llx, %lu iterations per image\n", (unsigned long long)seed, iter);
    for (i = 0; i < (int)(sizeof(gens) / sizeof(gens[0])); i++) {
        char label[64];

        gen_image(b, &gens[i], &o);
        snprintf(label, sizeof(label), "generated image %d", i);
        fuzz(label, b, o.len, iter);
    }
    for (i = 1; i < argc; i++) {
        size_t len;
        uint8_t *img = read_whole(argv[i], &len);

        if (!img || len == 0) {
            printf("FAIL cannot read %s\n", argv[i]);
            failures++;
            continue;
        }
        fuzz(argv[i], img, len, iter / 4 ? iter / 4 : 1);
        free(img);
    }
    printf("fuzz_test: %lu images run, %lu refused, %lu planned (%lu placed, %lu too large), "
           "%lu damaged plans, %lu failing readers\n",
           images, refused_n, planned, placed_ok, nomem, plans_damaged, io_checks);
    CHECK(planned > 0 && refused_n > 0);
    return finish("fuzz_test");
}
