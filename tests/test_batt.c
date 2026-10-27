/* ui_batt sysfs parsing against a fake power_supply node.
 *
 * Worth a test only because the two states that matter can't both be produced
 * by hand on the device: proving "Not charging" (cable in, topped off) does NOT
 * light the bolt would mean sitting on a full battery. The strings are exactly
 * those emitted by POWER_SUPPLY_PROP_STATUS in axp2101_battery.c. */
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui_batt.h"

/* The fake node lives in the build tree (CMake points UI_BATT_PSY at it), so it
 * SURVIVES between runs. The first assert below requires "no node at all", so
 * without this the test passes on a fresh build tree and fails on every re-run.
 * run.sh never saw it -- that harness hands each run a fresh mktemp dir. */
static void clear(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", UI_BATT_PSY, name);
    unlink(p);
}

static void put(const char *name, const char *val)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", UI_BATT_PSY, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(val, f);
    fclose(f);
}

int main(void)
{
    mkdir(UI_BATT_PSY, 0755);
    clear("status"); clear("capacity"); clear("voltage_now");   /* re-run safe */

    /* No node at all -> not charging, and capacity reports "unavailable". */
    assert(ui_batt_charging() == 0);
    assert(ui_batt_read(NULL) == -1);

    put("status", "Charging\n");
    assert(ui_batt_charging() == 1);

    /* Everything else is bolt-off. "Not charging" must not prefix-match. */
    put("status", "Not charging\n");   assert(ui_batt_charging() == 0);
    put("status", "Discharging\n");    assert(ui_batt_charging() == 0);
    put("status", "Full\n");           assert(ui_batt_charging() == 0);
    put("status", "");                 assert(ui_batt_charging() == 0);

    /* on_battery: only "Discharging"; any charger state or junk -> 0. */
    put("status", "Discharging\n");    assert(ui_batt_on_battery() == 1);
    put("status", "Charging\n");       assert(ui_batt_on_battery() == 0);
    put("status", "Not charging\n");   assert(ui_batt_on_battery() == 0);
    put("status", "Full\n");           assert(ui_batt_on_battery() == 0);
    put("status", "");                 assert(ui_batt_on_battery() == 0);
    clear("status");                   assert(ui_batt_on_battery() == 0);

    /* Gauge read: capacity as-is, voltage uV -> mV. */
    put("capacity", "25\n");
    put("voltage_now", "3744000\n");
    int mv = 0;
    assert(ui_batt_read(&mv) == 25);
    assert(mv == 3744);

    puts("ok");
    return 0;
}
