/* Host test for src/ui_wake_logic.h: the time-based lazy-open rescan throttle
 * and the blanked poll() timeout. Pure, dep-free (run by tests/run.sh). */
#include <assert.h>
#include <stdio.h>
#include "ui_wake_logic.h"

static void rescan(void)
{
    struct ui_rescan r = {0};
    /* first call always scans (old `tick++ & 63` scanned on tick 0) */
    assert(ui_rescan_wait(&r, 5000, 1000) == 0);
    assert(ui_rescan_due(&r, 5000, 1000) == 1);
    /* then not again until a full period has passed */
    assert(ui_rescan_due(&r, 5000, 1000) == 0);
    assert(ui_rescan_due(&r, 5999, 1000) == 0);
    assert(ui_rescan_wait(&r, 5000, 1000) == 1000);
    assert(ui_rescan_wait(&r, 5999, 1000) == 1);
    assert(ui_rescan_wait(&r, 6000, 1000) == 0);
    assert(ui_rescan_due(&r, 6000, 1000) == 1);
    /* cadence is by clock, not by call count: 10000 calls in 1 ms do nothing */
    int scans = 0;
    for (int i = 0; i < 10000; i++) scans += ui_rescan_due(&r, 6500, 1000);
    assert(scans == 0);
    /* long gap (loop asleep for minutes): scans once, not once per missed period */
    assert(ui_rescan_due(&r, 600000, 1000) == 1);
    assert(ui_rescan_due(&r, 600001, 1000) == 0);
    /* a first tick of 0 is still a real timestamp (primed, not "unset") */
    struct ui_rescan z = {0};
    assert(ui_rescan_due(&z, 0, 1000) == 1);
    assert(ui_rescan_due(&z, 999, 1000) == 0);
    assert(ui_rescan_due(&z, 1000, 1000) == 1);
}

static void rescan_wrap(void)
{
    /* lv_tick is CLOCK_MONOTONIC ms truncated to u32; ticks with the top bit
     * set (and the wrap past 0xFFFFFFFF) must not stall or spin the throttle. */
    struct ui_rescan r = {0};
    assert(ui_rescan_due(&r, 0x80000000u, 1000) == 1);
    assert(ui_rescan_due(&r, 0x800003E7u, 1000) == 0);
    assert(ui_rescan_due(&r, 0x800003E8u, 1000) == 1);
    r.last = 0xFFFFFE00u; r.primed = 1;                 /* 512 ms before wrap */
    assert(ui_rescan_due(&r, 0xFFFFFFFFu, 1000) == 0);  /* 511 ms elapsed */
    assert(ui_rescan_wait(&r, 0xFFFFFFFFu, 1000) == 489);
    assert(ui_rescan_due(&r, 0x000001E7u, 1000) == 0);  /* 999 ms elapsed, across 0 */
    assert(ui_rescan_wait(&r, 0x000001E7u, 1000) == 1);
    assert(ui_rescan_due(&r, 0x000001E8u, 1000) == 1);  /* 1000 ms elapsed */
    assert(r.last == 0x000001E8u);
}

static void timeout(void)
{
    /* nothing pending anywhere -> block until an fd fires */
    assert(ui_wake_timeout(UI_WAIT_NONE, UI_WAIT_NONE, -1) == -1);
    /* each source alone */
    assert(ui_wake_timeout(500, UI_WAIT_NONE, -1) == 500);
    assert(ui_wake_timeout(UI_WAIT_NONE, 300, -1) == 300);
    assert(ui_wake_timeout(UI_WAIT_NONE, UI_WAIT_NONE, 70) == 70);
    /* earliest wins; NO 16 ms clamp */
    assert(ui_wake_timeout(10000, 1000, -1) == 1000);
    assert(ui_wake_timeout(10000, 1000, 2500) == 1000);
    assert(ui_wake_timeout(800, 1000, 2500) == 800);
    assert(ui_wake_timeout(800, 1000, 25) == 25);
    /* due-now in any source -> don't block (lv_async_call = 0 ms timer) */
    assert(ui_wake_timeout(0, 1000, -1) == 0);
    assert(ui_wake_timeout(500, 0, -1) == 0);
    assert(ui_wake_timeout(500, 1000, 0) == 0);
    /* huge but finite LVGL deadline must not go negative */
    assert(ui_wake_timeout(0xFFFFFFFEu, UI_WAIT_NONE, -1) == INT_MAX);
}

int main(void)
{
    rescan();
    rescan_wrap();
    timeout();
    printf("wake_logic: OK\n");
    return 0;
}
