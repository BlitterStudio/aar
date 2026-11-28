/*
 * Host test of the reference loader over the C-ELF corpus of the conformance
 * suite (conformance/negative/): every image of the MANIFEST, for both
 * profiles, must be accepted or refused as its row says, with a code its row
 * allows, and an accepted image must place.
 *
 *   manifest_test conformance/negative/MANIFEST
 *
 * The rules of a row (the columns are described in gen_negative.py):
 *   load OK, load ANY or probe ok   the image is acceptable: plan accepts it for
 *                                   a platform that runs its profile, and
 *                                   refuses it with AAR_E_PROFILE for one that
 *                                   does not
 *   probe any                       the spec states no rule for the platform's
 *                                   memory: either; a refusal has a code the
 *                                   row lists
 *   otherwise                       plan refuses it with a code the row lists
 *                                   (PROFILE, INVAL or VERSION)
 * An image that is accepted is placed at an address that is not where it is
 * written; the images whose point is a relocation (C-ELF.16 to C-ELF.18, and
 * ok-reloc-bss of C-ELF.23) hold a
 * word that must be the runtime address of their own code, and it must not be
 * without the relocations.
 *
 * SPDX-License-Identifier: MIT
 */
#include "testutil.h"

#define BOTH (AAR_PROFILE_A32 | AAR_PROFILE_A64)
#define LIMIT (1u << 20)

struct row {
    char name[64], profile[8], ids[128], load[64], probe[16];
};

static int has_token(const char *list, const char *token)
{
    size_t n = strlen(token);
    const char *p = list;

    while (*p) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == n && memcmp(p, token, n) == 0)
            return 1;
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}

static int code_allowed(const struct row *r, int rc)
{
    if (rc == AAR_E_PROFILE)
        return has_token(r->load, "PROFILE");
    if (rc == AAR_E_INVAL)
        return has_token(r->load, "INVAL");
    if (rc == AAR_E_VERSION)
        return has_token(r->load, "VERSION");
    return 0;
}

/* The words of the writable segments that equal `value`. */
static unsigned count_words(const struct aar_load_plan *plan, const uint8_t *mem,
                            uint64_t value, unsigned width)
{
    unsigned n = 0;
    uint32_t i;
    uint64_t at;

    for (i = 0; i < plan->nsegments; i++) {
        const struct aar_load_segment *s = &plan->seg[i];

        if (!(s->flags & AAR_LOAD_W))
            continue;
        for (at = s->vaddr; at + width <= s->vaddr + s->memsz; at += width) {
            uint64_t w = 0;
            unsigned k;

            for (k = 0; k < width; k++)
                w |= (uint64_t)mem[at + k] << (8 * k);
            if (w == value)
                n++;
        }
    }
    return n;
}

static void check_note_of(const struct row *r, const struct aar_load_plan *plan)
{
    if (strcmp(r->name, "ok-stack-64k") == 0)
        CHECK(plan->stack_bytes == (64u << 10));
    else if (strcmp(r->name, "ok-stack-1m") == 0)
        CHECK(plan->stack_bytes == (1u << 20));
    else if (strcmp(r->name, "ok-stack0") == 0)
        CHECK(plan->stack_bytes == 0);
    else if (strcmp(r->name, "probe-note") == 0)
        CHECK(plan->stack_bytes == (128u << 10) && plan->min_heap_mib == 3 &&
              plan->groups_required == 3 && plan->groups_optional == 0x14);
    else if (strcmp(r->name, "group-experimental-28") == 0)
        CHECK(plan->groups_required == (1u << 28));
    else if (strcmp(r->name, "group-experimental-31") == 0)
        CHECK(plan->groups_required == (1u << 31));
    else if (strcmp(r->name, "group-reserved-5") == 0)
        CHECK(plan->groups_required == (1u << 5));
    else if (strcmp(r->name, "group-reserved-27") == 0)
        CHECK(plan->groups_required == (1u << 27));
    else if (strcmp(r->name, "group-optional-unknown") == 0)
        CHECK(plan->groups_required == 0 &&
              plan->groups_optional == ((1u << 5) | (1u << 27) | (1u << 28)));
    else if (strcmp(r->name, "heap-max") == 0)
        CHECK(plan->min_heap_mib == 0xFFFFFFFFu);
    else if (strcmp(r->name, "heap-wrap") == 0)
        CHECK(plan->min_heap_mib == 0x10000001u);
}

static unsigned long rows, accepted, refused, placed, relocation_images;

static void run_row(const struct row *r, const char *dir)
{
    char path[1024];
    uint8_t *img;
    size_t len;
    uint32_t own = strcmp(r->profile, "a64") == 0 ? AAR_PROFILE_A64 : AAR_PROFILE_A32;
    uint32_t other = own == AAR_PROFILE_A64 ? AAR_PROFILE_A32 : AAR_PROFILE_A64;
    int must_accept = strcmp(r->load, "OK") == 0 || strcmp(r->load, "ANY") == 0 ||
                      strcmp(r->probe, "ok") == 0;
    int either = !must_accept && strcmp(r->probe, "any") == 0;
    struct aar_load_plan plan;
    const uint32_t masks[3] = { own, BOTH, other };
    int rc[3];
    unsigned i;

    snprintf(path, sizeof(path), "%s/%s/%s.aarimg", dir, r->profile, r->name);
    img = read_whole(path, &len);
    if (!img) {
        printf("FAIL cannot read %s\n", path);
        failures++;
        return;
    }
    rows++;
    for (i = 0; i < 3; i++) {
        struct mem m;
        const char *why = "unset";

        mem_init(&m, img, len, 512);
        rc[i] = aar_load_plan(mem_read, &m, len, masks[i], &plan, &why);
        CHECK(!m.bad);
        CHECK(rc[i] == AAR_OK || rc[i] == AAR_E_PROFILE || rc[i] == AAR_E_INVAL ||
              rc[i] == AAR_E_VERSION);
        CHECK((rc[i] == AAR_OK) == (why == NULL));
        if (rc[i] != AAR_OK)
            continue;
        CHECK(plan.profile == own);
    }
    if (must_accept) {
        CHECK(rc[0] == AAR_OK);
        CHECK(rc[1] == AAR_OK);
        CHECK(rc[2] == AAR_E_PROFILE);
    } else {
        /* A refusal does not depend on the profiles the platform runs, except
         * that the image can be accepted (either) only when it has a profile
         * the platform runs. */
        for (i = 0; i < 3; i++) {
            if (rc[i] == AAR_OK)
                CHECK(either && i < 2);
            else
                CHECK(code_allowed(r, rc[i]) || (either && rc[i] == AAR_E_PROFILE) ||
                      (either && rc[i] == AAR_E_INVAL));
        }
        if (!either) {
            CHECK(rc[0] != AAR_OK);
            CHECK(rc[1] != AAR_OK);
        }
    }
    if (rc[0] == AAR_OK) {
        struct mem m;
        uint8_t *block = (uint8_t *)malloc(LIMIT);
        uint64_t entry = 0;
        const uint64_t base = own == AAR_PROFILE_A64 ? 0x100000000ull : 0x10000000ull;
        const unsigned width = own == AAR_PROFILE_A64 ? 8u : 4u;
        const char *why = "unset";
        int prc;

        accepted++;
        check_note_of(r, &plan);
        mem_init(&m, img, len, 0);
        if (plan.span > LIMIT) {
            /* More memory than the platform offers (segment-huge). */
            prc = aar_load_place(&plan, mem_read, &m, block, base, LIMIT, &entry, &why);
            CHECK(prc == AAR_E_NOMEM);
            CHECK(has_token(r->load, "NOMEM"));
        } else {
            prc = aar_load_place(&plan, mem_read, &m, block, base, LIMIT, &entry, &why);
            CHECK(prc == AAR_OK);
            CHECK(!m.bad);
            if (prc == AAR_OK) {
                struct aar_load_plan none = plan;

                placed++;
                CHECK(entry == base + plan.entry_offset);
                if (strstr(r->ids, "C-ELF.16") || strstr(r->ids, "C-ELF.17") ||
                    strstr(r->ids, "C-ELF.18") || strcmp(r->name, "ok-reloc-bss") == 0) {
                    /* The relocated word: the runtime address of the code. */
                    CHECK(plan.nrelocs >= 1 && plan.reloc[0].count >= 1);
                    CHECK(count_words(&plan, block, entry, width) == 1);
                    none.nrelocs = 0;
                    mem_init(&m, img, len, 0);
                    CHECK(aar_load_place(&none, mem_read, &m, block, base, LIMIT, &entry, &why) == AAR_OK);
                    CHECK(count_words(&plan, block, entry, width) == 0);
                    relocation_images++;
                }
            }
        }
        free(block);
    } else {
        refused++;
    }
    free(img);
}

int main(int argc, char **argv)
{
    char dir[512];
    char line[512];
    FILE *fp;
    const char *slash;
    size_t n;

    if (argc != 2) {
        fprintf(stderr, "usage: manifest_test MANIFEST\n");
        return 2;
    }
    slash = strrchr(argv[1], '/');
    n = slash ? (size_t)(slash - argv[1]) : 0;
    if (n >= sizeof(dir))
        return 2;
    if (slash) {
        memcpy(dir, argv[1], n);
        dir[n] = 0;
    } else {
        strcpy(dir, ".");
    }
    fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "manifest_test: cannot read %s\n", argv[1]);
        return 2;
    }
    while (fgets(line, sizeof(line), fp)) {
        struct row r;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63s %7s %127s %63s %15s", r.name, r.profile, r.ids, r.load,
                   r.probe) != 5) {
            printf("FAIL bad manifest line: %s", line);
            failures++;
            continue;
        }
        run_row(&r, dir);
    }
    fclose(fp);
    printf("manifest_test: %lu images, %lu accepted (%lu placed, %lu relocation images), %lu refused\n",
           rows, accepted, placed, relocation_images, refused);
    CHECK(rows >= 140);
    CHECK(relocation_images == 6); /* ok-nosections, ok-rel, ok-rela, ok-reloc-bss */
    return finish("manifest_test");
}
