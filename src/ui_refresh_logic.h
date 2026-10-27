#ifndef EROSQ_UI_REFRESH_LOGIC_H
#define EROSQ_UI_REFRESH_LOGIC_H
/* Display > Refresh Rate: which LVGL display-refresh period each choice means,
 * and how /data/erosq-ui.refresh is parsed. Pure + host-tested.
 * Only the LVGL refresh timer is driven by this -- never the panel's E8h
 * frame-rate register (write-only, no TE line: unverifiable). The panel runs
 * at ~70 Hz, so Smooth (14 ms, 71 Hz) is the ceiling worth offering. */
#include <stdlib.h>

enum { UI_REFRESH_SMOOTH = 0, UI_REFRESH_STANDARD = 1, UI_REFRESH_NCHOICES = 2 };
#define UI_REFRESH_DEFAULT UI_REFRESH_SMOOTH

static inline int ui_refresh_valid(int mode)
{
    return mode >= 0 && mode < UI_REFRESH_NCHOICES;
}

/* Unknown modes fall back to the default's period. */
static inline int ui_refresh_period_ms(int mode)
{
    return mode == UI_REFRESH_STANDARD ? 16 : 14;
}

static inline const char *ui_refresh_name(int mode)
{
    return mode == UI_REFRESH_STANDARD ? "Standard (60 fps)" : "Smooth (70 fps)";
}

/* File contents -> mode. Accepts "0"/"1" with optional trailing whitespace;
 * anything else (empty, garbage, out of range, NULL) -> default. */
static inline int ui_refresh_parse(const char *buf)
{
    if (!buf) return UI_REFRESH_DEFAULT;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) return UI_REFRESH_DEFAULT;
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    if (*end || !ui_refresh_valid((int)v)) return UI_REFRESH_DEFAULT;
    return (int)v;
}

#endif
