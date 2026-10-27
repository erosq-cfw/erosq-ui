#ifndef EROSQ_UI_CLOCK_H
#define EROSQ_UI_CLOCK_H

/* Status-bar clock: timezone table, format setting, and time rendering.
 *
 * Deliberately free of LVGL so tests/test_clock_logic.c can link it on the
 * host. The label itself lives with the rest of the status bar in ui_screens.c.
 *
 * Timezones are POSIX TZ strings (see src/ui_clock_tz.h, generated from tzdata
 * by tools/gen-tz-table.py) applied with setenv()+tzset(); the device ships no
 * zoneinfo. The RTC stays UTC -- only the display is localised.
 */

#include <stddef.h>
#include <time.h>

enum {
    UI_CLOCK_FMT_24 = 0,     /* 14:32 */
    UI_CLOCK_FMT_12,         /* 2:32 PM */
    UI_CLOCK_FMT_12_NOAP,    /* 2:32 */
    UI_CLOCK_FMT_OFF,        /* hidden */
    UI_CLOCK_FMT_N
};
#define UI_CLOCK_FMT_DEFAULT UI_CLOCK_FMT_24

/* Enough for "12:00 AM" plus NUL, with slack. */
#define UI_CLOCK_BUF 12

/* --- generated city table --------------------------------------------- */
int         ui_clock_tz_count(void);
const char *ui_clock_tz_code(int idx);   /* "LON"; NULL if out of range */
const char *ui_clock_tz_name(int idx);   /* "London"; NULL if out of range */
const char *ui_clock_tz_rule(int idx);   /* POSIX TZ; UTC's rule if out of range */
int         ui_clock_tz_find(const char *code);   /* index, or -1 if unknown */
int         ui_clock_tz_utc(void);                /* index of the UTC fallback row */

/* Standard (non-DST) UTC offset label for the picker, derived from the POSIX
 * TZ rule so there is no second table to keep in sync: "UTC-3", "UTC+5:30",
 * "UTC+0". POSIX offsets are west-positive, so "EST5EDT" -> "UTC-5". ASCII
 * '-' on purpose: the UI font carries no U+2212. Returns 0, or -1 (and an
 * empty buf) if the rule's std part doesn't parse. */
#define UI_CLOCK_OFS_BUF 12
int         ui_clock_std_offset_label(const char *rule, char *buf, size_t n);

/* --- settings (persisted under /data, defaults if absent) ------------- */
/* Persisted as the city CODE, not the index: the table is generated, so a
 * regenerated or trimmed table must not silently move the user's zone. */
int  ui_clock_tz_get(void);
void ui_clock_tz_set(int idx);      /* persist + apply; ignores a bad index */
void ui_clock_apply_tz(void);       /* setenv(TZ) + tzset(); call once at startup */

int  ui_clock_fmt_get(void);
void ui_clock_fmt_set(int fmt);

/* Resolve raw persisted file contents. Exposed for tests, and the single place
 * that decides what a corrupt setting degrades to. */
int  ui_clock_tz_resolve(const char *raw);    /* -> index, UTC on anything odd */
int  ui_clock_fmt_resolve(const char *raw);   /* -> 0..3, default on anything odd */

/* --- rendering -------------------------------------------------------- */
/* Writes a NUL-terminated string, empty when the clock should show nothing:
 * format Off or out of range, or t before 2020 (RTC never NTP-synced). */
void ui_clock_format(time_t t, int fmt, char *buf, size_t n);
void ui_clock_now(char *buf, size_t n);   /* format(time(NULL), fmt_get()) */

/* Short local date for the theme side panel: "Thu, Oct 5" (day unpadded).
 * Empty before 2020 (unsynced RTC), like the clock. */
#define UI_CLOCK_DATE_BUF 16
void ui_clock_date(time_t t, char *buf, size_t n);

#endif
