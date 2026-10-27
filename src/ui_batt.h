#ifndef EROSQ_UI_BATT_H
#define EROSQ_UI_BATT_H
/* Read AXP2101 battery state via the kernel power_supply class (read-only).
 * Returns percent 0-100 (or -1 on failure); if mv != NULL, fills battery
 * voltage in millivolts. */
int ui_batt_read(int *mv);

/* 1 only while the charger is actively pushing current in ("status" ==
 * "Charging"); 0 for Full / Not charging (cable in, topped off) /
 * Discharging, and 0 if the node can't be read. */
int ui_batt_charging(void);

/* 1 only when running off the pack ("status" == "Discharging": the kernel
 * reports that only without VBUS). 0 with any charger in, or if unreadable. */
int ui_batt_on_battery(void);
#endif
