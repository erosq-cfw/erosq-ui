#ifndef EROSQ_UI_SETTINGS_LOGIC_H
#define EROSQ_UI_SETTINGS_LOGIC_H
/* Pure helpers for the settings-batch1 options and the two persistence fixes.
 * Dependency-free, host-tested in tests/test_settings_logic.c. The glue (files,
 * MPD, sysfs, LVGL screens) lives in ui_power.c / ui_screens.c / ui_input.c. */
#include <stdlib.h>

/* /data file contents -> int in [lo, hi]; empty, garbage, trailing junk or out
 * of range -> def. Same grammar as ui_refresh_parse (trailing whitespace ok). */
static inline int ui_setting_parse(const char *buf, int lo, int hi, int def)
{
    if (!buf) return def;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) return def;
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    if (*end || v < lo || v > hi) return def;
    return (int)v;
}

/* --- Display > Screen-Off Buttons ---------------------------------------
 * Which keys still ACT while the backlight is off. A key that doesn't act
 * falls through to the normal wake policy (Display > Backlight Wake), like
 * any other key. So None + Backlight Wake "Power only" is a deliberate pocket
 * lock: nothing but POWER does anything while dark. Documented, not prevented. */
enum { UI_BLKEYS_ALL = 0, UI_BLKEYS_VOLUME = 1, UI_BLKEYS_NONE = 2, UI_BLKEYS_NCHOICES = 3 };
#define UI_BLKEYS_DEFAULT UI_BLKEYS_ALL

/* Vol+/Vol- act while blanked? Unknown mode -> default (All). */
static inline int ui_blkeys_volume(int mode) { return mode != UI_BLKEYS_NONE; }
/* Prev/Next/Play act while blanked? */
static inline int ui_blkeys_media(int mode)
{
    return mode != UI_BLKEYS_VOLUME && mode != UI_BLKEYS_NONE;
}

/* --- Settings > Volume Limit --------------------------------------------
 * 0 = Off, else the maximum percent. Applies to the wired DAC and BT alike. */
#define UI_VOLLIMIT_OFF 0
static const int ui_vollimit_choices[] = { 0, 50, 60, 70, 80, 90 };
#define UI_VOLLIMIT_NCHOICES ((int)(sizeof ui_vollimit_choices / sizeof ui_vollimit_choices[0]))

static inline int ui_vollimit_parse(const char *buf)
{
    int v = ui_setting_parse(buf, 0, 100, -1);
    for (int i = 0; i < UI_VOLLIMIT_NCHOICES; i++)
        if (v == ui_vollimit_choices[i]) return v;
    return UI_VOLLIMIT_OFF;
}

/* Clamp a volume to 0..100 and to the limit (0/out of range = no limit). */
static inline int ui_vol_cap(int pct, int limit)
{
    int hi = (limit > 0 && limit < 100) ? limit : 100;
    return pct < 0 ? 0 : (pct > hi ? hi : pct);
}

/* --- Settings > Repeat ---------------------------------------------------
 * MPD is the store (repeat/single live in its state_file, which mpd-persist
 * carries across power-off), so these only translate. */
enum { UI_REPEAT_OFF = 0, UI_REPEAT_ALL = 1, UI_REPEAT_ONE = 2, UI_REPEAT_NCHOICES = 3 };

/* MPD status fields -> mode. single without repeat is "stop after this song",
 * which we never set: show it as Off (picking Off clears it). */
static inline int ui_repeat_from_status(int repeat, int single)
{
    if (!repeat) return UI_REPEAT_OFF;
    return single ? UI_REPEAT_ONE : UI_REPEAT_ALL;
}

/* mode -> one MPD command list (single OK reply). Unknown -> Off. */
static inline const char *ui_repeat_cmd(int mode)
{
    switch (mode) {
    case UI_REPEAT_ALL: return "command_list_begin\nrepeat 1\nsingle 0\ncommand_list_end";
    case UI_REPEAT_ONE: return "command_list_begin\nrepeat 1\nsingle 1\ncommand_list_end";
    default:            return "command_list_begin\nrepeat 0\nsingle 0\ncommand_list_end";
    }
}

/* --- Settings > Playback > Bluetooth Buttons ------------------------------
 * Off drops play/pause/next/prev/stop coming from Bluetooth headphones or a
 * Bluetooth speaker (bluetoothd's "<name> (AVRCP)" uinput node), e.g. the pause
 * on-ear detection sends when the Bluetooth headphones come off. Volume is NOT
 * on that path (AVRCP absolute volume arrives as bluealsa's PCM Volume
 * property), so it is honoured either way. 1 = On (default). */
#define UI_BTBUTTONS_DEFAULT 1
static inline const char *ui_btbuttons_filter(int on, const char *cmd) { return on ? cmd : 0; }

/* --- Persistence fixes ---------------------------------------------------
 * Saved wired volume: 0..100, else -1 (no saved value -> keep the codec's).
 * Saved brightness: a raw backlight level 1..255, else -1. 0 is rejected: a
 * corrupt file must never boot to a black panel. */
static inline int ui_volfile_parse(const char *buf) { return ui_setting_parse(buf, 0, 100, -1); }
static inline int ui_blfile_parse(const char *buf)  { return ui_setting_parse(buf, 1, 255, -1); }

#endif
