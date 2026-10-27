#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "lvgl.h"
#include "ui_power.h"
#include "ui_power_logic.h"
#include "ui_refresh_logic.h"
#include "lvgl/src/display/lv_display_private.h"   /* disp->refr_timer: no public getter in 9.6 */
#include "ui_screens.h"   /* g_ctx (for the display's inactivity clock) */
#include "ui_mpd.h"
#include "ui_bt.h"        /* ui_bt_persist_volume() on graceful shutdown */
#include "ui_batt.h"
#include "ui_settings_logic.h"

#define AUTOOFF_PATH "/data/erosq-ui.autooff"
#define BLTIMEOUT_PATH "/data/erosq-ui.bltimeout"
#define BLWAKE_PATH    "/data/erosq-ui.blwake"
#define REFRESH_PATH   "/data/erosq-ui.refresh"
#define CHGLIMIT_PATH  "/data/erosq-ui.chglimit"
#define CRITBATT_PATH  "/data/erosq-ui.critbatt"
#define BLKEYS_PATH    "/data/erosq-ui.blkeys"
#define VOLLIMIT_PATH  "/data/erosq-ui.vollimit"
#define DISCPAUSE_PATH "/data/erosq-ui.discpause"
#define VOLUME_PATH    "/data/erosq-ui.volume"
#define BRIGHTNESS_PATH "/data/erosq-ui.brightness"
#define BTBUTTONS_PATH  "/data/erosq-ui.btbuttons"
#define CHGLIMIT_SYSFS "/sys/class/power_supply/axp2101-battery/charge_control_end_threshold"
#define UI_CHGLIMIT_MIN 50     /* kernel rejects anything lower */

/* Cached settings. -1 = not yet loaded. */
static int s_autooff = -1;
static int s_bltimeout = -1;   /* seconds; -1 = not yet loaded */
static int s_blwake    = -1;   /* 0/1/2;   -1 = not yet loaded */
static int s_refresh   = -1;   /* UI_REFRESH_*; -1 = not yet loaded */
static int s_chglimit  = -1;   /* percent; -1 = not yet loaded */
static int s_critbatt  = -1;   /* percent, 0 = Off; -1 = not yet loaded */

int ui_power_autooff_get(void)
{
    if (s_autooff < 0) {
        s_autooff = UI_AUTOOFF_DEFAULT;
        int fd = open(AUTOOFF_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; int v = atoi(buf); if (v >= 0) s_autooff = v; }
        }
    }
    return s_autooff;
}

void ui_power_autooff_set(int minutes)
{
    if (minutes < 0) minutes = 0;
    s_autooff = minutes;
    int fd = open(AUTOOFF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* ponytail: no /data -> just don't persist */
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", minutes);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

int ui_power_bltimeout_get(void)
{
    if (s_bltimeout < 0) {
        s_bltimeout = UI_BLTIMEOUT_DEFAULT;
        int fd = open(BLTIMEOUT_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; int v = atoi(buf); if (v >= 0) s_bltimeout = v; }
        }
    }
    return s_bltimeout;
}

void ui_power_bltimeout_set(int secs)
{
    if (secs < 0) secs = 0;
    s_bltimeout = secs;
    int fd = open(BLTIMEOUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* no /data -> just don't persist */
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", secs);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

int ui_power_blwake_get(void)
{
    if (s_blwake < 0) {
        s_blwake = UI_BLWAKE_DEFAULT;
        int fd = open(BLWAKE_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; int v = atoi(buf);
                         if (v >= 0 && v <= 2) s_blwake = v; }   /* else keep default */
        }
    }
    return s_blwake;
}

void ui_power_blwake_set(int mode)
{
    if (mode < 0 || mode > 2) mode = UI_BLWAKE_DEFAULT;
    s_blwake = mode;
    int fd = open(BLWAKE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", mode);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

/* Display refresh rate (Display > Refresh Rate). Drives only the LVGL display
 * refresh timer's period -- live, no restart; the indev read timers keep the
 * lv_conf.h LV_DEF_REFR_PERIOD cadence so input latency is unchanged. */
int ui_power_refresh_get(void)
{
    if (s_refresh < 0) {
        s_refresh = UI_REFRESH_DEFAULT;
        int fd = open(REFRESH_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; s_refresh = ui_refresh_parse(buf); }
        }
    }
    return s_refresh;
}

void ui_power_refresh_apply(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d && d->refr_timer)
        lv_timer_set_period(d->refr_timer, ui_refresh_period_ms(ui_power_refresh_get()));
}

void ui_power_refresh_set(int mode)
{
    if (!ui_refresh_valid(mode)) mode = UI_REFRESH_DEFAULT;
    s_refresh = mode;
    ui_power_refresh_apply();
    int fd = open(REFRESH_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", mode);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

/* Charge ceiling. The number lives in /data (ours, survives reboots) and is
 * pushed into the kernel's standard charge_control_end_threshold, where
 * axp2101_battery polls the gauge and gates the charger. The kernel default is
 * 100 (off) on every boot, hence ui_power_chglimit_apply() at startup.
 * ponytail: no state machine here -- one number, written twice. */
int ui_power_chglimit_get(void)
{
    if (s_chglimit < 0) {
        s_chglimit = UI_CHGLIMIT_OFF;
        int fd = open(CHGLIMIT_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; int v = atoi(buf);
                         if (v >= UI_CHGLIMIT_MIN && v <= UI_CHGLIMIT_OFF) s_chglimit = v; }
        }
    }
    return s_chglimit;
}

void ui_power_chglimit_set(int pct)
{
    if (pct < UI_CHGLIMIT_MIN || pct > UI_CHGLIMIT_OFF) pct = UI_CHGLIMIT_OFF;
    s_chglimit = pct;
    int fd = open(CHGLIMIT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", pct);
        if (write(fd, buf, n) < 0) { /* ignore */ }
        close(fd);
    }
    ui_power_chglimit_apply();
}

void ui_power_chglimit_apply(void)
{
    int fd = open(CHGLIMIT_SYSFS, O_WRONLY);
    if (fd < 0) return;               /* pre-charge-limit kernel: setting is inert */
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", ui_power_chglimit_get());
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

/* --- settings-batch1: small /data/erosq-ui.<name> int files ---
 * ponytail: shared read/write pair for the new settings only; the older ones
 * above keep their own copies (not worth churning tested code). */
static int file_read(const char *path, char *buf, int cap)   /* 1 = buf filled */
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return 1;
}

static void file_write_int(const char *path, int v)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* no /data -> just don't persist */
    char buf[12]; int n = lv_snprintf(buf, sizeof buf, "%d", v);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    fsync(fd);   /* else page cache holds it ~30 s (dirty_expire); a hard power-off loses it */
    close(fd);
}

static int s_blkeys = -1, s_vollimit = -1, s_discpause = -1;   /* -1 = not loaded */

/* Display > Screen-Off Buttons (UI_BLKEYS_*). */
int ui_power_blkeys_get(void)
{
    if (s_blkeys < 0) {
        char b[12];
        s_blkeys = file_read(BLKEYS_PATH, b, sizeof b)
            ? ui_setting_parse(b, 0, UI_BLKEYS_NCHOICES - 1, UI_BLKEYS_DEFAULT) : UI_BLKEYS_DEFAULT;
    }
    return s_blkeys;
}
void ui_power_blkeys_set(int mode)
{
    if (mode < 0 || mode >= UI_BLKEYS_NCHOICES) mode = UI_BLKEYS_DEFAULT;
    s_blkeys = mode;
    file_write_int(BLKEYS_PATH, mode);
}

/* Settings > Volume Limit (0 = Off, else max percent). */
int ui_power_vollimit_get(void)
{
    if (s_vollimit < 0) {
        char b[12];
        s_vollimit = file_read(VOLLIMIT_PATH, b, sizeof b) ? ui_vollimit_parse(b) : UI_VOLLIMIT_OFF;
    }
    return s_vollimit;
}
void ui_power_vollimit_set(int pct)
{
    char b[12]; lv_snprintf(b, sizeof b, "%d", pct);
    s_vollimit = ui_vollimit_parse(b);         /* off-list -> Off */
    file_write_int(VOLLIMIT_PATH, s_vollimit);
}

/* Settings > Pause on Disconnect (1 = pause when the last wired jack or the BT
 * sink goes away, 0 = keep playing). */
int ui_power_discpause_get(void)
{
    if (s_discpause < 0) {
        char b[12];
        s_discpause = file_read(DISCPAUSE_PATH, b, sizeof b) ? ui_setting_parse(b, 0, 1, 1) : 1;
    }
    return s_discpause;
}
void ui_power_discpause_set(int on)
{
    s_discpause = on ? 1 : 0;
    file_write_int(DISCPAUSE_PATH, s_discpause);
}

/* Settings > Playback > Bluetooth Buttons (1 = On). */
static int s_btbuttons = -1;
int ui_power_btbuttons_get(void)
{
    if (s_btbuttons < 0) {
        char b[12];
        s_btbuttons = file_read(BTBUTTONS_PATH, b, sizeof b)
            ? ui_setting_parse(b, 0, 1, UI_BTBUTTONS_DEFAULT) : UI_BTBUTTONS_DEFAULT;
    }
    return s_btbuttons;
}
void ui_power_btbuttons_set(int on)
{
    s_btbuttons = on ? 1 : 0;
    file_write_int(BTBUTTONS_PATH, s_btbuttons);
}

/* Last wired volume / backlight level, restored at boot. The saved copy is
 * cached so a save with no change costs no NAND write. */
static int s_saved_vol = -2, s_saved_bl = -2;   /* -2 = not loaded, -1 = none */
int ui_power_saved_volume(void)
{
    if (s_saved_vol == -2) {
        char b[12];
        s_saved_vol = file_read(VOLUME_PATH, b, sizeof b) ? ui_volfile_parse(b) : -1;
    }
    return s_saved_vol;
}
void ui_power_save_volume(int pct)
{
    if (pct < 0 || pct > 100 || pct == ui_power_saved_volume()) return;
    s_saved_vol = pct;
    file_write_int(VOLUME_PATH, pct);
}
int ui_power_saved_brightness(void)
{
    if (s_saved_bl == -2) {
        char b[12];
        s_saved_bl = file_read(BRIGHTNESS_PATH, b, sizeof b) ? ui_blfile_parse(b) : -1;
    }
    return s_saved_bl;
}
void ui_power_save_brightness(int level)
{
    if (level < 1 || level > 255 || level == ui_power_saved_brightness()) return;
    s_saved_bl = level;
    file_write_int(BRIGHTNESS_PATH, level);
}

/* Critical-battery power-off threshold (Settings > Power > Low Battery Off). */
int ui_power_critbatt_get(void)
{
    if (s_critbatt < 0) {
        s_critbatt = CRITBATT_DEFAULT;
        int fd = open(CRITBATT_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; s_critbatt = critbatt_parse(buf); }
        }
    }
    return s_critbatt;
}

void ui_power_critbatt_set(int pct)
{
    char buf[12];
    lv_snprintf(buf, sizeof buf, "%d", pct);
    s_critbatt = critbatt_parse(buf);          /* off-list -> default */
    int fd = open(CRITBATT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    int n = lv_snprintf(buf, sizeof buf, "%d", s_critbatt);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}

/* One gauge poll through the debounce. Called from the 10 s auto-off tick and,
 * while an Extras game owns the panel, from the launcher's wait loop. */
static int s_crit_low_reads;
int ui_power_critbatt_due(void)
{
    return critbatt_should_power_off(ui_power_critbatt_get(), ui_batt_read(NULL),
                                     ui_batt_on_battery(), &s_crit_low_reads);
}

/* "Battery critical" notice, then power off after CRITBATT_WARN_MS unless a
 * charger has gone in by then. One-shot timer: no new polling cadence. */
#define CRITBATT_WARN_MS 5000
static lv_obj_t *s_crit_box;

static void critbatt_expire_cb(lv_timer_t *t)
{
    (void)t;                                   /* repeat_count 1: LVGL deletes it */
    lv_obj_delete(s_crit_box);
    s_crit_box = NULL;
    if (ui_batt_on_battery()) { ui_power_off(); return; }
    s_crit_low_reads = 0;                      /* charger in: cancelled, re-arm */
}

void ui_power_critbatt_warn(void)
{
    if (s_crit_box) return;                    /* window already open */
    ui_blank_force_wake();                     /* the notice must be seen */
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    s_crit_box = box;
    lv_obj_set_width(box, 260);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xc62828), 0);   /* red: about to cut power */
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_center(box);
    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "Battery critical\nPowering off\n\nConnect a charger to cancel");
    lv_timer_t *tm = lv_timer_create(critbatt_expire_cb, CRITBATT_WARN_MS, NULL);
    lv_timer_set_repeat_count(tm, 1);
}

/* True iff MPD reports "state: play". Any error -> not playing (fails closed:
 * worst case the idle timeout still governs when MPD is unreachable). */
static int mpd_is_playing(void)
{
    char r[512], f[64];
    if (mpd_cmd("status", r, sizeof r) > 0 && mpd_field(r, "state", f, sizeof f))
        return strcmp(f, "play") == 0;    /* vs "pause"/"stop"; matches ui_screens.c:94 */
    return 0;
}

void ui_power_off(void)
{
    /* Paint a "Shutting down" notice and force it to the panel BEFORE the
     * blocking stop/poweroff calls, so the UI thread doesn't freeze on a stale
     * frame (mpdctl/poweroff can take a couple of seconds). Overlay lives on the
     * top layer so it covers whatever screen is loaded. */
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_t *lbl = lv_label_create(ov);
    lv_label_set_text(lbl, "Shutting down...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    lv_refr_now(NULL);   /* synchronous flush to the SLCD before we block */

    /* Persist the connected BT device's volume before power-down: we only write
     * on this graceful path + on disconnect, not per wheel step. poweroff below
     * runs busybox's sync(), so this on-disk write is flushed. */
    ui_bt_persist_volume();
    ui_volume_persist();       /* wired level, if changed since the last save */
    ui_brightness_persist();   /* covers POWER-hold off from the Brightness screen */

    /* Shut mpd down cleanly and snapshot queue+track+elapsed to /data so Now
     * Playing comes back (paused, same spot) after power-on. Also quiets the
     * output. Ignore failure: no MPD = nothing to save. See mpd-persist. */
    if (system("mpd-persist") == -1) { /* ignore */ }
    /* busybox installs the applet at /sbin/poweroff (not /usr/sbin); use the
     * path that exists on-device so system() doesn't 127 and silently no-op. */
    if (system("/sbin/poweroff") == -1) { /* ignore */ }
}

/* USB file-transfer mode hands the SD card to the host and STOPS mpd, so the
 * device looks maximally idle -- no keypresses, no playback -- while a long copy
 * is in flight. The mpd_is_playing() inhibit can't cover it, and cutting power
 * with the drive mounted on the host is the worst possible moment.
 *
 * Ask the gadget itself instead of tracking a UI flag: the mass-storage LUN has a
 * backing file only while the SD is exposed (see erosq-usbgadget), so this is
 * ground truth and also covers a transfer started outside the UI. Reads 0 bytes
 * in network mode; ENOENT before the gadget is composed. */
#define USB_LUN_FILE "/sys/kernel/config/usb_gadget/erosq/functions/mass_storage.0/lun.0/file"

static int usb_transfer_active(void)
{
    char buf[8];
    int fd = open(USB_LUN_FILE, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    return n > 0;
}

static void autooff_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ctx || !g_ctx->disp) return;
    /* Rides this always-on 10 s tick (batt_update skips while blanked). */
    if (!s_crit_box && ui_power_critbatt_due()) { ui_power_critbatt_warn(); return; }
    if (usb_transfer_active()) return;   /* a host file copy shows no UI activity */
    uint32_t idle = lv_display_get_inactive_time(g_ctx->disp);
    if (autooff_should_power_off(ui_power_autooff_get(), idle, mpd_is_playing()))
        ui_power_off();
}

void ui_power_autooff_init(void)
{
    static lv_timer_t *timer;
    if (timer) lv_timer_delete(timer);        /* re-init safe (launcher reacquire) */
    timer = lv_timer_create(autooff_cb, 10000, NULL);
}
