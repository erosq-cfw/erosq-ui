#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include "lvgl.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_input.h"
#include "ui_dbus.h"
#include "ui_bt.h"
#include "ui_mixer.h"
#include "ui_screens.h"
#include "ui_clock.h"
#include "ui_power.h"
#include "ui_wake_logic.h"

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Called by ui_dbus once the system bus is actually reachable. On a fast boot
 * erosq-ui starts before dbus-daemon, so this may fire a few hundred ms after
 * launch rather than immediately. ui_bt_init() then installs the org.bluez
 * name watch, which in turn seeds BT whenever bluetoothd appears. */
static void on_bus_ready(void) { ui_bt_init(); ui_bt_set_volume_cb(ui_volume_set_from_bt); }

/* Report hard faults to the log instead of dying silently. Async-signal-safe
 * only: fprintf/fflush can deadlock on the heap/stdio locks the fault may have
 * corrupted, leaving a hung process instead of an exit init can respawn. */
static void fault_handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc; (void)si;
    char msg[] = "erosq-ui: FATAL signal 00\n";
    msg[23] = (char)('0' + (sig / 10) % 10);
    msg[24] = (char)('0' + sig % 10);
    ssize_t r = write(2, msg, sizeof msg - 1); (void)r;
    _exit(128 + sig);
}

#ifndef EROSQ_HOST
/* Idle sleep, awake or blanked. Whenever nothing is held down, the indev read
 * timers + D-Bus pump are paused and we block in poll() on the input fds +
 * GLib's fds until LVGL's next real timer (refresh/animation while something
 * is moving, else the 1 s pollers) or the next lazy-open rescan. Before, the
 * awake loop woke every <=16 ms just to read idle evdev nodes (~60 wakeups/s
 * on a static Now Playing screen). Returns 1 if an input fd fired: the caller
 * then resumes the read timers so the unchanged read callbacks see the event
 * exactly as before (poll() never consumes evdev data) -- and sooner, since
 * poll() returns on the event instead of at the next 16 ms read tick. */
static int idle_wait(void)
{
    struct pollfd p[32];
    int glib_to;
    ui_input_blanked_rescan();                      /* BT pad lazy-open (its read cb is paused) */
    int ni = ui_input_pollfds(p, 16);
    int ng = ui_dbus_poll_begin(p + ni, 32 - ni, &glib_to);
    int to = ui_wake_timeout(lv_timer_get_time_to_next(), ui_input_rescan_wait(), glib_to);
    int r = poll(p, (nfds_t)(ni + ng), to);
    ui_dbus_poll_end(p + ni);                       /* dispatch D-Bus/GLib, release the context */
    if (r <= 0) return 0;
    for (int i = 0; i < ni; i++) if (p[i].revents) return 1;
    return 0;
}
#endif

int main(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* Localise the clock before anything paints: the RTC is UTC (ntpd runs on
     * WiFi connect) and there is no zoneinfo on the device, so the saved
     * timezone is applied as a POSIX TZ string. */
    ui_clock_apply_tz();

    struct ui_ctx ctx = {0};
    lv_init();
    lv_tick_set_cb(tick_cb);

#ifndef EROSQ_HOST
    ctx.disp = lv_linux_drm_create();
    /* No panel (card0 cannot be opened/set up) must be a non-zero exit: the
     * boot launcher only falls back to the splash when erosq-ui exits
     * non-zero. Ignoring this result left a headless UI running forever on a
     * black screen (HW-checked: bogus path -> no exit, no splash). Note this
     * does NOT catch "another client holds DRM master": setup succeeds and
     * only the later atomic commits fail with EACCES. */
    if (ctx.disp && lv_linux_drm_set_file(ctx.disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
        fprintf(stderr, "erosq-ui: cannot open /dev/dri/card0\n");
        return 1;
    }
    /* DIRECT (the lv_linux_drm default): render only dirty areas; LVGL keeps
     * the two dumb buffers in sync itself (refr_sync_areas draw_buf_copy), so
     * every committed buffer is still fully valid for the hold-GRAM SLCD.
     * The previous RENDER_MODE_FULL forced lv_inv_area to expand ALL damage
     * to full-screen, which defeated FB_DAMAGE_CLIPS band pushes AND re-drew
     * the whole UI every tick. */
    lv_display_set_render_mode(ctx.disp, LV_DISPLAY_RENDER_MODE_DIRECT);
#else
    ctx.disp = lv_sdl_window_create(320, 240);
#endif
    if (!ctx.disp) { fprintf(stderr, "erosq-ui: display init failed\n"); return 1; }
    ui_power_refresh_apply();   /* saved Display > Refresh Rate (default Smooth, 14 ms) */

    /* iPod look (or the saved Rockbox theme's colours): LVGL's default theme
     * recolours the stock widgets; ui_theme.c overrides bar/selection gloss. */
    ui_theme_load(ctx.disp);

#ifndef EROSQ_HOST
    /* Resolve by name — probe order (and thus eventN) is not stable across
     * kernel changes (gpio-keys polling -> interrupt-driven swapped these). */
    char keys_dev[32], wheel_dev[32];
    if (ui_evdev_by_name("gpio-keys",  keys_dev,  sizeof keys_dev))  snprintf(keys_dev,  sizeof keys_dev,  "/dev/input/event0");
    if (ui_evdev_by_name("scrollwheel", wheel_dev, sizeof wheel_dev)) snprintf(wheel_dev, sizeof wheel_dev, "/dev/input/event1");
    ui_input_init(&ctx, keys_dev);
    ui_wheel_init(&ctx, wheel_dev);
    ui_gamepad_init(&ctx);   /* BT gamepad (lazy-opens event node by name when connected) */
#else
    /* host: LVGL SDL keyboard, mapped to the same group. NOT a default group --
     * see ui_input_init: a default group auto-adds rows and scrolls long lists to
     * the bottom on open. Rows are added to their per-screen group explicitly. */
    ctx.group = lv_group_create();
    ctx.indev = lv_sdl_keyboard_create();
    lv_indev_set_group(ctx.indev, ctx.group);
#endif

    ui_mixer_open();      /* harmless -1 if codec absent; overlay degrades to UI-only */
    ui_volume_boot();     /* saved wired level (codec probes at -30 dB each boot) */
    ui_brightness_restore(); /* saved backlight level (driver default each boot) */

    ui_build(&ctx);

    ui_dbus_set_ready_cb(on_bus_ready);  /* wire BT up once the bus is reachable */
    ui_dbus_init();       /* connect to system bus, retrying if dbus-daemon isn't up yet */

    ctx.running = 1;
#ifndef EROSQ_HOST
    int sleeping = 0;      /* read timers + D-Bus pump paused (idle_wait mode) */
    int grace = 0;         /* one normal iteration after an input fd fired */
#endif
    while (ctx.running) {
        ui_avrcp_poll();   /* BT headphone play/pause/next/prev -> MPD */
        ui_jack_poll();    /* wired jacks all unplugged (and not on BT) -> pause MPD */
        uint32_t next = lv_timer_handler();
#ifndef EROSQ_HOST
        /* Nothing held down: sleep on the fds. The grace iteration after an
         * input event runs the old 16 ms path once, so even an fd that stays
         * readable can never spin faster than the old loop. Held keys/buttons
         * (POWER hold, long-press, key repeat) keep the 16 ms read cadence. */
        if (ui_input_quiet() && !grace) {
            if (!sleeping) { ui_input_reads_pause(&ctx, 1); ui_dbus_pump_pause(1); sleeping = 1; }
            if (idle_wait()) { ui_input_reads_pause(&ctx, 0); ui_dbus_pump_pause(0); sleeping = 0; grace = 1; }
            continue;
        }
        grace = 0;
        if (sleeping) { ui_input_reads_pause(&ctx, 0); ui_dbus_pump_pause(0); sleeping = 0; }
#endif
        if (next == LV_NO_TIMER_READY) next = 5;
        if (next > 16) next = 16;
        usleep(next * 1000);
    }
    ui_dbus_shutdown();
    return 0;
}
