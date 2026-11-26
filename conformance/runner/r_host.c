/*
 * Runner side of C-HOST (conformance/TESTS.md): the client of the payload conf_host. It
 * prepares what the payload reads (global ENV: variables, a variable local to the runner's
 * process, an IFF FTXT on clipboard unit 201), removes it after the run, and is the peer of
 * the tests that compare with the client's own view: the locale (C-HOST.3) and what the app
 * wrote to clipboard unit 202 (C-HOST.14). conf_host.h lists the fixtures and the keywords.
 *
 * Assumptions the card run confirms: the client reads the locale with OpenLocale(NULL) and
 * the clipboard through clipboard.device as IFF FTXT with CHRS chunks in ISO-8859-1; the
 * variables are plain ENV: files.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <devices/clipboard.h>
#include <dos/var.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <libraries/locale.h>
#include <proto/locale.h>

#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_host.h"

struct LocaleBase *LocaleBase;

static void send_result(struct conf_run *run, ULONG ok, ULONG count, ULONG errors)
{
    AARMessage am;
    int tries;

    cm_make_result(am.bytes, ok, count, errors, CM_NONE, 0, 0);
    for (tries = 0; tries < 100; tries++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            return;
        Delay(1);
    }
}

/* ---- Locale ------------------------------------------------------------------------- */

static ULONG hash_str(const char *s)
{
    ULONG h = 2166136261UL;

    while (*s) {
        h ^= (UBYTE)*s++;
        h *= 16777619UL;
    }
    return h;
}

/* The language name without the suffix ".language", in a buffer of its own. */
static void language_name(const struct Locale *loc, char *dst, ULONG size)
{
    const char *name = loc->loc_LanguageName ? (const char *)loc->loc_LanguageName : "";
    size_t n = strlen(name);

    if (n > 9 && strcmp(name + n - 9, ".language") == 0)
        n -= 9;
    if (n >= size)
        n = size - 1;
    memcpy(dst, name, n);
    dst[n] = '\0';
}

/* Opens the default locale. NULL when locale.library is not there. */
static struct Locale *open_locale(void)
{
    if (!LocaleBase)
        LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38);
    return LocaleBase ? OpenLocale(NULL) : NULL;
}

/* CONF_HOST_LOCALE: a = utc_offset_min, b = hash of the language, c = flags. */
static void h_locale(struct conf_run *run, const AARMessage *msg)
{
    LONG offset = (LONG)cm_get32(msg->bytes, 20);
    ULONG lang = cm_get32(msg->bytes, 24);
    struct Locale *loc = open_locale();
    char name[40];
    LONG want;

    if (!loc) {
        send_result(run, 2, 0, 0);
        return;
    }
    /* locale.library keeps minutes west of UTC; the call reports minutes east. */
    want = -(LONG)loc->loc_GMTOffset;
    language_name(loc, name, sizeof name);
    send_result(run, (offset == want && hash_str(name) == lang) ? 1UL : 0UL, (ULONG)want, 0);
    CloseLocale(loc);
}

/* A separator of locale.library as the API reports it (spec 7.4): its bytes are ISO-8859-1 and the
 * record holds them as UTF-8 in a field of 4 bytes with the NUL, so room for 3 bytes and only whole
 * characters. The client reads at most 7 bytes of the native string (its copy is 8 bytes with the NUL).
 * The expected value is made from the native bytes here, not taken from the app's record. */
static void separator_utf8(const char *native, char dst[4])
{
    ULONG o = 0, i;

    for (i = 0; i < 7 && native[i]; i++) {
        UBYTE c = (UBYTE)native[i];

        if (c < 0x80) {
            if (o + 1 > 3)
                break;
            dst[o++] = (char)c;
        } else {
            if (o + 2 > 3)
                break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

/* CONF_HOST_LOC2: a = hash of the decimal point, b = hash of the group separator (both the UTF-8 of the
 * native separator, see separator_utf8), c = hash of the country
 * (0: the client reported none). The country is the non-zero bytes of loc_CountryCode, most significant
 * first (spec 7.4). */
static void h_loc2(struct conf_run *run, const AARMessage *msg)
{
    ULONG dec = cm_get32(msg->bytes, 20);
    ULONG grp = cm_get32(msg->bytes, 24);
    ULONG cty = cm_get32(msg->bytes, 28);
    struct Locale *loc = open_locale();
    const char *d;
    const char *g;
    char dec_utf8[4];
    char grp_utf8[4];
    char country[8];
    ULONG code, k, n = 0;

    if (!loc) {
        send_result(run, 2, 0, 0);
        return;
    }
    d = loc->loc_DecimalPoint ? (const char *)loc->loc_DecimalPoint : "";
    g = loc->loc_GroupSeparator ? (const char *)loc->loc_GroupSeparator : "";
    separator_utf8(d, dec_utf8);
    separator_utf8(g, grp_utf8);
    code = (ULONG)loc->loc_CountryCode;
    for (k = 0; k < 4; k++) {
        UBYTE ch = (UBYTE)(code >> (24 - 8 * k));

        if (ch)
            country[n++] = (char)ch;
    }
    country[n] = '\0';
    send_result(run, (hash_str(dec_utf8) == dec && hash_str(grp_utf8) == grp && (cty == 0UL || hash_str(country) == cty))
                         ? 1UL : 0UL, 0, 0);
    CloseLocale(loc);
}

/* ---- Clipboard ---------------------------------------------------------------------- */

struct clip {
    struct MsgPort *port;
    struct IOClipReq *io;
};

static int clip_open(struct clip *c, ULONG unit)
{
    c->port = CreateMsgPort();
    c->io = c->port ? (struct IOClipReq *)CreateIORequest(c->port, sizeof(struct IOClipReq)) : NULL;
    if (c->io && OpenDevice((CONST_STRPTR)"clipboard.device", unit, (struct IORequest *)c->io, 0) == 0)
        return 1;
    if (c->io)
        DeleteIORequest((struct IORequest *)c->io);
    if (c->port)
        DeleteMsgPort(c->port);
    c->io = NULL;
    c->port = NULL;
    return 0;
}

static void clip_close(struct clip *c)
{
    CloseDevice((struct IORequest *)c->io);
    DeleteIORequest((struct IORequest *)c->io);
    DeleteMsgPort(c->port);
}

/* Puts one IFF FTXT with a single CHRS chunk of `text` on the clipboard. */
static int clip_put_text(ULONG unit, const char *text)
{
    struct clip c;
    UBYTE buf[200];
    ULONG n = (ULONG)strlen(text);
    ULONG pad = n & 1UL;
    ULONG form = 4UL + 8UL + n + pad;   /* FTXT, CHRS and its size, the text, a pad byte */
    ULONG total = 8UL + form;

    if (total > sizeof buf || !clip_open(&c, unit))
        return 0;
    memcpy(buf, "FORM", 4);
    buf[4] = (UBYTE)(form >> 24); buf[5] = (UBYTE)(form >> 16); buf[6] = (UBYTE)(form >> 8); buf[7] = (UBYTE)form;
    memcpy(buf + 8, "FTXT", 4);
    memcpy(buf + 12, "CHRS", 4);
    buf[16] = (UBYTE)(n >> 24); buf[17] = (UBYTE)(n >> 16); buf[18] = (UBYTE)(n >> 8); buf[19] = (UBYTE)n;
    memcpy(buf + 20, text, n);
    if (pad)
        buf[20 + n] = 0;
    c.io->io_Command = CMD_WRITE;
    c.io->io_Data = (STRPTR)buf;
    c.io->io_Length = (LONG)total;
    c.io->io_Offset = 0;
    c.io->io_ClipID = 0;
    DoIO((struct IORequest *)c.io);
    c.io->io_Command = CMD_UPDATE;
    DoIO((struct IORequest *)c.io);
    clip_close(&c);
    return 1;
}

/* Reads the clip of a unit and joins the CHRS chunks of its FORM FTXT into text (NUL-terminated).
 * Returns the text length, or -1 when the device did not open or the clip is not an FTXT. */
static LONG clip_get_text(ULONG unit, char *text, ULONG size)
{
    struct clip c;
    UBYTE buf[400];
    ULONG got = 0;
    ULONG pos;
    ULONG out = 0;
    ULONG limit;

    if (!clip_open(&c, unit))
        return -1;
    for (;;) {
        c.io->io_Command = CMD_READ;
        c.io->io_Data = (STRPTR)(buf + got);
        c.io->io_Length = (LONG)(sizeof buf - got);
        c.io->io_Offset = (ULONG)got;
        c.io->io_ClipID = 0;
        if (DoIO((struct IORequest *)c.io) != 0 || c.io->io_Actual == 0)
            break;
        got += c.io->io_Actual;
        if (got >= sizeof buf) {
            /* Read to the end so the device lets go of the clip. */
            UBYTE rest[64];

            do {
                c.io->io_Data = (STRPTR)rest;
                c.io->io_Length = (LONG)sizeof rest;
                c.io->io_Offset = (ULONG)got;
                if (DoIO((struct IORequest *)c.io) != 0)
                    break;
                got += c.io->io_Actual;
            } while (c.io->io_Actual != 0);
            got = sizeof buf;
            break;
        }
    }
    clip_close(&c);
    if (got < 12 || memcmp(buf, "FORM", 4) != 0 || memcmp(buf + 8, "FTXT", 4) != 0)
        return -1;
    limit = 8UL + (((ULONG)buf[4] << 24) | ((ULONG)buf[5] << 16) | ((ULONG)buf[6] << 8) | (ULONG)buf[7]);
    if (limit > got)
        limit = got;
    pos = 12;
    while (pos + 8 <= limit) {
        ULONG len = ((ULONG)buf[pos + 4] << 24) | ((ULONG)buf[pos + 5] << 16) |
                    ((ULONG)buf[pos + 6] << 8) | (ULONG)buf[pos + 7];

        if (memcmp(buf + pos, "CHRS", 4) == 0) {
            ULONG k;

            for (k = 0; k < len && pos + 8 + k < limit && out + 1 < size; k++)
                text[out++] = (char)buf[pos + 8 + k];
        }
        pos += 8 + len + (len & 1UL);
    }
    text[out] = '\0';
    return (LONG)out;
}

/* CONF_HOST_CLIP: a = the unit the app wrote. */
static void h_clip(struct conf_run *run, const AARMessage *msg)
{
    ULONG unit = cm_get32(msg->bytes, 20);
    char text[200];
    LONG n = clip_get_text(unit, text, sizeof text);

    if (n < 0) {
        /* Either the device did not open or the clip is not an FTXT: ask the open separately. */
        struct clip c;

        if (!clip_open(&c, unit))
            send_result(run, 2, 0, 0);
        else {
            clip_close(&c);
            send_result(run, 0, 0, 0);
        }
        return;
    }
    send_result(run, (ULONG)n == strlen(CH_APP_LATIN1) && memcmp(text, CH_APP_LATIN1, (size_t)n) == 0 ? 1UL : 0UL,
                (ULONG)n, 0);
}

static const struct conf_peer peers[] = {
    { CH_K_LOCALE, h_locale },
    { CH_K_LOC2, h_loc2 },
    { CH_K_CLIP, h_clip },
};

/* ---- The variant main ---------------------------------------------------------------- */

static void remove_fixtures(struct conf_run *run)
{
    (void)run;
    DeleteVar((STRPTR)CH_ENV_HELLO, GVF_GLOBAL_ONLY);
    DeleteVar((STRPTR)CH_ENV_LONG, GVF_GLOBAL_ONLY);
    DeleteVar((STRPTR)CH_ENV_LOCAL, GVF_LOCAL_ONLY);
    if (LocaleBase) {
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = NULL;
    }
}

static void prepare_main(struct conf_run *run)
{
    char longv[CH_LONG_BYTES];
    ULONG i;

    for (i = 0; i < CH_LONG_BYTES; i++)
        longv[i] = (char)ch_long_byte((uint32_t)i);
    (void)SetVar((STRPTR)CH_ENV_HELLO, (STRPTR)CH_HELLO_VALUE, (LONG)CH_HELLO_BYTES, GVF_GLOBAL_ONLY);
    (void)SetVar((STRPTR)CH_ENV_LONG, (STRPTR)longv, (LONG)CH_LONG_BYTES, GVF_GLOBAL_ONLY);
    (void)SetVar((STRPTR)CH_ENV_LOCAL, (STRPTR)"local", 5, GVF_LOCAL_ONLY);
    if (!clip_put_text(CH_RUNNER_UNIT, CH_RUNNER_LATIN1))
        conf_emitf("# C-HOST: clipboard.device did not open; C-HOST.13 will fail");
    run->timeout_s = 60;
    run->on_finished = remove_fixtures;
}

static const struct conf_variant variants[] = {
    { CH_VARIANT_MAIN, prepare_main },
};

const struct conf_area conf_area_host = {
    .name = "C-HOST",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
