#ifndef EROSQ_UI_BT_H
#define EROSQ_UI_BT_H
#include "ui_bt_logic.h"

/* Subscribe to org.bluez and seed the device table. Call after ui_dbus_init().
 * Returns 0 if org.bluez is reachable, -1 otherwise (BT screen shows offline). */
int  ui_bt_init(void);

void ui_bt_set_power(int on);   /* backgrounds the bringup/teardown scripts; state settles async */
void ui_bt_power_on_deferred(void); /* boot-restore alias of set_power(1) (kept for the call site) */
int  ui_bt_is_on(void);         /* optimistic during bringup; reverted if bringup fails */
int  ui_bt_ready(void);         /* 1 once bluetoothd owns org.bluez AND power is on (scan-safe) */
void bt_pref_save(int on);      /* persist Bluetooth on/off choice to /data */
void bt_pref_restore(void);     /* power BT on at boot if saved "on" */

void ui_bt_scan_start(void);
void ui_bt_scan_stop(void);

void ui_bt_connect(const char *path);     /* pair-if-needed -> connect -> codec upgrade */
void ui_bt_disconnect(const char *path);
void ui_bt_remove(const char *path);      /* unpair: Adapter1.RemoveDevice */

const bt_devtable_t *ui_bt_devices(void);

/* Audio routing/volume: when BT is the active sink the volume wheel should drive
 * the BlueALSA PCM (software volume) instead of the DAC hardware mixer. */
int  ui_bt_audio_active(void);          /* 1 if a connected BT device is the sink */
void ui_bt_set_volume_pct(int pct);     /* 0..100; applied to the active A2DP PCM */
void ui_bt_persist_volume(void);        /* flush the connected device's level to disk (call on graceful shutdown) */

/* Speaker -> UI: fired when the connected A2DP sink reports its own volume
 * (AVRCP absolute volume). pct is 0..100. */
typedef void (*ui_bt_volume_cb)(int pct);
void ui_bt_set_volume_cb(ui_bt_volume_cb cb);

/* One UI callback, fired on any adapter/device change. `msg`: NULL normally, or
 * a human-readable status/error to surface (e.g. "Pairing failed: ..."). */
typedef void (*ui_bt_changed_cb)(const char *msg);
void ui_bt_set_changed_cb(ui_bt_changed_cb cb);

#endif
