#!/usr/bin/env python3
# Generates conformance/payloads/conf_services_vec.h: the test vectors of the
# C-SERVICES payload (compressed streams, images, audio, digests). The header is
# committed; run this script only to change the vectors. It needs Pillow (JPEG,
# WebP and PNG), lz4 is not used (the LZ4 block is made here), and ffmpeg with
# libmp3lame and libvorbis (MP3, Vorbis and FLAC), and one LHa archive per
# method made with LHa for UNIX 1.14i (https://github.com/jca02266/lha):
#
#   python3 plain.py > plain.bin   (this script writes plain.bin with --plain)
#   lha a0o  lh1.lzh plain.bin     (lh1)
#   lha a0o5 lh5.lzh plain.bin     (lh5; o6 for lh6, o7 for lh7)
#
#   gen_service_vectors.py --plain DIR          write DIR/plain.bin
#   gen_service_vectors.py --lzh DIR --out FILE read DIR/lh1.lzh .. lh7.lzh
#
# The formulas for the plain text, the images and the audio are the same as in
# conf_services.c.
#
# SPDX-License-Identifier: MIT
import argparse
import gzip
import hashlib
import io
import lzma
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib

from PIL import Image

WORDS = [b"alpha ", b"beta ", b"gamma ", b"delta ", b"amiga ", b"arm ", b"zz9000 ", b"runtime\n"]
PLAIN_BYTES = 1000


def plain(n=PLAIN_BYTES):
    x = 12345
    out = bytearray()
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += WORDS[(x >> 16) & 7]
    return bytes(out[:n])


def sha_byte(i):
    return (i * 31 + (i >> 8) * 7 + 3) & 0xFF


# ---- LZ4 block (greedy, no dependency) ------------------------------------------------------

def lz4_block(data):
    out = bytearray()
    n = len(data)
    table = {}
    anchor = 0
    i = 0
    while i + 4 <= n - 5:  # the last 5 bytes are literals
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand < 65536:
            m = 4
            while i + m < n - 5 and data[cand + m] == data[i + m]:
                m += 1
            lit = data[anchor:i]
            token_lit = min(len(lit), 15)
            token_m = min(m - 4, 15)
            out.append((token_lit << 4) | token_m)
            if len(lit) >= 15:
                rest = len(lit) - 15
                while rest >= 255:
                    out.append(255)
                    rest -= 255
                out.append(rest)
            out += lit
            out += struct.pack("<H", i - cand)
            if m - 4 >= 15:
                rest = m - 4 - 15
                while rest >= 255:
                    out.append(255)
                    rest -= 255
                out.append(rest)
            i += m
            anchor = i
        else:
            i += 1
    lit = data[anchor:]
    token_lit = min(len(lit), 15)
    out.append(token_lit << 4)
    if len(lit) >= 15:
        rest = len(lit) - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    out += lit
    return bytes(out)


def lz4_unblock(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        m = token & 15
        if m == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        m += 4
        for _ in range(m):
            out.append(out[-off])
    assert len(out) == size
    return bytes(out)


def lzh_stream(path, method):
    d = open(path, "rb").read()
    assert d[2:7] == b"-lh%s-" % method, d[2:7]
    hdr = d[0] + 2
    packed = struct.unpack("<I", d[7:11])[0]
    orig = struct.unpack("<I", d[11:15])[0]
    return d[hdr:hdr + packed], orig


# ---- Images ---------------------------------------------------------------------------------------

PNG_W, PNG_H = 24, 16
SMOOTH = 16


def png_px(x, y):
    return ((x * 9 + y * 5) & 255, (x * y * 3 + 17) & 255, ((x ^ y) * 13) & 255)


def smooth_px(x, y):
    return (x * 14, y * 14, (x + y) * 7)


def make_images(tmp):
    png = Image.new("RGB", (PNG_W, PNG_H))
    for y in range(PNG_H):
        for x in range(PNG_W):
            png.putpixel((x, y), png_px(x, y))
    b = io.BytesIO()
    png.save(b, "PNG", optimize=True)
    png_bytes = b.getvalue()
    assert Image.open(io.BytesIO(png_bytes)).convert("RGB").tobytes() == png.tobytes()

    sm = Image.new("RGB", (SMOOTH, SMOOTH))
    for y in range(SMOOTH):
        for x in range(SMOOTH):
            sm.putpixel((x, y), smooth_px(x, y))
    b = io.BytesIO()
    sm.save(b, "JPEG", quality=92, subsampling=0, optimize=True)
    jpg = b.getvalue()
    jpg_ref = Image.open(io.BytesIO(jpg)).convert("RGB").tobytes()
    b = io.BytesIO()
    sm.save(b, "WEBP", quality=92, method=6)
    webp = b.getvalue()
    webp_ref = Image.open(io.BytesIO(webp)).convert("RGB").tobytes()
    return png_bytes, jpg, jpg_ref, webp, webp_ref


# ---- Audio ----------------------------------------------------------------------------------------

FLAC_FRAMES = 768
FLAC_RATE = 22050


def flac_sample(ch, n):
    k = n + 7 * ch
    a = abs((k * 6) % 4000 - 2000) - 1000
    b = 500 if (k >> 6) & 1 else -500
    return a * 16 + b


def tone(n, rate):
    v = 9000 * math.sin(2 * math.pi * 440.0 * n / rate) + 3000 * math.sin(2 * math.pi * 1250.0 * n / rate)
    return int(round(v))


def ffmpeg(args, **kw):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] + args, check=True, **kw)


def flac_streaminfo_only(data):
    """Drops every metadata block but STREAMINFO (ffmpeg adds an 8 KiB padding block)."""
    assert data[:4] == b"fLaC"
    i = 4
    keep = None
    while True:
        h = data[i]
        ln = int.from_bytes(data[i + 1:i + 4], "big")
        if (h & 0x7F) == 0:
            keep = data[i + 4:i + 4 + ln]
        i += 4 + ln
        if h & 0x80:
            break
    assert keep is not None and len(keep) == 34
    return b"fLaC" + bytes([0x80]) + (34).to_bytes(3, "big") + keep + data[i:]


def make_audio(tmp):
    # FLAC: stereo, exact.
    pcm = b"".join(struct.pack("<hh", flac_sample(0, n), flac_sample(1, n)) for n in range(FLAC_FRAMES))
    raw = os.path.join(tmp, "flac.raw")
    open(raw, "wb").write(pcm)
    out = os.path.join(tmp, "t.flac")
    ffmpeg(["-f", "s16le", "-ar", str(FLAC_RATE), "-ac", "2", "-i", raw, "-c:a", "flac",
            "-map_metadata", "-1", "-fflags", "+bitexact", out])
    flac = flac_streaminfo_only(open(out, "rb").read())

    # MP3: MPEG-1 layer 3, 44100 Hz, mono, 64 kbit/s, two frames.
    rate = 44100
    n = 1152 * 4
    raw = os.path.join(tmp, "mp3.raw")
    open(raw, "wb").write(b"".join(struct.pack("<h", tone(i, rate)) for i in range(n)))
    out = os.path.join(tmp, "t.mp3")
    ffmpeg(["-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", raw, "-c:a", "libmp3lame", "-b:a", "64k",
            "-write_xing", "0", "-id3v2_version", "0", "-write_id3v2", "0", "-fflags", "+bitexact", out])
    mp3 = open(out, "rb").read()
    # Cut at frame boundaries: the first two frames of the file.
    frames = []
    i = 0
    while i + 4 <= len(mp3):
        h = mp3[i:i + 4]
        assert h[0] == 0xFF and (h[1] & 0xE0) == 0xE0, "lost sync"
        kbps = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320][h[2] >> 4]
        pad = (h[2] >> 1) & 1
        ln = 144 * kbps * 1000 // 44100 + pad
        frames.append(mp3[i:i + ln])
        i += ln
    use = frames[0:2]
    mp3_cut = b"".join(use)
    open(out, "wb").write(mp3_cut)
    refraw = os.path.join(tmp, "mp3_ref.raw")
    ffmpeg(["-i", out, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", refraw])
    mp3_ref = struct.unpack("<%dh" % (os.path.getsize(refraw) // 2), open(refraw, "rb").read())
    assert len(mp3_ref) == 1152 * len(use), (len(mp3_ref), len(use))

    # Vorbis: mono, 22050 Hz.
    rate = 22050
    n = 1536
    raw = os.path.join(tmp, "ogg.raw")
    open(raw, "wb").write(b"".join(struct.pack("<h", tone(i, rate)) for i in range(n)))
    out = os.path.join(tmp, "t.ogg")
    ffmpeg(["-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", raw, "-c:a", "libvorbis", "-q:a", "3",
            "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact", out])
    ogg = open(out, "rb").read()
    refraw = os.path.join(tmp, "ogg_ref.raw")
    ffmpeg(["-i", out, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", refraw])
    ogg_ref = struct.unpack("<%dh" % (os.path.getsize(refraw) // 2), open(refraw, "rb").read())
    return flac, mp3_cut, [len(f) for f in use], mp3_ref, ogg, ogg_ref


# ---- Output ---------------------------------------------------------------------------------------

def c_bytes(name, data, comment=""):
    lines = []
    if comment:
        lines.append("/* %s */" % comment)
    lines.append("static const uint8_t %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_shorts(name, vals, comment=""):
    lines = []
    if comment:
        lines.append("/* %s */" % comment)
    lines.append("static const int16_t %s[%d] = {" % (name, len(vals)))
    for i in range(0, len(vals), 12):
        lines.append("    " + ", ".join("%d" % v for v in vals[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plain", metavar="DIR")
    ap.add_argument("--lzh", metavar="DIR")
    ap.add_argument("--out", metavar="FILE")
    args = ap.parse_args()
    if args.plain:
        open(os.path.join(args.plain, "plain.bin"), "wb").write(plain())
        return
    if not args.lzh or not args.out:
        ap.error("--lzh and --out are needed")
    p = plain()
    o = []
    o.append("""/*
 * Test vectors of the C-SERVICES payload (conf_services.c). Generated by
 * conformance/tools/gen_service_vectors.py; do not edit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_SERVICES_VEC_H
#define AAR_CONF_SERVICES_VEC_H

#include <stdint.h>
""")
    o.append("#define VEC_PLAIN_BYTES %d" % len(p))
    # Digests.
    o.append("""
/* SHA-256 of the first n bytes of sha_byte(i) = i * 31 + (i >> 8) * 7 + 3. */
#define VEC_SHA_COUNT 11""")
    lens = [0, 1, 55, 56, 63, 64, 65, 119, 120, 1000, 1048576]
    rows = []
    for n in lens:
        d = hashlib.sha256(bytes(sha_byte(i) for i in range(n))).digest()
        rows.append("    { %du, { %s } }," % (n, ", ".join("0x%02x" % b for b in d)))
    o.append("static const struct { uint32_t bytes; uint8_t digest[32]; } vec_sha[VEC_SHA_COUNT] = {\n" +
             "\n".join(rows) + "\n};")
    o.append(c_bytes("vec_sha_abc", hashlib.sha256(b"abc").digest(), "SHA-256 of \"abc\""))
    o.append(c_bytes("vec_sha_empty", hashlib.sha256(b"").digest(), "SHA-256 of the empty message"))
    # Compression.
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    deflate = co.compress(p) + co.flush()
    assert zlib.decompress(deflate, -15) == p
    z = zlib.compress(p, 9)
    gz = gzip.compress(p, 9, mtime=0)
    lz4 = lz4_block(p)
    assert lz4_unblock(lz4, len(p)) == p
    lzma_alone = lzma.compress(p, format=lzma.FORMAT_ALONE,
                               filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "preset": 6}])
    assert lzma.decompress(lzma_alone, format=lzma.FORMAT_ALONE) == p
    flt = [{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 16, "preset": 6}]
    lzma2 = bytes([8]) + lzma.compress(p, format=lzma.FORMAT_RAW, filters=flt)
    assert lzma.decompress(lzma2[1:], format=lzma.FORMAT_RAW, filters=flt) == p
    o.append("\n/* The plain text is VEC_PLAIN_BYTES bytes (conf_services.c makes it with the same\n"
             " * formula as the generator). Each stream below decodes to it. */")
    o.append(c_bytes("vec_deflate", deflate, "raw deflate"))
    o.append(c_bytes("vec_zlib", z, "zlib"))
    o.append(c_bytes("vec_gzip", gz, "gzip, mtime 0"))
    o.append(c_bytes("vec_lz4", lz4, "one LZ4 block (no size prefix)"))
    o.append(c_bytes("vec_lzma_alone", lzma_alone, "LZMA alone (.lzma), end marker, unknown size"))
    o.append(c_bytes("vec_lzma2", lzma2, "one byte of LZMA2 properties (dictionary 64 KiB), then the raw LZMA2 stream"))
    for m, name in ((b"1", "lh1"), (b"5", "lh5"), (b"6", "lh6"), (b"7", "lh7")):
        s, orig = lzh_stream(os.path.join(args.lzh, "%s.lzh" % name), m)
        assert orig == len(p)
        o.append(c_bytes("vec_" + name, s, "LHA -%s- data of a member, no header; the decode length is the\n"
                         " * destination size" % name))
    # Images.
    with tempfile.TemporaryDirectory() as tmp:
        png, jpg, jpg_ref, webp, webp_ref = make_images(tmp)
        flac, mp3, mp3_lens, mp3_ref, ogg, ogg_ref = make_audio(tmp)
    o.append("\n#define VEC_PNG_W %d\n#define VEC_PNG_H %d" % (PNG_W, PNG_H))
    o.append("#define VEC_SMOOTH %d" % SMOOTH)
    o.append(c_bytes("vec_png", png, "PNG, 24x16 RGB; pixel (x, y) is png_px of conf_services.c"))
    o.append(c_bytes("vec_jpeg", jpg, "JPEG, 16x16, 4:4:4, quality 92"))
    o.append(c_bytes("vec_jpeg_ref", jpg_ref, "its pixels (RGB) as libjpeg decodes them"))
    o.append(c_bytes("vec_webp", webp, "WebP, 16x16, lossy"))
    o.append(c_bytes("vec_webp_ref", webp_ref, "its pixels (RGB) as libwebp decodes them"))
    # Audio.
    o.append("\n#define VEC_FLAC_FRAMES %d\n#define VEC_FLAC_RATE %d" % (FLAC_FRAMES, FLAC_RATE))
    o.append(c_bytes("vec_flac", flac, "FLAC, stereo, samples flac_sample(ch, n) of conf_services.c"))
    o.append("#define VEC_MP3_FRAMES %d" % len(mp3_lens))
    o.append("static const uint32_t vec_mp3_len[VEC_MP3_FRAMES] = { %s };" % ", ".join("%du" % x for x in mp3_lens))
    o.append(c_bytes("vec_mp3", mp3, "MPEG-1 layer 3, 44100 Hz, mono, 64 kbit/s, whole frames"))
    o.append(c_shorts("vec_mp3_ref", mp3_ref, "what ffmpeg decodes from vec_mp3, mono"))
    o.append("#define VEC_OGG_RATE 22050\n#define VEC_OGG_REF_FRAMES %d" % len(ogg_ref))
    o.append(c_bytes("vec_ogg", ogg, "Ogg Vorbis, mono, 22050 Hz"))
    o.append(c_shorts("vec_ogg_ref", ogg_ref, "what ffmpeg decodes from vec_ogg"))
    o.append("\n#endif\n")
    with open(args.out, "w", newline="\n", encoding="utf-8") as fh:
        fh.write("\n".join(o))
    print("wrote %s: png %d jpeg %d webp %d flac %d mp3 %d (frames %s, ref %d) ogg %d (ref %d)" % (
        args.out, len(png), len(jpg), len(webp), len(flac), len(mp3), mp3_lens, len(mp3_ref), len(ogg), len(ogg_ref)))


if __name__ == "__main__":
    main()
