#include <stdio.h>
#include <poll.h>
#include "ui_dbus.h"

#ifndef EROSQ_HOST
/*
 * Device build: open the system bus with g_bus_get_sync() and pump the
 * GLib main context non-blocking via an LVGL timer (30 ms period, 8
 * iterations max per tick so the LVGL frame budget is not blown).
 *
 * Design notes:
 * - We use the default GMainContext (NULL) so that any GLib/GIO async
 *   operations that future modules register against the default context
 *   are drained here automatically.
 * - g_main_context_iteration(NULL, FALSE) returns immediately if there is
 *   nothing pending, so it adds at most a handful of microseconds per tick
 *   when idle.
 * - The connection is created synchronously at startup; if the system bus
 *   is absent (e.g. dbus-daemon not running) we log a warning and return -1.
 */

#include "lvgl.h"

static GDBusConnection *s_conn;
static lv_timer_t      *s_pump_timer;
static lv_timer_t      *s_retry_timer;
static void           (*s_ready_cb)(void);

GDBusConnection *ui_dbus_conn(void)
{
    return s_conn;
}

void ui_dbus_set_ready_cb(void (*cb)(void))
{
    s_ready_cb = cb;
}

static void pump_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < 8 && g_main_context_iteration(NULL, FALSE); i++) {}
}

/* --- Blanked sleep (main.c) ------------------------------------------------
 * While blanked the 30 ms pump timer is paused and main.c sleeps in poll() on
 * the GLib context's own fds instead, with GLib's timeout: the canonical
 * external-poll split of g_main_context_iteration (prepare/query, poll,
 * check/dispatch). The context's wakeup eventfd is among those fds, so a D-Bus
 * message (queued by the gdbus worker thread) or a source attached from
 * another thread wakes poll() at once. We must hold the context acquired from
 * prepare to dispatch: GLib only signals that eventfd for an attach when the
 * context has an owner. */
#define DBUS_MAX_FDS 16
static GPollFD s_gfds[DBUS_MAX_FDS];
static int     s_gn, s_gprio, s_gacq;
static int     s_pump_paused;   /* last request from main.c; applied to a pump created later */

void ui_dbus_pump_pause(int pause)
{
    s_pump_paused = pause;
    if (!s_pump_timer) return;
    if (pause) lv_timer_pause(s_pump_timer);
    else { lv_timer_resume(s_pump_timer); lv_timer_ready(s_pump_timer); }
}

int ui_dbus_poll_begin(struct pollfd *p, int max, int *timeout)
{
    *timeout = -1;
    s_gn = 0;
    if (!s_conn) return 0;                   /* no pump yet: nothing GLib to service */
    if (!g_main_context_acquire(NULL)) { *timeout = 30; return 0; }   /* paranoia: old cadence */
    s_gacq = 1;
    g_main_context_prepare(NULL, &s_gprio);
    int n = g_main_context_query(NULL, s_gprio, timeout, s_gfds, DBUS_MAX_FDS);
    if (n > DBUS_MAX_FDS || n > max) {       /* too many fds: fall back to the old cadence */
        n = 0;
        if (*timeout < 0 || *timeout > 30) *timeout = 30;
    }
    for (int i = 0; i < n; i++) { p[i].fd = s_gfds[i].fd; p[i].events = (short)s_gfds[i].events; p[i].revents = 0; }
    s_gn = n;
    return n;
}

void ui_dbus_poll_end(const struct pollfd *p)
{
    if (!s_gacq) return;
    for (int i = 0; i < s_gn; i++) s_gfds[i].revents = (gushort)p[i].revents;
    if (g_main_context_check(NULL, s_gprio, s_gfds, s_gn)) g_main_context_dispatch(NULL);
    g_main_context_release(NULL);
    s_gacq = 0;
}

/* Try once to open the system bus. On success: start the pump, fire the ready
 * callback, and return 1. On failure (dbus-daemon not up yet): return 0. */
static int try_connect(void)
{
    GError *err = NULL;
    s_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!s_conn) {
        g_clear_error(&err);
        return 0;
    }
    s_pump_timer = lv_timer_create(pump_cb, 30, NULL);
    /* On a fast boot the bus comes up (retry_cb) while main.c already sleeps
     * in idle_wait() with the pump "paused" -- before the timer existed. A
     * pump created running then woke the idle loop every 30 ms until the next
     * input (auto-blank idle: ~36 wakeups/s instead of ~4). idle_wait()
     * services GLib's fds itself, so start paused when that was requested. */
    if (s_pump_paused) lv_timer_pause(s_pump_timer);
    if (s_ready_cb) s_ready_cb();
    return 1;
}

/* Retry connecting until dbus-daemon is up, then stop. Runs on the LVGL loop,
 * so it works before the GLib pump exists. */
static void retry_cb(lv_timer_t *t)
{
    if (try_connect()) {
        lv_timer_delete(t);
        s_retry_timer = NULL;
    }
}

int ui_dbus_init(void)
{
    if (try_connect()) return 0;
    /* Fast boot: erosq-ui started before dbus-daemon. Keep retrying; the bus
     * (and BT, via the ready callback) will come up within a second or two. */
    if (!s_retry_timer)
        s_retry_timer = lv_timer_create(retry_cb, 500, NULL);
    return 0;
}

void ui_dbus_shutdown(void)
{
    if (s_retry_timer) {
        lv_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    if (s_pump_timer) {
        lv_timer_delete(s_pump_timer);
        s_pump_timer = NULL;
    }
    if (s_conn) {
        g_object_unref(s_conn);
        s_conn = NULL;
    }
}

#else  /* EROSQ_HOST — glib not available */

void ui_dbus_set_ready_cb(void (*cb)(void)) { (void)cb; }
void ui_dbus_pump_pause(int pause) { (void)pause; }
int  ui_dbus_poll_begin(struct pollfd *p, int max, int *timeout) { (void)p; (void)max; *timeout = -1; return 0; }
void ui_dbus_poll_end(const struct pollfd *p) { (void)p; }
int  ui_dbus_init(void)     { return -1; }
void ui_dbus_shutdown(void) {}

#endif /* EROSQ_HOST */
