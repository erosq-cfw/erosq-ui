#ifndef EROSQ_UI_POWER_LOGIC_H
#define EROSQ_UI_POWER_LOGIC_H
#include <stdint.h>

/* Auto-off fires only when enabled (minutes>0), nothing is playing, and the
 * device has been idle for at least the configured minutes. Pure + host-tested
 * (no LVGL/syscalls) so the one piece of real logic has a runnable check. */
int autooff_should_power_off(int minutes, uint32_t idle_ms, int playing);

/* Blank the backlight when enabled (timeout_s>0) and idle at least timeout_s.
 * Pure + host-tested, mirrors autooff_should_power_off. */
int backlight_should_blank(int timeout_s, uint32_t idle_ms);

/* Should a wake event un-blank the screen?
 *   mode:         0=power only, 1=auto, 2=any button (unknown -> auto)
 *   auto_blanked: 1 if the current blank came from the idle timeout, else 0
 *   is_power_key: 1 if the wake event is the POWER key (wheel -> 0) */
int backlight_should_wake(int mode, int auto_blanked, int is_power_key);

/* Critical-battery power-off. Settings choices are Off (0) or one of
 * CRITBATT_CHOICES percent; default CRITBATT_DEFAULT. */
#define CRITBATT_DEFAULT 20      /* user keeps the cell in 20-80% (pairs with Charge Limit 80%) */
#define CRITBATT_READS   2      /* consecutive low polls before acting (gauge dips under load) */
extern const int critbatt_choices[];   /* 0, 5, 10, 15, 20 */
#define CRITBATT_NCHOICES 5

/* Persisted value (file contents) -> a valid choice; anything else -> default. */
int critbatt_parse(const char *s);

/* One gauge poll. threshold_pct: 0 = Off. capacity: 0-100, <0 = read failed.
 * on_battery: 1 only when the pack is discharging (no charger/VBUS).
 * *low_reads is the caller's consecutive-low counter (start at 0). Returns 1
 * once CRITBATT_READS consecutive polls are at/below the threshold on battery;
 * any other poll resets the count. */
int critbatt_should_power_off(int threshold_pct, int capacity, int on_battery, int *low_reads);

#endif
