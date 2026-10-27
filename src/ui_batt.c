/* Battery state via the kernel power_supply class
 * (/sys/class/power_supply/axp2101-battery), exposed by the in-kernel
 * axp2101_battery driver. Replaces the old direct /dev/i2c-1 register poke:
 * no I2C_SLAVE_FORCE, no contention with the axp20x driver -- just sysfs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_batt.h"

#ifndef UI_BATT_PSY   /* overridable so the host test can point at a fake node */
#define UI_BATT_PSY "/sys/class/power_supply/axp2101-battery"
#endif

/* $EROSQ_BATT_PSY swaps in a fake node at runtime, so the critical-battery
 * power-off can be HITL-tested on a charged cell (see ui_power.c). */
static FILE *open_node(const char *name)
{
    static const char *psy;
    if (!psy) { psy = getenv("EROSQ_BATT_PSY"); if (!psy || !*psy) psy = UI_BATT_PSY; }
    char p[256];
    snprintf(p, sizeof p, "%s/%s", psy, name);
    return fopen(p, "r");
}

static int read_int(const char *name)
{
    FILE *f = open_node(name);
    if (!f) return -1;
    int v;
    int n = fscanf(f, "%d", &v);
    fclose(f);
    return n == 1 ? v : -1;
}

int ui_batt_read(int *mv)
{
    if (mv) {
        int uv = read_int("voltage_now");   /* microvolts */
        *mv = uv > 0 ? uv / 1000 : 0;            /* -> millivolts */
    }
    return read_int("capacity");            /* 0-100, or -1 if unavailable */
}

/* First word of "status" ("Not charging" -> "Not"); "" if unreadable. */
static void read_status(char s[16])
{
    s[0] = 0;
    FILE *f = open_node("status");
    if (!f) return;
    if (fscanf(f, "%15s", s) != 1) s[0] = 0;
    fclose(f);
}

int ui_batt_charging(void)
{
    char s[16];
    read_status(s);
    return strcmp(s, "Charging") == 0;
}

int ui_batt_on_battery(void)
{
    char s[16];
    read_status(s);
    return strcmp(s, "Discharging") == 0;
}
