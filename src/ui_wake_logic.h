#ifndef EROSQ_UI_WAKE_LOGIC_H
#define EROSQ_UI_WAKE_LOGIC_H
/* Pure helpers for the main loop's blanked sleep (main.c) and the lazy-open
 * rescans in ui_input.c. Dependency-free, host-tested in tests/test_wake_logic.c.
 *
 * All times are uint32_t milliseconds from lv_tick_get() (CLOCK_MONOTONIC ms
 * truncated to 32 bits), so they wrap every ~49.7 days: compare only via the
 * unsigned difference (now - last), never with < / > on raw ticks. */
#include <stdint.h>
#include <limits.h>

/* Lazy-open rescan throttle: "scan for an absent evdev node about once a
 * period". The first call always scans (matches the old `tick++ & 63`, which
 * scanned on tick 0). Time-based, so the cadence no longer depends on how
 * fast the main loop spins (it sleeps far longer than 16 ms while blanked). */
struct ui_rescan { uint32_t last; int primed; };

/* 1 = scan now (and remember now as the last scan); 0 = not yet. */
static inline int ui_rescan_due(struct ui_rescan *r, uint32_t now, uint32_t period)
{
    if (r->primed && (uint32_t)(now - r->last) < period) return 0;
    r->last = now;
    r->primed = 1;
    return 1;
}

/* Milliseconds until ui_rescan_due() would next return 1 (0 = due now). */
static inline uint32_t ui_rescan_wait(const struct ui_rescan *r, uint32_t now, uint32_t period)
{
    if (!r->primed) return 0;
    uint32_t el = (uint32_t)(now - r->last);
    return el >= period ? 0 : period - el;
}

#define UI_WAIT_NONE UINT32_MAX   /* "no deadline" (== LV_NO_TIMER_READY) */

/* poll() timeout while blanked: the earliest of LVGL's next timer, the next
 * lazy-open rescan, and GLib's own timeout. UI_WAIT_NONE / glib -1 mean "no
 * deadline from this source"; none at all -> -1 (block until an fd fires).
 * No 16 ms clamp: that is the whole point. */
static inline int ui_wake_timeout(uint32_t lv_next, uint32_t rescan_wait, int glib_timeout)
{
    uint32_t t = lv_next < rescan_wait ? lv_next : rescan_wait;
    if (glib_timeout >= 0 && (uint32_t)glib_timeout < t) t = (uint32_t)glib_timeout;
    if (t == UI_WAIT_NONE) return -1;
    return t > (uint32_t)INT_MAX ? INT_MAX : (int)t;
}
/* 1 = the main loop may stop the indev read timers and block in poll() on the
 * input fds (awake or blanked): nothing is held down. A held key or pad button
 * needs LVGL's periodic indev reads -- long-press (LV_EVENT_LONG_PRESSED rows),
 * key repeat (held volume / nav keys) and POWER hold-to-shutdown -- so it keeps
 * the 16 ms read cadence until released. A wheel has no held state. */
static inline int ui_input_idle(int power_held, int key_pressed, int pad_pressed)
{
    return !power_held && !key_pressed && !pad_pressed;
}
#endif
