/* Host unit tests for src/ui_clock.c -- the status-bar clock's timezone table,
 * setting persistence, and time formatting.
 *
 * ui_clock.c deliberately includes no LVGL, so this links it directly. The
 * label plumbing in ui_screens.c stays HITL.
 *
 * Persistence writes to UI_CLOCK_DATA_DIR, overridden at compile time (see
 * tests/run.sh) so these tests never touch a real /data.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui_clock.h"

/* Fixed epochs, all 2026-07-30 UTC (see the generator run that produced them).
 * Formatting assertions run under TZ=UTC0 so local == UTC. */
#define T_0000  1785369600L    /* 00:00 -- the midnight 12-hour trap */
#define T_1200  1785412800L    /* 12:00 -- the noon 12-hour trap */
#define T_1305  1785416700L    /* 13:05 -- ordinary afternoon */
#define T_2359  1785455940L    /* 23:59 */

/* 12:00 UTC on the 15th, used for the DST-rule assertions. */
#define T_JAN15 1768478400L
#define T_JUL15 1784116800L

#define T_PRE2020    1000000000L   /* 2001-09-09; never-synced RTC */
#define T_GUARD_EDGE 1577836800L   /* exactly 2020-01-01 00:00Z: must render */

static void set_tz(const char *tz)
{
    assert(setenv("TZ", tz, 1) == 0);
    tzset();
}

static const char *fmt(time_t t, int mode)
{
    static char buf[UI_CLOCK_BUF];
    ui_clock_format(t, mode, buf, sizeof buf);
    return buf;
}

/* --- formatting -------------------------------------------------------- */

static void test_format_24h(void)
{
    set_tz("UTC0");
    assert(strcmp(fmt(T_0000, UI_CLOCK_FMT_24), "00:00") == 0);
    assert(strcmp(fmt(T_1200, UI_CLOCK_FMT_24), "12:00") == 0);
    assert(strcmp(fmt(T_1305, UI_CLOCK_FMT_24), "13:05") == 0);
    assert(strcmp(fmt(T_2359, UI_CLOCK_FMT_24), "23:59") == 0);
}

static void test_format_12h_midnight_and_noon_are_both_twelve(void)
{
    /* The whole reason this function exists rather than strftime("%l"): musl
     * has no %l, and a naive tm_hour%12 renders midnight as "0:00". */
    set_tz("UTC0");
    assert(strcmp(fmt(T_0000, UI_CLOCK_FMT_12), "12:00 AM") == 0);
    assert(strcmp(fmt(T_1200, UI_CLOCK_FMT_12), "12:00 PM") == 0);
}

static void test_format_12h(void)
{
    set_tz("UTC0");
    assert(strcmp(fmt(T_1305, UI_CLOCK_FMT_12), "1:05 PM") == 0);
    assert(strcmp(fmt(T_2359, UI_CLOCK_FMT_12), "11:59 PM") == 0);
    /* 11:59 *AM* -- one minute before the PM flip */
    assert(strcmp(fmt(T_1200 - 60, UI_CLOCK_FMT_12), "11:59 AM") == 0);
}

static void test_format_12h_no_ampm_is_unpadded(void)
{
    set_tz("UTC0");
    assert(strcmp(fmt(T_1305, UI_CLOCK_FMT_12_NOAP), "1:05") == 0);   /* not " 1:05" */
    assert(strcmp(fmt(T_0000, UI_CLOCK_FMT_12_NOAP), "12:00") == 0);
    assert(strcmp(fmt(T_2359, UI_CLOCK_FMT_12_NOAP), "11:59") == 0);
}

static void test_format_off_and_bad_mode_render_empty(void)
{
    set_tz("UTC0");
    assert(fmt(T_1305, UI_CLOCK_FMT_OFF)[0] == '\0');
    assert(fmt(T_1305, -1)[0] == '\0');
    assert(fmt(T_1305, 99)[0] == '\0');
}

static void test_format_unsynced_clock_renders_empty(void)
{
    /* Never-synced RTC: show nothing rather than a confident wrong time. */
    set_tz("UTC0");
    for (int m = 0; m < UI_CLOCK_FMT_N; m++)
        assert(fmt(T_PRE2020, m)[0] == '\0');
    /* ...but the guard is exclusive: 2020-01-01T00:00Z itself is valid. */
    assert(strcmp(fmt(T_GUARD_EDGE, UI_CLOCK_FMT_24), "00:00") == 0);
}

static void test_format_truncates_rather_than_overflows(void)
{
    char small[4];
    set_tz("UTC0");
    ui_clock_format(T_1305, UI_CLOCK_FMT_24, small, sizeof small);
    assert(strlen(small) < sizeof small);   /* NUL-terminated, no overrun */
}

/* --- the generated table honours real DST rules ------------------------ */

static void test_northern_dst_rule(void)
{
    set_tz(ui_clock_tz_rule(ui_clock_tz_find("LON")));
    assert(strcmp(fmt(T_JAN15, UI_CLOCK_FMT_24), "12:00") == 0);   /* GMT  */
    assert(strcmp(fmt(T_JUL15, UI_CLOCK_FMT_24), "13:00") == 0);   /* BST  */
}

static void test_southern_dst_rule_is_inverted(void)
{
    /* Santiago is the row added on top of the GA-B2100 list precisely because
     * its DST runs the opposite half of the year from Halifax, which Casio
     * uses to stand in for the whole -4 offset. */
    set_tz(ui_clock_tz_rule(ui_clock_tz_find("SCL")));
    assert(strcmp(fmt(T_JAN15, UI_CLOCK_FMT_24), "09:00") == 0);   /* -03, DST */
    assert(strcmp(fmt(T_JUL15, UI_CLOCK_FMT_24), "08:00") == 0);   /* -04, std */

    set_tz(ui_clock_tz_rule(ui_clock_tz_find("YHZ")));
    assert(strcmp(fmt(T_JAN15, UI_CLOCK_FMT_24), "08:00") == 0);   /* -04, std */
    assert(strcmp(fmt(T_JUL15, UI_CLOCK_FMT_24), "09:00") == 0);   /* -03, DST */
}

static void test_quarter_hour_offset(void)
{
    /* Kathmandu +5:45 -- confirms the TZ parser takes :45, not just :30. */
    set_tz(ui_clock_tz_rule(ui_clock_tz_find("KTM")));
    assert(strcmp(fmt(T_JUL15, UI_CLOCK_FMT_24), "17:45") == 0);
}

/* --- table sanity ------------------------------------------------------ */

static void test_table_is_well_formed(void)
{
    int n = ui_clock_tz_count();
    assert(n == 40);
    for (int i = 0; i < n; i++) {
        const char *code = ui_clock_tz_code(i);
        const char *name = ui_clock_tz_name(i);
        const char *rule = ui_clock_tz_rule(i);
        assert(code && name && rule);
        assert(code[0] && name[0] && rule[0]);
        assert(strlen(code) == 3);            /* Casio codes are 3 letters */
        assert(ui_clock_tz_find(code) == i);  /* codes unique + findable */
    }
}

static void test_table_accessors_reject_out_of_range(void)
{
    int n = ui_clock_tz_count();
    assert(ui_clock_tz_code(-1) == NULL);
    assert(ui_clock_tz_name(-1) == NULL);
    assert(ui_clock_tz_code(n) == NULL);
    assert(ui_clock_tz_name(n) == NULL);
    /* rule() must stay safe to hand straight to setenv, so it falls back to
     * UTC rather than NULL. */
    assert(strcmp(ui_clock_tz_rule(-1), "UTC0") == 0);
    assert(strcmp(ui_clock_tz_rule(n), "UTC0") == 0);
}

static void test_utc_is_the_fallback_row(void)
{
    int utc = ui_clock_tz_utc();
    assert(utc >= 0 && utc < ui_clock_tz_count());
    assert(strcmp(ui_clock_tz_code(utc), "UTC") == 0);
}

static void test_find_rejects_unknown(void)
{
    assert(ui_clock_tz_find("ZZZ") == -1);
    assert(ui_clock_tz_find("") == -1);
    assert(ui_clock_tz_find(NULL) == -1);
    assert(ui_clock_tz_find("LONDON") == -1);   /* no prefix matching */
    assert(ui_clock_tz_find("lon") == -1);      /* codes are upper-case */
}

/* --- resolving persisted file contents --------------------------------- */

static void test_resolve_tz_accepts_a_clean_code(void)
{
    assert(ui_clock_tz_resolve("LON") == ui_clock_tz_find("LON"));
}

static void test_resolve_tz_tolerates_trailing_whitespace(void)
{
    /* A hand-edited /data file, or `echo LON > file`, leaves a newline. */
    assert(ui_clock_tz_resolve("LON\n") == ui_clock_tz_find("LON"));
    assert(ui_clock_tz_resolve("LON\r\n") == ui_clock_tz_find("LON"));
    assert(ui_clock_tz_resolve(" LON ") == ui_clock_tz_find("LON"));
}

static void test_resolve_tz_falls_back_to_utc_on_junk(void)
{
    int utc = ui_clock_tz_utc();
    assert(ui_clock_tz_resolve(NULL) == utc);
    assert(ui_clock_tz_resolve("") == utc);
    assert(ui_clock_tz_resolve("\n") == utc);
    assert(ui_clock_tz_resolve("ZZZ") == utc);
    assert(ui_clock_tz_resolve("garbage garbage") == utc);
    /* An index, not a code -- what a naive implementation would have stored.
     * Must not be honoured, or a regenerated table would silently move zones. */
    assert(ui_clock_tz_resolve("17") == utc);
}

static void test_resolve_fmt(void)
{
    assert(ui_clock_fmt_resolve("0") == UI_CLOCK_FMT_24);
    assert(ui_clock_fmt_resolve("2") == UI_CLOCK_FMT_12_NOAP);
    assert(ui_clock_fmt_resolve("3\n") == UI_CLOCK_FMT_OFF);
    /* junk and out-of-range fall back to the default, never to Off */
    assert(ui_clock_fmt_resolve(NULL) == UI_CLOCK_FMT_DEFAULT);
    assert(ui_clock_fmt_resolve("") == UI_CLOCK_FMT_DEFAULT);
    assert(ui_clock_fmt_resolve("4") == UI_CLOCK_FMT_DEFAULT);
    assert(ui_clock_fmt_resolve("-1") == UI_CLOCK_FMT_DEFAULT);
    assert(ui_clock_fmt_resolve("banana") == UI_CLOCK_FMT_DEFAULT);
}

/* --- persistence round-trip ------------------------------------------- */

static void test_settings_round_trip_through_data_dir(void)
{
    if (mkdir(UI_CLOCK_DATA_DIR, 0755) != 0)
        assert(errno == EEXIST);

    ui_clock_tz_set(ui_clock_tz_find("TYO"));
    assert(ui_clock_tz_get() == ui_clock_tz_find("TYO"));

    ui_clock_fmt_set(UI_CLOCK_FMT_12);
    assert(ui_clock_fmt_get() == UI_CLOCK_FMT_12);

    /* setting the zone must also apply it, so the next render is local */
    set_tz("UTC0");
    ui_clock_tz_set(ui_clock_tz_find("TYO"));
    assert(strcmp(fmt(T_JUL15, UI_CLOCK_FMT_24), "21:00") == 0);   /* JST +9 */

    /* out-of-range set is ignored rather than persisting a bad index */
    int before = ui_clock_tz_get();
    ui_clock_tz_set(-1);
    ui_clock_tz_set(ui_clock_tz_count());
    assert(ui_clock_tz_get() == before);
}

static void test_missing_data_dir_uses_defaults(void)
{
    /* Deleting the files is the "fresh device" / "no /data" case: get() must
     * return the documented defaults, and set() must not crash. */
    unlink(UI_CLOCK_DATA_DIR "/erosq-ui.tz");
    unlink(UI_CLOCK_DATA_DIR "/erosq-ui.clockfmt");
    assert(ui_clock_tz_resolve(NULL) == ui_clock_tz_utc());
    assert(ui_clock_fmt_resolve(NULL) == UI_CLOCK_FMT_DEFAULT);
}

static void test_date(void)
{
    char b[UI_CLOCK_DATE_BUF];
    set_tz("UTC0");
    ui_clock_date(T_1305, b, sizeof b);
    assert(strcmp(b, "Thu, Jul 30") == 0);
    ui_clock_date(1791201600L, b, sizeof b);           /* 2026-10-05: day unpadded */
    assert(strcmp(b, "Mon, Oct 5") == 0);
    set_tz("EST5EDT,M3.2.0,M11.1.0");                   /* local, not UTC: UTC is already the 31st */
    ui_clock_date(T_1305 + 11 * 3600L, b, sizeof b);
    assert(strcmp(b, "Thu, Jul 30") == 0);
    set_tz("UTC0");
    ui_clock_date(T_PRE2020, b, sizeof b);
    assert(b[0] == '\0');
    char small[6];
    ui_clock_date(T_1305, small, sizeof small);        /* truncates, never overflows */
    assert(strcmp(small, "Thu, ") == 0);
}

int main(void)
{
    test_format_24h();
    test_date();
    test_format_12h_midnight_and_noon_are_both_twelve();
    test_format_12h();
    test_format_12h_no_ampm_is_unpadded();
    test_format_off_and_bad_mode_render_empty();
    test_format_unsynced_clock_renders_empty();
    test_format_truncates_rather_than_overflows();

    test_northern_dst_rule();
    test_southern_dst_rule_is_inverted();
    test_quarter_hour_offset();

    test_table_is_well_formed();
    test_table_accessors_reject_out_of_range();
    test_utc_is_the_fallback_row();
    test_find_rejects_unknown();

    test_resolve_tz_accepts_a_clean_code();
    test_resolve_tz_tolerates_trailing_whitespace();
    test_resolve_tz_falls_back_to_utc_on_junk();
    test_resolve_fmt();

    test_settings_round_trip_through_data_dir();
    test_missing_data_dir_uses_defaults();

    printf("ok\n");
    return 0;
}
