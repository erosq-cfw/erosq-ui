/* Host unit tests for ui_clock_std_offset_label() -- the "UTC-3" label on each
 * Timezone picker row, derived from the generated POSIX TZ rule.
 *
 * Two oracles: hand-written spec cases, and libc itself -- every row of the
 * generated table is applied with setenv(TZ)+tzset() and its standard-time
 * tm_gmtoff must format to the same label. That keeps the parser honest
 * against the same rules the device's musl will parse.
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui_clock.h"

#define T_JAN15 1768478400L   /* 2026-01-15 12:00Z */
#define T_JUL15 1784116800L   /* 2026-07-15 12:00Z */

static const char *lbl(const char *rule)
{
    static char buf[UI_CLOCK_OFS_BUF];
    memset(buf, 'x', sizeof buf);
    int r = ui_clock_std_offset_label(rule, buf, sizeof buf);
    assert(strlen(buf) < sizeof buf);
    assert(r == 0 ? buf[0] != '\0' : buf[0] == '\0');
    return buf;
}

static void spec_cases(void)
{
    /* sign inversion: POSIX is west-positive */
    assert(strcmp(lbl("EST5EDT,M3.2.0,M11.1.0"), "UTC-5") == 0);
    assert(strcmp(lbl("CET-1CEST,M3.5.0,M10.5.0/3"), "UTC+1") == 0);
    assert(strcmp(lbl("<-03>3"), "UTC-3") == 0);
    assert(strcmp(lbl("<+14>-14"), "UTC+14") == 0);
    assert(strcmp(lbl("<-12>12"), "UTC-12") == 0);
    /* explicit '+' is still west */
    assert(strcmp(lbl("FOO+2"), "UTC-2") == 0);
    /* zero: never "UTC-0" */
    assert(strcmp(lbl("UTC0"), "UTC+0") == 0);
    assert(strcmp(lbl("GMT0BST,M3.5.0/1,M10.5.0"), "UTC+0") == 0);
    assert(strcmp(lbl("GMT-0"), "UTC+0") == 0);
    /* fractional hours */
    assert(strcmp(lbl("<-0930>9:30"), "UTC-9:30") == 0);
    assert(strcmp(lbl("NST3:30NDT,M3.2.0,M11.1.0"), "UTC-3:30") == 0);
    assert(strcmp(lbl("IST-5:30"), "UTC+5:30") == 0);
    assert(strcmp(lbl("<+0545>-5:45"), "UTC+5:45") == 0);
    assert(strcmp(lbl("<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45"), "UTC+12:45") == 0);
    /* seconds are accepted but not shown */
    assert(strcmp(lbl("LMT-0:30:17"), "UTC+0:30") == 0);

    /* malformed -> -1 and empty */
    char b[UI_CLOCK_OFS_BUF];
    assert(ui_clock_std_offset_label(NULL, b, sizeof b) == -1 && b[0] == '\0');
    assert(ui_clock_std_offset_label("", b, sizeof b) == -1 && b[0] == '\0');
    assert(ui_clock_std_offset_label("EST", b, sizeof b) == -1);     /* no offset */
    assert(ui_clock_std_offset_label("ES5", b, sizeof b) == -1);     /* name < 3 */
    assert(ui_clock_std_offset_label("<-03", b, sizeof b) == -1);    /* unclosed */
    assert(ui_clock_std_offset_label("<-03>", b, sizeof b) == -1);
    assert(ui_clock_std_offset_label("FOO123", b, sizeof b) == -1);  /* 3-digit hour */
    assert(ui_clock_std_offset_label("FOO25", b, sizeof b) == -1);
    assert(ui_clock_std_offset_label("FOO5:6", b, sizeof b) == -1);
    assert(ui_clock_std_offset_label("FOO5:60", b, sizeof b) == -1);
    assert(ui_clock_std_offset_label("FOO-", b, sizeof b) == -1);
    /* too-small buffer truncates to empty, never overruns */
    char tiny[5];
    assert(ui_clock_std_offset_label("IST-5:30", tiny, sizeof tiny) == -1 && tiny[0] == '\0');
    assert(ui_clock_std_offset_label("UTC0", NULL, 8) == -1);
    assert(ui_clock_std_offset_label("UTC0", b, 0) == -1);
}

/* Independent formatter from libc's east-positive seconds. */
static void expect_from_gmtoff(long off, char *buf, size_t n)
{
    char sign = off < 0 ? '-' : '+';
    long a = off < 0 ? -off : off;
    assert(a <= 24 * 3600);
    int h = (int)(a / 3600) % 100, m = (int)((a % 3600) / 60);
    if (m) snprintf(buf, n, "UTC%c%d:%02d", sign, h, m);
    else   snprintf(buf, n, "UTC%c%d", sign, h);
}

static void table_vs_libc(void)
{
    int n = ui_clock_tz_count();
    assert(n > 0);
    for (int i = 0; i < n; i++) {
        const char *rule = ui_clock_tz_rule(i);
        char got[UI_CLOCK_OFS_BUF], want[UI_CLOCK_OFS_BUF];
        assert(ui_clock_std_offset_label(rule, got, sizeof got) == 0);

        assert(setenv("TZ", rule, 1) == 0);
        tzset();
        int found = 0;
        const time_t ts[2] = { T_JAN15, T_JUL15 };
        for (int k = 0; k < 2 && !found; k++) {
            struct tm tm;
            assert(localtime_r(&ts[k], &tm));
            if (tm.tm_isdst) continue;
            expect_from_gmtoff(tm.tm_gmtoff, want, sizeof want);
            found = 1;
        }
        assert(found);
        if (strcmp(got, want) != 0) {
            fprintf(stderr, "row %d %s: rule %s got %s want %s\n",
                    i, ui_clock_tz_code(i), rule, got, want);
            assert(0);
        }
        /* Fits the picker's right column (widest real label is "UTC+12:45"). */
        assert(strlen(got) <= 9);
    }
}

int main(void)
{
    spec_cases();
    table_vs_libc();
    printf("clock_offset: all tests passed\n");
    return 0;
}
