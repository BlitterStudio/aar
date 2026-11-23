/*
 * Conformance payload for C-SERVICES (spec 8.6): hashing, decompression and the
 * image and audio decode sessions, judged from the app's side with embedded test
 * vectors (conf_services_vec.h, made by tools/gen_service_vectors.py). The group
 * allows a platform to implement any subset of the algorithms and codecs: a
 * call that answers AAR_E_NOSYS for one passes with a note, and one that
 * answers anything else must give the exact bytes (or the pixels and samples
 * within the tolerance the catalogue names).
 *
 * The standard does not describe some stream details; the tests follow the zzapp
 * 0.x behaviour that the group was taken from (TESTS.md, spec question 38): the
 * LZMA2 stream has one byte of properties before the raw stream, an LHA stream
 * has no end marker (the destination size is the decode length), an image
 * session decodes into one tile that holds the whole image, and its EOF flag is
 * bit 0.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_services_vec.h"

AAR_NOTE(0, 0, AAR_GROUP_SERVICES, 0)

#define ALG_COUNT 10u
#define GUARD 64u
#define OUT_BYTES (VEC_PLAIN_BYTES + 16u + GUARD)
#define SESSIONS_TRIED 12u
#define EOF_FLAG 1u              /* the image session's end-of-input flag */

static uint8_t plain[VEC_PLAIN_BYTES];
static uint8_t outbuf[OUT_BYTES];
static uint8_t big[1048576u + 8u];

/* ---- The vectors ----------------------------------------------------------------------- */

struct alg {
    uint32_t id;
    const char *name;
    const uint8_t *data;
    uint32_t bytes;
    int ends;           /* the stream ends itself; an LHA stream does not */
};

static const struct alg algs[ALG_COUNT] = {
    { AAR_COMPRESS_DEFLATE_RAW, "deflate", vec_deflate, sizeof vec_deflate, 1 },
    { AAR_COMPRESS_ZLIB, "zlib", vec_zlib, sizeof vec_zlib, 1 },
    { AAR_COMPRESS_GZIP, "gzip", vec_gzip, sizeof vec_gzip, 1 },
    { AAR_COMPRESS_LZ4_BLOCK, "lz4", vec_lz4, sizeof vec_lz4, 1 },
    { AAR_COMPRESS_LZMA_ALONE, "lzma", vec_lzma_alone, sizeof vec_lzma_alone, 1 },
    { AAR_COMPRESS_LZMA2, "lzma2", vec_lzma2, sizeof vec_lzma2, 1 },
    { AAR_COMPRESS_LH1, "lh1", vec_lh1, sizeof vec_lh1, 0 },
    { AAR_COMPRESS_LH5, "lh5", vec_lh5, sizeof vec_lh5, 0 },
    { AAR_COMPRESS_LH6, "lh6", vec_lh6, sizeof vec_lh6, 0 },
    { AAR_COMPRESS_LH7, "lh7", vec_lh7, sizeof vec_lh7, 0 },
};

/* The plain text: words picked by an LCG (the generator makes the same). */
static void make_plain(void)
{
    static const char *const words[8] = {
        "alpha ", "beta ", "gamma ", "delta ", "amiga ", "arm ", "zz9000 ", "runtime\n"
    };
    uint32_t x = 12345u;
    uint32_t n = 0;

    while (n < VEC_PLAIN_BYTES) {
        const char *w;

        x = (x * 1103515245u + 12345u) & 0x7FFFFFFFu;
        for (w = words[(x >> 16) & 7u]; *w && n < VEC_PLAIN_BYTES; w++)
            plain[n++] = (uint8_t)*w;
    }
}

static uint8_t sha_byte(uint32_t i)
{
    return (uint8_t)(i * 31u + (i >> 8) * 7u + 3u);
}

/* PNG test image: a pixel of 24x16. */
static void png_px(uint32_t x, uint32_t y, uint8_t rgb[3])
{
    rgb[0] = (uint8_t)(x * 9u + y * 5u);
    rgb[1] = (uint8_t)(x * y * 3u + 17u);
    rgb[2] = (uint8_t)((x ^ y) * 13u);
}

/* FLAC test audio: sample n of channel ch. */
static int32_t flac_sample(uint32_t ch, uint32_t n)
{
    int32_t k = (int32_t)(n + 7u * ch);
    int32_t t = (k * 6) % 4000 - 2000;
    int32_t a = (t < 0 ? -t : t) - 1000;
    int32_t b = ((k >> 6) & 1) ? 500 : -500;

    return a * 16 + b;
}

static void text_rc(struct conf_buf *b, int rc)
{
    conf_buf_i64(b, rc);
}

/* Which services work, found first: 1 when at least one codec opens. */
static int image_impl;
static int audio_impl;

/* ---- C-SERVICES.1: SHA-256 -------------------------------------------------------------------- */

static void test_sha(void)
{
    struct conf_test t;
    uint8_t digest[32];
    uint32_t i;
    uint32_t off;
    int rc;

    conf_begin(&t, "C-SERVICES.1");
    rc = aar_sha256("", 0, digest);
    if (rc != AAR_OK) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "aar_sha256 gave ");
            text_rc(&t.why, rc);
        }
    } else {
        for (i = 0; i < 32u; i++) {
            if (digest[i] != vec_sha_empty[i]) {
                conf_check(&t, 0, "the digest of the empty message is wrong");
                break;
            }
        }
    }
    rc = aar_sha256("abc", 3, digest);
    if (rc != AAR_OK) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "aar_sha256 of abc gave ");
            text_rc(&t.why, rc);
        }
    } else {
        for (i = 0; i < 32u; i++) {
            if (digest[i] != vec_sha_abc[i]) {
                conf_check(&t, 0, "the digest of abc is wrong");
                break;
            }
        }
    }
    for (i = 0; i < 1048576u; i++)
        big[i] = sha_byte(i);
    for (i = 0; i < VEC_SHA_COUNT; i++) {
        uint32_t k;

        rc = aar_sha256(big, vec_sha[i].bytes, digest);
        if (rc != AAR_OK) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "aar_sha256 of ");
                conf_buf_u64(&t.why, vec_sha[i].bytes);
                conf_buf_str(&t.why, " bytes gave ");
                text_rc(&t.why, rc);
            }
            continue;
        }
        for (k = 0; k < 32u; k++) {
            if (digest[k] != vec_sha[i].digest[k]) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "the digest of ");
                    conf_buf_u64(&t.why, vec_sha[i].bytes);
                    conf_buf_str(&t.why, " bytes is wrong");
                }
                break;
            }
        }
    }
    /* The 1000 bytes at odd addresses. */
    for (off = 1; off <= 3u; off++) {
        uint32_t k;

        for (i = 0; i < 1000u; i++)
            big[off + i] = sha_byte(i);
        rc = aar_sha256(big + off, 1000, digest);
        if (rc != AAR_OK) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "aar_sha256 of an unaligned buffer gave ");
                text_rc(&t.why, rc);
            }
            continue;
        }
        for (k = 0; k < 32u; k++) {
            if (digest[k] != vec_sha[9].digest[k]) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "the digest of 1000 bytes at offset ");
                    conf_buf_u64(&t.why, off);
                    conf_buf_str(&t.why, " is wrong");
                }
                break;
            }
        }
    }
    conf_buf_str(&t.note, "empty, abc, the padding boundaries, 1 MiB, and 1000 bytes unaligned");
    conf_end(&t);
}

/* ---- C-SERVICES.2 to C-SERVICES.4: decompression ------------------------------------------------ */

static int supported[ALG_COUNT];

static void test_decompress(void)
{
    struct conf_test t;
    uint32_t a;
    uint32_t ok_count = 0;
    uint32_t nosys = 0;

    conf_begin(&t, "C-SERVICES.2");
    for (a = 0; a < ALG_COUNT; a++) {
        aar_service_result res = { 0, 0, 0, 0 };
        uint64_t room = algs[a].ends ? VEC_PLAIN_BYTES + 16u : VEC_PLAIN_BYTES;
        uint32_t i;
        int rc;

        for (i = 0; i < OUT_BYTES; i++)
            outbuf[i] = 0xA5;
        rc = aar_decompress(algs[a].id, 0, algs[a].data, algs[a].bytes, outbuf, room, &res);
        if (rc == AAR_E_NOSYS) {
            nosys++;
            continue;
        }
        if (rc != AAR_OK) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, algs[a].name);
                conf_buf_str(&t.why, " gave ");
                text_rc(&t.why, rc);
            }
            continue;
        }
        supported[a] = 1;
        ok_count++;
        if (res.bytes_written != VEC_PLAIN_BYTES && conf_fail_first(&t)) {
            conf_buf_str(&t.why, algs[a].name);
            conf_buf_str(&t.why, ": bytes_written ");
            conf_buf_u64(&t.why, res.bytes_written);
            conf_buf_str(&t.why, ", want 1000");
        }
        if (algs[a].ends ? res.bytes_consumed != algs[a].bytes
                         : (res.bytes_consumed == 0 || res.bytes_consumed > algs[a].bytes)) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, algs[a].name);
                conf_buf_str(&t.why, ": bytes_consumed ");
                conf_buf_u64(&t.why, res.bytes_consumed);
                conf_buf_str(&t.why, " of ");
                conf_buf_u64(&t.why, algs[a].bytes);
            }
        }
        for (i = 0; i < VEC_PLAIN_BYTES; i++) {
            if (outbuf[i] != plain[i]) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, algs[a].name);
                    conf_buf_str(&t.why, ": byte ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " differs");
                }
                break;
            }
        }
        for (i = (uint32_t)room; i < OUT_BYTES; i++) {
            if (outbuf[i] != 0xA5) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, algs[a].name);
                    conf_buf_str(&t.why, " wrote past the destination");
                }
                break;
            }
        }
    }
    conf_buf_u64(&t.note, ok_count);
    conf_buf_str(&t.note, " algorithms decode exactly, ");
    conf_buf_u64(&t.note, nosys);
    conf_buf_str(&t.note, " are not implemented (AAR_E_NOSYS)");
    conf_end(&t);
}

static void test_bad_algorithm(void)
{
    static const uint32_t bad[] = { AAR_COMPRESS_NONE, 11u, 99u, 0xFFFFFFFFu };
    struct conf_test t;
    aar_service_result res = { 0, 0, 0, 0 };
    uint32_t i;

    conf_begin(&t, "C-SERVICES.3");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = aar_decompress(bad[i], 0, vec_deflate, sizeof vec_deflate, outbuf,
                                VEC_PLAIN_BYTES, &res);

        if (rc != AAR_E_INVAL && rc != AAR_E_NOSYS && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "algorithm ");
            conf_buf_u64(&t.why, bad[i]);
            conf_buf_str(&t.why, " gave ");
            text_rc(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL");
        }
    }
    conf_buf_str(&t.note, "0, 11, 99 and 0xFFFFFFFF are refused with AAR_E_INVAL or AAR_E_NOSYS");
    conf_end(&t);
}

static void test_bad_streams(void)
{
    struct conf_test t;
    uint32_t a;
    uint32_t tried = 0;

    conf_begin(&t, "C-SERVICES.4");
    for (a = 0; a < ALG_COUNT; a++) {
        aar_service_result res = { 0, 0, 0, 0 };
        uint32_t i;
        int rc;

        if (!supported[a] || !algs[a].ends)
            continue;
        tried++;
        for (i = 0; i < OUT_BYTES; i++)
            outbuf[i] = 0xA5;
        rc = aar_decompress(algs[a].id, 0, algs[a].data, algs[a].bytes, outbuf, 500, &res);
        if (rc >= 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, algs[a].name);
            conf_buf_str(&t.why, ": a destination of 500 bytes for 1000 gave ");
            text_rc(&t.why, rc);
        }
        for (i = 500; i < OUT_BYTES; i++) {
            if (outbuf[i] != 0xA5) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, algs[a].name);
                    conf_buf_str(&t.why, " wrote past a destination of 500 bytes");
                }
                break;
            }
        }
        rc = aar_decompress(algs[a].id, 0, algs[a].data, algs[a].bytes / 2u, outbuf,
                            VEC_PLAIN_BYTES + 16u, &res);
        if (rc >= 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, algs[a].name);
            conf_buf_str(&t.why, ": a stream cut in half gave ");
            text_rc(&t.why, rc);
        }
    }
    if (tried == 0)
        conf_buf_str(&t.note, "no algorithm with an end marker is implemented");
    else {
        conf_buf_u64(&t.note, tried);
        conf_buf_str(&t.note, " algorithms: too small a destination and a cut stream are errors, "
                              "nothing written past the end");
    }
    conf_end(&t);
}

/* ---- Image sessions ----------------------------------------------------------------------------- */

static uint8_t tile[32u * 32u * 4u];

/* 0 decoded within the tolerance, 1 the codec is not implemented, 2 failed (why set). */
static int image_run(uint32_t codec, const uint8_t *src, uint32_t len, uint32_t w, uint32_t h,
                     const uint8_t *ref_rgb, uint32_t tol, struct conf_buf *why)
{
    aar_image_params p;
    aar_image_result res = { 0, 0, 0, 0, 0 };
    uint32_t session = 0;
    uint32_t pos = 0;
    uint32_t seen_w = 0;
    uint32_t seen_h = 0;
    uint32_t iter;
    uint32_t idle = 0;
    uint32_t worst = 0;
    int done = 0;
    int written_all = 0;
    int rc;
    uint32_t i;

    for (i = 0; i < sizeof tile; i++)
        tile[i] = 0xCC;
    p.codec = codec;
    p.output_format = AAR_FMT_BGRA8888;
    p.stride = w * 4u;
    p.rows = h;
    p.tile_bytes = w * 4u * h;
    p.flags = 0;
    p.tile = tile;
    rc = aar_image_open(&p, &session, &res);
    if (rc == AAR_E_NOSYS)
        return 1;
    if (rc != AAR_OK) {
        conf_buf_str(why, "image_open gave ");
        text_rc(why, rc);
        return 2;
    }
    if (res.image_width)
        seen_w = res.image_width;
    if (res.image_height)
        seen_h = res.image_height;
    for (iter = 0; iter < 32u && !done && !written_all; iter++) {
        if (pos < len)
            rc = aar_image_decode(session, src + pos, len - pos, 0, &res);
        else
            rc = aar_image_decode(session, src, 0, EOF_FLAG, &res);
        if (rc != AAR_OK) {
            conf_buf_str(why, "image_decode gave ");
            text_rc(why, rc);
            (void)aar_image_close(session);
            return 2;
        }
        if (res.bytes_consumed > len - pos) {
            conf_buf_str(why, "bytes_consumed is more than the input");
            (void)aar_image_close(session);
            return 2;
        }
        pos += res.bytes_consumed;
        if (res.bytes_consumed == 0 && pos < len && ++idle > 3u) {
            conf_buf_str(why, "the session consumed no input");
            (void)aar_image_close(session);
            return 2;
        }
        if (res.image_width)
            seen_w = res.image_width;
        if (res.image_height)
            seen_h = res.image_height;
        if (res.bytes_written >= w * h * 4u)
            written_all = 1;
        /* Is the tile the image yet? */
        worst = 0;
        done = 1;
        for (i = 0; i < w * h && done; i++) {
            uint8_t rgb[3];
            const uint8_t *px = tile + 4u * i;
            uint32_t c;

            if (ref_rgb) {
                rgb[0] = ref_rgb[3u * i];
                rgb[1] = ref_rgb[3u * i + 1u];
                rgb[2] = ref_rgb[3u * i + 2u];
            } else {
                png_px(i % w, i / w, rgb);
            }
            for (c = 0; c < 3u; c++) {
                uint32_t got = px[2u - c];
                uint32_t want = rgb[c];
                uint32_t d = got > want ? got - want : want - got;

                if (d > worst)
                    worst = d;
                if (d > tol)
                    done = 0;
            }
            if (px[3] != 255u)
                done = 0;
        }
    }
    (void)aar_image_close(session);
    if (!done) {
        conf_buf_str(why, "the tile does not hold the image (largest channel difference ");
        conf_buf_u64(why, worst);
        conf_buf_str(why, ")");
        return 2;
    }
    if (seen_w != w || seen_h != h) {
        conf_buf_str(why, "the session reports ");
        conf_buf_u64(why, seen_w);
        conf_buf_chr(why, 'x');
        conf_buf_u64(why, seen_h);
        conf_buf_str(why, ", the image is ");
        conf_buf_u64(why, w);
        conf_buf_chr(why, 'x');
        conf_buf_u64(why, h);
        return 2;
    }
    conf_buf_str(why, "largest channel difference ");
    conf_buf_u64(why, worst);
    return 0;
}

static void test_png(void)
{
    struct conf_test t;
    struct conf_buf why;
    int r;

    conf_begin(&t, "C-SERVICES.5");
    conf_buf_clear(&why);
    r = image_run(AAR_IMAGE_PNG, vec_png, sizeof vec_png, VEC_PNG_W, VEC_PNG_H, 0, 0, &why);
    if (r == 2 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "PNG: ");
        conf_buf_str(&t.why, why.b);
    } else if (r == 1) {
        conf_buf_str(&t.note, "PNG is not implemented (AAR_E_NOSYS from image_open)");
    } else if (r == 0) {
        conf_buf_str(&t.note, "PNG 24x16 exact in BGRA tiles");
    }
    conf_end(&t);
}

static void test_jpeg_webp(void)
{
    struct conf_test t;
    struct conf_buf why;
    int r;
    int impl = 0;

    conf_begin(&t, "C-SERVICES.6");
    conf_buf_clear(&why);
    r = image_run(AAR_IMAGE_JPEG, vec_jpeg, sizeof vec_jpeg, VEC_SMOOTH, VEC_SMOOTH, vec_jpeg_ref, 3, &why);
    if (r == 2 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "JPEG: ");
        conf_buf_str(&t.why, why.b);
    }
    impl += r == 0;
    conf_buf_str(&t.note, r == 0 ? "JPEG: " : "");
    if (r == 0)
        conf_buf_str(&t.note, why.b);
    conf_buf_clear(&why);
    r = image_run(AAR_IMAGE_WEBP, vec_webp, sizeof vec_webp, VEC_SMOOTH, VEC_SMOOTH, vec_webp_ref, 3, &why);
    if (r == 2) {
        /* Both codecs are reported, not only the first failure. */
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "WebP: ");
            conf_buf_str(&t.why, why.b);
        } else {
            conf_buf_str(&t.why, "; WebP: ");
            conf_buf_str(&t.why, why.b);
        }
    }
    impl += r == 0;
    conf_buf_str(&t.note, r == 0 ? "; WebP: " : "");
    if (r == 0)
        conf_buf_str(&t.note, why.b);
    conf_buf_str(&t.note, "; ");
    conf_buf_u64(&t.note, (uint64_t)impl);
    conf_buf_str(&t.note, " of JPEG and WebP decode within 3 per channel, the rest not implemented");
    conf_end(&t);
}

/* ---- Audio sessions ------------------------------------------------------------------------------- */

static int16_t pcm_all[6144];
static int16_t pcm_call[2u * 2048u];

struct audio_got {
    uint32_t frames;
    uint32_t rate;
    uint32_t channels;
};

/* Decodes a whole FLAC or Vorbis file into pcm_all (interleaved). 0 done, 1 not
 * implemented, 2 failed. */
static int audio_stream(uint32_t codec, const uint8_t *src, uint32_t len, struct audio_got *got,
                        struct conf_buf *why)
{
    aar_audio_decode_result res;
    uint32_t session = 0;
    uint32_t pos = 0;
    uint32_t iter;
    uint32_t idle = 0;
    uint32_t drain = 0;
    int rc = aar_audio_decode_open(codec, &session);

    got->frames = got->rate = got->channels = 0;
    if (rc == AAR_E_NOSYS)
        return 1;
    if (rc != AAR_OK) {
        conf_buf_str(why, "audio_decode_open gave ");
        text_rc(why, rc);
        return 2;
    }
    for (iter = 0; iter < 400u && drain < 8u; iter++) {
        uint32_t flags = AAR_AUDIO_DECODE_EOF;
        uint32_t k;

        res.bytes_consumed = res.frames_written = res.rate = res.channels = 0;
        if (pos < len) {
            rc = aar_audio_decode(session, src + pos, len - pos, flags, pcm_call, 2048, &res);
        } else {
            rc = aar_audio_decode(session, src, 0, AAR_AUDIO_DECODE_EOF | AAR_AUDIO_DECODE_DRAIN,
                                  pcm_call, 2048, &res);
            drain++;
        }
        if (rc != AAR_OK) {
            conf_buf_str(why, "audio_decode gave ");
            text_rc(why, rc);
            (void)aar_audio_decode_close(session);
            return 2;
        }
        if (res.bytes_consumed > len - pos) {
            conf_buf_str(why, "bytes_consumed is more than the input");
            (void)aar_audio_decode_close(session);
            return 2;
        }
        pos += res.bytes_consumed;
        if (res.rate)
            got->rate = res.rate;
        if (res.channels)
            got->channels = res.channels;
        if (res.frames_written) {
            uint32_t ch = got->channels ? got->channels : 1u;

            drain = 0;
            for (k = 0; k < res.frames_written * ch &&
                        got->frames * ch + k < sizeof pcm_all / sizeof pcm_all[0]; k++)
                pcm_all[got->frames * ch + k] = pcm_call[k];
            got->frames += res.frames_written;
        }
        if (res.bytes_consumed == 0 && res.frames_written == 0 && pos < len && ++idle > 4u) {
            conf_buf_str(why, "the session makes no progress");
            (void)aar_audio_decode_close(session);
            return 2;
        }
    }
    (void)aar_audio_decode_close(session);
    return 0;
}

/* MP3: one frame per call. */
static int audio_mp3(struct audio_got *got, struct conf_buf *why)
{
    aar_audio_decode_result res;
    uint32_t session = 0;
    uint32_t pos = 0;
    uint32_t f;
    int rc = aar_audio_decode_open(AAR_AUDIO_MP3, &session);

    got->frames = got->rate = got->channels = 0;
    if (rc == AAR_E_NOSYS)
        return 1;
    if (rc != AAR_OK) {
        conf_buf_str(why, "audio_decode_open gave ");
        text_rc(why, rc);
        return 2;
    }
    for (f = 0; f < VEC_MP3_FRAMES; f++) {
        uint32_t k;
        uint32_t ch;

        res.bytes_consumed = res.frames_written = res.rate = res.channels = 0;
        rc = aar_audio_decode(session, vec_mp3 + pos, vec_mp3_len[f], 0, pcm_call, 1152, &res);
        if (rc != AAR_OK) {
            conf_buf_str(why, "audio_decode of frame ");
            conf_buf_u64(why, f);
            conf_buf_str(why, " gave ");
            text_rc(why, rc);
            (void)aar_audio_decode_close(session);
            return 2;
        }
        if (res.bytes_consumed != vec_mp3_len[f]) {
            conf_buf_str(why, "a frame was not consumed whole");
            (void)aar_audio_decode_close(session);
            return 2;
        }
        pos += vec_mp3_len[f];
        got->rate = res.rate ? res.rate : got->rate;
        got->channels = res.channels ? res.channels : got->channels;
        ch = got->channels ? got->channels : 1u;
        for (k = 0; k < res.frames_written * ch &&
                    got->frames * ch + k < sizeof pcm_all / sizeof pcm_all[0]; k++)
            pcm_all[got->frames * ch + k] = pcm_call[k];
        got->frames += res.frames_written;
    }
    (void)aar_audio_decode_close(session);
    return 0;
}

/* The mean square error between the decoded first channel and a reference is at
 * most 64 squared. Returns the rounded root of the mean square, or -1 if there
 * is nothing to compare. */
static int32_t rms_error(const int16_t *ref, uint32_t ref_frames, const struct audio_got *got)
{
    uint32_t n = got->frames < ref_frames ? got->frames : ref_frames;
    uint32_t ch = got->channels ? got->channels : 1u;
    uint64_t sum = 0;
    uint32_t i;
    uint32_t r = 0;

    if (n == 0)
        return -1;
    for (i = 0; i < n; i++) {
        int32_t d = (int32_t)pcm_all[i * ch] - (int32_t)ref[i];

        sum += (uint64_t)((int64_t)d * d);
    }
    sum /= n;
    while ((uint64_t)(r + 1u) * (r + 1u) <= sum)
        r++;
    return (int32_t)r;
}

static void test_audio_codecs(void)
{
    struct conf_test t;
    struct conf_buf why;
    struct audio_got got;
    uint32_t i;
    uint32_t impl = 0;
    int r;

    conf_begin(&t, "C-SERVICES.7");

    /* FLAC: exact. */
    conf_buf_clear(&why);
    r = audio_stream(AAR_AUDIO_FLAC, vec_flac, sizeof vec_flac, &got, &why);
    if (r == 2) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "FLAC: ");
            conf_buf_str(&t.why, why.b);
        }
    } else if (r == 0) {
        impl++;
        if ((got.frames != VEC_FLAC_FRAMES || got.rate != VEC_FLAC_RATE || got.channels != 2u) &&
            conf_fail_first(&t)) {
            conf_buf_str(&t.why, "FLAC: ");
            conf_buf_u64(&t.why, got.frames);
            conf_buf_str(&t.why, " frames at ");
            conf_buf_u64(&t.why, got.rate);
            conf_buf_str(&t.why, " Hz in ");
            conf_buf_u64(&t.why, got.channels);
            conf_buf_str(&t.why, " channels, want 768 at 22050 in 2");
        } else {
            for (i = 0; i < VEC_FLAC_FRAMES; i++) {
                if (pcm_all[2u * i] != flac_sample(0, i) || pcm_all[2u * i + 1u] != flac_sample(1, i)) {
                    if (conf_fail_first(&t)) {
                        conf_buf_str(&t.why, "FLAC: frame ");
                        conf_buf_u64(&t.why, i);
                        conf_buf_str(&t.why, " is not exact");
                    }
                    break;
                }
            }
        }
    }

    /* MP3: root mean square error of at most 64. */
    conf_buf_clear(&why);
    r = audio_mp3(&got, &why);
    if (r == 2) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "MP3: ");
            conf_buf_str(&t.why, why.b);
        }
    } else if (r == 0) {
        int32_t e;

        impl++;
        e = rms_error(vec_mp3_ref, 1152u * VEC_MP3_FRAMES, &got);
        if (got.frames != 1152u * VEC_MP3_FRAMES || got.rate != 44100u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "MP3: ");
                conf_buf_u64(&t.why, got.frames);
                conf_buf_str(&t.why, " frames at ");
                conf_buf_u64(&t.why, got.rate);
                conf_buf_str(&t.why, " Hz, want 2304 at 44100");
            }
        } else if ((e < 0 || e > 64) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "MP3: root mean square error ");
            conf_buf_i64(&t.why, e);
            conf_buf_str(&t.why, " against the reference, at most 64 allowed");
        }
    }

    /* Vorbis: the same. */
    conf_buf_clear(&why);
    r = audio_stream(AAR_AUDIO_VORBIS, vec_ogg, sizeof vec_ogg, &got, &why);
    if (r == 2) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "Vorbis: ");
            conf_buf_str(&t.why, why.b);
        }
    } else if (r == 0) {
        int32_t e = rms_error(vec_ogg_ref, VEC_OGG_REF_FRAMES, &got);

        impl++;
        if (got.rate != VEC_OGG_RATE || got.frames + 512u < 1536u ||
            got.frames > VEC_OGG_REF_FRAMES + 1152u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "Vorbis: ");
                conf_buf_u64(&t.why, got.frames);
                conf_buf_str(&t.why, " frames at ");
                conf_buf_u64(&t.why, got.rate);
                conf_buf_str(&t.why, " Hz, want about 1536 to 1920 at 22050");
            }
        } else if ((e < 0 || e > 64) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "Vorbis: root mean square error ");
            conf_buf_i64(&t.why, e);
            conf_buf_str(&t.why, " against the reference, at most 64 allowed");
        }
    }
    conf_buf_u64(&t.note, impl);
    conf_buf_str(&t.note, " of FLAC, MP3 and Vorbis decode as required, the rest are not implemented");
    conf_end(&t);
}

/* ---- C-SERVICES.8 and C-SERVICES.9: sessions ---------------------------------------------------------- */

static uint8_t tiles[SESSIONS_TRIED][16u * 16u * 4u];
static uint32_t image_codec;
static uint32_t audio_codec;

static void probe_codecs(void)
{
    static const uint32_t img[] = { AAR_IMAGE_PNG, AAR_IMAGE_JPEG, AAR_IMAGE_WEBP };
    static const uint32_t aud[] = { AAR_AUDIO_FLAC, AAR_AUDIO_MP3, AAR_AUDIO_VORBIS };
    uint32_t i;

    for (i = 0; i < 3u && !image_codec; i++) {
        aar_image_params p;
        aar_image_result res = { 0, 0, 0, 0, 0 };
        uint32_t s = 0;

        p.codec = img[i];
        p.output_format = AAR_FMT_BGRA8888;
        p.stride = 64;
        p.rows = 16;
        p.tile_bytes = sizeof tiles[0];
        p.flags = 0;
        p.tile = tiles[0];
        if (aar_image_open(&p, &s, &res) == AAR_OK) {
            image_codec = img[i];
            image_impl = 1;
            (void)aar_image_close(s);
        }
    }
    for (i = 0; i < 3u && !audio_codec; i++) {
        uint32_t s = 0;

        if (aar_audio_decode_open(aud[i], &s) == AAR_OK) {
            audio_codec = aud[i];
            audio_impl = 1;
            (void)aar_audio_decode_close(s);
        }
    }
}

static void test_session_count(void)
{
    struct conf_test t;
    uint32_t ids[SESSIONS_TRIED];
    uint32_t n;
    uint32_t i;
    int rc = AAR_OK;

    conf_begin(&t, "C-SERVICES.8");
    if (!image_codec) {
        conf_buf_str(&t.note, "no image codec is implemented; ");
    } else {
        for (n = 0; n < SESSIONS_TRIED; n++) {
            aar_image_params p;
            aar_image_result res = { 0, 0, 0, 0, 0 };

            p.codec = image_codec;
            p.output_format = AAR_FMT_BGRA8888;
            p.stride = 64;
            p.rows = 16;
            p.tile_bytes = sizeof tiles[0];
            p.flags = 0;
            p.tile = tiles[n];
            rc = aar_image_open(&p, &ids[n], &res);
            if (rc != AAR_OK)
                break;
        }
        if (n < AAR_MIN_DECODE_SESSIONS && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "only ");
            conf_buf_u64(&t.why, n);
            conf_buf_str(&t.why, " image sessions open at once, 4 are required; the refusal was ");
            text_rc(&t.why, rc);
        } else if (n < SESSIONS_TRIED && rc != AAR_E_NOMEM && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the refusal of image session ");
            conf_buf_u64(&t.why, n + 1u);
            conf_buf_str(&t.why, " was ");
            text_rc(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_NOMEM");
        }
        conf_buf_u64(&t.note, n);
        conf_buf_str(&t.note, " image sessions; ");
        for (i = 0; i < n; i++) {
            if (aar_image_close(ids[i]) != AAR_OK && conf_fail_first(&t))
                conf_buf_str(&t.why, "closing an image session failed");
        }
    }
    if (!audio_codec) {
        conf_buf_str(&t.note, "no audio codec is implemented");
    } else {
        for (n = 0; n < SESSIONS_TRIED; n++) {
            rc = aar_audio_decode_open(audio_codec, &ids[n]);
            if (rc != AAR_OK)
                break;
        }
        if (n < AAR_MIN_DECODE_SESSIONS && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "only ");
            conf_buf_u64(&t.why, n);
            conf_buf_str(&t.why, " audio sessions open at once, 4 are required; the refusal was ");
            text_rc(&t.why, rc);
        } else if (n < SESSIONS_TRIED && rc != AAR_E_NOMEM && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the refusal of audio session ");
            conf_buf_u64(&t.why, n + 1u);
            conf_buf_str(&t.why, " was ");
            text_rc(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_NOMEM");
        }
        conf_buf_u64(&t.note, n);
        conf_buf_str(&t.note, " audio sessions");
        for (i = 0; i < n; i++) {
            if (aar_audio_decode_close(ids[i]) != AAR_OK && conf_fail_first(&t))
                conf_buf_str(&t.why, "closing an audio session failed");
        }
    }
    conf_end(&t);
}

/* The first failure names the call, the rest are counted. */
static void bad_call(struct conf_test *t, uint32_t *more, const char *what, uint32_t id, int rc)
{
    if (!conf_fail_first(t)) {
        (*more)++;
        return;
    }
    conf_buf_str(&t->why, what);
    conf_buf_str(&t->why, " of session ");
    conf_buf_u64(&t->why, id);
    conf_buf_str(&t->why, " gave ");
    text_rc(&t->why, rc);
}

static void test_session_ids(void)
{
    static const uint32_t unknown[] = { 0u, 0xDEADBEEFu, 0x7FFFFFFFu };
    struct conf_test t;
    aar_image_result ires = { 0, 0, 0, 0, 0 };
    aar_audio_decode_result ares = { 0, 0, 0, 0 };
    uint32_t more = 0;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-SERVICES.9");
    for (i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        rc = aar_image_decode(unknown[i], vec_png, 4, 0, &ires);
        if (rc != AAR_E_INVAL && !(rc == AAR_E_NOSYS && !image_impl))
            bad_call(&t, &more, "image_decode", unknown[i], rc);
        rc = aar_image_close(unknown[i]);
        if (rc != AAR_E_INVAL && !(rc == AAR_E_NOSYS && !image_impl))
            bad_call(&t, &more, "image_close", unknown[i], rc);
        rc = aar_audio_decode(unknown[i], vec_flac, 4, 0, pcm_call, 2048, &ares);
        if (rc != AAR_E_INVAL && !(rc == AAR_E_NOSYS && !audio_impl))
            bad_call(&t, &more, "audio_decode", unknown[i], rc);
        rc = aar_audio_decode_close(unknown[i]);
        if (rc != AAR_E_INVAL && !(rc == AAR_E_NOSYS && !audio_impl))
            bad_call(&t, &more, "audio_decode_close", unknown[i], rc);
    }
    if (image_codec) {
        aar_image_params p;
        uint32_t s = 0;

        p.codec = image_codec;
        p.output_format = AAR_FMT_BGRA8888;
        p.stride = 64;
        p.rows = 16;
        p.tile_bytes = sizeof tiles[0];
        p.flags = 0;
        p.tile = tiles[0];
        if (aar_image_open(&p, &s, &ires) == AAR_OK) {
            (void)aar_image_close(s);
            rc = aar_image_decode(s, vec_png, 4, 0, &ires);
            if (rc >= 0)
                bad_call(&t, &more, "image_decode (closed)", s, rc);
            rc = aar_image_close(s);
            if (rc >= 0)
                bad_call(&t, &more, "a second image_close (closed)", s, rc);
        }
    }
    if (audio_codec) {
        uint32_t s = 0;

        if (aar_audio_decode_open(audio_codec, &s) == AAR_OK) {
            (void)aar_audio_decode_close(s);
            rc = aar_audio_decode(s, vec_flac, 4, 0, pcm_call, 2048, &ares);
            if (rc >= 0)
                bad_call(&t, &more, "audio_decode (closed)", s, rc);
            rc = aar_audio_decode_close(s);
            if (rc >= 0)
                bad_call(&t, &more, "a second audio_decode_close (closed)", s, rc);
        }
    }
    if (more && t.failed) {
        conf_buf_str(&t.why, " and ");
        conf_buf_u64(&t.why, more);
        conf_buf_str(&t.why, " more");
    }
    conf_buf_str(&t.note, "unknown ids are AAR_E_INVAL, closed ids are refused");
    conf_end(&t);
}

int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    make_plain();
    probe_codecs();
    test_sha();
    test_decompress();
    test_bad_algorithm();
    test_bad_streams();
    test_png();
    test_jpeg_webp();
    test_audio_codecs();
    test_session_count();
    test_session_ids();
    return conf_finish("C-SERVICES");
}
