#ifndef EROSQ_UI_DBUS_H
#define EROSQ_UI_DBUS_H

/*
 * ui_dbus — shared D-Bus infrastructure for erosq-ui.
 *
 * Opens the system bus once and pumps the GLib main context non-blocking
 * via an LVGL timer registered inside ui_dbus_init().  All D-Bus code is
 * compiled out on the SDL host build (EROSQ_HOST) so that glib is not a
 * host build-time dependency.
 *
 * Usage:
 *   ui_dbus_set_ready_cb(cb) — register a callback fired (once) when the
 *                        system bus becomes reachable. Call BEFORE ui_dbus_init().
 *   ui_dbus_init()     — call once after lv_init(); connects to the system bus,
 *                        retrying on an LVGL timer if dbus-daemon isn't up yet
 *                        (fast boot launches erosq-ui before dbus-daemon).
 *                        Registers the GLib pump timer on success.
 *   ui_dbus_shutdown() — call before exit; deletes timers, unrefs connection.
 *   ui_dbus_conn()     — returns the GDBusConnection* (only on device build).
 */

#ifndef EROSQ_HOST
#include <gio/gio.h>
GDBusConnection *ui_dbus_conn(void);
#endif

void ui_dbus_set_ready_cb(void (*cb)(void));
int  ui_dbus_init(void);
void ui_dbus_shutdown(void);

/* Blanked sleep (main.c): pause the 30 ms pump and instead poll() the GLib
 * context's fds. poll_begin acquires+prepares the context and appends its fds
 * to p[] (returns the count; *timeout = GLib's timeout in ms, -1 = none);
 * poll_end MUST follow (same p) to check/dispatch and release. */
struct pollfd;
void ui_dbus_pump_pause(int pause);
int  ui_dbus_poll_begin(struct pollfd *p, int max, int *timeout);
void ui_dbus_poll_end(const struct pollfd *p);

#endif /* EROSQ_UI_DBUS_H */
