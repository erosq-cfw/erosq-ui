/* Pure formatting for the About and Brightness screens -- dependency-free,
 * host-testable (tests/test_about_logic.c). No LVGL, no I/O.
 *
 * "Unavailable" renders as AL_NA. The iPod font set (gen-fonts.sh) carries only
 * ASCII + degree + bullet, so an em dash would draw as a missing glyph. */
#ifndef EROSQ_ABOUT_LOGIC_H
#define EROSQ_ABOUT_LOGIC_H
#include <stdio.h>

#define AL_NA "--"

/* Battery row: pct is ui_batt_read()'s return (-1 = no power_supply node),
 * mv its millivolts (0 = voltage unreadable). */
static inline void al_batt(char *out, int n, int pct, int mv)
{
    if (pct < 0 || pct > 100)  snprintf(out, n, AL_NA);
    else if (mv <= 0)          snprintf(out, n, "%d%%", pct);
    else                       snprintf(out, n, "%d%%  %d.%02dV", pct, mv / 1000, (mv % 1000) / 10);
}

/* Free-space row: whole MB below 1 GB, else GB with one decimal (truncated,
 * integer math -- never over-reports what fits). 1 GB = 1024 MB. */
static inline void al_free(char *out, int n, unsigned long long bytes)
{
    unsigned long long mb = bytes >> 20;
    if (mb < 1024) snprintf(out, n, "%llu MB", mb);
    else           snprintf(out, n, "%llu.%llu GB", mb >> 10, ((mb & 1023) * 10) >> 10);
}

/* Brightness readout: backlight PWM level (0-255) -> rounded percent. */
static inline int al_bl_pct(int level)
{
    if (level <= 0)   return 0;
    if (level >= 255) return 100;
    return (level * 100 + 127) / 255;
}

#endif
