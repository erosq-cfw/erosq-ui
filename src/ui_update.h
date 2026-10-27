#ifndef EROSQ_UI_UPDATE_H
#define EROSQ_UI_UPDATE_H
#include <stddef.h>

#define UI_UPDATE_SD_DIR "/media/sd"

/* Scan UI_UPDATE_SD_DIR for *.swu bundles (readdir + stat, no bundle opened;
 * cheap -- a card's root directory is small). Fills `name` (basename only, NOT
 * a full path) with the NEWEST by mtime (equal mtimes -> larger name; see
 * update_logic.h ul_swu_better) and returns 1; returns 0 and leaves name
 * untouched if none is present (card empty, absent, or unmounted). */
int ui_update_find_swu(char *name, size_t namelen);

/* 1 iff the bundle's top-level version (read from its first 2 KB) equals the
 * running firmware's (/etc/sw-versions), i.e. Install would only hit the -R
 * reject. 0 on any read/parse failure -- the caller then offers Install as
 * before and swupdate's own gate still applies. */
int ui_update_swu_installed(const char *swu_name);

typedef enum {
    UI_UPD_OK = 0,      /* installed; caller should reboot */
    UI_UPD_ALREADY = 1, /* swupdate declined: no-op / same version already installed */
    UI_UPD_FAILED = 2,  /* swupdate failed; msg holds a short reason */
} ui_update_result_t;

/* Run swupdate against `swu_name` (a basename inside UI_UPDATE_SD_DIR),
 * targeting the slot NOT currently running (read from /proc/cmdline via
 * update_logic.h's ul_running_slot/ul_inactive_slot -- see that header for
 * why the slot logic itself isn't reimplemented here). Stops MPD first to
 * free RAM. BLOCKS the caller for the duration of the install (the brief
 * accepts this; the UI paints an "Installing..." screen and calls
 * lv_refr_now() before invoking this, same pattern as ui_power_off() and
 * eject_sd_click_cb()).
 *
 * msg (msglen bytes) is always filled with a short human-readable reason,
 * used for both the ALREADY and FAILED cases.
 *
 * Version gating is bundle-level: the running firmware version (the `firmware`
 * line of /etc/sw-versions) is passed to swupdate as -R (--no-reinstalling), so
 * an identical bundle is rejected in the parser with "No reinstalling allowed"
 * (UI_UPD_ALREADY) BEFORE any volume/bootenv is touched, and any different
 * version installs both components. UI_UPD_OK is gated on the positive
 * "Installing image" log line. */
ui_update_result_t ui_update_run(const char *swu_name, char *msg, size_t msglen);

/* Reboot the board (busybox applet at /sbin/reboot -- NOT /usr/sbin; see the
 * poweroff-path lesson in ui_power.c). Does not return on success. */
void ui_update_reboot(void);

#endif
