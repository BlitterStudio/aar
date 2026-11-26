/*
 * The host environment of the core: wall-clock time, locale, environment
 * variables and the clipboard (spec 7.3 and 7.4). A target's client serves
 * these from dos.library, locale.library and clipboard.device; the runner
 * answers from the host.
 *
 * time_unix_us is the host's real-time clock. locale_info reports the UTC
 * offset the host's time zone has now, the language and country of the
 * environment (LC_ALL, LC_MESSAGES, LANG; on Windows the user locale when none
 * is set) in the names an Amiga uses, and the decimal point and group
 * separator of the numeric locale. env_get reads the process environment, or
 * with --env DIR a file DIR/<name>, as ENV: holds a variable. The clipboard is
 * the SDL text clipboard: unit 0 and AAR_CLIP_TEXT only.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

/* ---- time ----------------------------------------------------------------- */

static int api_time_unix_us(int64_t *out_us)
{
    if (!out_us)
        return AAR_E_INVAL;
#ifdef _WIN32
    {
        FILETIME ft;
        ULARGE_INTEGER ticks;

        GetSystemTimeAsFileTime(&ft);
        ticks.LowPart = ft.dwLowDateTime;
        ticks.HighPart = ft.dwHighDateTime;
        /* 100 ns ticks since 1601, to microseconds since 1970. */
        *out_us = (int64_t)((ticks.QuadPart - 116444736000000000ull) / 10ull);
    }
#else
    {
        struct timespec ts;

        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return AAR_E_IO;
        *out_us = (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
    }
#endif
    return AAR_OK;
}

/* ---- locale --------------------------------------------------------------- */

struct name_map {
    const char *from;
    const char *to;
};

/* ISO 639-1 to the name of an Amiga language, spelled without accents. */
static const struct name_map languages[] = {
    {"en", "english"},   {"de", "deutsch"},   {"fr", "francais"},
    {"es", "espanol"},   {"it", "italiano"},  {"nl", "nederlands"},
    {"sv", "svenska"},   {"da", "dansk"},     {"no", "norsk"},
    {"nb", "norsk"},     {"nn", "norsk"},     {"fi", "suomi"},
    {"pt", "portugues"}, {"pl", "polski"},
};

/* ISO 3166-1 alpha-2 to the country code of an Amiga locale where it differs.
 * Any other country is reported as its ISO code. */
static const struct name_map countries[] = {
    {"DE", "D"},   {"AT", "A"},   {"FR", "F"},   {"IT", "I"},   {"ES", "E"},
    {"US", "USA"}, {"BE", "B"},   {"SE", "S"},   {"NO", "N"},   {"FI", "FIN"},
    {"PT", "P"},   {"CA", "CDN"}, {"AU", "AUS"}, {"IE", "IRL"},
};

static const char *lookup(const struct name_map *map, size_t n,
                          const char *from)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strcmp(map[i].from, from) == 0)
            return map[i].to;
    }
    return 0;
}

/* The locale name of the environment: LC_ALL, then LC_MESSAGES, then LANG. */
static const char *locale_name(char *buf, size_t n)
{
    static const char *const vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    size_t i;

    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);

        if (v && v[0])
            return v;
    }
#ifdef _WIN32
    {
        WCHAR wide[LOCALE_NAME_MAX_LENGTH];

        if (GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH) > 0 &&
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, (int)n, NULL,
                                NULL) > 0)
            return buf;
    }
#else
    (void)buf;
    (void)n;
#endif
    return "";
}

/* "de_DE.UTF-8@euro" and "de-DE" give "de" and "DE". */
static void split_locale(const char *name, char lang[8], char country[8])
{
    size_t i = 0;

    lang[0] = 0;
    country[0] = 0;
    while (name[i] && i < 7u &&
           ((name[i] >= 'a' && name[i] <= 'z') ||
            (name[i] >= 'A' && name[i] <= 'Z'))) {
        lang[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32
                                                            : name[i]);
        i++;
    }
    lang[i] = 0;
    if (name[i] == '_' || name[i] == '-') {
        size_t j = 0;

        i++;
        while (name[i] && j < 7u &&
               ((name[i] >= 'a' && name[i] <= 'z') ||
                (name[i] >= 'A' && name[i] <= 'Z'))) {
            country[j++] = (char)((name[i] >= 'a' && name[i] <= 'z')
                                      ? name[i] - 32
                                      : name[i]);
            i++;
        }
        country[j] = 0;
    }
}

/* Days from 1970-01-01 to a civil date (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    int64_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t tm_seconds(const struct tm *t)
{
    return days_from_civil((int64_t)t->tm_year + 1900, (int64_t)t->tm_mon + 1,
                           (int64_t)t->tm_mday) * 86400 +
           (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 +
           (int64_t)t->tm_sec;
}

/* Minutes east of UTC that the host's time zone has now. */
static int32_t utc_offset_minutes(void)
{
    time_t now = time(NULL);
    struct tm local;
    struct tm *p;

    p = localtime(&now);
    if (!p)
        return 0;
    local = *p;
    p = gmtime(&now);
    if (!p)
        return 0;
    return (int32_t)((tm_seconds(&local) - tm_seconds(p)) / 60);
}

/* A separator that fits in four bytes with the NUL and is valid UTF-8, else
 * the fallback. */
static void copy_utf8(char dst[4], const char *src, const char *fallback)
{
    const unsigned char *s = (const unsigned char *)src;
    size_t n = src ? strlen(src) : 0;
    size_t i = 0;
    int ok = n <= 3u;

    while (ok && i < n) {
        size_t len = s[i] < 0x80u ? 1u : (s[i] & 0xe0u) == 0xc0u ? 2u
                   : (s[i] & 0xf0u) == 0xe0u ? 3u : 0u;
        size_t k;

        if (len == 0u || i + len > n) {
            ok = 0;
            break;
        }
        for (k = 1; k < len; k++) {
            if ((s[i + k] & 0xc0u) != 0x80u)
                ok = 0;
        }
        i += len;
    }
    memcpy(dst, ok ? src : fallback, (ok ? n : strlen(fallback)) + 1u);
}

/* The decimal point and group separator of the numeric locale. The process
 * locale is left as it was. */
static void numeric_format(char dec[4], char grp[4])
{
    char saved[128] = "C";
    const char *old = setlocale(LC_NUMERIC, NULL);

    memcpy(dec, ".", 2);
    grp[0] = 0;
    if (old && strlen(old) < sizeof saved)
        memcpy(saved, old, strlen(old) + 1u);
    if (setlocale(LC_NUMERIC, "") != NULL) {
        const struct lconv *lc = localeconv();

        if (lc) {
            copy_utf8(dec, lc->decimal_point, ".");
            copy_utf8(grp, lc->thousands_sep, "");
            if (dec[0] == 0)
                memcpy(dec, ".", 2);
        }
    }
    setlocale(LC_NUMERIC, saved);
}

static int api_locale_info(aar_locale_info *out)
{
    aar_locale_info li;
    char wide_name[128];
    char lang[8];
    char country[8];
    const char *amiga;
    uint32_t size;

    if (!out)
        return AAR_E_INVAL;
    size = out->size;
    if (size < (uint32_t)sizeof out->size)
        return AAR_E_INVAL;
    memset(&li, 0, sizeof li);
    li.size = size;
    li.utc_offset_min = utc_offset_minutes();
    li.flags = AAR_LOCALEF_OFFSET_KNOWN;
    split_locale(locale_name(wide_name, sizeof wide_name), lang, country);
    amiga = lookup(languages, sizeof languages / sizeof languages[0], lang);
    if (amiga)
        memcpy(li.language, amiga, strlen(amiga) + 1u);
    if (country[0]) {
        amiga = lookup(countries, sizeof countries / sizeof countries[0],
                       country);
        if (!amiga)
            amiga = country;
        memcpy(li.country, amiga, strlen(amiga) + 1u);
    }
    numeric_format(li.decimal_point, li.group_separator);
    memcpy(out, &li, size < sizeof li ? size : sizeof li);
    if (size > sizeof li)
        memset((uint8_t *)out + sizeof li, 0, size - sizeof li);
    return AAR_OK;
}

/* ---- environment ---------------------------------------------------------- */

/* Copies a value of n bytes the way aar_env_get promises. */
static int give_value(const void *value, size_t n, char *buf, uint32_t bytes,
                      uint32_t *out_n)
{
    size_t take = n < bytes ? n : bytes;

    if (n > 0xffffffffu)
        return AAR_E_IO;
    *out_n = (uint32_t)n;
    if (take)
        memcpy(buf, value, take);
    if (bytes > n) {
        buf[n] = 0;
        return AAR_OK;
    }
    return AAR_E_NOSPC;
}

/* A name that leaves the directory is refused as a path would be: a ':' or a
 * backslash, a leading or doubled '/', or a '.' or '..' component. */
static int env_name_ok(const char *name)
{
    const char *p = name;

    if (strchr(name, ':') || strchr(name, '\\'))
        return 0;
    for (;;) {
        size_t len = strcspn(p, "/");

        if (len == 0u || (len == 1u && p[0] == '.') ||
            (len == 2u && p[0] == '.' && p[1] == '.'))
            return 0;
        p += len;
        if (*p == 0)
            return 1;
        p++;
    }
}

static int env_from_dir(const char *dir, const char *name, char *buf,
                        uint32_t bytes, uint32_t *out_n)
{
    char path[4096];
    struct stat st;
    FILE *fp;
    size_t take;
    char *tmp;
    int rc;

    if (!env_name_ok(name))
        return AAR_E_ACCES;
    if (strlen(dir) + strlen(name) + 2u > sizeof path)
        return AAR_E_INVAL;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
        return AAR_E_NOENT;
    if ((uint64_t)st.st_size > 0xffffffffull)
        return AAR_E_IO;
    fp = fopen(path, "rb");
    if (!fp)
        return AAR_E_NOENT;
    take = (size_t)st.st_size < bytes ? (size_t)st.st_size : bytes;
    tmp = (char *)malloc(take ? take : 1u);
    if (!tmp) {
        fclose(fp);
        return AAR_E_NOMEM;
    }
    if (take && fread(tmp, 1, take, fp) != take) {
        free(tmp);
        fclose(fp);
        return AAR_E_IO;
    }
    fclose(fp);
    /* The value is NUL-terminated when it fits, so a file as long as the
     * buffer minus one leaves room, and one as long as the buffer does not. */
    rc = give_value(tmp, (size_t)st.st_size, buf, bytes, out_n);
    free(tmp);
    return rc;
}

static int api_env_get(const char *name, char *buf, uint32_t bytes,
                       uint32_t *out_n)
{
    const char *dir = host_options()->env_dir;
    size_t len;
    const char *value;

    if (!name || !out_n || (bytes && !buf))
        return AAR_E_INVAL;
    len = strlen(name);
    if (len == 0u || len > 255u)
        return AAR_E_INVAL;
    if (dir)
        return env_from_dir(dir, name, buf, bytes, out_n);
    value = getenv(name);
    if (!value)
        return AAR_E_NOENT;
    return give_value(value, strlen(value), buf, bytes, out_n);
}

int host_env_init(void)
{
    const char *dir = host_options()->env_dir;
    struct stat st;

    if (dir && (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))) {
        fprintf(stderr, "aar-host: --env %s is not a directory\n", dir);
        return -1;
    }
    return 0;
}

/* ---- clipboard ------------------------------------------------------------ */

static int clip_args_ok(uint32_t unit, uint32_t type)
{
    return unit < AAR_CLIP_UNITS &&
           (type == AAR_CLIP_TEXT || type == AAR_CLIP_IFF);
}

static int api_clip_read(uint32_t unit, uint32_t type, void *buf,
                         uint32_t bytes, uint32_t *out_n)
{
    char *text;
    size_t n;
    size_t take;

    if (!clip_args_ok(unit, type) || !out_n || (bytes && !buf))
        return AAR_E_INVAL;
    if (unit != 0u || type != AAR_CLIP_TEXT)
        return AAR_E_NOENT;
    if (!host_sdl_video_ok())
        return AAR_E_NOSYS;
    text = SDL_GetClipboardText();
    if (!text)
        return AAR_E_IO;
    n = strlen(text);
    if (n == 0u) {
        SDL_free(text);
        return AAR_E_NOENT;
    }
    if (n > 0xffffffffu) {
        SDL_free(text);
        return AAR_E_IO;
    }
    take = n < bytes ? n : bytes;
    if (take)
        memcpy(buf, text, take);
    SDL_free(text);
    *out_n = (uint32_t)n;
    return take == n ? AAR_OK : AAR_E_NOSPC;
}

static int api_clip_write(uint32_t unit, uint32_t type, const void *data,
                          uint32_t bytes)
{
    char *text;
    int rc;

    if (!clip_args_ok(unit, type) || (bytes && !data))
        return AAR_E_INVAL;
    if (unit != 0u || type != AAR_CLIP_TEXT || !host_sdl_video_ok())
        return AAR_E_NOSYS;
    text = (char *)malloc((size_t)bytes + 1u);
    if (!text)
        return AAR_E_NOMEM;
    if (bytes)
        memcpy(text, data, bytes);
    text[bytes] = 0; /* SDL takes a C string: the text ends at a NUL byte */
    rc = SDL_SetClipboardText(text) == 0 ? AAR_OK : AAR_E_IO;
    free(text);
    return rc;
}

void host_env_bind(aar_api *api)
{
    api->time_unix_us = api_time_unix_us;
    api->locale_info = api_locale_info;
    api->env_get = api_env_get;
    api->clip_read = api_clip_read;
    api->clip_write = api_clip_write;
}
