/*
 * Conformance payload for C-FREE (spec 3.8, 5.6, 8.6, 10.2): the freestanding
 * build, and apps that exit without releasing what they hold. Built with
 * AAR_FREESTANDING=1 (conf_free.freestanding): crt0, libaar and libgcc, no libc.
 * See conformance/TESTS.md for the ids, conformance/payloads/conf.h for the
 * protocol and conf_free.h for the variants.
 *
 * `main` judges C-FREE.1 (a 3 MiB buffer from heap_range and heap_grow only,
 * hashed to a known FNV-1a value) and C-FREE.2 (the weak memcpy, memmove, memset
 * and memcmp of libaar that the compiler calls). The other variants are the apps
 * the runner starts 20 times to see that the platform got everything back; they
 * report what they got through one peer message and never free it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_free.h"

AAR_NOTE(0, 0, 0, AAR_GROUP_DISPLAY | AAR_GROUP_AUDIO | AAR_GROUP_SERVICES)

/* The four routines the compiler may call. libaar's freestanding.c defines them
 * (weak); newlib does in a libc build, the host libc on the host. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#define MIB ((uint64_t)1024u * 1024u)
#define BUF_BYTES (3u << 20)
#define FNV_EXPECTED 0x4c37cd60u /* FNV-1a over the first 3 MiB of the xorshift32 stream from 0x2545f491 */

/* ---- A bump allocator over heap_range and heap_grow ------------------------------------ */

static uint8_t *heap_cur;
static uint8_t *heap_end;

static void heap_init(void)
{
    void *base = 0;
    uint64_t bytes = 0;

    aar_heap_range(&base, &bytes);
    heap_cur = (uint8_t *)base;
    heap_end = (uint8_t *)base + bytes;
}

static void *bump(uint32_t bytes)
{
    uint8_t *at = heap_cur;

    if (!at)
        return 0;
    while ((uintptr_t)at + bytes > (uintptr_t)heap_end) {
        uint32_t step = (uint32_t)(((uintptr_t)at + bytes - (uintptr_t)heap_end +
                                    AAR_HEAP_GRANULE - 1u) &
                                   ~(uintptr_t)(AAR_HEAP_GRANULE - 1u));
        void *old_break = 0;

        /* The table grows the heap at its end: the new range starts where the
         * old one stopped. */
        if (aar_heap_grow(step, &old_break) != AAR_OK || (uint8_t *)old_break != heap_end)
            return 0;
        heap_end += step;
    }
    heap_cur = at + bytes;
    return at;
}

static uint32_t fnv1a32(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;

    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* ---- C-FREE.1 and C-FREE.2 ------------------------------------------------------------------ */

static uint8_t *test_hash(void)
{
    struct conf_test t;
    uint8_t *buf;
    uint32_t seed = 0x2545f491u;
    uint32_t i;
    uint32_t hash;

    conf_begin(&t, "C-FREE.1");
    heap_init();
    buf = (uint8_t *)bump(BUF_BYTES);
    if (!conf_check(&t, buf != 0, "the 3 MiB buffer could not be taken from heap_range and heap_grow")) {
        conf_end(&t);
        return 0;
    }
    for (i = 0; i < BUF_BYTES; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)seed;
    }
    hash = fnv1a32(buf, BUF_BYTES);
    if (hash != FNV_EXPECTED && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "FNV-1a of the 3 MiB buffer is ");
        conf_buf_hex(&t.why, hash);
        conf_buf_str(&t.why, ", not ");
        conf_buf_hex(&t.why, FNV_EXPECTED);
    }
    conf_buf_str(&t.note, "3 MiB hash ");
    conf_buf_hex(&t.note, hash);
    conf_end(&t);
    return buf;
}

struct big {
    uint8_t b[4096];
};

static struct big big_src;
static struct big big_dst;
static uint8_t ref_area[8192 + 64];
static uint8_t work_area[8192 + 64];

/* Called through volatile pointers, so the compiler cannot replace the calls by
 * code of its own. */
static void *(*volatile call_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile call_memmove)(void *, const void *, size_t) = memmove;
static void *(*volatile call_memset)(void *, int, size_t) = memset;
static int (*volatile call_memcmp)(const void *, const void *, size_t) = memcmp;

static void test_mem(uint8_t *buf)
{
    struct conf_test t;
    struct big *volatile src = &big_src;
    struct big *volatile dst = &big_dst;
    uint32_t i;
    uint32_t k;
    static const uint32_t shifts[] = { 1u, 3u, 4u, 7u, 64u, 4096u };

    conf_begin(&t, "C-FREE.2");
    /* A struct copy of 4 KiB: the compiler calls memcpy for it. */
    for (i = 0; i < sizeof big_src.b; i++)
        big_src.b[i] = (uint8_t)(i * 7u + 3u);
    *dst = *src;
    for (i = 0; i < sizeof big_src.b; i++) {
        if (big_dst.b[i] != big_src.b[i]) {
            conf_check(&t, 0, "a struct copy of 4 KiB differs from its source");
            break;
        }
    }
    /* Overlapping moves, forward and backward, at several offsets. */
    for (k = 0; k < sizeof shifts / sizeof shifts[0]; k++) {
        uint32_t n = 8192u - shifts[k];
        int dir;

        for (dir = 0; dir < 2; dir++) {
            const uint32_t from = dir ? shifts[k] : 0u;
            const uint32_t to = dir ? 0u : shifts[k];
            int bad = 0;

            for (i = 0; i < sizeof work_area; i++) {
                work_area[i] = (uint8_t)(i * 13u + 1u);
                ref_area[i] = work_area[i];
            }
            call_memmove(work_area + to, work_area + from, n);
            for (i = 0; i < n; i++) {
                if (work_area[to + i] != ref_area[from + i])
                    bad = 1;
            }
            for (i = 0; i < to; i++) {
                if (work_area[i] != ref_area[i])
                    bad = 1;
            }
            for (i = to + n; i < sizeof work_area; i++) {
                if (work_area[i] != ref_area[i])
                    bad = 1;
            }
            if (bad && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "memmove of ");
                conf_buf_u64(&t.why, n);
                conf_buf_str(&t.why, " bytes from offset ");
                conf_buf_u64(&t.why, from);
                conf_buf_str(&t.why, " to ");
                conf_buf_u64(&t.why, to);
                conf_buf_str(&t.why, " is wrong");
            }
        }
    }
    /* memcpy between unaligned addresses. */
    for (i = 0; i < sizeof work_area; i++)
        work_area[i] = 0;
    call_memcpy(work_area + 5, big_src.b + 3, 1001u);
    for (i = 0; i < 1001u; i++) {
        if (work_area[5 + i] != big_src.b[3 + i]) {
            conf_check(&t, 0, "an unaligned memcpy of 1001 bytes is wrong");
            break;
        }
    }
    conf_check(&t, work_area[4] == 0 && work_area[5 + 1001u] == 0, "memcpy wrote outside its range");
    /* A large fill, with guard bytes on both sides. */
    if (buf) {
        const size_t fill = (size_t)(1u << 20);

        buf[2] = 0x11;
        buf[3 + fill] = 0x11;
        call_memset(buf + 3, 0xA7, fill);
        for (i = 0; i < fill; i++) {
            if (buf[3 + i] != 0xA7) {
                conf_check(&t, 0, "a memset of 1 MiB at an odd address is wrong");
                break;
            }
        }
        conf_check(&t, buf[2] == 0x11 && buf[3 + fill] == 0x11, "memset wrote outside its range");
    } else {
        conf_check(&t, 0, "no buffer for the large fill");
    }
    /* Comparisons: equal, first byte, last byte, bytes above 0x7F compare as unsigned. */
    for (i = 0; i < 256u; i++) {
        work_area[i] = (uint8_t)i;
        ref_area[i] = (uint8_t)i;
    }
    conf_check(&t, call_memcmp(work_area, ref_area, 256u) == 0, "memcmp of equal blocks is not 0");
    conf_check(&t, call_memcmp(work_area, ref_area, 0u) == 0, "memcmp of length 0 is not 0");
    ref_area[255] = 0x01;
    conf_check(&t, call_memcmp(work_area, ref_area, 256u) > 0, "memcmp with a larger last byte is not positive");
    ref_area[255] = 255;
    ref_area[0] = 0x80;
    conf_check(&t, call_memcmp(work_area, ref_area, 256u) < 0,
               "memcmp does not compare bytes as unsigned (0x00 against 0x80)");
    conf_check(&t, call_memcmp(ref_area, work_area, 256u) > 0, "memcmp is not antisymmetric");
    conf_buf_str(&t.note, "struct copy, 12 overlapping moves, a 1 MiB fill and compares");
    conf_end(&t);
}

/* ---- The variants the runner starts 20 times ------------------------------------------------------- */

static void send_words(const char *keyword, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint8_t msg[AAR_MSG_BYTES];
    const uint32_t w[4] = { w0, w1, w2, w3 };
    uint32_t i;
    uint32_t j;
    uint64_t start;
    int rc;

    for (i = 0; i < AAR_MSG_BYTES; i++)
        msg[i] = 0;
    for (i = 0; keyword[i] && i < CONF_FREE_WORDS_OFFSET; i++)
        msg[i] = (uint8_t)keyword[i];
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++)
            msg[CONF_FREE_WORDS_OFFSET + 4u * i + j] = (uint8_t)(w[i] >> (8u * j));
    }
    start = aar_time_us();
    do {
        rc = aar_msg_send(msg);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
    /* Let the runner take the message before the app ends. */
    aar_sleep_us(100000u);
}

/* One comment line on the console, so that its buffer exists. */
static void console_line(const char *text)
{
    struct conf_buf b;

    conf_buf_clear(&b);
    conf_buf_str(&b, text);
    conf_emit(&b);
}

static void variant_hog(void)
{
    static const uint64_t steps[3] = { 16u * MIB, 4u * MIB, 1u * MIB };
    uint64_t total = 0;
    uint32_t s;

    for (s = 0; s < 3; s++) {
        int rc;

        for (;;) {
            void *ob = 0;

            rc = aar_heap_grow(steps[s], &ob);
            if (rc != AAR_OK)
                break;
            /* Touch the first byte of every granule so that the memory is used. */
            {
                uint64_t g;

                for (g = 0; g < steps[s]; g += AAR_HEAP_GRANULE)
                    ((volatile uint8_t *)ob)[g] = 0xA5;
            }
            total += steps[s];
        }
        if (rc != AAR_E_NOMEM)
            break;
    }
    send_words(CONF_MSG_FREE_HEAP, (uint32_t)(total / MIB), 0, 0, 0);
}

static void variant_sharedhog(void)
{
    uint64_t total = 0;
    int fd;
    uint32_t n = 0;
    static const uint8_t data[16] = { 'c', 'o', 'n', 'f', '-', 'f', 'r', 'e', 'e', ' ', 's', 'c', 'r', 'a', 't', '\n' };

    /* The runtime's own shared buffers (console, file payloads) are in use. They
     * are charged to the same limit as the app's blocks. The file stays open. */
    console_line("# sharedhog: console and file buffers in use");
    fd = aar_file_open("conf_free_scratch.bin", AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_TRUNCATE);
    if (fd >= AAR_FD_BASE)
        (void)aar_file_write(fd, data, sizeof data, &n);
    for (;;) {
        aar_shared sh;
        int rc = aar_shared_alloc(1u * MIB, 64u, 0u, &sh);

        if (rc != AAR_OK)
            break;
        ((volatile uint8_t *)sh.ptr)[0] = 0xA5;
        total += sh.bytes;
    }
    send_words(CONF_MSG_FREE_SHARED, (uint32_t)(total / 1024u), 0, 0, 0);
}

static uint8_t tile[4096] __attribute__((aligned(64)));

static void variant_services(void)
{
    static const uint32_t image_codecs[] = { AAR_IMAGE_JPEG, AAR_IMAGE_PNG, AAR_IMAGE_WEBP };
    static const uint32_t audio_codecs[] = { AAR_AUDIO_MP3, AAR_AUDIO_FLAC, AAR_AUDIO_VORBIS };
    uint32_t image_ok = 0;
    uint32_t audio_ok = 0;
    uint32_t k;

    for (k = 0; k < sizeof image_codecs / sizeof image_codecs[0] && image_ok == 0; k++) {
        uint32_t i;

        for (i = 0; i < CONF_FREE_SESSIONS; i++) {
            aar_image_params p;
            aar_image_result r;
            uint32_t session = 0;

            p.codec = image_codecs[k];
            p.output_format = AAR_FMT_BGRA8888;
            p.stride = 256u;
            p.rows = 16u;
            p.tile_bytes = sizeof tile;
            p.flags = 0;
            p.tile = tile;
            r.image_width = 0;
            if (aar_image_open(&p, &session, &r) != AAR_OK)
                break;
            image_ok++;
        }
    }
    for (k = 0; k < sizeof audio_codecs / sizeof audio_codecs[0] && audio_ok == 0; k++) {
        uint32_t i;

        for (i = 0; i < CONF_FREE_SESSIONS; i++) {
            uint32_t session = 0;

            if (aar_audio_decode_open(audio_codecs[k], &session) != AAR_OK)
                break;
            audio_ok++;
        }
    }
    send_words(CONF_MSG_FREE_SVC, image_ok, image_ok == 0, audio_ok, audio_ok == 0);
}

static void variant_display(void)
{
    aar_surface surf;
    int rc = aar_display_open(320u, 240u, AAR_FMT_RGB565, 0u, &surf);

    send_words(CONF_MSG_FREE_DISPLAY, (uint32_t)rc, 0, 0, 0);
}

static void variant_audio(void)
{
    aar_audio audio;
    int rc = aar_audio_open(AAR_AUDIO_RATE_22050, 2u, &audio);

    send_words(CONF_MSG_FREE_AUDIO, (uint32_t)rc, 0, 0, 0);
}

static int variant_is(const char *v, const char *name)
{
    return v && conf_str_prefix(v, name) && conf_str_prefix(name, v);
}

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");

    if (variant_is(variant, "quick"))
        return 0;
    if (variant_is(variant, "hog"))
        variant_hog();
    else if (variant_is(variant, "sharedhog"))
        variant_sharedhog();
    else if (variant_is(variant, "services"))
        variant_services();
    else if (variant_is(variant, "display"))
        variant_display();
    else if (variant_is(variant, "audio"))
        variant_audio();
    else
        test_mem(test_hash());
    return conf_finish("C-FREE");
}
