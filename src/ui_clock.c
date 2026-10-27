/* Status-bar clock logic: city table lookup, setting persistence, rendering.
 * No LVGL here on purpose -- see ui_clock.h.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui_clock.h"
#include "ui_clock_tz.h"

/* Overridden by tests so they never touch a real /data. */
#ifndef UI_CLOCK_DATA_DIR
#define UI_CLOCK_DATA_DIR "/data"
#endif
#define TZ_PATH  UI_CLOCK_DATA_DIR "/erosq-ui.tz"
#define FMT_PATH UI_CLOCK_DATA_DIR "/erosq-ui.clockfmt"

/* 2020-01-01T00:00Z. Below this the RTC was never NTP-synced (wifi_connect.sh
 * runs ntpd on connect), so we show nothing rather than a wrong time. */
#define EPOCH_2020 1577836800L

/* Cached settings. -1 = not yet loaded. Mirrors ui_power.c. */
static int s_tz  = -1;
static int s_fmt = -1;

/* --- generated city table --------------------------------------------- */

int ui_clock_tz_count(void) { return UI_CLOCK_TZ_COUNT; }

static int in_range(int idx) { return idx >= 0 && idx < UI_CLOCK_TZ_COUNT; }

int ui_clock_tz_utc(void)
{
    static int idx = -1;
    if (idx < 0) {
        idx = 0;   /* the generator guarantees a UTC row; don't trust it blindly */
        for (int i = 0; i < UI_CLOCK_TZ_COUNT; i++)
            if (strcmp(ui_clock_tz[i].code, "UTC") == 0) { idx = i; break; }
    }
    return idx;
}

const char *ui_clock_tz_code(int idx) { return in_range(idx) ? ui_clock_tz[idx].code : NULL; }
const char *ui_clock_tz_name(int idx) { return in_range(idx) ? ui_clock_tz[idx].name : NULL; }

/* Never NULL: the result goes straight to setenv(), so a bad index has to
 * degrade to a usable rule rather than crash the caller. */
const char *ui_clock_tz_rule(int idx)
{
    if (!in_range(idx)) idx = ui_clock_tz_utc();
    return ui_clock_tz[idx].tz;
}

int ui_clock_std_offset_label(const char *rule, char *buf, size_t n)
{
    if (!buf || n == 0) return -1;
    buf[0] = '\0';
    if (!rule) return -1;

    /* std name: <quoted> or 3+ letters (POSIX.1-2017 TZ, first form). */
    const char *p = rule;
    if (*p == '<') {
        p = strchr(p, '>');
        if (!p) return -1;
        p++;
    } else {
        const char *s = p;
        while (isalpha((unsigned char)*p)) p++;
        if (p - s < 3) return -1;
    }

    /* offset: [+|-]hh[:mm[:ss]], west-positive. */
    int west = 1;
    if (*p == '+' || *p == '-') { west = (*p == '+'); p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    int h = 0, m = 0, digits = 0;
    while (isdigit((unsigned char)*p) && digits < 2) { h = h * 10 + (*p++ - '0'); digits++; }
    if (isdigit((unsigned char)*p) || h > 24) return -1;
    if (*p == ':') {
        p++;
        if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
        m = (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
        if (m > 59) return -1;
        if (*p == ':') {   /* seconds: no real zone uses them; accept, don't show */
            p++;
            if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
            p += 2;
        }
    }

    char sign = (h == 0 && m == 0) ? '+' : (west ? '-' : '+');
    int r = m ? snprintf(buf, n, "UTC%c%d:%02d", sign, h, m)
              : snprintf(buf, n, "UTC%c%d", sign, h);
    if (r < 0 || (size_t)r >= n) { buf[0] = '\0'; return -1; }
    return 0;
}

int ui_clock_tz_find(const char *code)
{
    if (!code || !*code) return -1;
    for (int i = 0; i < UI_CLOCK_TZ_COUNT; i++)
        if (strcmp(ui_clock_tz[i].code, code) == 0) return i;
    return -1;
}

/* --- resolving persisted contents -------------------------------------- */

int ui_clock_tz_resolve(const char *raw)
{
    char code[8];
    size_t n = 0;

    if (raw) {
        while (*raw && isspace((unsigned char)*raw)) raw++;
        while (n < sizeof code - 1 && raw[n] && !isspace((unsigned char)raw[n])) {
            code[n] = raw[n];
            n++;
        }
    }
    code[n] = '\0';

    int idx = ui_clock_tz_find(code);
    return idx >= 0 ? idx : ui_clock_tz_utc();
}

int ui_clock_fmt_resolve(const char *raw)
{
    if (!raw) return UI_CLOCK_FMT_DEFAULT;

    char *end;
    long v = strtol(raw, &end, 10);
    if (end == raw) return UI_CLOCK_FMT_DEFAULT;            /* no digits */
    if (v < 0 || v >= UI_CLOCK_FMT_N) return UI_CLOCK_FMT_DEFAULT;
    return (int)v;
}

/* --- tiny /data helpers ------------------------------------------------ */

/* Returns 1 and a NUL-terminated buf on success, 0 if unreadable or empty. */
static int read_setting(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    size_t off = 0;
    while (off < n - 1) {
        ssize_t r = read(fd, buf + off, n - 1 - off);
        if (r < 0) { close(fd); return 0; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = '\0';
    return off > 0;
}

static void write_setting(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;   /* ponytail: no /data -> just don't persist */

    size_t len = strlen(val), off = 0;
    while (off < len) {
        ssize_t w = write(fd, val + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
}

/* --- settings --------------------------------------------------------- */

int ui_clock_tz_get(void)
{
    if (s_tz < 0) {
        char buf[16];
        s_tz = ui_clock_tz_resolve(read_setting(TZ_PATH, buf, sizeof buf) ? buf : NULL);
    }
    return s_tz;
}

void ui_clock_tz_set(int idx)
{
    if (!in_range(idx)) return;   /* never persist an index we can't resolve back */
    s_tz = idx;
    write_setting(TZ_PATH, ui_clock_tz[idx].code);
    ui_clock_apply_tz();
}

void ui_clock_apply_tz(void)
{
    setenv("TZ", ui_clock_tz_rule(ui_clock_tz_get()), 1);
    tzset();
}

int ui_clock_fmt_get(void)
{
    if (s_fmt < 0) {
        char buf[16];
        s_fmt = ui_clock_fmt_resolve(read_setting(FMT_PATH, buf, sizeof buf) ? buf : NULL);
    }
    return s_fmt;
}

void ui_clock_fmt_set(int fmt)
{
    if (fmt < 0 || fmt >= UI_CLOCK_FMT_N) return;
    s_fmt = fmt;
    char buf[8];
    snprintf(buf, sizeof buf, "%d", fmt);
    write_setting(FMT_PATH, buf);
}

/* --- rendering -------------------------------------------------------- */

void ui_clock_format(time_t t, int fmt, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = '\0';

    /* Off, or a corrupt mode -- render nothing. */
    if (fmt < 0 || fmt >= UI_CLOCK_FMT_OFF) return;
    if (t < EPOCH_2020) return;

    struct tm tm;
    if (!localtime_r(&t, &tm)) return;

    /* No strftime: musl has no %l (glibc extension), and %I zero-pads. Doing
     * the 12-hour conversion here keeps host and target identical. */
    int h12 = tm.tm_hour % 12;
    if (h12 == 0) h12 = 12;   /* 00:xx and 12:xx both display as 12 */

    switch (fmt) {
    case UI_CLOCK_FMT_24:
        snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
        break;
    case UI_CLOCK_FMT_12:
        snprintf(buf, n, "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        break;
    case UI_CLOCK_FMT_12_NOAP:
        snprintf(buf, n, "%d:%02d", h12, tm.tm_min);
        break;
    }
}

void ui_clock_date(time_t t, char *buf, size_t n)
{
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!buf || n == 0) return;
    buf[0] = '\0';
    struct tm tm;
    if (t < EPOCH_2020 || !localtime_r(&t, &tm)) return;
    /* by hand, like ui_clock_format: no locale, and %e space-pads */
    snprintf(buf, n, "%s, %s %d", wd[tm.tm_wday], mo[tm.tm_mon], tm.tm_mday);
}

void ui_clock_now(char *buf, size_t n)
{
    ui_clock_format(time(NULL), ui_clock_fmt_get(), buf, n);
}
