#include <stdlib.h>
#include "ui_power_logic.h"

int autooff_should_power_off(int minutes, uint32_t idle_ms, int playing)
{
    if (minutes <= 0 || playing) return 0;
    return idle_ms >= (uint32_t)minutes * 60000u;
}

int backlight_should_blank(int timeout_s, uint32_t idle_ms)
{
    if (timeout_s <= 0) return 0;
    return idle_ms >= (uint32_t)timeout_s * 1000u;
}

int backlight_should_wake(int mode, int auto_blanked, int is_power_key)
{
    if (mode == 2) return 1;            /* any button */
    if (mode == 0) return is_power_key; /* power only  */
    return auto_blanked || is_power_key;/* auto (and unknown -> auto) */
}

const int critbatt_choices[CRITBATT_NCHOICES] = { 0, 5, 10, 15, 20 };

int critbatt_parse(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return CRITBATT_DEFAULT;          /* empty/garbage: not "Off" */
    for (int i = 0; i < CRITBATT_NCHOICES; i++)
        if (v == critbatt_choices[i]) return (int)v;
    return CRITBATT_DEFAULT;
}

int critbatt_should_power_off(int threshold_pct, int capacity, int on_battery, int *low_reads)
{
    if (threshold_pct <= 0 || capacity < 0 || !on_battery || capacity > threshold_pct) {
        *low_reads = 0;
        return 0;
    }
    if (*low_reads < CRITBATT_READS) (*low_reads)++;
    return *low_reads >= CRITBATT_READS;
}
