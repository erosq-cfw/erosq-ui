#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "ui_input.h"
#include "ui_batt.h"
#include "ui_launch.h"
#include "ui_net.h"
#include "ui_mixer.h"
#include "ui_mpd.h"
#include "ui_vlist.h"
#include "ui_seek.h"
#include "ui_np.h"
#include "ui_art.h"
#include "ui_bt.h"
#include "ui_power.h"
#include "ui_power_logic.h"
#include "ui_refresh_logic.h"
#include "ui_settings_logic.h"
#include "ui_usb_logic.h"
#include "ui_update.h"
#include "ui_clock.h"
#include "update_logic.h"
#include "about_logic.h"
#include "ui_memtrim.h"
#include "lvgl/src/misc/lv_text_private.h"   /* lv_text_get_width: private since 9.6 */

#define BL_DIR "/sys/class/backlight/erosq-bl/"
static void bl_write(const char *attr, int val)
{
    char path[64], buf[12];
    lv_snprintf(path, sizeof path, BL_DIR "%s", attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    int n = lv_snprintf(buf, sizeof buf, "%d", val);
    if (write(fd, buf, n) < 0) { /* ignore */ }
    close(fd);
}
static int bl_read(const char *attr)
{
    char path[64], buf[12];
    lv_snprintf(path, sizeof path, BL_DIR "%s", attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

struct ui_ctx *g_ctx;
static ui_nav_t s_nav;

/* Transient toast on the top layer; auto-dismisses (~0.8s). */
static void toast_del_cb(lv_timer_t *t) { lv_obj_delete(lv_timer_get_user_data(t)); }
/* The toast box itself, with no auto-dismiss: for a "busy" notice the caller
 * deletes the moment the operation finishes (a timed toast would linger on top
 * of whatever result screen follows). */
static lv_obj_t *ui_toast_box(const char *txt)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, UI_COL_ACCENT, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_color(l, UI_COL_ON_ACCENT, 0);   /* white on the blue toast */
    lv_label_set_text(l, txt);
    return box;
}
static void ui_toast_ms(const char *txt, uint32_t ms)
{
    lv_obj_t *box = ui_toast_box(txt);
    lv_timer_t *tm = lv_timer_create(toast_del_cb, ms, box);
    lv_timer_set_repeat_count(tm, 1);
}
static void ui_toast(const char *txt) { ui_toast_ms(txt, 800); }

/* Playback-error popup: a centred box on the top layer with a message line and
 * a smaller hint line. Longer-lived than ui_toast (~5s) so the hint is readable,
 * and only one is ever shown -- a new error replaces the old (both the box and
 * its pending auto-dismiss timer, so the old timer can't delete the new box). */
static lv_obj_t  *s_err_popup;
static lv_timer_t *s_err_timer;
static void err_popup_close(void)
{
    if (s_err_timer) { lv_timer_delete(s_err_timer); s_err_timer = NULL; }
    if (s_err_popup) { lv_obj_delete(s_err_popup);   s_err_popup = NULL; }
}
static void err_popup_timeout(lv_timer_t *t)
{
    (void)t;
    s_err_timer = NULL;   /* repeat_count 1: LVGL auto-deletes it after this cb */
    err_popup_close();
}
void ui_error_popup(const char *msg, const char *hint)
{
    err_popup_close();                       /* replace any current one */
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    s_err_popup = box;
    lv_obj_set_width(box, 260);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, UI_COL_ACCENT, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(box);

    lv_obj_t *m = lv_label_create(box);
    lv_obj_set_style_text_color(m, UI_COL_ON_ACCENT, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_label_set_text(m, msg);

    lv_obj_t *h = lv_label_create(box);
    lv_obj_set_style_text_color(h, lv_color_hex(0xd0d8e8), 0);   /* dimmer than the white msg */
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
    lv_label_set_text(h, hint);

    s_err_timer = lv_timer_create(err_popup_timeout, 5000, NULL);
    lv_timer_set_repeat_count(s_err_timer, 1);
}

/* Point BOTH input devices (button keypad + wheel encoder) at a screen's group. */
static void clock_refresh(void);

/* Every screen change ends here, after the screen load showed or hid a theme's
 * side panel: the status clock follows (see clock_refresh). */
static void set_active_group(lv_obj_t *scr)
{
    lv_group_t *g = ui_screen_group(scr);
    lv_indev_set_group(g_ctx->indev, g);
    if (g_ctx->enc) lv_indev_set_group(g_ctx->enc, g);
    if (g_ctx->pad) lv_indev_set_group(g_ctx->pad, g);
    if (ui_theme_panel_w()) clock_refresh();
}

/* Backlight blank state: set by blank_set(). Read by the periodic visual poll
 * callbacks below to skip redraws (and thus SLCD DMAs) while the screen is off. */
static int s_blanked;
static lv_timer_t *s_preview_timer;   /* Home now-playing preview poll (session-lived) */

/* lv_label_set_text() invalidates even when the text is identical, so any
 * periodic poll that re-sets labels marks them dirty every tick. With
 * FB_DAMAGE_CLIPS that unions the 1s status-icon (top) + Now Playing time
 * (bottom) rewrites into a full-screen clip -> full SLCD push instead of a
 * band. Compare before setting: unchanged text = zero damage. */
static void lbl_set(lv_obj_t *l, const char *txt)
{
    if (l && strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

/* Persistent battery widget on the top layer (floats over every screen's status
 * bar, top-right). Polled every 10s from the AXP2101 fuel gauge. */
static lv_obj_t  *s_batt_lbl;
static lv_timer_t *s_batt_timer;
static void batt_update(lv_timer_t *t)
{
    (void)t;
    if (s_blanked) return;   /* screen off: skip gauge read + icon redraw */
    int p = ui_batt_read(NULL);
    if (p < 0) return;   /* transient sysfs read failure: keep the last icon, don't fake "empty" */
    /* iPod status bar shows the battery as an icon only (no percentage).
     * Charging swaps the level glyph for the bolt -- the real iPod overlays the
     * two, but FontAwesome has no charging-battery glyph and stacking labels
     * would widen the icon into the clock. LV_SYMBOL_CHARGE is 9px vs the
     * battery's 18px, so the clock's fixed gap only ever grows (see
     * ui_theme_clock_right). Exact % stays on the About screen. */
    const char *sym = ui_batt_charging() ? LV_SYMBOL_CHARGE :
                      p >= 80 ? LV_SYMBOL_BATTERY_FULL  : p >= 55 ? LV_SYMBOL_BATTERY_3 :
                      p >= 30 ? LV_SYMBOL_BATTERY_2     : p >= 10 ? LV_SYMBOL_BATTERY_1 :
                                LV_SYMBOL_BATTERY_EMPTY;
    lbl_set(s_batt_lbl, sym);
}

/* iPod play/pause indicator in the top-left of the status bar (▶ playing, ❙❙
 * paused, hidden when stopped). Floats on the top layer like the battery. */
static lv_obj_t  *s_play_lbl;
static lv_timer_t *s_play_timer;
/* Clock label, left of the battery. Updated from this same 1s poll rather than
 * getting its own timer: lbl_set() suppresses the redraw while HH:MM is
 * unchanged, so the clock only dirties the status bar once a minute. */
static lv_obj_t  *s_clock_lbl;
/* Repaint the clock now. The 1s poll would catch it within a second anyway, but
 * a timezone/format change should show the moment you leave the picker. */
static void clock_refresh(void)
{
    char clk[UI_CLOCK_BUF];
    ui_clock_now(clk, sizeof clk);
    if (ui_theme_panel_w()) {   /* theme side panel shows the time (+ date) instead */
        char date[UI_CLOCK_DATE_BUF];
        ui_clock_date(time(NULL), date, sizeof date);
        ui_theme_panel_clock(clk, date);
        if (ui_theme_panel_shown()) clk[0] = '\0';   /* full-width screens keep the status clock */
    }
    lbl_set(s_clock_lbl, clk);
}

static void playstate_update(lv_timer_t *t)
{
    (void)t;
    if (s_blanked) return;   /* screen off: skip status poll + icon redraw */
    char r[512], f[64];
    const char *g = "";
    if (mpd_cmd("status", r, sizeof r) > 0 && mpd_field(r, "state", f, sizeof f)) {
        if      (strcmp(f, "play")  == 0) g = LV_SYMBOL_PLAY;
        else if (strcmp(f, "pause") == 0) g = LV_SYMBOL_PAUSE;
    }
    lbl_set(s_play_lbl, g);
    clock_refresh();
}
static void backlight_tick_cb(lv_timer_t *t);   /* forward decl; defined below blank_set */
static void blank_set(int on, int auto_flag);    /* forward decl; ui_build un-blanks at start */
static lv_timer_t *s_bl_timer;   /* backlight_tick_cb; paused while blanked (blank_set) */
static lv_timer_t *s_libupd_tm;  /* Update/Rescan Library poll; paused while blanked */

static void ui_status_init(void)
{
    s_batt_lbl = lv_label_create(lv_layer_top());
    lv_label_set_text(s_batt_lbl, "");   /* blank until the first successful gauge read */
    ui_style_bar_text(s_batt_lbl);   /* white over the blue title bar */
    lv_obj_align(s_batt_lbl, LV_ALIGN_TOP_RIGHT, -THEME_BATT_RIGHT, 4);
    if (s_batt_timer) lv_timer_delete(s_batt_timer);   /* re-init safe (launcher reacquire) */
    s_batt_timer = lv_timer_create(batt_update, 10000, NULL);
    batt_update(NULL);

    s_play_lbl = lv_label_create(lv_layer_top());
    ui_style_bar_text(s_play_lbl);
    lv_obj_align(s_play_lbl, LV_ALIGN_TOP_LEFT, THEME_PLAY_X, 4);

    /* Clock sits left of the battery glyph, right-aligned so the wider 12-hour
     * strings grow leftwards (away from the battery) rather than into it.
     * Offset is derived, not eyeballed (ui_theme_clock_right): with the iPod
     * font the battery's left edge is 5+18=23px from the right, +6px gap. The
     * widest string ("12:00 PM", 61px) then spans x=230..291, clear of the
     * fixed 128px title (x=96..224); a theme font is measured the same way. */
    s_clock_lbl = lv_label_create(lv_layer_top());
    lv_label_set_text(s_clock_lbl, "");
    ui_style_bar_text(s_clock_lbl);
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_RIGHT, -ui_theme_clock_right(), 4);

    if (s_play_timer) lv_timer_delete(s_play_timer);
    s_play_timer = lv_timer_create(playstate_update, 1000, NULL);
    playstate_update(NULL);
    ui_power_autooff_init();     /* idle auto-shutdown watchdog */
    ui_power_chglimit_apply();   /* kernel resets the ceiling to "off" each boot */
    /* Backlight idle blank/wake watchdog. Root menu lives for the whole
     * session, so this poll needs no teardown. */
    s_bl_timer = lv_timer_create(backlight_tick_cb, 500, NULL);
    /* Start awake for real: bl_power outlives the process, and a restart (no
     * modeset, so erosq-drm's backlight_enable doesn't run) inherited the dead
     * instance's 4 = off -> dark panel while s_blanked said awake. Boot and the
     * U-Boot splash handover already have it at 0, where the sysfs write is a
     * no-op (no PWM re-apply, no flicker). After ui_brightness_restore (main.c). */
    blank_set(0, 0);
}

/* Transient volume overlay (centered, on lv_layer_top). No audio backend yet, so
 * the level is a UI value 0-100; wire to the ALSA mixer when audio lands. */
static int s_vol = 50;
static lv_obj_t *s_vol_box, *s_vol_bar;
static lv_timer_t *s_vol_timer;
static int s_vol_dirty;   /* wired (DAC) level changed since the last /data save */

/* Settings > Volume Limit: pull both outputs down to the cap. The DAC keeps its
 * level while BT is the sink, so it is checked even then. */
void ui_volume_enforce_limit(void)
{
    int lim = ui_power_vollimit_get();
    int p = ui_mixer_get_pct();
    if (p >= 0 && ui_vol_cap(p, lim) != p) { ui_mixer_set_pct(ui_vol_cap(p, lim)); s_vol_dirty = 1; }
    if (ui_bt_audio_active() && ui_vol_cap(s_vol, lim) != s_vol)
        ui_bt_set_volume_pct(s_vol);   /* caps inside */
    s_vol = ui_vol_cap(s_vol, lim);
}
void ui_volume_sync(void){ int p = ui_mixer_get_pct(); if (p >= 0) { s_vol = p; s_vol_dirty = 1; } ui_volume_enforce_limit(); }  /* dirty: an unchanged level is a cached no-op */

/* Boot: the codec probes at its -30 dB default every power-on, so put back the
 * last saved wired level (capped). No saved file -> keep the codec's. */
void ui_volume_boot(void)
{
    int v = ui_power_saved_volume();
    if (v < 0) v = ui_mixer_get_pct();
    if (v < 0) return;
    s_vol = ui_vol_cap(v, ui_power_vollimit_get());
    ui_mixer_set_pct(s_vol);
}

/* Save the wired level if it changed. Runs when the overlay hides (1.2 s after
 * the last press: the debounce, no extra timer) and on graceful power-off. */
void ui_volume_persist(void)
{
    if (!s_vol_dirty) return;
    s_vol_dirty = 0;
    int p = ui_mixer_get_pct();   /* the DAC's own level, not s_vol (may be BT's) */
    if (p >= 0) ui_power_save_volume(p);
}
static void vol_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(s_vol_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
    ui_volume_persist();
}
/* Create-if-needed + show the volume overlay at the current s_vol. */
static void vol_show(void)
{
    if (!s_vol_box) {
        s_vol_box = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_vol_box, 220, 50);
        lv_obj_center(s_vol_box);
        lv_obj_set_style_border_width(s_vol_box, 2, 0);
        lv_obj_clear_flag(s_vol_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic = lv_label_create(s_vol_box);
        lv_label_set_text(ic, LV_SYMBOL_VOLUME_MAX);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, -4);
        s_vol_bar = lv_bar_create(s_vol_box);
        lv_obj_set_size(s_vol_bar, 200, 12);
        lv_obj_align(s_vol_bar, LV_ALIGN_BOTTOM_MID, 0, 2);
        lv_bar_set_range(s_vol_bar, 0, 100);
        s_vol_timer = lv_timer_create(vol_hide_cb, 1200, NULL);
    }
    lv_obj_set_style_bg_color(s_vol_box, UI_COL_BG, 0);          /* per show: the theme may */
    lv_obj_set_style_border_color(s_vol_box, UI_COL_ACCENT, 0);  /* have changed since */
    lv_obj_clear_flag(s_vol_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_vol_box);
    lv_bar_set_value(s_vol_bar, s_vol, LV_ANIM_OFF);
    lv_timer_reset(s_vol_timer);
    lv_timer_resume(s_vol_timer);
}

void ui_volume(int delta)
{
    s_vol = ui_vol_cap(s_vol + delta, ui_power_vollimit_get());   /* 0..100 and Volume Limit */
    /* Single volume choke point. DAC output = hardware mixer. When a Bluetooth
     * A2DP sink is active it bypasses the DAC, so drive the BlueALSA software
     * volume instead of the codec. */
    if (ui_bt_audio_active()) ui_bt_set_volume_pct(s_vol);
    else { ui_mixer_set_pct(s_vol); s_vol_dirty = 1; }  /* no-op if mixer unavailable */
    vol_show();
}

/* Volume reported BY the BT speaker (AVRCP VOLUME_CHANGED, via ui_bt). Update the
 * UI only -- do NOT push back to the speaker (redundant echo + ping-pong risk).
 * Cheap + burst-safe: early-out on no change. */
void ui_volume_set_from_bt(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    if (pct == s_vol) return;
    s_vol = pct;
    vol_show();
}

/* Power key: real backlight off/on via the erosq-drm backlight class. */
static int s_auto_blanked;   /* 1 iff the current blank came from the idle timeout */

/* Single entry point for all blank-state changes. auto_flag is recorded only
 * when blanking; the wake gate / poll timer use it to decide what may wake. */
static void blank_set(int on, int auto_flag)
{
    s_blanked = on;
    s_auto_blanked = on ? auto_flag : 0;
    bl_write("bl_power", on ? 4 : 0);   /* 4 = BACKLIGHT_POWER_OFF, 0 = ON */
    /* backlight_tick_cb only ever BLANKS (wake is input-driven), so it has no
     * work while blanked: pause it so it doesn't wake the loop twice a second.
     * Every wake goes through here, so it always resumes; the caller resets
     * the idle clock (lv_display_trigger_activity) so it can't re-blank. */
    if (s_bl_timer) { if (on) lv_timer_pause(s_bl_timer); else lv_timer_resume(s_bl_timer); }
    /* Library-update poll: nothing to show in the dark, so don't poll there;
     * on wake it runs at once and a scan that ended meanwhile toasts now. */
    if (s_libupd_tm) { if (on) lv_timer_pause(s_libupd_tm); else { lv_timer_resume(s_libupd_tm); lv_timer_ready(s_libupd_tm); } }
    if (!on) {
        if (s_batt_timer) lv_timer_ready(s_batt_timer);
        if (s_play_timer) lv_timer_ready(s_play_timer);
        if (s_preview_timer) lv_timer_ready(s_preview_timer);
    }
}

/* POWER tap: toggle the backlight. Tagged non-auto (deliberate -> pocket-safe,
 * only POWER wakes it). Resets the idle clock on wake so the poll doesn't
 * immediately re-blank. POWER is handled entirely in the input layer now
 * (tap = this toggle, long-press = shutdown), so it never reaches a widget. */
void ui_blank_power_tap(void)
{
    blank_set(!s_blanked, 0);
    if (!s_blanked && g_ctx && g_ctx->disp)   /* state after toggle == woke */
        lv_display_trigger_activity(g_ctx->disp);
}

/* ~500ms poll: auto-blank once the display has been idle past the timeout.
 * Wake is handled in the input layer (ui_blank_wake), not here, so the waking
 * press can be consumed -- never reaching a widget -- which a poll-based wake
 * could not do (the wheel/ENTER would act before the poll saw the idle reset).
 * Runs regardless of playback (the screen sleeps during pocket playback). */
static void backlight_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ctx || !g_ctx->disp) return;
    if (!s_blanked &&
        backlight_should_blank(ui_power_bltimeout_get(),
                               lv_display_get_inactive_time(g_ctx->disp)))
        blank_set(1, 1);            /* auto blank */
}

/* Unconditional wake (ignores the wake policy) for notices that must be seen,
 * e.g. the critical-battery warning. */
void ui_blank_force_wake(void)
{
    if (s_blanked) blank_set(0, 0);
    if (g_ctx && g_ctx->disp) lv_display_trigger_activity(g_ctx->disp);
}

/* Is the backlight currently blanked? Queried by the indev read callbacks. */
int ui_blank_active(void) { return s_blanked; }

/* Called from an indev read callback while blanked, on a wake-candidate input
 * (a key press edge or wheel motion). The caller SWALLOWS the input -- it never
 * reaches a widget -- so the wake press only wakes and never fires its action.
 * Here we decide whether it wakes, per the wake policy and how the screen was
 * blanked (s_auto_blanked). On wake, reset the idle clock so the poll above
 * doesn't immediately re-blank. is_power_key=1 only for the POWER key. */
void ui_blank_wake(int is_power_key)
{
    if (!s_blanked) return;
    if (!backlight_should_wake(ui_power_blwake_get(), s_auto_blanked, is_power_key))
        return;
    blank_set(0, 0);
    if (g_ctx && g_ctx->disp) lv_display_trigger_activity(g_ctx->disp);
}

static void item_click_cb(lv_event_t *e)
{
    lv_obj_t *(*builder)(void) = lv_event_get_user_data(e);
    if (builder) ui_goto(builder);
}

/* Group-level keys. Focus nav is the wheel (encoder); buttons do actions:
 * ESC=back, HOME=home, Prev/Next=track skip (toast for now, no audio backend).
 * Attached to every focusable object so it fires regardless of which has focus. */
void group_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    /* While blanked, input is swallowed at the source (ui_input.c, via
     * ui_blank_active/ui_blank_wake) so it never reaches a widget -- this gate
     * shouldn't even be reached. Defensive: ignore keys if somehow blanked. */
    if (s_blanked) return;
    if (k == LV_KEY_ESC)            ui_back();
    else if (k == UI_KEY_HOME)      ui_home();
    /* Transport buttons act on the MPD player from any screen (output-agnostic:
     * same commands whether audio leaves via the DAC or a future bluealsa sink). */
    else if (k == UI_KEY_PREVTRACK) { char r[64]; mpd_cmd("previous", r, sizeof r); }
    else if (k == UI_KEY_NEXTTRACK) { char r[64]; mpd_cmd("next",     r, sizeof r); }
    else if (k == UI_KEY_VOLUP)     ui_volume(+UI_VOL_STEP); /* 2% step (~5/256 DAC); was 5%. finer control */
    else if (k == UI_KEY_VOLDOWN)   ui_volume(-UI_VOL_STEP);
    /* POWER is handled entirely in ui_input.c (tap=blank/wake, hold=shutdown)
     * and never delivered here. */
}

void ui_focus_add(lv_obj_t *item)
{
    lv_group_add_obj(ui_screen_group(lv_obj_get_screen(item)), item);
}

/* For display-only screens (no focusable widgets): make the body the sole key
 * receiver so Back/Menu (ESC/HOME) still work. Prev/Next become no-ops there. */
void ui_screen_keysink(lv_obj_t *scr)
{
    lv_obj_t *body = ui_screen_body(scr);
    lv_group_add_obj(ui_screen_group(scr), body);
    lv_obj_add_event_cb(body, group_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(body);
}

/* Centered empty-state message with an optional dimmer hint line below it,
 * both center-aligned and wrapped to the screen width; the body becomes the
 * key sink so ESC/HOME still work on the otherwise focus-less screen. */
void ui_empty_state(lv_obj_t *scr, const char *msg, const char *hint)
{
    lv_obj_t *box = lv_obj_create(ui_screen_body(scr));
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, ui_screen_body_w(scr) - 20, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_center(box);
    for (int i = 0; i < 2; i++) {
        const char *t = i ? hint : msg;
        if (!t) continue;
        lv_obj_t *l = lv_label_create(box);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (i) lv_obj_set_style_text_color(l, lv_color_hex(0x606060), 0);
        lv_label_set_text(l, t);
    }
    ui_screen_keysink(scr);
}

/* Right-aligned value on a list row (iPod "Bluetooth      On"). Added after the
 * row's flex-grow label, so flex puts it at the right edge. No explicit colour:
 * it inherits black/white from the row like the chevron, and text_opa dims it
 * to grey on white and soft white on the blue selection. */
static lv_obj_t *row_value(lv_obj_t *row, const char *txt)
{
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, txt);
    lv_obj_set_style_text_opa(v, LV_OPA_60, 0);
    return v;
}

/* Set a list row's name, cut with "..." to the width its right-hand value
 * leaves (instead of the default marquee). Own ellipsis: LVGL 9.2 LONG_DOT
 * places the dots break-anywhere but draws word-wrapped, so a name that wraps
 * at a space lost its "..." entirely. Widths come from styles, not a layout
 * pass, so it stays cheap on the 700ms BT refresh. Rows are body-wide (every
 * list here is ui_screen_list): 320, less beside a theme's side panel. */
/* Pixel width of the first n bytes of t (no letter spacing). */
static int32_t text_w(const char *t, uint32_t n, const lv_font_t *f)
{
    lv_text_attributes_t a;
    lv_text_attributes_init(&a);
    return lv_text_get_width(t, n, f, &a);
}

static void row_name_fit(lv_obj_t *row, const char *name, const char *val)
{
    lv_obj_t *l = lv_obj_get_child(row, 0);
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    int32_t avail = ui_screen_body_w(lv_obj_get_screen(row))
                    - lv_obj_get_style_pad_left(row, 0) - lv_obj_get_style_pad_right(row, 0);
    if (val && val[0])
        avail -= text_w(val, strlen(val), f) + lv_obj_get_style_pad_column(row, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    char buf[80];
    size_t n = lv_snprintf(buf, sizeof buf, "%s", name);
    if (n >= sizeof buf) n = sizeof buf - 1;
    if (text_w(buf, n, f) > avail) {
        int32_t dots = text_w("...", 3, f);
        while (n > 0 && text_w(buf, n, f) + dots > avail) {
            n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   /* whole UTF-8 chars */
        }
        while (n > 0 && buf[n - 1] == ' ') n--;   /* "Long..." not "Long ..." */
        memcpy(buf + n, "...", 4);
    }
    lv_label_set_text(l, buf);
}

/* Non-focusable grey hint line ("Press Scan to search", errors). An
 * lv_list_add_text minus the section-header band, so hints don't read as
 * headers. Wraps: an error message is shown whole. */
static lv_obj_t *list_hint(lv_obj_t *list, const char *txt)
{
    lv_obj_t *t = lv_list_add_text(list, txt);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x808080), 0);
    lv_obj_set_style_pad_ver(t, 6, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    return t;
}

/* Centred empty-state block at the bottom of a settings list: faint 40px
 * icon (optional), bold message, grey hint. With an icon it flex-grows to
 * fill and centre in the space under the rows -- callers pass one only where
 * that space fits it (~110px), because non-focusable content past the bottom
 * can't be reached with the wheel. Without one it's compact (content height). */
#define UI_ICON_BT   "\xEF\x8A\x93"   /* U+F293, lv_font_ipod_icon40 */
#define UI_ICON_WIFI "\xEF\x87\xAB"   /* U+F1EB */
#define UI_ICON_MUSIC "\xEF\x80\x81"  /* U+F001 */
static lv_obj_t *empty_block(lv_obj_t *parent, const char *icon, const char *msg, const char *hint)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(box, 20, 0);
    lv_obj_set_style_pad_ver(box, 10, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    const char *t[3] = { icon, msg, hint };
    for (int i = 0; i < 3; i++) {
        if (!t[i]) continue;
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, t[i]);
        if (i == 0) {
            lv_obj_set_style_text_font(l, &lv_font_ipod_icon40, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0xc8c8c8), 0);
            lv_obj_set_style_pad_bottom(l, 4, 0);
        } else {
            lv_obj_set_width(l, LV_PCT(100));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            if (i == 1) lv_obj_set_style_text_font(l, UI_FONT_BOLD, 0);
            else        lv_obj_set_style_text_color(l, lv_color_hex(0x808080), 0);
        }
    }
    return box;
}
static void list_empty(lv_obj_t *list, const char *icon, const char *msg, const char *hint)
{
    lv_obj_t *box = empty_block(list, icon, msg, hint);
    lv_obj_set_width(box, LV_PCT(100));
    if (icon) lv_obj_set_flex_grow(box, 1); else lv_obj_set_height(box, LV_SIZE_CONTENT);
}

/* Word-wrap `txt` into label `l` (fixed width, `lines` lines tall), ending in
 * "..." if it doesn't fit. Own fit, not LONG_DOT: LVGL 9.2 places LONG_DOT's
 * dots break-anywhere but draws word-wrapped, so a cut at a space lost the
 * "..." (see row_name_fit). Linear shrink: only runs when the text changes. */
static void label_fit_lines(lv_obj_t *l, const char *txt, int lines)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    int32_t w = lv_obj_get_style_width(l, 0), maxh = lines * lv_font_get_line_height(f);
    char buf[256];
    size_t n = lv_snprintf(buf, sizeof buf, "%s", txt);
    if (n >= sizeof buf) n = sizeof buf - 1;
    lv_point_t sz;
    lv_text_get_size(&sz, buf, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    if (sz.y > maxh) {
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   /* whole UTF-8 chars */
            size_t k = n;
            while (k > 0 && buf[k - 1] == ' ') k--;
            memcpy(buf + k, "...", 4);
            lv_text_get_size(&sz, buf, f, 0, 0, w, LV_TEXT_FLAG_NONE);
            if (sz.y <= maxh) break;
        }
    }
    lv_label_set_text(l, buf);
}

/* Focusable list item; on click runs `builder` (NULL = no-op row). */
static lv_obj_t *menu_item(lv_obj_t *list, const char *txt, lv_obj_t *(*builder)(void))
{
    lv_obj_t *b = lv_list_add_button(list, NULL, txt);
    ui_focus_add(b);
    ui_apply_focus_style(b);
    if (builder) {   /* iPod drill-in chevron; no explicit color so it inherits the
                      * row's text color (black normally, white when selected). */
        lv_obj_t *chev = lv_label_create(b);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_add_event_cb(b, item_click_cb, LV_EVENT_CLICKED, (void *)builder);
    lv_obj_add_event_cb(b, group_key_cb,  LV_EVENT_KEY,     NULL);
    return b;
}

/* Action row: runs `cb` on click instead of navigating. No chevron (iPod shows
 * none on commands like "Shuffle Songs"). */
static lv_obj_t *menu_cmd(lv_obj_t *list, const char *txt, void (*cb)(lv_event_t *))
{
    lv_obj_t *b = lv_list_add_button(list, NULL, txt);
    ui_focus_add(b);
    ui_apply_focus_style(b);
    lv_obj_add_event_cb(b, cb,           LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY,     NULL);
    return b;
}

/* iPod "Shuffle Songs": shuffle the whole library and start playing. */
static void shuffle_click_cb(lv_event_t *e)
{
    (void)e; char r[256];
    mpd_cmd("clear", r, sizeof r);
    mpd_cmd("add \"\"", r, sizeof r);   /* MPD: empty URI adds the entire database */
    mpd_cmd("random 1", r, sizeof r);
    mpd_cmd("play", r, sizeof r);
    ui_goto(scr_now_playing);
}

void ui_goto(lv_obj_t *(*builder)(void))
{
    lv_obj_t *scr = builder();
    if (!scr) return;                         /* defensive: builder failed */
    if (!ui_nav_push(&s_nav, scr)) {          /* stack full: don't show an untracked screen */
        lv_group_t *g = ui_screen_group(scr);
        lv_obj_delete(scr);
        if (g) lv_group_delete(g);
        return;
    }
    lv_screen_load(scr);
    set_active_group(scr);
}

/* Dispose a screen + its per-screen group. MUST be deferred via lv_async_call:
 * ui_back/ui_home run inside an LV_EVENT_KEY callback on a focused object that
 * lives on the screen being removed. Deleting it synchronously frees the object
 * mid-event-dispatch -> use-after-free crash. Async runs after the event loop. */
static void screen_dispose_cb(void *p)
{
    lv_obj_t *s = p;
    lv_group_t *g = ui_screen_group(s);
    lv_obj_delete(s);              /* removes its children from g first */
    if (g) lv_group_delete(g);
    ui_mem_trim();                 /* hand the screen's now-free pool pages back */
}

void ui_back(void)
{
    if (ui_nav_depth(&s_nav) <= 1) return;
    lv_obj_t *old = ui_nav_top(&s_nav);
    lv_obj_t *scr = ui_nav_pop(&s_nav);
    lv_screen_load(scr);
    set_active_group(scr);
    lv_async_call(screen_dispose_cb, old);
}

void ui_home(void)
{
    while (ui_nav_depth(&s_nav) > 1) {           /* dispose every screen above root */
        lv_obj_t *o = ui_nav_top(&s_nav);
        ui_nav_pop(&s_nav);
        lv_async_call(screen_dispose_cb, o);
    }
    lv_obj_t *root = ui_nav_top(&s_nav);
    lv_screen_load(root);
    set_active_group(root);
}

/* Right-pane now-playing preview on the main menu (iPod Classic split view). */
static void menu_preview_refresh(lv_timer_t *t)
{
    lv_obj_t **pv = lv_timer_get_user_data(t);   /* pv[0]=title pv[1]=artist pv[2]=art */
    if (s_blanked) return;   /* screen off: skip the MPD poll; blank_set() kicks us on wake */
    char r[1024], f[256];
    if (mpd_cmd("currentsong", r, sizeof r) > 0) {
        lbl_set(pv[0], mpd_field(r, "Title", f, sizeof f) ? f : "Not Playing");
        char art[256] = ""; mpd_field(r, "Artist", art, sizeof art);
        lbl_set(pv[1], art);
        ui_art_update(pv[2], mpd_field(r, "file", f, sizeof f) ? f : "");
    }
}

lv_obj_t *scr_menu(void)
{
    lv_obj_t *scr = ui_screen_base_kind("EROS Q", UI_SCR_HOME);
    lv_obj_t *body = ui_screen_body(scr);

    /* Left: the menu list (the only focusable half). Beside a theme's side
     * panel the panel takes the right half's place: list only, no preview. */
    int panel = ui_theme_panel_w() > 0;
    lv_obj_t *list = lv_list_create(body);
    lv_obj_set_size(list, panel ? ui_screen_body_w(scr) : 170, 218);
    lv_obj_align(list, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    menu_item(list, "Music",        scr_music);
    menu_item(list, "Podcasts",     scr_podcasts);
    menu_item(list, "Extras",       scr_extras);
    menu_item(list, "Settings",     scr_settings);
    menu_cmd (list, "Shuffle Songs", shuffle_click_cb);
    menu_item(list, "Now Playing",  scr_now_playing);
    if (panel) return scr;

    /* Right: artwork + now-playing preview pane, divided by a hairline. */
    lv_obj_t *pane = lv_obj_create(body);
    lv_obj_set_size(pane, 150, 218);
    lv_obj_align(pane, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pane, 0, 0);
    ui_style_surface(pane);                       /* also the hairline's colour */
    lv_obj_set_style_border_width(pane, 1, 0);
    lv_obj_set_style_border_side(pane, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(pane, 0, 0);

    static lv_obj_t *pv[3];   /* pv[2] = art tile */
    pv[2] = ui_art_create(pane, 120);
    lv_obj_align(pv[2], LV_ALIGN_TOP_MID, 0, 10);

    pv[0] = lv_label_create(pane);
    lv_obj_set_width(pv[0], 140);
    lv_label_set_long_mode(pv[0], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pv[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(pv[0], "Not Playing");
    /* One line tall: LONG_DOT only truncates a label with a fixed height; with
     * content height a long title wrapped onto the artist line below it. */
    lv_obj_set_height(pv[0], lv_font_get_line_height(lv_obj_get_style_text_font(pv[0], 0)));
    lv_obj_align(pv[0], LV_ALIGN_TOP_MID, 0, 140);
    pv[1] = lv_label_create(pane);
    lv_obj_set_width(pv[1], 140);
    lv_label_set_long_mode(pv[1], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pv[1], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(pv[1], lv_color_hex(0x808080), 0);
    lv_label_set_text(pv[1], "");
    lv_obj_set_height(pv[1], lv_font_get_line_height(lv_obj_get_style_text_font(pv[1], 0)));
    lv_obj_align(pv[1], LV_ALIGN_TOP_MID, 0, 164);

    /* Root menu lives for the whole session, so this 1s poll needs no teardown. */
    lv_timer_t *t = lv_timer_create(menu_preview_refresh, 1000, pv);
    s_preview_timer = t;
    menu_preview_refresh(t);
    return scr;
}

/* Deferred radio boot-restore, run a beat after the menu is on screen. BOTH
 * radios are fully backgrounded off the LVGL thread: BT via bt_pref_restore ->
 * ui_bt_power_on_deferred (scripts launched with `&`, UI attaches via the
 * org.bluez name watch); WiFi via wifi_pref_restore -> wifi_set_enabled, which
 * backgrounds wifi_up.sh -- the UI reads the outcome later through
 * wifi_is_enabled() ground truth (power rfkill + wpa PING). One-shot timers. */
static void wifi_restore_tm_cb(lv_timer_t *t) { (void)t; wifi_pref_restore(); }
static void bt_restore_tm_cb(lv_timer_t *t)   { (void)t; bt_pref_restore(); }

void ui_build(struct ui_ctx *ctx)
{
    g_ctx = ctx;
    ui_theme_init();
    ui_status_init();
    wifi_apply_saved_country();   /* regdomain resets on reboot; restore the user's choice */
    lv_obj_t *root = scr_menu();
    ui_nav_reset(&s_nav, root);
    lv_screen_load(root);
    set_active_group(root);

    lv_timer_t *wr = lv_timer_create(wifi_restore_tm_cb, 400, NULL);
    if (wr) lv_timer_set_repeat_count(wr, 1);    /* restore saved WiFi state once, post-paint */
    lv_timer_t *br = lv_timer_create(bt_restore_tm_cb, 1200, NULL);
    if (br) lv_timer_set_repeat_count(br, 1);    /* then BT (staggered so the menu breathes) */
}

/* Extras: launchable apps from the manifest. Click hands off display+input to
 * the child (ui_launch_run), which re-masters the panel and reads input itself;
 * on child exit the UI is rebuilt and we land back here. */
static void launch_click_cb(lv_event_t *e)
{
    const char *exec = lv_event_get_user_data(e);
    ui_launch_run(g_ctx, exec);
}

lv_obj_t *scr_extras(void)
{
    static ui_app_t apps[16]; static int n = -1;
    if (n < 0) {                                  /* parse manifest once */
        char buf[2048]; n = 0;
        FILE *f = fopen("/usr/share/erosq-ui/extras.tsv", "r");
        if (f) { size_t r = fread(buf, 1, sizeof buf - 1, f); buf[r] = 0; fclose(f);
                 n = ui_manifest_parse(buf, apps, 16); }
    }
    lv_obj_t *scr = ui_screen_base("Extras");
    if (n == 0) {                                 /* nothing installed: keysink so Back/Menu work */
        ui_screen_keysink(scr);
        lv_obj_t *l = lv_label_create(ui_screen_body(scr));
        lv_label_set_text(l, "No extras installed"); lv_obj_center(l);
        return scr;
    }
    lv_obj_t *list = ui_screen_list(scr);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, apps[i].name);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        lv_obj_add_event_cb(b, launch_click_cb, LV_EVENT_CLICKED, apps[i].exec);
        lv_obj_add_event_cb(b, group_key_cb,    LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* ---- Now Playing: live view of the MPD player ------------------------------
 * Polls `currentsong` + `status` once a second. Widgets live in a file-static
 * array (only one Now Playing screen is ever alive); the poll timer is stashed
 * on the title label and torn down when the screen is disposed (same per-screen-
 * instance idiom as wifi_status_refresh). All transport is output-agnostic MPD
 * commands, so it works unchanged once a Bluetooth sink is added. */
/* iPod Now Playing widgets: w[0]=title w[1]=artist w[2]=scrubber w[3]=elapsed
 * w[4]=album w[5]=counter("n of m" + play/pause) w[6]=remaining w[7]=art.
 *
 * Wheel scrub: turning the wheel (or gamepad d-pad left/right) previews a new
 * position on the scrubber + time labels, elapsed in accent blue = pending.
 * Playback carries on underneath. Play confirms (seekcur); Back, 5s idle, or a
 * track change cancels. s_np holds the last polled position the scrub starts
 * from. ponytail: file-static, like w[] -- one Now Playing alive at a time. */
static struct {
    lv_obj_t *scr, **w; lv_timer_t *idle;
    int el, du, song;          /* last polled position */
    int pending, target;       /* scrub preview */
    int empty;                 /* showing the "Nothing playing" state (-1 = unset) */
    lv_obj_t *empty_box;       /* centred "Nothing playing" block */
    char title[256];           /* raw title last fitted into w[0] (fit is not free) */
} s_np;

static void np_show_pos(lv_obj_t **w, int el, int du)
{
    char buf[16];
    lv_bar_set_range(w[2], 0, du > 0 ? du : 1);
    lv_bar_set_value(w[2], el, LV_ANIM_OFF);
    ui_np_fmt_time(buf, sizeof buf, el);
    lbl_set(w[3], buf);
    ui_np_fmt_remaining(buf, sizeof buf, el, du);   /* "" when duration unknown */
    lbl_set(w[6], buf);
}

static void np_seek_end(void)   /* leave scrub mode; show the real position */
{
    if (!s_np.pending) return;
    s_np.pending = 0;
    lv_timer_pause(s_np.idle);
    lv_obj_remove_local_style_prop(s_np.w[3], LV_STYLE_TEXT_COLOR, 0);
    np_show_pos(s_np.w, s_np.el, s_np.du);
}

/* Empty state: no current song (or MPD unreachable). Every track widget is
 * hidden and the centred icon block (same look as the BT/WiFi empty pages)
 * shows instead -- a countdown with nothing to count is noise. Called only on
 * a transition (s_np.empty), so a steady state costs no restyle/relayout. */
static void np_set_empty(lv_obj_t **w, int empty)
{
    if (s_np.empty == empty) return;
    s_np.empty = empty;
    for (int i = 0; i < 8; i++) lv_obj_update_flag(w[i], LV_OBJ_FLAG_HIDDEN, empty);
    lv_obj_update_flag(s_np.empty_box, LV_OBJ_FLAG_HIDDEN, !empty);
    if (empty) {
        np_seek_end();
        s_np.title[0] = 0;              /* refit on the next track */
        ui_art_update(w[7], "");
    }
}

static int np_scrub(int detents)
{
    if (!s_np.scr || lv_screen_active() != s_np.scr || s_np.du <= 0) return 0;
    s_np.target = ui_seek_target(s_np.pending ? s_np.target : s_np.el, detents, s_np.du);
    if (!s_np.pending) {
        s_np.pending = 1;
        lv_obj_set_style_text_color(s_np.w[3], lv_color_hex(0x2c75e8), 0);
    }
    np_show_pos(s_np.w, s_np.target, s_np.du);
    lv_timer_reset(s_np.idle);
    lv_timer_resume(s_np.idle);
    /* consumed detents never reach LVGL, which only stamps activity for a
     * nonzero enc_diff -- stamp it here or the backlight blanks mid-scrub */
    lv_display_trigger_activity(g_ctx->disp);
    return 1;
}
static void np_seek_idle(lv_timer_t *t) { (void)t; np_seek_end(); }

static void np_refresh(lv_timer_t *t)
{
    if (s_blanked) return;   /* screen off: skip poll, scrubber redraw, and
                              * error-popup detection (surfaces on wake). */
    lv_obj_t **w = lv_timer_get_user_data(t);
    /* 4 KiB: status carries MPD's error, which can name a ~250-char path twice;
     * a cut-short reply returns -2 and would freeze the position + skip the popup */
    char r[4096], f[256];
    char file[256];
    int got = mpd_cmd("currentsong", r, sizeof r) > 0;
    if (got && mpd_field(r, "file", file, sizeof file)) {
        np_set_empty(w, 0);
        const char *title = mpd_field(r, "Title", f, sizeof f) ? f : NULL;
        const char *tt = ui_np_title(title, file);   /* untagged: file name */
        if (strcmp(tt, s_np.title)) {               /* 2-line fit only on change */
            lv_snprintf(s_np.title, sizeof s_np.title, "%s", tt);
            label_fit_lines(w[0], s_np.title, 2);
        }
        lbl_set(w[1], mpd_field(r, "Artist", f, sizeof f) ? f : "");
        lbl_set(w[4], mpd_field(r, "Album",  f, sizeof f) ? f : "");
        ui_art_update(w[7], file);
    } else if (got || s_np.empty < 0) {
        /* no current song, or MPD unreachable on first paint. A transient
         * poll failure mid-song keeps the last track rather than flashing. */
        np_set_empty(w, 1);
    }
    if (mpd_cmd("status", r, sizeof r) > 0) {
        int el = mpd_field(r, "elapsed",  f, sizeof f) ? (int)atof(f) : 0;
        int du = mpd_field(r, "duration", f, sizeof f) ? (int)atof(f) : 0;
        /* capture state as a bool BEFORE the next mpd_field overwrites f */
        int playing = mpd_field(r, "state", f, sizeof f) && strcmp(f, "play") == 0;
        int song = mpd_field(r, "song",           f, sizeof f) ? atoi(f) + 1 : 0;
        int plen = mpd_field(r, "playlistlength", f, sizeof f) ? atoi(f) : 0;
        /* track changed under a pending scrub: its target belongs to the old
         * track, so drop it rather than seek the new one there */
        if (song != s_np.song) np_seek_end();
        s_np.el = el; s_np.du = du; s_np.song = song;
        if (!s_np.pending) np_show_pos(w, el, du);   /* pending: keep the preview */
        char buf[64];
        /* no play/pause glyph: the title bar already shows it */
        (void)playing;
        lv_snprintf(buf, sizeof buf, "%d of %d", song, plen);
        lbl_set(w[5], buf);

        /* Sticky playback error: MPD keeps `error:` in status until clearerror
         * or a successful play. Pop a hint once per distinct error episode
         * (debounced by string equality), not once per second.
         * ponytail: scoped to Now Playing's existing 1s poll -- zero new timers
         * or sockets. If errors should surface from any screen, move this check
         * onto the always-alive batt_update timer. */
        static char last_err[256];
        char err[256];
        if (mpd_field(r, "error", err, sizeof err)) {
            if (strcmp(err, last_err) != 0) {          /* new/changed error */
                char msg[64], hint[192];
                int sd = access("/media/sd/audio", F_OK) == 0;
                if (mpd_classify_error(err, sd, msg, sizeof msg, hint, sizeof hint))
                    ui_error_popup(msg, hint);
                lv_snprintf(last_err, sizeof last_err, "%s", err);
            }
        } else {
            last_err[0] = 0;   /* error cleared -> re-arm for the next one */
        }
    }
}
static void np_del_cb(lv_event_t *e)
{
    lv_timer_t *t = lv_obj_get_user_data(lv_event_get_target(e));
    if (t) lv_timer_delete(t);
    if (s_np.scr != lv_obj_get_screen(lv_event_get_target(e))) return;  /* not ours */
    ui_wheel_set_hook(NULL);
    if (s_np.idle) lv_timer_delete(s_np.idle);
    memset(&s_np, 0, sizeof s_np);
}
/* Play/Pause (hardware Play button -> LV_KEY_ENTER). Lives only on Now Playing's
 * keysink body, NOT the shared group_key_cb -- there ENTER means "click the
 * focused menu item", so a global pause toggle would fire on every selection.
 * Registered BEFORE group_key_cb so a pending scrub can claim ESC (cancel)
 * without it also navigating back. */
static void np_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    char r[64];
    if (k == LV_KEY_LEFT || k == LV_KEY_RIGHT) { np_scrub(k == LV_KEY_RIGHT ? 1 : -1); return; }
    if (!s_np.pending) {
        if (k == LV_KEY_ENTER) ui_media_cmd("pause");
        return;
    }
    if (k == LV_KEY_ENTER) {           /* confirm */
        char cmd[32];
        lv_snprintf(cmd, sizeof cmd, "seekcur %d", s_np.target);
        mpd_cmd(cmd, r, sizeof r);
        s_np.el = s_np.target;         /* show it now; the next poll confirms */
        np_seek_end();
    } else if (k == LV_KEY_ESC) {      /* cancel, stay on Now Playing */
        np_seek_end();
    } else return;
    lv_event_stop_processing(e);
}
lv_obj_t *scr_now_playing(void)
{
    lv_obj_t *scr = ui_screen_base_kind("Now Playing", UI_SCR_FULL);
    lv_obj_t *body = ui_screen_body(scr);
    static lv_obj_t *w[8];   /* file-static: only one Now Playing alive at a time */

    /* Layout (body 320x218): counter top-left, 120px art + text column,
     * scrubber + times below -- margins balanced top/bottom. */
    /* "n of m", iPod-style top-left */
    w[5] = lv_label_create(body);
    lv_obj_set_style_text_color(w[5], lv_color_hex(0x808080), 0);
    lv_label_set_text(w[5], "");
    lv_obj_align(w[5], LV_ALIGN_TOP_LEFT, 12, 8);

    /* album-art tile, left */
    w[7] = ui_art_create(body, 120);
    lv_obj_align(w[7], LV_ALIGN_TOP_LEFT, 12, 32);

    /* title (1-2 lines) / artist / album, right of the art. A flex column so
     * the artist sits right under a one-line title instead of below an empty
     * second line. */
    int lh = lv_font_get_line_height(UI_FONT);
    lv_obj_t *col = lv_obj_create(body);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, 164, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_align(col, LV_ALIGN_TOP_LEFT, 144, 34);
    w[0] = lv_label_create(col);
    lv_obj_set_style_text_font(w[0], UI_FONT_LARGE, 0);
    lv_label_set_long_mode(w[0], LV_LABEL_LONG_WRAP);   /* label_fit_lines caps it at 2 + "..." */
    lv_obj_set_width(w[0], 164);
    lv_obj_set_style_pad_bottom(w[0], 2, 0);
    lv_label_set_text(w[0], "");
    w[1] = lv_label_create(col);
    lv_label_set_long_mode(w[1], LV_LABEL_LONG_DOT);
    lv_obj_set_size(w[1], 164, lh);   /* one line: LONG_DOT needs a fixed height */
    lv_label_set_text(w[1], "");
    w[4] = lv_label_create(col);
    lv_obj_set_style_text_color(w[4], lv_color_hex(0x808080), 0);
    lv_label_set_long_mode(w[4], LV_LABEL_LONG_DOT);
    lv_obj_set_size(w[4], 164, lh);
    lv_label_set_text(w[4], "");

    /* scrubber + elapsed/remaining */
    w[2] = lv_bar_create(body);
    lv_obj_set_size(w[2], 296, 6);
    lv_obj_align(w[2], LV_ALIGN_TOP_MID, 0, 170);
    ui_style_scrubber(w[2]);
    w[3] = lv_label_create(body);
    lv_label_set_text(w[3], "0:00");
    lv_obj_align(w[3], LV_ALIGN_TOP_LEFT, 12, 182);
    w[6] = lv_label_create(body);
    lv_label_set_text(w[6], "");
    lv_obj_align(w[6], LV_ALIGN_TOP_RIGHT, -12, 182);

    s_np.empty_box = empty_block(body, UI_ICON_MUSIC, "Nothing playing", "Choose music from the menu.");
    lv_obj_set_size(s_np.empty_box, 320, 218);
    lv_obj_add_flag(s_np.empty_box, LV_OBJ_FLAG_HIDDEN);

    s_np.scr = scr; s_np.w = w; s_np.song = -1; s_np.empty = -1;
    s_np.idle = lv_timer_create(np_seek_idle, 5000, NULL);
    lv_timer_pause(s_np.idle);
    ui_wheel_set_hook(np_scrub);

    lv_timer_t *t = lv_timer_create(np_refresh, 1000, w);
    lv_obj_set_user_data(w[0], t);
    lv_obj_add_event_cb(w[0], np_del_cb, LV_EVENT_DELETE, NULL);
    np_refresh(t);                 /* immediate first paint */

    /* np_key_cb FIRST: ENTER=play/pause or confirm scrub, ESC=cancel scrub */
    lv_obj_add_event_cb(body, np_key_cb, LV_EVENT_KEY, NULL);
    ui_screen_keysink(scr);        /* body catches ESC(back)/HOME(menu)/transport */
    return scr;
}

/* ---- Library: browse MPD's database (Folders + Artists/Albums/Tracks) -------
 * "Both" model from the spec. Each list screen heap-allocates its own listing
 * (freed on the screen's deferred DELETE), so a parent's row callbacks stay
 * valid after Back -- the old shared file-static arrays made a stale parent
 * fire with the child's data (album name passed as artist, wrong folder rows).
 * Browsing state (current dir / artist / album) stays file-static -- it is
 * only read at build time, and one browser is alive at a time. */
static lv_obj_t *scr_lib_folders(void);
static lv_obj_t *scr_lib_artists(void);
static lv_obj_t *scr_lib_albums(void);
static lv_obj_t *scr_lib_tracks(void);

/* Queue+play a track in the context of its album: enqueue the containing
 * folder (one album per folder in this Artist/Album/track.flac library) and
 * start at the chosen track, so playback rolls on through the album instead of
 * stopping after one song. random is cleared so the album plays in order.
 * Falls back to the bare track when the URI has no folder, and to the album's
 * first track if the clicked track can't be located in the queue.
 * ponytail: assumes one-album-per-folder; a folder with sub-folders would
 * enqueue those too -- fine for this library's layout. */
static void lib_play_uri(const char *uri)
{
    char q[400], cmd[440], r[1024];
    const char *slash = strrchr(uri, '/');
    mpd_cmd("clear", r, sizeof r);
    if (!slash) {                       /* no folder: play the bare track */
        mpd_quote(uri, q, sizeof q);
        lv_snprintf(cmd, sizeof cmd, "add %s", q);
        mpd_cmd(cmd, r, sizeof r);
        mpd_cmd("play", r, sizeof r);
        return;
    }
    char dir[256];
    int dl = (int)(slash - uri); if (dl > (int)sizeof dir - 1) dl = (int)sizeof dir - 1;
    memcpy(dir, uri, dl); dir[dl] = 0;
    mpd_cmd("random 0", r, sizeof r);
    mpd_quote(dir, q, sizeof q);
    lv_snprintf(cmd, sizeof cmd, "add %s", q);   mpd_cmd(cmd, r, sizeof r);
    mpd_quote(uri, q, sizeof q);
    lv_snprintf(cmd, sizeof cmd, "playlistfind file %s", q);
    char pos[16] = "0";
    if (mpd_cmd(cmd, r, sizeof r) > 0) mpd_field(r, "Pos", pos, sizeof pos);
    lv_snprintf(cmd, sizeof cmd, "play %s", pos);
    mpd_cmd(cmd, r, sizeof r);
}

static char s_libdir[256] = "";   /* current MPD directory (empty = root) */
/* Library-empty hint; the path is mpd.conf music_directory (/media/sd/audio). */
#define EMPTY_HINT "Copy music to the SD card's audio folder"
/* Browse data on the libc heap, freed with its screen. Reply cap: a 500-entry
 * folder's lsinfo is ~150 KB (full song metadata), 2,000 albums ~80 KB. */
#define BROWSE_REPLY_CAP (2 * 1024 * 1024)
static void free_tab_cb(lv_event_t *e)
{
    strtab_t *t = lv_event_get_user_data(e);
    strtab_free(t); free(t);
}
static strtab_t *screen_tab(lv_obj_t *scr)
{
    strtab_t *t = calloc(1, sizeof *t);
    if (t) lv_obj_add_event_cb(scr, free_tab_cb, LV_EVENT_DELETE, t);
    return t;
}
/* Run cmd and parse its reply into t: tag values, or (tag NULL) listing entries
 * as "d<path>"/"f<path>". Entries added, -1 on any failure -- which also sets
 * s_browse_err, so the screen says the library couldn't be read instead of
 * claiming it is empty (the old fixed buffers showed "No albums" for a reply
 * that merely didn't fit). */
static int s_browse_err;
static int tab_query(strtab_t *t, const char *cmd, const char *tag)
{
    char *r;
    if (!t || mpd_cmd_dyn(cmd, &r, BROWSE_REPLY_CAP) <= 0) { s_browse_err = 1; return -1; }
    int n = tag ? mpd_taglist_tab(r, tag, t) : mpd_listing_tab(r, t);
    free(r);
    if (n < 0) s_browse_err = 1;
    return n;
}
/* Empty-list message, or the read error if the last queries failed. */
static void browse_empty(lv_obj_t *scr, const char *msg, const char *hint)
{
    if (s_browse_err) ui_empty_state(scr, "Can't read the library", "Music player not responding");
    else              ui_empty_state(scr, msg, hint);
}
static void base_text(void *ctx, int i, char *buf, int n)   /* "d/f<path>" -> its file name */
{
    const char *p = strtab_get(ctx, i), *base = strrchr(p + 1, '/');
    lv_snprintf(buf, n, "%s%s", p[0] == 'd' ? LV_SYMBOL_DIRECTORY " " : "", base ? base + 1 : p + 1);
}
static void tab_text(void *ctx, int i, char *buf, int n) { lv_snprintf(buf, n, "%s", strtab_get(ctx, i)); }

static void folder_click(void *ctx, int i)
{
    const char *p = strtab_get(ctx, i);
    if (p[0] == 'd') { lv_snprintf(s_libdir, sizeof s_libdir, "%s", p + 1); ui_goto(scr_lib_folders); }
    else             { lib_play_uri(p + 1); ui_goto(scr_now_playing); }
}
static lv_obj_t *scr_lib_folders(void)
{
    lv_obj_t *scr = ui_screen_base_kind(s_libdir[0] ? s_libdir : "Folders", UI_SCR_MUSIC);
    char cmd[300], q[256];
    if (s_libdir[0]) { mpd_quote(s_libdir, q, sizeof q); lv_snprintf(cmd, sizeof cmd, "lsinfo %s", q); }
    else lv_snprintf(cmd, sizeof cmd, "lsinfo");
    strtab_t *t = screen_tab(scr);
    s_browse_err = 0;
    if (tab_query(t, cmd, NULL) <= 0) {
        if (s_libdir[0]) browse_empty(scr, "This folder is empty", NULL);
        else             browse_empty(scr, "No music found", EMPTY_HINT);
        return scr;
    }
    ui_vlist_create(scr, t->n, base_text, folder_click, t);
    return scr;
}

static char s_artist[128], s_album[128];
static void track_click(void *ctx, int i) { lib_play_uri(strtab_get(ctx, i) + 1); ui_goto(scr_now_playing); }
static void album_click(void *ctx, int i) { lv_snprintf(s_album, sizeof s_album, "%s", strtab_get(ctx, i)); ui_goto(scr_lib_tracks); }
static void artist_click(void *ctx, int i) { lv_snprintf(s_artist, sizeof s_artist, "%s", strtab_get(ctx, i)); ui_goto(scr_lib_albums); }

/* Build a list screen from the values of one tag (Artist/Album), optionally
 * merging over several commands and case-folding duplicates ("TOOL"/"Tool"). */
static lv_obj_t *list_from_taglist_multi(const char *title, const char *const *cmds, int ncmds,
                                         const char *tag, ui_vlist_click_fn cb, int fold,
                                         const char *empty, const char *hint)
{
    lv_obj_t *scr = ui_screen_base_kind(title, UI_SCR_MUSIC);
    strtab_t *t = screen_tab(scr);
    s_browse_err = !t;
    for (int i = 0; i < ncmds; i++) tab_query(t, cmds[i], tag);
    if (t && fold && strtab_fold_case(t) < 0) { t->n = 0; s_browse_err = 1; }
    if (!t || t->n == 0) {
        browse_empty(scr, empty, hint);
        return scr;
    }
    ui_vlist_create(scr, t->n, tab_text, cb, t);
    return scr;
}
static lv_obj_t *list_from_taglist(const char *title, const char *cmd, const char *tag,
                                   ui_vlist_click_fn cb, const char *empty, const char *hint)
{ return list_from_taglist_multi(title, &cmd, 1, tag, cb, 0, empty, hint); }

/* All tag spellings that case-fold to canon ("Tool" -> "TOOL","Tool"). The
 * artist list folds case-duplicates for display, so album/track queries must
 * fan out over the real spellings again. Re-derived from a cheap `list artist`
 * instead of plumbing variant state between screens. */
#define MAX_VARIANTS 4   /* ponytail: >4 case-spellings of one artist won't happen */
static int artist_variants(const char *canon, char (*out)[128])
{
    int n = 0;
    strtab_t all = { 0 };
    if (tab_query(&all, "list artist", "Artist") > 0)
        for (int i = 0; i < all.n && n < MAX_VARIANTS; i++)
            if (strcasecmp(strtab_get(&all, i), canon) == 0) lv_snprintf(out[n++], 128, "%s", strtab_get(&all, i));
    strtab_free(&all);
    if (!n) { lv_snprintf(out[0], 128, "%s", canon); n = 1; }   /* query failed: exact match only */
    return n;
}
static lv_obj_t *scr_lib_artists(void)
{
    const char *cmd = "list artist";
    return list_from_taglist_multi("Artists", &cmd, 1, "Artist", artist_click, 1,
                                   "No artists", EMPTY_HINT);
}
static lv_obj_t *scr_lib_albums(void)
{
    char vars[MAX_VARIANTS][128], cmds[MAX_VARIANTS][200];
    const char *cp[MAX_VARIANTS];
    int nvar = artist_variants(s_artist, vars);
    for (int i = 0; i < nvar; i++) {
        char q[140]; mpd_quote(vars[i], q, sizeof q);
        lv_snprintf(cmds[i], sizeof cmds[i], "list album artist %s", q);
        cp[i] = cmds[i];
    }
    return list_from_taglist_multi(s_artist, cp, nvar, "Album", album_click, 1,
                                   "No albums", NULL);
}
static lv_obj_t *scr_lib_tracks(void)
{
    char vars[MAX_VARIANTS][128], qa[140], qb[140], cmd[320];
    mpd_quote(s_album, qb, sizeof qb);
    int nvar = s_artist[0] ? artist_variants(s_artist, vars) : 0;
    lv_obj_t *scr = ui_screen_base_kind(s_album, UI_SCR_MUSIC);
    strtab_t *t = screen_tab(scr);
    s_browse_err = !t;   /* artist_variants' own failure already fell back to an exact match */
    if (!nvar) {   /* all-albums path */
        lv_snprintf(cmd, sizeof cmd, "find album %s", qb);
        tab_query(t, cmd, NULL);
    } else for (int i = 0; i < nvar; i++) {
        mpd_quote(vars[i], qa, sizeof qa);
        lv_snprintf(cmd, sizeof cmd, "find artist %s album %s", qa, qb);
        tab_query(t, cmd, NULL);
    }
    if (!t || t->n == 0) {   /* find returns file: lines only */
        browse_empty(scr, "No songs", NULL);
        return scr;
    }
    ui_vlist_create(scr, t->n, base_text, track_click, t);   /* ponytail: filename, upgrade to Title tag if ugly */
    return scr;
}

/* ---- iPod "Music" submenu: Playlists / Artists / Albums / Songs / Genres ---- */

/* Albums (all, no artist filter): reuse the tracks screen with s_artist cleared. */
static void album_all_click(void *ctx, int i)
{
    s_artist[0] = 0;
    lv_snprintf(s_album, sizeof s_album, "%s", strtab_get(ctx, i));
    ui_goto(scr_lib_tracks);
}
/* Genre / Playlist: queue the whole thing and play.
 * random is set EXPLICITLY here, either way, mirroring how the album path always
 * clears it (lib_play_uri). Previously these two set it neither way, so the flag
 * was merely inherited: play one album (random 0) and every playlist afterwards
 * ran in file order, the same order every time, with no way back except
 * "Shuffle Songs". Setting it on every bucket play is what makes the Settings
 * toggle actually authoritative. */
static void genre_click(void *ctx, int i)
{
    char q[140], cmd[200], r[256]; mpd_quote(strtab_get(ctx, i), q, sizeof q);
    mpd_cmd("clear", r, sizeof r);
    lv_snprintf(cmd, sizeof cmd, "findadd genre %s", q); mpd_cmd(cmd, r, sizeof r);
    mpd_cmd(mpd_shuffle_get() ? "random 1" : "random 0", r, sizeof r);
    mpd_cmd("play", r, sizeof r);
    ui_goto(scr_now_playing);
}
static void playlist_click(void *ctx, int i)
{
    char q[140], cmd[200], r[256]; mpd_quote(strtab_get(ctx, i), q, sizeof q);
    mpd_cmd("clear", r, sizeof r);
    lv_snprintf(cmd, sizeof cmd, "load %s", q); mpd_cmd(cmd, r, sizeof r);
    mpd_cmd(mpd_shuffle_get() ? "random 1" : "random 0", r, sizeof r);
    mpd_cmd("play", r, sizeof r);
    ui_goto(scr_now_playing);
}
static lv_obj_t *scr_music_albums(void)   { return list_from_taglist("Albums",    "list album",   "Album",    album_all_click,
                                                                     "No albums", EMPTY_HINT); }
static lv_obj_t *scr_music_genres(void)   { return list_from_taglist("Genres",    "list genre",   "Genre",    genre_click,
                                                                     "No genres", "Songs need a Genre tag"); }
static lv_obj_t *scr_music_playlists(void){ return list_from_taglist("Playlists", "listplaylists","playlist", playlist_click,
                                                                     "No playlists", NULL); }

/* Songs: flat list of every track. 20,000 songs of full `search` metadata is
 * ~6 MB, so the list fetches pages of SONGS_PAGE through `search ... window`
 * as the rows come into view and keeps the last two (~40 KB each); the
 * virtual list keeps the LVGL rows fixed. Order: MPD's database order, as the
 * search returns it on every call. */
#define SONGS_PAGE 500
typedef struct { int total, page[2], lru; strtab_t tab[2]; } songs_t;

static const char *song_uri(songs_t *s, int i)   /* "f<path>", or NULL if MPD fails */
{
    int pg = i / SONGS_PAGE, k = s->page[0] == pg ? 0 : s->page[1] == pg ? 1 : -1;
    if (k < 0) {
        k = s->lru; s->lru ^= 1;
        strtab_free(&s->tab[k]);
        s->page[k] = -1;
        char cmd[64];
        lv_snprintf(cmd, sizeof cmd, "search any \"\" window %d:%d", pg * SONGS_PAGE, (pg + 1) * SONGS_PAGE);
        if (tab_query(&s->tab[k], cmd, NULL) < 0) return NULL;
        s->page[k] = pg;
    } else s->lru = k ^ 1;
    int o = i - pg * SONGS_PAGE;
    return o < s->tab[k].n ? strtab_get(&s->tab[k], o) : NULL;
}
static void song_text(void *ctx, int i, char *buf, int n)
{
    const char *p = song_uri(ctx, i), *base = p ? strrchr(p, '/') : NULL;
    lv_snprintf(buf, n, "%s", p ? (base ? base + 1 : p + 1) : "...");
}
static void song_click(void *ctx, int i)
{
    const char *p = song_uri(ctx, i);
    if (p) { lib_play_uri(p + 1); ui_goto(scr_now_playing); }
}
static void free_songs_cb(lv_event_t *e)
{
    songs_t *s = lv_event_get_user_data(e);
    strtab_free(&s->tab[0]); strtab_free(&s->tab[1]); free(s);
}
static lv_obj_t *scr_music_songs(void)
{
    lv_obj_t *scr = ui_screen_base_kind("Songs", UI_SCR_MUSIC);
    songs_t *s = calloc(1, sizeof *s);
    char st[512], v[16];
    s_browse_err = !s;
    if (s) {
        s->page[0] = s->page[1] = -1;
        lv_obj_add_event_cb(scr, free_songs_cb, LV_EVENT_DELETE, s);
        if (mpd_cmd("stats", st, sizeof st) > 0 && mpd_field(st, "songs", v, sizeof v)) s->total = atoi(v);
        else s_browse_err = 1;
    }
    if (!s || s->total <= 0 || !song_uri(s, 0)) {
        browse_empty(scr, "No songs", EMPTY_HINT);
        return scr;
    }
    ui_vlist_create(scr, s->total, song_text, song_click, s);
    return scr;
}

/* Folders entry point for the Music menu. Resets s_libdir first so Folders
 * always opens at the MPD root -- folder_click_cb walks s_libdir downwards and
 * it persists between visits, so entering scr_lib_folders() directly would
 * resume wherever the user last was. This is what the old (orphaned)
 * scr_library() wrapper did. */
static lv_obj_t *scr_music_folders(void)
{
    s_libdir[0] = 0;
    return scr_lib_folders();
}

lv_obj_t *scr_music(void)
{
    lv_obj_t *scr = ui_screen_base_kind("Music", UI_SCR_MUSIC);
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Playlists", scr_music_playlists);
    menu_item(list, "Artists",   scr_lib_artists);
    menu_item(list, "Albums",    scr_music_albums);
    menu_item(list, "Songs",     scr_music_songs);
    menu_item(list, "Genres",    scr_music_genres);
    menu_item(list, "Folders",   scr_music_folders);
    return scr;
}

/* One iPod-style About row: label left, value right-aligned (LONG_DOT so a long
 * describe string ellipsises instead of overlapping the label), hairline under. */
static int s_about_w = 320;   /* body width of the About screen being built */
static void about_row(lv_obj_t *parent, const char *key, const char *val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, s_about_w, 27);
    lv_obj_set_style_border_color(row, lv_color_hex(0xd0d0d0), 0);
    lv_obj_set_style_border_width(row, ui_theme_row_dividers(), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v, s_about_w - 110, lv_font_get_line_height(lv_obj_get_style_text_font(v, 0)));   /* one line: LONG_DOT */
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0x606060), 0);
    lv_label_set_text(v, val);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
}

/* Static snapshot taken once on entry (no timer): every read is a local file,
 * a statvfs, or one MPD `stats` round-trip on the local socket -- the same
 * synchronous pattern the menu's `status` poll already uses on this thread. */
lv_obj_t *scr_about(void)
{
    lv_obj_t *scr = ui_screen_base("About");
    s_about_w = ui_screen_body_w(scr);
    lv_obj_t *col = lv_obj_create(ui_screen_body(scr));
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, s_about_w, 218);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* Versions from /etc/sw-versions (written by erosq-br post-build.sh). The
     * composite `firmware` line is too long for the row, so show its parts. */
    char sw[512] = "", rootfs[64], uiver[64];
    int fd = open("/etc/sw-versions", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, sw, sizeof sw - 1);
        close(fd);
        sw[n > 0 ? n : 0] = 0;
    }
    if (!ul_sw_version(sw, "rootfs", rootfs, sizeof rootfs)) snprintf(rootfs, sizeof rootfs, AL_NA);
    if (!ul_sw_version(sw, "ui", uiver, sizeof uiver))       snprintf(uiver, sizeof uiver, "dev " __DATE__);

    /* Kernel: sw-versions' `kernel` line carries the build's git hash
     * (6.18.53-g5ced6eefad) -- uname alone says only "6.18.53" (LOCALVERSION is
     * empty), which can't tell two kernel builds apart (HITL 10-20). Fall back
     * to the live uname: a hardcoded literal once went stale across bumps. */
    struct utsname u;
    if (!ul_sw_version(sw, "kernel", u.release, sizeof u.release) && uname(&u) < 0)
        snprintf(u.release, sizeof u.release, AL_NA);

    int mv = 0, p = ui_batt_read(&mv);
    char batt[32]; al_batt(batt, sizeof batt, p, mv);

    char songs[24] = AL_NA, r[512];
    if (mpd_cmd("stats", r, sizeof r) > 0) mpd_field(r, "songs", songs, sizeof songs);

    /* Only report the card when it is actually mounted: an empty /media/sd is
     * a directory on the root fs, and statvfs would report THAT instead. */
    char fr[24] = AL_NA;
    struct stat sd, media; struct statvfs vf;
    if (stat("/media/sd", &sd) == 0 && stat("/media", &media) == 0 &&
        sd.st_dev != media.st_dev && statvfs("/media/sd", &vf) == 0)
        al_free(fr, sizeof fr, (unsigned long long)vf.f_bavail * vf.f_frsize);

    about_row(col, "Model",      "EROS Q");
    about_row(col, "Software",   rootfs);
    about_row(col, "UI",         uiver);
    about_row(col, "Kernel",     u.release);
    about_row(col, "CPU",        "Ingenic X1000");
    about_row(col, "Battery",    batt);
    about_row(col, "Songs",      songs);
    about_row(col, "Free space", fr);
    ui_screen_keysink(scr);   /* display-only: body catches ESC(back)/HOME(menu) */
    return scr;
}

/* Real brightness: a slider over log-spaced levels (perceptually even, and only
 * ~13 wheel detents end-to-end vs 245 linear). The slider index maps through a
 * table to the actual 10-255 PWM brightness. Group in editing mode -> wheel
 * adjusts the slider directly. */
/* Floor at 39: the panel gets uncomfortably dim below this, so the slider
 * doesn't go lower (per request). Log-spaced 39..255. */
static const int bl_levels[] = { 39, 48, 59, 73, 90, 111, 137, 168, 207, 255 };
#define BL_NLEVELS ((int)(sizeof bl_levels / sizeof bl_levels[0]))

static int bl_nearest_level(int b)
{
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < BL_NLEVELS; i++) {
        int d = bl_levels[i] - b; if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}
static void brightness_pct_set(lv_obj_t *pct, int i)
{
    lv_label_set_text_fmt(pct, "%d%%", al_bl_pct(bl_levels[i]));
}
/* VALUE_CHANGED only: the readout redraws when the level moves, never on a timer. */
static void brightness_cb(lv_event_t *e)
{
    int i = lv_slider_get_value(lv_event_get_target(e));
    if (i < 0) i = 0;
    if (i >= BL_NLEVELS) i = BL_NLEVELS - 1;
    bl_write("brightness", bl_levels[i]);
    brightness_pct_set(lv_event_get_user_data(e), i);
}
/* Leaving the screen saves the level once (not per wheel detent); boot puts it
 * back -- the backlight driver starts at its own default every power-on. */
void ui_brightness_persist(void)
{
    int b = bl_read("brightness");            /* level survives blanking (bl_power) */
    if (b > 0) ui_power_save_brightness(b);   /* no write if unchanged */
}
static void brightness_del_cb(lv_event_t *e) { (void)e; ui_brightness_persist(); }
void ui_brightness_restore(void)
{
    int b = ui_power_saved_brightness();
    if (b > 0) bl_write("brightness", b);
}
lv_obj_t *scr_brightness(void)
{
    lv_obj_t *scr = ui_screen_base("Brightness");
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *hint = lv_label_create(body);
    lv_label_set_text(hint, "Turn the wheel to adjust");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t *sl = lv_slider_create(body);
    lv_obj_set_width(sl, 240);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 8);
    lv_slider_set_range(sl, 0, BL_NLEVELS - 1);
    int cur = bl_read("brightness");
    int lvl = bl_nearest_level(cur < 0 ? 200 : cur);
    lv_slider_set_value(sl, lvl, LV_ANIM_OFF);
    lv_obj_t *pct = lv_label_create(body);
    lv_obj_set_style_text_font(pct, UI_FONT_LARGE, 0);
    lv_obj_align_to(pct, sl, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    brightness_pct_set(pct, lvl);
    lv_obj_add_event_cb(sl, brightness_cb, LV_EVENT_VALUE_CHANGED, pct);
    lv_obj_add_event_cb(sl, group_key_cb,  LV_EVENT_KEY, NULL);   /* Back/Menu */
    lv_group_add_obj(ui_screen_group(scr), sl);
    lv_group_focus_obj(sl);
    lv_group_set_editing(ui_screen_group(scr), true);   /* wheel adjusts directly */
    lv_obj_add_event_cb(scr, brightness_del_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* Shared destructive-action confirm: question text above two centred buttons,
 * the action on top and `cancel` below it, focused (the safe default). Both
 * buttons fire on SHORT_CLICKED: when the confirm was opened by a long press
 * (BT forget, podcast delete) that press is still held, and its release would
 * deliver CLICKED to the freshly-focused button, instantly dismissing -- or
 * confirming -- the popup. SHORT_CLICKED is suppressed after a long press.
 * `on_confirm` receives the question label as user data (to report a failure
 * in place); cancel just goes back. */
static void confirm_cancel_cb(lv_event_t *e) { (void)e; ui_back(); }

static lv_obj_t *confirm_button(lv_obj_t *scr, lv_obj_t *body, const char *txt,
                                int y, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(body);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    lv_label_set_text(lv_label_create(b), txt);
    ui_apply_focus_style(b);
    lv_obj_add_event_cb(b, cb,           LV_EVENT_SHORT_CLICKED, ud);
    lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY,           NULL);
    lv_group_add_obj(ui_screen_group(scr), b);
    return b;
}

lv_obj_t *ui_confirm_screen(const char *title, const char *question,
                            const char *action, const char *cancel,
                            lv_event_cb_t on_confirm, int danger)
{
    lv_obj_t *scr = ui_screen_base(title);
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *q = lv_label_create(body);
    /* LONG_DOT against a fixed size: device/episode names can be long enough
     * to run off a 320px screen and would otherwise wrap over the buttons. */
    lv_obj_set_size(q, 300, 38);
    lv_label_set_long_mode(q, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(q, question);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *yes = confirm_button(scr, body, action, 0, on_confirm, q);
    if (danger) ui_style_danger(yes);
    lv_obj_t *no = confirm_button(scr, body, cancel, 44, confirm_cancel_cb, NULL);

    lv_group_focus_obj(no);   /* default-focus the safe choice */
    return scr;
}

/* Picker row holding the current value: checkmark it and open the screen with
 * focus on it, so a stray Play press doesn't change the setting (and long
 * lists like Timezone don't start a wheel-scroll away). */
/* A list row's label: "..." at its width instead of the default marquee.
 * LONG_DOT only truncates with a fixed height, else it wraps. */
static void label_one_line_dot(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static void picker_mark_current(lv_obj_t *b)
{
    lv_obj_t *ck = lv_label_create(b);
    lv_label_set_text(ck, LV_SYMBOL_OK);
    lv_obj_align(ck, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_group_focus_obj(b);
    lv_obj_scroll_to_view(b, LV_ANIM_OFF);
}

static void poweroff_yes_cb(lv_event_t *e) { (void)e; ui_power_off(); }

lv_obj_t *scr_poweroff(void)
{
    return ui_confirm_screen("Power Off", "Shut down?", "Yes, shut down", "No",
                             poweroff_yes_cb, 1);
}

/* --- Backlight Timeout selector (seconds; 0 = Off) --- */
static const int bltimeout_secs[] = { 0, 15, 30, 60, 120, 300 };
#define BLTIMEOUT_NCHOICES ((int)(sizeof bltimeout_secs / sizeof bltimeout_secs[0]))

static void bltimeout_pick_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    ui_power_bltimeout_set(s);
    ui_toast(s ? "Saved" : "Off");
    ui_back();
}

static void bltimeout_label(int s, char *txt, size_t n)
{
    if (s == 0)        lv_snprintf(txt, n, "Off");
    else if (s < 60)   lv_snprintf(txt, n, "%ds", s);
    else               lv_snprintf(txt, n, "%d min", s / 60);
}

lv_obj_t *scr_bltimeout(void)
{
    lv_obj_t *scr = ui_screen_base("Backlight Timeout");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_bltimeout_get();
    for (int i = 0; i < BLTIMEOUT_NCHOICES; i++) {
        int s = bltimeout_secs[i];
        char txt[24]; bltimeout_label(s, txt, sizeof txt);
        lv_obj_t *b = lv_list_add_button(list, NULL, txt);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (s == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, bltimeout_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s);
        lv_obj_add_event_cb(b, group_key_cb,      LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Backlight Wake selector (0=Power only, 1=Auto, 2=Any button) --- */
static const char *const blwake_names[] = { "Power only", "Auto", "Any button" };
#define BLWAKE_NCHOICES 3

static void blwake_pick_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    ui_power_blwake_set(m);
    ui_toast("Saved");
    ui_back();
}

lv_obj_t *scr_blwake(void)
{
    lv_obj_t *scr = ui_screen_base("Backlight Wake");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_blwake_get();
    for (int m = 0; m < BLWAKE_NCHOICES; m++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, blwake_names[m]);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (m == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, blwake_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        lv_obj_add_event_cb(b, group_key_cb,   LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Refresh Rate selector (UI_REFRESH_*; applies live) --- */
static void refresh_pick_cb(lv_event_t *e)
{
    ui_power_refresh_set((int)(intptr_t)lv_event_get_user_data(e));
    ui_toast("Saved");
    ui_back();
}

lv_obj_t *scr_refresh(void)
{
    lv_obj_t *scr = ui_screen_base("Refresh Rate");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_refresh_get();
    for (int m = 0; m < UI_REFRESH_NCHOICES; m++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, ui_refresh_name(m));
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (m == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, refresh_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        lv_obj_add_event_cb(b, group_key_cb,    LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Theme selector: the iPod look, an erosq theme or a Rockbox theme on the
 * SD card (THEMES.md). Phase 1 applies colours + backdrop. */
static char s_theme_ids[64][THEME_ID_MAX];

/* Every live screen was built for the old body width: rebuild Home and the
 * path back here (Settings > Display), disposing the rest. */
static void ui_relayout(void)
{
    while (ui_nav_depth(&s_nav) > 0) {
        lv_obj_t *o = ui_nav_top(&s_nav);
        if (ui_nav_depth(&s_nav) == 1) { lv_async_call(screen_dispose_cb, o); break; }
        ui_nav_pop(&s_nav);
        lv_async_call(screen_dispose_cb, o);
    }
    if (s_preview_timer) { lv_timer_delete(s_preview_timer); s_preview_timer = NULL; }   /* points at the old Home */
    lv_obj_t *root = scr_menu();
    ui_nav_reset(&s_nav, root);
    lv_screen_load(root);
    set_active_group(root);
    ui_goto(scr_settings);
    ui_goto(scr_display);
    lv_obj_t *d = lv_screen_active();
    /* focus the Theme row (scr_display's first), as a plain ui_back from the picker would */
    lv_obj_t *list = lv_obj_get_child(ui_screen_body(d), 0);
    lv_group_focus_obj(lv_obj_get_child(list, 0));
    clock_refresh();
}

static void theme_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int was = ui_theme_panel_w();
    unsigned fonts = ui_theme_font_gen();
    int rc = ui_theme_select(i ? s_theme_ids[i - 1] : "");
    /* new panel or fonts: rebuild every screen (font pointers sit in local styles) */
    if (!rc && (ui_theme_panel_w() != was || ui_theme_font_gen() != fonts)) ui_relayout();
    else ui_back();
    /* Stay on the Theme list (2e.2 HITL: picking a theme "exits out of the
     * theme page"): reopen it on top of Display -- rebuilt, so it carries the
     * new look and the checkmark -- focused on the row just picked. */
    ui_goto(scr_theme);
    lv_obj_t *tl = lv_obj_get_child(ui_screen_body(lv_screen_active()), 0);
    lv_obj_t *row = tl ? lv_obj_get_child(tl, i) : NULL;
    if (row) { lv_group_focus_obj(row); lv_obj_scroll_to_view(row, LV_ANIM_OFF); }
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_RIGHT, -ui_theme_clock_right(), 4);
    if (!rc) clock_refresh();
    ui_toast(rc ? "Couldn't read theme" : "Saved");
}

lv_obj_t *scr_theme(void)
{
    lv_obj_t *scr = ui_screen_base("Theme");
    lv_obj_t *list = ui_screen_list(scr);
    int n = ui_theme_list(s_theme_ids, 64);
    const char *cur = ui_theme_current();
    for (int i = 0; i <= n; i++) {
        const char *id = i ? s_theme_ids[i - 1] : "";
        char label[THEME_ID_MAX + 12];
        if (!i)                                  snprintf(label, sizeof label, "iPod (default)");
        else if (!strncmp(id, "rockbox:", 8))    snprintf(label, sizeof label, "%s (Rockbox)", id + 8);
        else                                     snprintf(label, sizeof label, "%s", strchr(id, ':') + 1);
        lv_obj_t *b = lv_list_add_button(list, NULL, label);
        label_one_line_dot(lv_obj_get_child(b, -1));   /* theme names can be long */
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (!strcmp(id, cur)) picker_mark_current(b);
        lv_obj_add_event_cb(b, theme_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, group_key_cb,  LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* Auto power-off timeout selector. 0 = Off. Selecting a row persists it and
 * returns; the current value is checkmarked. */
static const int autooff_mins[] = { 0, 5, 10, 15, 30, 60 };
#define AUTOOFF_NCHOICES ((int)(sizeof autooff_mins / sizeof autooff_mins[0]))

static void autooff_pick_cb(lv_event_t *e)
{
    int min = (int)(intptr_t)lv_event_get_user_data(e);
    ui_power_autooff_set(min);
    ui_toast(min ? "Saved" : "Off");
    ui_back();
}

lv_obj_t *scr_autooff(void)
{
    lv_obj_t *scr = ui_screen_base("Auto Power Off");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_autooff_get();
    for (int i = 0; i < AUTOOFF_NCHOICES; i++) {
        int m = autooff_mins[i];
        char txt[24];
        if (m == 0) lv_snprintf(txt, sizeof txt, "Off");
        else        lv_snprintf(txt, sizeof txt, "%d min", m);
        lv_obj_t *b = lv_list_add_button(list, NULL, txt);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (m == cur) {   /* checkmark the active choice (inherits row text color) */
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, autooff_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
        lv_obj_add_event_cb(b, group_key_cb,    LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Timezone selector (Casio GA-B2100 city list, see src/ui_clock_tz.h) --- */
static void timezone_pick_cb(lv_event_t *e)
{
    ui_clock_tz_set((int)(intptr_t)lv_event_get_user_data(e));   /* persists + applies */
    clock_refresh();   /* show the new zone before the next 1s tick */
    ui_toast("Saved");
    ui_back();
}

/* Timezone row focus: the UTC offset is secondary (50%) on white rows but
 * full-strength white on the focused blue row. */
static void tz_ofs_focus_cb(lv_event_t *e)
{
    lv_obj_set_style_text_opa(lv_event_get_user_data(e),
        lv_event_get_code(e) == LV_EVENT_FOCUSED ? LV_OPA_COVER : LV_OPA_50, 0);
}

lv_obj_t *scr_timezone(void)
{
    lv_obj_t *scr = ui_screen_base("Timezone");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_clock_tz_get(), n = ui_clock_tz_count();
    int ck_w = -1;   /* checkmark column width, measured once from the first row */
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, ui_clock_tz_name(i));
        label_one_line_dot(lv_obj_get_child(b, -1));   /* "..." if a side panel narrows the row */
        ui_focus_add(b);
        ui_apply_focus_style(b);
        /* Standard UTC offset, right-aligned before the checkmark column. It's a
         * flex item after the growing name label, so it sits flush right; rows
         * without the checkmark pad by its width so the offsets line up.
         * Secondary color via 50% opacity of the inherited row color: #808080
         * on white like the other secondary text; full opacity while the row is
         * focused, since 50% white on the blue gloss is too faint. Formatted here at build
         * time only -- no timers. */
        char ofs[UI_CLOCK_OFS_BUF];
        if (ui_clock_std_offset_label(ui_clock_tz_rule(i), ofs, sizeof ofs) == 0) {
            if (ck_w < 0)
                ck_w = text_w(LV_SYMBOL_OK, strlen(LV_SYMBOL_OK),
                         lv_obj_get_style_text_font(b, LV_PART_MAIN))
                       + lv_obj_get_style_pad_column(b, LV_PART_MAIN);
            lv_obj_t *ol = lv_label_create(b);
            lv_label_set_text(ol, ofs);
            lv_obj_set_style_text_opa(ol, LV_OPA_50, 0);
            lv_obj_add_event_cb(b, tz_ofs_focus_cb, LV_EVENT_FOCUSED,   ol);
            lv_obj_add_event_cb(b, tz_ofs_focus_cb, LV_EVENT_DEFOCUSED, ol);
            if (i != cur) lv_obj_set_style_pad_right(ol, ck_w, 0);
        }
        if (i == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, timezone_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, group_key_cb,     LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Clock Format selector --- */
static const char *const clockfmt_names[] = {
    "24-hour",            /* UI_CLOCK_FMT_24 */
    "12-hour",            /* UI_CLOCK_FMT_12 */
    "12-hour, no AM/PM",  /* UI_CLOCK_FMT_12_NOAP */
    "Off",                /* UI_CLOCK_FMT_OFF */
};

static void clockfmt_pick_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    ui_clock_fmt_set(f);
    clock_refresh();   /* show the new format before the next 1s tick */
    ui_toast(f == UI_CLOCK_FMT_OFF ? "Off" : "Saved");
    ui_back();
}

lv_obj_t *scr_clockfmt(void)
{
    lv_obj_t *scr = ui_screen_base("Clock Format");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_clock_fmt_get();
    for (int f = 0; f < UI_CLOCK_FMT_N; f++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, clockfmt_names[f]);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (f == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, clockfmt_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
        lv_obj_add_event_cb(b, group_key_cb,     LV_EVENT_KEY,     NULL);
    }
    return scr;
}

static const char *const plshuffle_names[] = { "Off", "On" };

static void plshuffle_pick_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    mpd_shuffle_set(v);
    ui_toast(v ? "On" : "Off");
    ui_back();
}

/* Settings > Shuffle Playlists. Applies to playlists and genres (the bucket
 * plays); albums are unaffected and always play in track order. */
lv_obj_t *scr_plshuffle(void)
{
    lv_obj_t *scr = ui_screen_base("Shuffle Playlists");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = mpd_shuffle_get();
    for (int v = 0; v < 2; v++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, plshuffle_names[v]);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (v == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, plshuffle_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
        lv_obj_add_event_cb(b, group_key_cb,      LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* Date & Time submenu, grouped like Display/Power so Settings doesn't sprawl.
 * No time *setting* here: ntpd runs on every WiFi connect (wifi_connect.sh). */
lv_obj_t *scr_datetime(void)
{
    lv_obj_t *scr = ui_screen_base("Date & Time");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Timezone",     scr_timezone);
    menu_item(list, "Clock Format", scr_clockfmt);
    return scr;
}

/* --- settings-batch1 pickers ----------------------------------------------
 * One list of choices, ✓ on the current one; the row index is the value. */
static lv_obj_t *choice_picker(const char *title, const char *const *names, int n,
                               int cur, lv_event_cb_t pick_cb)
{
    lv_obj_t *scr = ui_screen_base(title);
    lv_obj_t *list = ui_screen_list(scr);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_list_add_button(list, NULL, names[i]);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (i == cur) picker_mark_current(b);
        lv_obj_add_event_cb(b, pick_cb,      LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY,     NULL);
    }
    return scr;
}
static int pick_index(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }

/* Display > Buttons When Off. "None" + Backlight Wake "Power only" is a pocket
 * lock (only POWER does anything while dark) -- allowed on purpose. */
static const char *const blkeys_names[UI_BLKEYS_NCHOICES] = { "All buttons", "Volume only", "None" };
static void blkeys_pick_cb(lv_event_t *e)
{
    ui_power_blkeys_set(pick_index(e));
    ui_toast("Saved");
    ui_back();
}
lv_obj_t *scr_blkeys(void)
{
    return choice_picker("Buttons When Off", blkeys_names, UI_BLKEYS_NCHOICES,
                         ui_power_blkeys_get(), blkeys_pick_cb);
}

/* Settings > Repeat. MPD holds the mode (state_file + mpd-persist carry it
 * across power-off), so read it back from status rather than a /data copy. */
static const char *const repeat_names[UI_REPEAT_NCHOICES] = { "Off", "All", "One" };
static void repeat_pick_cb(lv_event_t *e)
{
    char r[64];
    ui_toast(mpd_cmd(ui_repeat_cmd(pick_index(e)), r, sizeof r) > 0 && !strstr(r, "ACK")
             ? "Saved" : "Music player not running");
    ui_back();
}
lv_obj_t *scr_repeat(void)
{
    char r[1024], f[16];
    int rep = 0, sing = 0;
    if (mpd_cmd("status", r, sizeof r) > 0) {
        if (mpd_field(r, "repeat", f, sizeof f)) rep  = f[0] == '1';
        if (mpd_field(r, "single", f, sizeof f)) sing = f[0] == '1';   /* "oneshot" -> 0 */
    }
    return choice_picker("Repeat", repeat_names, UI_REPEAT_NCHOICES,
                         ui_repeat_from_status(rep, sing), repeat_pick_cb);
}

/* Settings > Volume Limit. Applies at once: a level above the new cap drops. */
static const char *const vollimit_names[UI_VOLLIMIT_NCHOICES] = { "Off", "50%", "60%", "70%", "80%", "90%" };
static void vollimit_pick_cb(lv_event_t *e)
{
    ui_power_vollimit_set(ui_vollimit_choices[pick_index(e)]);
    ui_volume_enforce_limit();
    ui_toast("Saved");
    ui_back();
}
lv_obj_t *scr_vollimit(void)
{
    int cur = 0, lim = ui_power_vollimit_get();
    for (int i = 0; i < UI_VOLLIMIT_NCHOICES; i++) if (ui_vollimit_choices[i] == lim) cur = i;
    return choice_picker("Volume Limit", vollimit_names, UI_VOLLIMIT_NCHOICES, cur, vollimit_pick_cb);
}

/* Settings > Playback > Disconnect Pause: wired (last jack pulled) and BT (sink drops). */
static const char *const discpause_names[] = { "Off", "On" };
static void discpause_pick_cb(lv_event_t *e)
{
    ui_power_discpause_set(pick_index(e));
    ui_toast("Saved");
    ui_back();
}
/* Settings > Playback > Bluetooth Buttons: play/pause/next/prev from Bluetooth
 * headphones or a Bluetooth speaker. Title is 127 px of the 128 px limit. */
static const char *const btbuttons_names[] = { "Off", "On" };
static void btbuttons_pick_cb(lv_event_t *e)
{
    ui_power_btbuttons_set(pick_index(e));
    ui_toast("Saved");
    ui_back();
}
lv_obj_t *scr_btbuttons(void)
{
    return choice_picker("Bluetooth Buttons", btbuttons_names, 2,
                         ui_power_btbuttons_get(), btbuttons_pick_cb);
}

lv_obj_t *scr_discpause(void)
{
    return choice_picker("Disconnect Pause", discpause_names, 2,
                         ui_power_discpause_get(), discpause_pick_cb);
}

/* Display submenu: groups all the screen/backlight options so the Settings
 * root doesn't sprawl (brightness + the two backlight-idle settings). */
lv_obj_t *scr_display(void)
{
    lv_obj_t *scr = ui_screen_base("Display");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Theme",             scr_theme);   /* first row: the most-visited Display setting */
    menu_item(list, "Brightness",        scr_brightness);
    menu_item(list, "Backlight Timeout", scr_bltimeout);
    menu_item(list, "Backlight Wake",    scr_blwake);
    menu_item(list, "Buttons When Off", scr_blkeys);
    menu_item(list, "Refresh Rate",      scr_refresh);
    return scr;
}

/* --- Charge Limit selector (percent; 100 = charge to full) ---
 * Stopping short of full is easier on a lithium cell if the player spends its
 * life on the charger. Enforced by the kernel (axp2101_battery polls the gauge
 * and gates the charger), so it keeps working if the UI is restarted -- but only
 * while the device is ON: charging with the system powered off is the PMU's own
 * business and will run to 100%. */
static const int chglimit_pcts[] = { UI_CHGLIMIT_OFF, 80 };
#define CHGLIMIT_NCHOICES ((int)(sizeof chglimit_pcts / sizeof chglimit_pcts[0]))

static void chglimit_pick_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    ui_power_chglimit_set(p);
    ui_toast(p >= UI_CHGLIMIT_OFF ? "Off" : "Saved");
    ui_back();
}

lv_obj_t *scr_chglimit(void)
{
    lv_obj_t *scr = ui_screen_base("Charge Limit");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_chglimit_get();
    for (int i = 0; i < CHGLIMIT_NCHOICES; i++) {
        int p = chglimit_pcts[i];
        char txt[24];
        if (p >= UI_CHGLIMIT_OFF) lv_snprintf(txt, sizeof txt, "Off (100%%)");
        else                      lv_snprintf(txt, sizeof txt, "Stop at %d%%", p);
        lv_obj_t *b = lv_list_add_button(list, NULL, txt);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (p == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, chglimit_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        lv_obj_add_event_cb(b, group_key_cb,     LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* --- Low Battery Off selector: power off cleanly at N% on battery (0 = Off). */
static void critbatt_pick_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    ui_power_critbatt_set(p);
    ui_toast(p ? "Saved" : "Off");
    ui_back();
}

lv_obj_t *scr_critbatt(void)
{
    lv_obj_t *scr = ui_screen_base("Low Battery");
    lv_obj_t *list = ui_screen_list(scr);
    int cur = ui_power_critbatt_get();
    for (int i = 0; i < CRITBATT_NCHOICES; i++) {
        int p = critbatt_choices[i];
        char txt[24];
        if (p == 0) lv_snprintf(txt, sizeof txt, "Off");
        else        lv_snprintf(txt, sizeof txt, "Power off at %d%%", p);
        lv_obj_t *b = lv_list_add_button(list, NULL, txt);
        ui_focus_add(b);
        ui_apply_focus_style(b);
        if (p == cur) {
            picker_mark_current(b);
        }
        lv_obj_add_event_cb(b, critbatt_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        lv_obj_add_event_cb(b, group_key_cb,     LV_EVENT_KEY,     NULL);
    }
    return scr;
}

/* Power submenu: charge ceiling + auto power-off timeout + manual shutdown. */
lv_obj_t *scr_power(void)
{
    lv_obj_t *scr = ui_screen_base("Power");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Charge Limit",   scr_chglimit);
    menu_item(list, "Auto Power Off", scr_autooff);
    menu_item(list, "Low Battery Off", scr_critbatt);
    menu_item(list, "Power Off",      scr_poweroff);
    return scr;
}

/* Settings actions: refresh the MPD library, and safely eject the SD card. */
/* Update / Rescan Library: MPD scans in the background; `status` carries
 * updating_db (the running job's id) while it runs. Poll it once a second
 * against the id `update` returned (mpd_update_poll_job), then report the new
 * song count, or why it failed, in a 3 s toast. One job at a time: MPD would
 * queue a second one, but the toast could only speak for one.
 *
 * While it runs a small note sits at the bottom of every screen (lv_layer_top)
 * so a 6-minute rescan is visibly in progress. It is created once and deleted
 * once -- no per-tick redraw. The poll is paused while the panel is blanked
 * (blank_set) and runs at once on wake, so a scan that ends in the dark is
 * reported when the screen comes back instead of toasting unseen (2d: the
 * result "only showed on the second press"). */
static lv_obj_t   *s_libupd_note;
static unsigned    s_libupd_job;
static int s_libupd_fail;
/* No row may sit under the note: while it is up, every scrollable child that
 * reaches the bottom of a screen's body (the row lists) is shortened by the
 * note's height, and the focused row is scrolled clear; restored when the note
 * goes. Applied to the active screen when the note comes/goes, and to each
 * screen as it loads (screens kept on the nav stack catch up there). Marked
 * with LV_OBJ_FLAG_USER_1 so restore only undoes what was shortened. Runs on
 * screen loads only -- nothing per frame or per poll. */
static int32_t s_libupd_inset;   /* px the note covers at the bottom; 0 = no note */
static void libupd_inset_apply(lv_obj_t *scr)
{
    if (!scr || lv_obj_get_child_count(scr) < 2) return;
    /* Now Playing has no rows to shorten and its times sit just above the
     * note's spot (closer still with a theme font): hide the note there, show
     * it again on the next screen. One flag flip per load, no per-frame work. */
    if (s_libupd_note) {
        if (scr == s_np.scr) lv_obj_add_flag(s_libupd_note, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_remove_flag(s_libupd_note, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_update_layout(scr);
    int32_t ch = lv_obj_get_content_height(body), changed = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) {
        lv_obj_t *c = lv_obj_get_child(body, i);
        int marked = lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1);
        if (s_libupd_inset && !marked && lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE)
            && lv_obj_get_y(c) + lv_obj_get_height(c) >= ch) {
            lv_obj_set_height(c, lv_obj_get_height(c) - s_libupd_inset);
            lv_obj_add_flag(c, LV_OBJ_FLAG_USER_1);
            changed = 1;
        } else if (!s_libupd_inset && marked) {
            /* inset stored per object: the note height is fixed for the session */
            lv_obj_set_height(c, ch - lv_obj_get_y(c));
            lv_obj_remove_flag(c, LV_OBJ_FLAG_USER_1);
            changed = 1;
        }
    }
    if (!changed) return;
    lv_obj_update_layout(scr);
    lv_group_t *g = lv_obj_get_user_data(scr);
    lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
    if (f) lv_obj_scroll_to_view_recursive(f, LV_ANIM_OFF);
}
static void libupd_scr_load_cb(lv_event_t *e)
{
    libupd_inset_apply(lv_event_get_param(e));
}
static void libupd_note_show(const char *txt)
{
    if (s_libupd_note) lv_obj_delete(s_libupd_note);
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, UI_COL_ACCENT, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 3, 0);
    lv_obj_set_style_pad_hor(box, 6, 0);
    lv_obj_set_style_pad_ver(box, 1, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -1);   /* status bar (battery) is at the top */
    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_color(l, UI_COL_ON_ACCENT, 0);
    lv_label_set_text(l, txt);
    s_libupd_note = box;
    static int hooked;
    if (!hooked && g_ctx && g_ctx->disp) {   /* once: cheap no-op on loads without a note */
        lv_display_add_event_cb(g_ctx->disp, libupd_scr_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
        hooked = 1;
    }
    lv_obj_update_layout(box);
    s_libupd_inset = lv_obj_get_height(box) + 1;   /* + the 1 px it sits off the bottom */
    libupd_inset_apply(lv_screen_active());
}
static void libupd_finish(void)
{
    if (s_libupd_note) { lv_obj_delete(s_libupd_note); s_libupd_note = NULL; }
    s_libupd_inset = 0;
    libupd_inset_apply(lv_screen_active());
    if (s_libupd_tm)   { lv_timer_delete(s_libupd_tm); s_libupd_tm = NULL; }
}
static void libupd_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_blanked) return;          /* belt-and-braces: blank_set pauses this timer */
    char r[4096], v[32], msg[64];   /* status: same size np_refresh reads it with */
    int st = mpd_update_poll_job(mpd_cmd("status", r, sizeof r) > 0 ? r : NULL,
                                 s_libupd_job, &s_libupd_fail);
    if (st == MPD_UPD_RUNNING) return;
    libupd_finish();                /* note gone before the result toast goes up */
    if (st == MPD_UPD_FAILED)
        ui_toast_ms("Update failed: music player not responding", 3000);
    else if (mpd_cmd("stats", r, sizeof r) > 0 && mpd_field(r, "songs", v, sizeof v))
        { lv_snprintf(msg, sizeof msg, "Library updated: %s songs", v); ui_toast_ms(msg, 3000); }
    else
        ui_toast_ms("Library updated", 3000);
}
static void libupd_start(const char *cmd, const char *busy_msg)
{
    char r[256], why[40], msg[64];   /* why: short enough for a one-line toast */
    if (s_libupd_tm) { ui_toast("Library update already running"); return; }
    if (mpd_cmd(cmd, r, sizeof r) <= 0) { ui_toast_ms("Update failed: music player not responding", 3000); return; }
    if (mpd_ack_text(r, why, sizeof why)) {          /* "ACK [..] {update} <why>" */
        lv_snprintf(msg, sizeof msg, "Update failed: %s", why);
        ui_toast_ms(msg, 3000);
        return;
    }
    s_libupd_job = mpd_update_job(r);
    s_libupd_fail = 0;
    libupd_note_show(busy_msg);     /* persistent until the job ends (replaces the 0.8 s toast) */
    s_libupd_tm = lv_timer_create(libupd_tick_cb, 1000, NULL);
}
static void update_lib_click_cb(lv_event_t *e) { (void)e; libupd_start("update", "Updating library..."); }

/* "update" skips files whose mtime didn't change, so tags edited by a tool that
 * keeps the timestamp stay stale. "rescan" re-reads every file (slower). */
static void rescan_lib_click_cb(lv_event_t *e) { (void)e; libupd_start("rescan", "Rescanning library..."); }

static void eject_sd_click_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("Ejecting...");
    lv_refr_now(g_ctx->disp);       /* paint the toast before the blocking eject */
    system("/usr/sbin/sd-eject");   /* stop+sync+umount+clear db; brief blocking call */
    ui_toast("Safe to remove card");
}

/* USB File Transfer mode. Entering this screen turns the USB mass-storage LUN
 * ON (stops mpd, unmounts the SD, exposes /dev/mmcblk0 to the host); usb0 (the
 * ssh/dev path) stays up throughout -- the ECM + storage functions are both
 * bound at once, so the toggle only swaps the LUN's backing file.
 *
 * Leaving is handled two ways: usb_transfer_key_cb gates Back/Home on a
 * SUCCESSFUL SD handoff (so it can keep the user here and say "eject on your
 * computer first" when the host still holds the drive), and usb_transfer_del_cb
 * is a belt-and-suspenders restore on the screen's DELETE lifecycle so any other
 * teardown path can never strand the device in storage mode (mpd stopped, SD
 * unmounted). The del cb is idempotent -- a no-op once already in network mode. */

/* The on-screen message and the state it shows. Every state change goes through
 * usb_xfer_show() so the screen always shows exactly one message (the text per
 * state + the transitions live in ui_usb_logic.c, host-tested). */
static lv_obj_t *s_usb_xfer_lbl;
static enum usb_xfer_state s_usb_xfer_state;

static void usb_xfer_show(enum usb_xfer_state s)
{
    s_usb_xfer_state = s;
    if (s_usb_xfer_lbl) lv_label_set_text(s_usb_xfer_lbl, usb_xfer_text(s));
}

static void usb_transfer_del_cb(lv_event_t *e)
{
    (void)e;
    s_usb_xfer_lbl = NULL;                          /* label is being freed; drop the ref */
    /* Safety net only: a normal Back/Home already ran the handoff. Idempotent and
     * never forces a busy eject (storage_off returns early when already network). */
    system("/usr/bin/erosq-usbgadget network");
}

/* Leaving this screen must hand the SD card back to the player, which can only
 * succeed once the *host* has ejected the USB drive -- the mass-storage gadget
 * returns EBUSY otherwise, and we must not force it (the computer may hold
 * unflushed data). So Back/Home are gated here while connected: attempt the
 * handoff and only navigate away when it succeeds; while the computer still
 * holds the drive we stay put and tell the user to eject there first.
 * The FAILED state is NOT gated: storage-on aborted, so the host never had the
 * card, and gating it only trapped the user whenever `network` also failed (e.g.
 * the gadget couldn't compose) -- behind a false "Still connected" message.
 * Leaving still runs `network` via usb_transfer_del_cb. */
static void usb_transfer_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_ESC && k != UI_KEY_HOME) return;   /* only Back / Home leave */
    if (usb_xfer_back_gated(s_usb_xfer_state)) {
        usb_xfer_show(USB_XFER_EXITING);
        lv_refr_now(g_ctx->disp);                      /* paint before the blocking switch */
        /* rc 0 = SD reclaimed (network mode); non-zero = host still holds the drive. */
        if (!usb_xfer_exit_ok(system("/usr/bin/erosq-usbgadget network"))) {
            usb_xfer_show(USB_XFER_BUSY);
            return;                                    /* stay in transfer mode */
        }
    }
    if (k == UI_KEY_HOME) ui_home();
    else                  ui_back();
}

lv_obj_t *scr_usb_transfer(void)
{
    /* CONNECTING: shown over the calling screen while the switch blocks, then
     * removed explicitly -- a timed toast would still be up (a blue box reading
     * "Connecting...") on top of the result message. */
    lv_obj_t *busy = ui_toast_box(usb_xfer_text(USB_XFER_CONNECTING));
    lv_refr_now(g_ctx->disp);                      /* paint before the blocking switch */
    int rc = system("/usr/bin/erosq-usbgadget storage");  /* stop mpd + unmount SD + expose it */
    lv_obj_delete(busy);

    lv_obj_t *scr  = ui_screen_base("USB Transfer");
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *lbl  = lv_label_create(body);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(90));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    s_usb_xfer_lbl = lbl;
    /* storage-on aborts (and restores network) if it can't free the SD. Don't
     * claim we're a drive when we aren't. */
    usb_xfer_show(usb_xfer_after_enter(rc));
    /* Display-only screen (label, nothing focusable): without a key receiver the
     * ESC(Back)/HOME keys have no object to fire on and Back does nothing. Add the
     * body to the group and route its keys through usb_transfer_key_cb, which
     * gates leaving on a successful SD handoff (see above). */
    lv_group_add_obj(ui_screen_group(scr), body);
    lv_obj_add_event_cb(body, usb_transfer_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(body);
    lv_obj_add_event_cb(scr, usb_transfer_del_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* Playback submenu: everything that changes how music plays. */
lv_obj_t *scr_playback(void)
{
    lv_obj_t *scr = ui_screen_base("Playback");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Shuffle Playlists", scr_plshuffle); /* playlists + genres */
    menu_item(list, "Repeat",            scr_repeat);
    menu_item(list, "Volume Limit",      scr_vollimit);
    menu_item(list, "Disconnect Pause",  scr_discpause);  /* wired + BT */
    menu_item(list, "Bluetooth Buttons", scr_btbuttons);  /* AVRCP play/pause/next/prev */
    return scr;
}

/* Library submenu: the SD/database actions. */
lv_obj_t *scr_library(void)
{
    lv_obj_t *scr = ui_screen_base("Library");
    lv_obj_t *list = ui_screen_list(scr);
    menu_cmd (list, "Update Library", update_lib_click_cb);
    menu_cmd (list, "Rescan Library", rescan_lib_click_cb);
    menu_cmd (list, "Eject SD Card",  eject_sd_click_cb);
    return scr;
}

/* System submenu: clock, firmware, About (iPod nests About under Settings). */
lv_obj_t *scr_system(void)
{
    lv_obj_t *scr = ui_screen_base("System");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Date & Time",     scr_datetime);
    menu_item(list, "Software Update", scr_update);
    menu_item(list, "About",           scr_about);
    return scr;
}

/* Settings root: groups only, every leaf lives one level down. */
lv_obj_t *scr_settings(void)
{
    lv_obj_t *scr = ui_screen_base("Settings");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "Playback",    scr_playback);
    menu_item(list, "Display",     scr_display);
    menu_item(list, "Power",       scr_power);
    menu_item(list, "Connections", scr_network);   /* WiFi, Bluetooth, USB File Transfer */
    menu_item(list, "Library",     scr_library);
    menu_item(list, "System",      scr_system);
    return scr;
}

/* ---- SD firmware update (Task B6): scan for a *.swu, confirm, swupdate,
 * reboot. Business logic (SD scan, slot selection via update_logic.h, the
 * blocking swupdate invocation) lives in ui_update.c/.h -- this is just the
 * LVGL screen, mirroring the WiFi/Bluetooth split (ui_net.c/ui_bt.c own the
 * logic, ui_screens.c owns the widgets). */

static lv_obj_t *s_upd_scr;             /* the live Update screen, or NULL */
static int       s_upd_present = -1;    /* last-shown SD-has-.swu state; -1 = not yet built */
static char      s_upd_swu[128];        /* basename of the found bundle (valid iff s_upd_present) */
static int       s_upd_installing;      /* guard: don't poll-rebuild or double-run mid-install */

/* Low-frequency SD poll (2s), matching the project's perf rule of never
 * touching the SD card on the render hot path. opendir/readdir over a card's
 * root directory is cheap; this only runs while the Update screen is open
 * (timer is owned by the screen and deleted with it, like wifi_tick_cb). */
static void upd_tick_cb(lv_timer_t *t)
{
    if (lv_timer_get_user_data(t) != s_upd_scr) return;   /* stale instance */
    if (s_upd_installing) return;                          /* don't yank the screen mid-install */
    char name[128];
    int present = ui_update_find_swu(name, sizeof name);
    if (present != s_upd_present || (present && strcmp(name, s_upd_swu) != 0)) {
        if (ui_nav_top(&s_nav) == s_upd_scr) { ui_back(); ui_goto(scr_update); }
    }
}
static void upd_del_cb(lv_event_t *e)
{
    lv_timer_t *t = lv_obj_get_user_data(lv_event_get_target(e));
    if (t) lv_timer_delete(t);
    if (lv_event_get_user_data(e) == s_upd_scr) s_upd_scr = NULL;
}

/* Runs the (blocking) install, then shows the result. Reuses the
 * toast+lv_refr_now-before-blocking-call idiom already used by
 * eject_sd_click_cb/ui_power_off so the panel doesn't show a stale frame
 * while swupdate runs. */
static void upd_install_cb(lv_event_t *e)
{
    (void)e;
    if (s_upd_installing || !s_upd_scr) return;
    s_upd_installing = 1;
    lv_obj_t *scr = s_upd_scr;
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_clean(body);   /* drop the file-found UI; show progress instead */

    lv_obj_t *spin = lv_spinner_create(body);
    lv_obj_set_size(spin, 40, 40);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *lbl = lv_label_create(body);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(90));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, "Installing...\nKeep the charger connected.");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 30);

    lv_refr_now(g_ctx->disp);   /* paint before the blocking swupdate call */

    char msg[160];
    ui_update_result_t r = ui_update_run(s_upd_swu, msg, sizeof msg);

    lv_obj_clean(body);
    lv_obj_t *res = lv_label_create(body);
    lv_obj_set_width(res, lv_pct(90));
    lv_label_set_long_mode(res, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(res, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(res);

    if (r == UI_UPD_OK) {
        lv_label_set_text(res, "Update installed -- rebooting");
        lv_refr_now(g_ctx->disp);
        s_upd_installing = 0;
        ui_update_reboot();   /* does not return on success */
        return;
    }
    if (r == UI_UPD_ALREADY) lv_label_set_text(res, msg);
    else                     lv_label_set_text_fmt(res, "Update failed:\n%s", msg);

    s_upd_installing = 0;
    ui_screen_keysink(scr);   /* result is display-only now: body catches ESC/HOME */
}

lv_obj_t *scr_update(void)
{
    lv_obj_t *scr = ui_screen_base("Update");
    lv_obj_t *body = ui_screen_body(scr);

    s_upd_scr = scr;
    int present = ui_update_find_swu(s_upd_swu, sizeof s_upd_swu);
    s_upd_present = present;

    lv_timer_t *tt = lv_timer_create(upd_tick_cb, 2000, scr);
    lv_obj_set_user_data(body, tt);
    lv_obj_add_event_cb(body, upd_del_cb, LV_EVENT_DELETE, scr);

    if (!present) {
        lv_obj_t *l = lv_label_create(body);
        lv_label_set_text(l, "No update found on SD card.");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        ui_screen_keysink(scr);   /* display-only: body catches ESC/HOME */
        return scr;
    }

    lv_obj_t *nm = lv_label_create(body);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nm, lv_pct(90));
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    if (ui_update_swu_installed(s_upd_swu)) {   /* nothing to install: no Install button */
        lv_label_set_text_fmt(nm, "Already installed:\n%s", s_upd_swu);
        lv_obj_center(nm);
        ui_screen_keysink(scr);
        return scr;
    }
    lv_label_set_text_fmt(nm, "Found:\n%s", s_upd_swu);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *btn = lv_button_create(body);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 10);
    lv_label_set_text(lv_label_create(btn), "Install");
    ui_apply_focus_style(btn);
    ui_focus_add(btn);
    lv_obj_add_event_cb(btn, upd_install_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, group_key_cb,   LV_EVENT_KEY,     NULL);
    lv_group_focus_obj(btn);

    lv_obj_t *hint = lv_label_create(body);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(90));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "Keep the charger connected during install.");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    return scr;
}

/* ---- Task 8: WiFi (wpa_supplicant ctrl socket) ---------------------------- */

/* Passphrase keyboard screen. SSID is stashed here so the screen can be built
 * by a no-arg builder and routed through ui_goto (which handles nav push +
 * input-group retarget + async dispose for us). */
static char s_pending_ssid[33];

/* A join is saved (and the other saved networks re-enabled) only once it
 * worked: wpa_join_poll() finishes it. 1 s timer, alive only while a join is
 * pending (<= WPA_JOIN_POLLS s), independent of which screen is up. */
static lv_timer_t *s_join_timer;
static int s_join_saved;   /* the pending join is of a SAVED network (stays saved) */
static void join_tm_cb(lv_timer_t *t)
{
    /* Not gated on s_blanked: this finishes a transaction (save + re-enable
     * the others), runs only while a join is pending (<= WPA_JOIN_POLLS s). */
    int r = wpa_join_poll();
    if (r == 0 && wpa_join_pending()) return;
    lv_timer_delete(t); s_join_timer = NULL;
    if (r < 0) ui_toast(s_join_saved ? "Couldn't join" : "Couldn't join - not saved");
}
static void join_watch(void)
{
    s_join_saved = 0;
    if (!s_join_timer) s_join_timer = lv_timer_create(join_tm_cb, 1000, NULL);
}

static lv_obj_t *s_pw_ta, *s_pw_chk;

static void kb_ready_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    const char *psk = lv_textarea_get_text(lv_keyboard_get_textarea(kb));
    int rc = (psk[0] == '\0') ? wpa_connect_open(s_pending_ssid)  /* empty -> open network */
                              : wpa_connect_psk(s_pending_ssid, psk);
    if (rc == -2) { ui_toast("Bad passphrase (8-63 chars)"); return; }  /* stay; retry */
    if (rc < 0)  { ui_toast("WiFi not responding"); return; }  /* wpa unreachable -- NOT
                                                                * the passphrase's fault */
    join_watch();
    ui_toast("Connecting...");
    ui_back();   /* pops + disposes this keyboard screen (deferred) */
}

/* Vol+/- toggles password visibility (checkbox reflects it). Driven by a button,
 * not a focusable widget: the wheel is locked into "editing" the keyboard, so it
 * can't also navigate to a separate checkbox. */
static void kb_pw_toggle(void)
{
    if (!s_pw_ta) return;
    bool hidden = lv_textarea_get_password_mode(s_pw_ta);
    lv_textarea_set_password_mode(s_pw_ta, !hidden);
    if (s_pw_chk) {
        if (hidden) lv_obj_add_state(s_pw_chk, LV_STATE_CHECKED);     /* now visible */
        else        lv_obj_remove_state(s_pw_chk, LV_STATE_CHECKED);
    }
}

/* Keyboard-screen keys: Back/Menu and Vol=show-password. The wheel (LEFT/RIGHT)
 * and Play (ENTER) are left for the buttonmatrix to handle. */
static void kb_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ESC)  ui_back();
    else if (k == UI_KEY_HOME) ui_home();
    else if (k == UI_KEY_VOLUP || k == UI_KEY_VOLDOWN) kb_pw_toggle();
}

static lv_obj_t *scr_wifi_keyboard(void)
{
    lv_obj_t *scr = ui_screen_base_kind(s_pending_ssid, UI_SCR_FULL);
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *ta = lv_textarea_create(body);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_obj_set_width(ta, 300);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 2);
    s_pw_ta = ta;

    /* show-password checkbox under the field (toggled by Vol±, see kb_pw_toggle) */
    s_pw_chk = lv_checkbox_create(body);
    lv_checkbox_set_text(s_pw_chk, "Vol: show password");
    lv_obj_align_to(s_pw_chk, ta, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    lv_obj_t *kb = lv_keyboard_create(body);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_key_cb,   LV_EVENT_KEY,   NULL);
    lv_group_add_obj(ui_screen_group(scr), kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(ui_screen_group(scr), true);   /* wheel walks the keys */
    return scr;
}

/* Manually add a network (e.g. a hidden SSID): type the name, then chain into
 * the passphrase screen (empty passphrase -> open). scan_ssid=1 (set in
 * wpa_connect_*) makes wpa actively probe for it. */
static void ssid_ready_cb(lv_event_t *e)
{
    const char *txt = lv_textarea_get_text(lv_keyboard_get_textarea(lv_event_get_target(e)));
    if (txt[0] == '\0') { ui_toast("Enter a name"); return; }
    lv_snprintf(s_pending_ssid, sizeof s_pending_ssid, "%s", txt);
    ui_back();                    /* drop the SSID screen ... */
    ui_goto(scr_wifi_keyboard);   /* ... and ask for the passphrase */
}

static lv_obj_t *scr_wifi_addnet(void)
{
    lv_obj_t *scr = ui_screen_base("Add Network");
    lv_obj_t *body = ui_screen_body(scr);
    s_pw_ta = NULL;               /* no password field here; guard kb_pw_toggle */
    lv_obj_t *ta = lv_textarea_create(body);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Network name (SSID)");
    lv_obj_set_width(ta, 300);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *kb = lv_keyboard_create(body);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, ssid_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_key_cb,     LV_EVENT_KEY,   NULL);
    lv_group_add_obj(ui_screen_group(scr), kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(ui_screen_group(scr), true);
    return scr;
}

static void ap_click_cb(lv_event_t *e)
{
    ui_wifi_ap_t *ap = lv_event_get_user_data(e);
    if (!ap->secured) {
        if (wpa_connect_open(ap->ssid) < 0) { ui_toast("WiFi not responding"); return; }
        join_watch();
        ui_toast("Connecting...");
        return;
    }
    lv_snprintf(s_pending_ssid, sizeof s_pending_ssid, "%s", ap->ssid);
    ui_goto(scr_wifi_keyboard);
}

/* ---- Regulatory country picker (roller) ---- */
/* Curated common codes; first 2 chars of each line are the ISO code passed to
 * `iw reg set`. A country outside this list isn't selectable (per design). */
static const char COUNTRIES[] =
    "US  United States\nGB  United Kingdom\nDE  Germany\nFR  France\n"
    "ES  Spain\nIT  Italy\nNL  Netherlands\nSE  Sweden\nNO  Norway\n"
    "DK  Denmark\nFI  Finland\nPL  Poland\nIE  Ireland\nCH  Switzerland\n"
    "AT  Austria\nAU  Australia\nNZ  New Zealand\nJP  Japan\nCN  China\n"
    "KR  South Korea\nIN  India\nCA  Canada\nBR  Brazil\nMX  Mexico\n"
    "ZA  South Africa\nTR  Turkey\nAE  UAE\nSG  Singapore\nHK  Hong Kong";

static int country_index(const char *cc)   /* roller row for code cc, or -1 */
{
    int idx = 0; const char *p = COUNTRIES;
    for (;;) {
        if (p[0] == cc[0] && p[1] == cc[1]) return idx;
        const char *nl = strchr(p, '\n');
        if (!nl) return -1;
        p = nl + 1; idx++;
    }
}

static void country_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ESC)       ui_back();
    else if (k == UI_KEY_HOME) ui_home();
    /* ENTER/Play confirm is handled on CLICKED, not here: doing it on the KEY
     * press lets the key release bleed through and re-trigger the "Country"
     * button on the WiFi screen we return to (roller kept popping back up). */
}

static void country_click_cb(lv_event_t *e)
{
    char opt[40]; lv_roller_get_selected_str(lv_event_get_target(e), opt, sizeof opt);
    char cc[3] = { opt[0], opt[1], 0 };
    wifi_set_country(cc);
    ui_toast(cc);
    /* Rebuild the WiFi screen so its "Country:" row reflects the new value
     * (ui_back alone returns to the stale, already-built screen). */
    ui_back();             /* roller -> WiFi (stale) */
    ui_back();             /* WiFi  -> Network */
    ui_goto(scr_wifi);     /* fresh WiFi reads the saved country */
}

static lv_obj_t *scr_wifi_country(void)
{
    lv_obj_t *scr = ui_screen_base("Country");
    lv_obj_t *r = lv_roller_create(ui_screen_body(scr));
    lv_roller_set_options(r, COUNTRIES, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, 280);
    lv_obj_center(r);
    char cur[3];
    if (wifi_get_country(cur) == 0) {
        int i = country_index(cur);
        if (i >= 0) lv_roller_set_selected(r, i, LV_ANIM_OFF);
    }
    lv_obj_add_event_cb(r, country_key_cb,   LV_EVENT_KEY,     NULL);
    lv_obj_add_event_cb(r, country_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(ui_screen_group(scr), r);
    lv_group_focus_obj(r);
    lv_group_set_editing(ui_screen_group(scr), true);   /* wheel scrolls the roller */
    return scr;
}

/* Scan results, cached at file scope so the WiFi screen displays the last scan
 * without re-scanning on every entry; the Scan button refreshes them. Cleared
 * on every power toggle -- after an off->on cycle (radio genuinely down in
 * between) the old list is guaranteed stale and redisplaying it as current
 * offered APs that may no longer exist. */
static ui_wifi_ap_t s_aps[24];
static int s_ap_n;
static int s_wifi_scanned;   /* a scan ran since the last power toggle: empty = "none found" */

/* WiFi power transitions are ASYNC (wifi_set_enabled backgrounds wifi_up.sh /
 * wifi_down.sh; a cold bring-up takes ~5-15s): these track the transient.
 * s_wifi_pending = in-flight intent (+1 turning on, -1 turning off, 0 idle);
 * s_wifi_shown = what the current WiFi screen displays (0 off / 1 on / 2
 * starting -- a pending OFF displays as plain off: teardown is sub-second, and
 * if it ever fails the tick's mismatch-rebuild flips the screen back to the
 * truth); s_wifi_scr/-status_lbl = the LIVE screen instance (guarded like
 * s_bt_scr: a rebuilt screen's deferred DELETE must not clobber the new one). */
static int s_wifi_pending, s_wifi_ticks, s_wifi_shown;
static lv_obj_t *s_wifi_scr, *s_wifi_status_lbl;
static lv_obj_t *s_ap_val[24];   /* each AP row's right-hand label (parallel to s_aps) */

/* Saved networks (wpa LIST_NETWORKS), shown like the BT page's My Devices:
 * Select joins, a long press offers Forget. Read on every build and on the 2s
 * tick (values in place; a changed SET rebuilds the screen, keeping focus). */
static wifi_saved_t s_saved[WIFI_SAVED_MAX];
static int s_saved_n;
static unsigned s_saved_sig;                     /* identity of the rows shown */
static lv_obj_t *s_saved_val[WIFI_SAVED_MAX];    /* right-hand labels (parallel) */
static int s_saved_shown[WIFI_SAVED_MAX];        /* value last drawn: skip no-op redraws */

static unsigned saved_sig(const wifi_saved_t *v, int n)   /* FNV-1a over id/flag/ssid */
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) {
        unsigned char k[3] = { (unsigned char)v[i].id, (unsigned char)v[i].disabled,
                               (unsigned char)v[i].ssid_len };
        for (int j = 0; j < 3; j++) h = (h ^ k[j]) * 16777619u;
        for (int j = 0; j < v[i].ssid_len; j++) h = (h ^ (unsigned char)v[i].ssid[j]) * 16777619u;
    }
    return h ^ (unsigned)n;
}

static int scan_find(const char *ssid, int len)   /* s_aps index of an SSID, or -1 */
{
    for (int i = 0; i < s_ap_n; i++)
        if ((int)strlen(s_aps[i].ssid) == len && !memcmp(s_aps[i].ssid, ssid, len)) return i;
    return -1;
}

/* Right-hand label of saved row i: Connected / Connecting... / Disabled, else
 * the signal word if the last scan saw it, "Not in Range" if it did not, and
 * nothing before any scan (no blocking scan on entry, as for the AP list). */
static void wifi_saved_val(int i)
{
    if (!s_saved_val[i]) return;
    const wifi_saved_t *w = &s_saved[i];
    int a = scan_find(w->ssid, w->ssid_len);
    enum { K_CONN = 1001, K_JOIN, K_DIS, K_GONE, K_UNKNOWN };   /* above any dBm */
    int key = w->state == WIFI_SAVED_CONNECTED ? K_CONN : w->state == WIFI_SAVED_JOINING ? K_JOIN
            : w->disabled ? K_DIS : a >= 0 ? s_aps[a].signal : s_wifi_scanned ? K_GONE : K_UNKNOWN;
    if (key == s_saved_shown[i]) return;   /* unchanged: no relabel, no redraw */
    s_saved_shown[i] = key;
    const char *v = key == K_CONN ? "Connected" : key == K_JOIN ? "Connecting..." : key == K_DIS ? "Disabled"
                  : key == K_GONE ? "Not in Range" : key == K_UNKNOWN ? "" : wifi_strength(s_aps[a].signal);
    lv_label_set_text(s_saved_val[i], v);
    row_name_fit(lv_obj_get_parent(s_saved_val[i]), w->name, v);
}

/* Right-hand label of AP row i: a tick on the joined network (iPod picker
 * idiom), else "Open" for open networks plus a plain-word signal. */
static void wifi_ap_val(int i, const char *joined)
{
    if (!s_ap_val[i]) return;
    char v[32];
    int on = joined[0] && !strcmp(s_aps[i].ssid, joined);
    lv_obj_set_style_text_opa(s_ap_val[i], on ? LV_OPA_COVER : LV_OPA_60, 0);   /* tick at full strength */
    if (on)
        lv_snprintf(v, sizeof v, LV_SYMBOL_OK);
    else
        lv_snprintf(v, sizeof v, "%s%s", s_aps[i].secured ? "" : "Open  ",
                    wifi_strength(s_aps[i].signal));
    lv_label_set_text(s_ap_val[i], v);
    char name[33];   /* raw over-the-air bytes: never hand LVGL bad UTF-8/controls */
    wifi_ssid_display(s_aps[i].ssid, strlen(s_aps[i].ssid), name, sizeof name);
    row_name_fit(lv_obj_get_parent(s_ap_val[i]), name, v);   /* re-cut to the new value */
}

/* Status hint + AP ticks, both live: association/DHCP land seconds after a
 * connect, while the user is already back on this screen. */
static void wifi_status_refresh(void)
{
    char st[96], joined[33];
    wifi_status(st, sizeof st, joined);
    if (s_wifi_status_lbl) lv_label_set_text(s_wifi_status_lbl, st);
    for (int i = 0; i < s_ap_n; i++) wifi_ap_val(i, joined);
}

/* Rebuild the WiFi screen, keeping the cursor on the same network (rows
 * carry their SSID as user data), else on the same list index -- after a
 * Forget that is the row that took the forgotten one's place. */
static char s_wifi_refocus[33];
static int  s_wifi_refocus_idx = -1;
static void wifi_rebuild(void)
{
    lv_group_t *g = s_wifi_scr ? ui_screen_group(s_wifi_scr) : NULL;
    lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
    s_wifi_refocus[0] = 0; s_wifi_refocus_idx = -1;
    if (f) {
        const char *id = lv_obj_get_user_data(f);
        if (id) lv_snprintf(s_wifi_refocus, sizeof s_wifi_refocus, "%s", id);
        s_wifi_refocus_idx = lv_obj_get_index(f);
    }
    ui_back(); ui_goto(scr_wifi);
}

/* 2s tick: re-read the saved list. Same rows -> update values in place;
 * different rows (a join just saved one, a Forget, wpa restarted) -> rebuild,
 * but only when this screen is on top (a popup over it is never yanked). */
static void wifi_saved_refresh(void)
{
    wifi_saved_t v[WIFI_SAVED_MAX];
    int n = wifi_saved_list(v, WIFI_SAVED_MAX);
    if (n < 0) return;
    if (saved_sig(v, n) != s_saved_sig) {
        if (ui_nav_top(&s_nav) == s_wifi_scr) wifi_rebuild();
        return;
    }
    for (int i = 0; i < n; i++) { s_saved[i].state = v[i].state; wifi_saved_val(i); }
}

/* One network row (name + right-hand value label) / one section header:
 * shared by the Saved and Other sections. */
static lv_obj_t *net_row(lv_obj_t *list, lv_obj_t **val)
{
    lv_obj_t *b = lv_list_add_button(list, NULL, "");
    *val = row_value(b, "");
    return b;
}
static void net_header(lv_obj_t *list, const char *t) { lv_list_add_text(list, t); }

static void saved_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_saved_n) return;
    const wifi_saved_t *w = &s_saved[i];
    if (w->disabled)                         { ui_toast("Disabled - hold to forget"); return; }
    if (w->state == WIFI_SAVED_CONNECTED)    { ui_toast("Connected"); return; }
    if (w->state == WIFI_SAVED_JOINING)      return;
    if (wpa_join_saved(w->id) < 0)           { ui_toast("WiFi not responding"); return; }
    join_watch();
    s_join_saved = 1;
    ui_toast("Connecting...");
    wifi_saved_refresh();   /* the row reads "Connecting..." now, not in 2 s */
}

/* Forget confirm. The SSID is SNAPSHOTTED (raw bytes + label name): the tick
 * can rebuild s_saved while the popup is up. */
static char s_forget_ssid[33], s_forget_wname[33];
static int  s_forget_len;
static void wifi_forget_yes_cb(lv_event_t *e)
{
    (void)e;
    int cur;
    int rc = wpa_forget(s_forget_ssid, s_forget_len, &cur);
    if (cur) wifi_lease_drop();   /* deauthenticated: the old lease must go too */
    ui_back();                                   /* popup -> WiFi */
    if (rc < 0) ui_toast("WiFi not responding");
    if (s_wifi_scr && ui_nav_top(&s_nav) == s_wifi_scr) wifi_rebuild();
}
static lv_obj_t *scr_wifi_forget(void)
{
    char q[48];
    lv_snprintf(q, sizeof q, "Forget %s?", s_forget_wname);
    return ui_confirm_screen("Forget Network", q, "Forget", "Cancel", wifi_forget_yes_cb, 0);
}
static void saved_longpress_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_saved_n) return;
    memcpy(s_forget_ssid, s_saved[i].ssid, sizeof s_forget_ssid);
    s_forget_len = s_saved[i].ssid_len;
    memcpy(s_forget_wname, s_saved[i].name, sizeof s_forget_wname);
    ui_goto(scr_wifi_forget);   /* its buttons take SHORT_CLICKED: the release of
                                 * this long press must not hit Cancel/Forget */
}
#ifdef EROSQ_HOST
/* Host screenshots: LVGL's SDL keyboard sends press+release per key, so it can
 * never long-press. Delete stands in for the long press on a saved row. */
static void saved_hostkey_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_DEL) saved_longpress_cb(e);
}
#endif

static void wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
    /* While a transition is pending the tap means "reverse it"; otherwise
     * toggle ground truth. The scripts converge on the LAST intent via
     * /run/wifi-want, so a fast off<->on is safe in either order. */
    int on = s_wifi_pending ? (s_wifi_pending < 0) : !wifi_is_enabled();
    s_wifi_pending = on ? 1 : -1; s_wifi_ticks = 0;
    s_ap_n = 0;                     /* cached scan is stale across a power cycle */
    s_wifi_scanned = 0;
    ui_toast(on ? "Turning WiFi on..." : "WiFi off");
    wifi_set_enabled(on);  /* backgrounded; the 2s tick below tracks the result */
    wifi_pref_save(on);    /* remember across reboots */
    ui_back();             /* rebuild this screen so it reflects the new state */
    ui_goto(scr_wifi);
}
static void country_open_cb(lv_event_t *e) { (void)e; ui_goto(scr_wifi_country); }
static void addnet_open_cb(lv_event_t *e)  { (void)e; ui_goto(scr_wifi_addnet); }
static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("Scanning...");
    lv_refr_now(g_ctx->disp);            /* show the toast before the blocking scan */
    char r[4096];
    wpa_cmd("SCAN", r, sizeof r);
    usleep(1500 * 1000);                 /* ponytail: blocking scan, only on demand now */
    wpa_cmd("SCAN_RESULTS", r, sizeof r);
    s_ap_n = wpa_parse_scan(r, s_aps, 24);
    s_wifi_scanned = 1;
    ui_back(); ui_goto(scr_wifi);        /* rebuild to show results */
}

/* Per-screen 2s tick, owned by the screen's list (deleted with it). Three
 * jobs: (1) refresh the status line (association + DHCP land seconds after a
 * connect); (2) detect enable-state changes -- the backgrounded bring-up /
 * teardown, or a boot-restore finishing -- and rebuild the screen to match
 * (only when it is the top screen; a covered stale instance rebuilds on the
 * first tick after it resurfaces); (3) give the pending bring-up an honest
 * deadline: wifi_up.sh worst case is ~25s in-lock, so if 30s pass without
 * wifi_is_enabled() flipping, report the failure instead of showing
 * "starting..." forever. wpa PING costs 2 syscalls; connect() fails fast when
 * the daemon is down -- cheap enough for a 2s poll on this screen only. */
static void wifi_tick_cb(lv_timer_t *t)
{
    if (lv_timer_get_user_data(t) != s_wifi_scr) return;   /* stale instance */
    if (s_blanked) return;   /* screen off: no wpa polling; the first tick after wake catches up */
    int en = wifi_is_enabled();
    if (s_wifi_pending > 0) {                              /* turning on */
        if (en) s_wifi_pending = 0;                        /* bring-up confirmed */
        else if (++s_wifi_ticks >= 15) {
            s_wifi_pending = 0;
            ui_toast("WiFi failed to start");
        }
    } else if (s_wifi_pending < 0) {                       /* turning off */
        if (!en) s_wifi_pending = 0;                       /* teardown confirmed */
        else if (++s_wifi_ticks >= 15) s_wifi_pending = 0; /* give up the grace;
                                                            * mismatch-rebuild
                                                            * shows the truth */
    }
    int cur = s_wifi_pending > 0 ? 2 : s_wifi_pending < 0 ? 0 : en;
    if (cur != s_wifi_shown) {
        if (ui_nav_top(&s_nav) == s_wifi_scr) { ui_back(); ui_goto(scr_wifi); }
        return;
    }
    if (en && s_wifi_status_lbl) { wifi_status_refresh(); wifi_saved_refresh(); }
}
static void wifi_list_del_cb(lv_event_t *e)
{
    lv_timer_t *t = lv_obj_get_user_data(lv_event_get_target(e));
    if (t) lv_timer_delete(t);
    /* Only forget the live instance if WE are it: on rebuild the new screen is
     * built before the old one's deferred DELETE runs (same guard as s_bt_scr). */
    if (lv_event_get_user_data(e) == s_wifi_scr) {
        s_wifi_scr = NULL;
        s_wifi_status_lbl = NULL;
        memset(s_ap_val, 0, sizeof s_ap_val);
        memset(s_saved_val, 0, sizeof s_saved_val);
    }
}

lv_obj_t *scr_wifi(void)
{
    lv_obj_t *scr = ui_screen_base("WiFi");
    lv_obj_t *list = ui_screen_list(scr);
    /* wifi_rebuild's focus request is for THIS build only, whichever branch */
    char refocus[33]; long refocus_idx = s_wifi_refocus_idx;
    memcpy(refocus, s_wifi_refocus, sizeof refocus);
    s_wifi_refocus_idx = -1; s_wifi_refocus[0] = 0;

    int en = wifi_is_enabled();
    if (s_wifi_pending > 0 && en)  s_wifi_pending = 0;   /* bring-up confirmed */
    if (s_wifi_pending < 0 && !en) s_wifi_pending = 0;   /* teardown confirmed */
    if (s_wifi_pending < 0) en = 0;         /* pending OFF displays as off */
    s_wifi_shown = s_wifi_pending > 0 ? 2 : en;
    s_wifi_scr = scr;
    s_wifi_status_lbl = NULL;
    memset(s_ap_val, 0, sizeof s_ap_val);
    memset(s_saved_val, 0, sizeof s_saved_val);
    s_saved_n = 0; s_saved_sig = saved_sig(s_saved, 0);
    /* Regulatory domain: the saved choice, else the kernel's (`iw reg get`).
     * No answer, or "00" (the world default = nothing chosen), reads as not set. */
    char cc[3]; int cc_set = wifi_get_country(cc) == 0 && strcmp(cc, "00") != 0;
    lv_obj_t *bt = lv_list_add_button(list, NULL, "WiFi");
    row_value(bt, s_wifi_shown == 2 ? "Starting..." : en ? "On" : "Off");
    ui_focus_add(bt); ui_apply_focus_style(bt);
    lv_obj_add_event_cb(bt, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bt, group_key_cb,   LV_EVENT_KEY,     NULL);

    /* Drill-in rows carry the chevron, like menu_item. */
    lv_obj_t *bc = lv_list_add_button(list, NULL, "Country");
    row_value(bc, cc_set ? cc : "Not Set");
    lv_obj_t *chev = lv_label_create(bc);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_pad_left(chev, 6, 0);   /* air between "GB" and the chevron */
    ui_focus_add(bc); ui_apply_focus_style(bc);
    lv_obj_add_event_cb(bc, country_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bc, group_key_cb,    LV_EVENT_KEY,     NULL);

    /* The tick lives on every instance (even "off": a boot-restore or the
     * backgrounded toggle can flip the state while this screen is up). */
    lv_timer_t *tt = lv_timer_create(wifi_tick_cb, 2000, scr);
    lv_obj_set_user_data(list, tt);
    lv_obj_add_event_cb(list, wifi_list_del_cb, LV_EVENT_DELETE, scr);

    if (!en) {
        if (s_wifi_shown == 2) list_empty(list, UI_ICON_WIFI, "Turning on...", NULL);
        else list_empty(list, UI_ICON_WIFI, "WiFi is off", "Turn on to join a network.");
        lv_group_focus_obj(bt);
        return scr;
    }

    lv_obj_t *bs = lv_list_add_button(list, NULL, "Scan for Networks");
    ui_focus_add(bs); ui_apply_focus_style(bs);
    lv_obj_add_event_cb(bs, scan_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bs, group_key_cb,  LV_EVENT_KEY,     NULL);

    lv_obj_t *ba = lv_list_add_button(list, NULL, "Add Network");   /* matches its screen title */
    lv_label_set_text(lv_label_create(ba), LV_SYMBOL_RIGHT);
    ui_focus_add(ba); ui_apply_focus_style(ba);
    lv_obj_add_event_cb(ba, addnet_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ba, group_key_cb,   LV_EVENT_KEY,     NULL);

    char st[96], joined[33];
    wifi_status(st, sizeof st, joined);
    s_wifi_status_lbl = list_hint(list, st);   /* the 2s tick keeps it live */

    /* Saved networks first (BT's "My Devices"; hidden when none). Select
     * joins, long press -> Forget; SHORT_CLICKED so a long press's release
     * doesn't also join. [DISABLED] ones are listed (they are still in the
     * conf, so the user can see and forget them) but are not joinable. */
    int n = wifi_saved_list(s_saved, WIFI_SAVED_MAX);
    s_saved_n = n > 0 ? n : 0;
    s_saved_sig = saved_sig(s_saved, s_saved_n);
    if (s_saved_n) net_header(list, "Saved Networks");
    for (int i = 0; i < s_saved_n; i++) {
        lv_obj_t *b = net_row(list, &s_saved_val[i]);
        s_saved_shown[i] = 0x7fffffff;   /* matches no key: forces the first draw */
        wifi_saved_val(i);      /* value before first layout, so the name's "..." fits it */
        lv_obj_set_user_data(b, s_saved[i].ssid);   /* identity for wifi_rebuild */
        ui_focus_add(b); ui_apply_focus_style(b);
        lv_obj_add_event_cb(b, saved_click_cb,     LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, saved_longpress_cb, LV_EVENT_LONG_PRESSED,  (void *)(intptr_t)i);
#ifdef EROSQ_HOST
        lv_obj_add_event_cb(b, saved_hostkey_cb,   LV_EVENT_KEY,           (void *)(intptr_t)i);
#endif
        lv_obj_add_event_cb(b, group_key_cb,       LV_EVENT_KEY,           NULL);
    }

    /* Then the last scan's other networks (no blocking scan on entry; Scan
     * refreshes). A joinable saved SSID is listed above only; a disabled one
     * also shows here, so it can be re-joined with its password.
     * Empty: compact block, no icon -- 4 rows + status leave only ~60px. */
    int n_other = 0;
    for (int i = 0; i < s_ap_n; i++) {
        int k = 0;
        while (k < s_saved_n && (s_saved[k].disabled || (int)strlen(s_aps[i].ssid) != s_saved[k].ssid_len
                                 || memcmp(s_aps[i].ssid, s_saved[k].ssid, s_saved[k].ssid_len))) k++;
        n_other += k == s_saved_n;
    }
    if (n_other == 0)
        list_empty(list, NULL, s_wifi_scanned ? (s_saved_n ? "No other networks" : "No networks found")
                                              : "No networks yet",
                   s_wifi_scanned ? "Move closer to your router and scan again."
                                  : "Press Scan to look for networks.");
    else
        net_header(list, s_saved_n ? "Other Networks" : "Networks");
    for (int i = 0; i < s_ap_n; i++) {
        int k = 0;
        while (k < s_saved_n && (s_saved[k].disabled || (int)strlen(s_aps[i].ssid) != s_saved[k].ssid_len
                                 || memcmp(s_aps[i].ssid, s_saved[k].ssid, s_saved[k].ssid_len))) k++;
        if (k < s_saved_n) continue;   /* listed under Saved Networks */
        lv_obj_t *b = net_row(list, &s_ap_val[i]);
        wifi_ap_val(i, joined);   /* value before first layout, so the name's "..." fits it */
        ui_focus_add(b);
        ui_apply_focus_style(b);
        lv_obj_set_user_data(b, s_aps[i].ssid);   /* identity for wifi_rebuild */
        lv_obj_add_event_cb(b, ap_click_cb,  LV_EVENT_CLICKED, &s_aps[i]);
        lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY,     NULL);
    }
    lv_obj_t *want = bt;
    if (refocus_idx >= 0) {   /* wifi_rebuild: same network, else same index */
        uint32_t cnt = lv_obj_get_child_count(list);
        lv_obj_t *byidx = NULL;
        for (uint32_t c = 0; c < cnt; c++) {
            lv_obj_t *ch = lv_obj_get_child(list, c);
            if (!lv_obj_get_group(ch)) continue;           /* headers, hints */
            const char *id = lv_obj_get_user_data(ch);
            if (refocus[0] && id && !strcmp(id, refocus)) { byidx = ch; break; }
            if ((long)c <= refocus_idx || !byidx) byidx = ch;   /* last focusable <= idx */
        }
        if (byidx) want = byidx;
    }
    lv_group_focus_obj(want);
    if (want != bt) lv_obj_scroll_to_view(want, LV_ANIM_OFF);
    return scr;
}

lv_obj_t *scr_network(void)
{
    /* Connections. WiFi's Country row stays inside WiFi: picking a country
     * rebuilds that screen so its row shows the new value (country_click_cb). */
    lv_obj_t *scr = ui_screen_base("Connections");
    lv_obj_t *list = ui_screen_list(scr);
    menu_item(list, "WiFi",      scr_wifi);
    menu_item(list, "Bluetooth", scr_bluetooth);   /* Task 9 */
    menu_item(list, "USB File Transfer", scr_usb_transfer);
    return scr;
}

/* ---- Bluetooth (Task 6) ------------------------------------------------- */
/* Event-driven: device list from ui_bt_devices(); status label updated by the
 * ui_bt_set_changed_cb callback, which rebuilds the screen on any adapter or
 * device change. Toggle calls ui_bt_set_power; Scan calls ui_bt_scan_start. */

static char s_bt_msg[160];   /* last error/status from ui_bt; "" = none (shown by rebuild) */

static lv_obj_t *s_bt_scr;   /* the live BT screen, or NULL when not on it */
static lv_obj_t *s_bt_list;  /* its device list, for in-place refresh (NULL when off/torn down) */
static lv_obj_t *s_bt_scan_val;  /* Scan row's right-hand "Scanning..." label (NULL with s_bt_list) */
static uint32_t  s_bt_static_n;      /* # of leading list children that survive a refresh (toggle+scan) */
static int       s_bt_shown_on = -1; /* BT on/off state currently rendered; -1 = not yet built */
static lv_timer_t *s_bt_scan_tm;     /* one-shot: auto-stop discovery after the scan window */
static int       s_bt_scan_pending;  /* toggle-on wants a scan once the adapter is actually up */

/* BlueZ inquiry finds all nearby devices within a few seconds; running it
 * indefinitely just pins the (single) CPU via the per-event PropertiesChanged
 * storm and drains battery/RF for nothing. Bound each scan; Scan re-runs it. */
/* 15s was too short to pair a real headset: BlueZ prunes every temporary
 * (unpaired, unconnected) device the moment discovery stops, so anything found
 * VANISHED from the list at the timeout -- press Scan, look away, and the row
 * you wanted is gone. Bounded rather than "scan while on-screen" so a device
 * left on the BT page in a pocket can't hold the radio in inquiry. */
#define BT_SCAN_SECS 60

static void bt_refresh_devices(void);   /* fwd: in-place device-list refill */
static void bt_scan_begin(void);        /* fwd: start discovery + arm auto-stop */

/* Detach the D-Bus changed callback when the BT screen is torn down for good,
 * so a later device connect can't yank the user back here from another page.
 * A self-rebuild (ui_back+ui_goto) builds the new screen first -- updating
 * s_bt_scr and re-arming the callback -- so the old screen's deferred DELETE
 * (ui_back disposes async) sees user_data != s_bt_scr and leaves it alone. */
static void bt_scr_del_cb(lv_event_t *e)
{
    if (lv_event_get_user_data(e) == s_bt_scr) {
        ui_bt_set_changed_cb(NULL);
        ui_bt_scan_stop();   /* leaving BT for good: don't let inquiry run forever (RF congestion + battery) */
        if (s_bt_scan_tm) { lv_timer_delete(s_bt_scan_tm); s_bt_scan_tm = NULL; }
        s_bt_scan_pending = 0;   /* scan is BT-screen UX: don't fire it on a later revisit */
        s_bt_scr = NULL;
        s_bt_list = NULL;
        s_bt_scan_val = NULL;
    }
}

/* Debounced BT-screen rebuild. During discovery in busy RF, PropertiesChanged
 * events arrive many times per second; rebuilding per event saturated the CPU
 * (HW-observed load 4, input starved for tens of seconds, memory pressure).
 * Coalesce to at most one rebuild per 700ms; the guard is re-checked at fire
 * time because the user may have navigated away while the timer was pending. */
static lv_timer_t *s_bt_rebuild_tm;
static void bt_rebuild_tm_cb(lv_timer_t *t)
{
    (void)t;
    s_bt_rebuild_tm = NULL;   /* one-shot: LVGL auto-deletes it after this cb */
    if (!s_bt_scr || ui_nav_top(&s_nav) != s_bt_scr) return;
    if (ui_bt_is_on() != s_bt_shown_on) {
        /* Power state flipped: the screen's structure changes (the Scan button
         * and the on/off body appear or vanish), so a full rebuild is warranted.
         * Rare -- only on an explicit user toggle. */
        ui_bt_set_changed_cb(NULL);   /* de-register before rebuild to prevent re-entry */
        ui_back(); ui_goto(scr_bluetooth);
        return;
    }
    bt_refresh_devices();   /* frequent path (RSSI/connect churn): refill rows in place */
}

static void bt_changed_cb(const char *msg)
{
    /* Fired from the GLib/D-Bus pump (inside lv_timer_handler via ui_dbus). */
    /* Seed the BT sink to the current UI volume only on the connect EDGE (a
     * freshly connected headset would otherwise start at bluealsa's loud
     * full-scale default). Pushing on every D-Bus event would clobber any
     * volume the user set on the headset itself between events. */
    static int s_bt_was_active;
    int bt_active = ui_bt_audio_active();
    if (bt_active && !s_bt_was_active) {
        ui_bt_set_volume_pct(s_vol);
        /* Connected: discovery is stopped on connect, so drop the pending
         * auto-stop timer -- else it would overwrite the status with "Scan
         * complete" ~15s after the user is already listening. */
        if (s_bt_scan_tm) { lv_timer_delete(s_bt_scan_tm); s_bt_scan_tm = NULL; }
        s_bt_msg[0] = 0;   /* an earlier "Connect failed" is now stale */
    }
    s_bt_was_active = bt_active;
    if (msg && msg[0]) snprintf(s_bt_msg, sizeof s_bt_msg, "%s", msg);
    /* Deferred scan kick (toggle-on): fire once the adapter is genuinely up --
     * StartDiscovery into a bluez that isn't on the bus yet is a silent no-op.
     * Only while the user is still on the BT screen; if they left, drop it. */
    if (s_bt_scan_pending && ui_bt_ready()) {
        s_bt_scan_pending = 0;
        if (ui_nav_top(&s_nav) == s_bt_scr) bt_scan_begin();
    }
    /* Rebuild ONLY when the BT screen is top-of-stack. ui_back() pops whatever
     * is on top: with the poweroff confirm (or anything else) pushed above us,
     * a BT event would yank it away; and after HOME, an event landing before
     * the deferred DELETE clears s_bt_scr would re-push the BT screen. The
     * status text is already saved, so the next build shows it. */
    if (ui_nav_top(&s_nav) != s_bt_scr) return;
    if (s_bt_rebuild_tm) return;             /* rebuild already scheduled: coalesce */
    s_bt_rebuild_tm = lv_timer_create(bt_rebuild_tm_cb, 700, NULL);
    if (s_bt_rebuild_tm) lv_timer_set_repeat_count(s_bt_rebuild_tm, 1);
}

static void bt_toggle_cb(lv_event_t *e)
{
    (void)e;
    int on = !ui_bt_is_on();
    ui_toast(on ? "Turning Bluetooth on..." : "Bluetooth off");
    if (!on) s_bt_msg[0] = 0;
    ui_bt_set_power(on);   /* backgrounded: returns immediately, state settles async */
    bt_pref_save(on);      /* remember across reboots */
    /* Bringup is backgrounded, so discovery can't start here -- bluez isn't on
     * the bus yet. bt_changed_cb kicks the scan once ui_bt_ready(). */
    s_bt_scan_pending = on;
    /* The changed callback will fire once BlueZ reports the new Powered state
     * and will call ui_back()+ui_goto(scr_bluetooth) to rebuild. */
}

static void bt_scan_click_cb(lv_event_t *e)
{
    (void)e;
    bt_scan_begin();   /* the row itself shows "Scanning..." -- no toast */
    /* The changed callback fires as devices appear and refreshes the list;
     * discovery auto-stops after BT_SCAN_SECS. */
}

static void bt_dev_click_cb(lv_event_t *e)
{
    const bt_device_t *d = lv_event_get_user_data(e);
    if (!d) return;
    if (d->connected) {
        ui_toast("Disconnecting...");
        ui_bt_disconnect(d->path);
    } else {
        ui_toast("Connecting...");
        ui_bt_connect(d->path);
    }
    /* The changed callback rebuilds the screen when the connection state changes. */
}

/* Forget-device confirm. Path+name are SNAPSHOTTED here: a BT event can
 * remove/compact the device table while this popup is up, so holding the
 * row's bt_device_t* would dangle. */
static char s_forget_path[BT_PATH_MAX];
static char s_forget_name[BT_NAME_MAX];

static void bt_forget_yes_cb(lv_event_t *e)
{
    (void)e;
    ui_toast("Forgetting...");
    ui_bt_remove(s_forget_path);
    ui_back();
    /* If the device already vanished (bluetoothd restart), RemoveDevice
     * errors with DoesNotExist -> toast; list is already correct. */
}
static lv_obj_t *scr_bt_forget(void)
{
    char q[BT_NAME_MAX + 16];
    lv_snprintf(q, sizeof q, "Forget %s?", s_forget_name);
    return ui_confirm_screen("Forget Device", q, "Forget", "Cancel", bt_forget_yes_cb, 0);
}

static void bt_dev_longpress_cb(lv_event_t *e)
{
    const bt_device_t *d = lv_event_get_user_data(e);
    if (!d || !d->paired) return;
    lv_snprintf(s_forget_path, sizeof s_forget_path, "%s", d->path);
    lv_snprintf(s_forget_name, sizeof s_forget_name, "%s",
                d->name[0] ? d->name : d->addr);
    ui_goto(scr_bt_forget);
    /* While this popup is top-of-stack, bt_changed_cb's
     * "ui_nav_top(&s_nav) != s_bt_scr" guard already suppresses BT-screen
     * rebuilds -- the popup cannot be yanked away by a BT event. */
}

/* Render one device row (used by both the Paired and Available sections). */
static void bt_add_device_row(lv_obj_t *list, const bt_device_t *d)
{
    /* Name left, state right (iOS "Connected"/"Not Connected"); scan results
     * carry no state. The name label flex-grows, so it scrolls instead of
     * pushing the state off-screen. */
    char val[40] = "";
    if (d->connected && d->battery >= 0)   /* beside a side panel the battery alone says connected */
        lv_snprintf(val, sizeof val, ui_theme_panel_w() ? LV_SYMBOL_BATTERY_FULL " %d%%"
                                                         : LV_SYMBOL_BATTERY_FULL " %d%%  Connected", d->battery);
    else if (d->connected)
        lv_snprintf(val, sizeof val, "Connected");
    else if (d->paired)
        lv_snprintf(val, sizeof val, "Not Connected");
    lv_obj_t *b = lv_list_add_button(list, NULL, "");
    /* Ellipsise, not marquee: refills every 700ms mid-scan would restart the
     * scroll, and an unfocused scrolling name reads as a glitch beside the state. */
    row_name_fit(b, d->name[0] ? d->name : d->addr, val);
    if (val[0]) row_value(b, val);
    ui_focus_add(b); ui_apply_focus_style(b);
    /* Row identity for focus restore across a refill (see bt_refresh_devices). */
    lv_obj_set_user_data(b, (void *)d);
    /* Stable table pointer as user-data; the table is file-static inside ui_bt
     * and lives for the process lifetime. */
    /* SHORT_CLICKED, not CLICKED: CLICKED also fires on release after a long
     * press, which would connect/disconnect right after the forget popup. */
    lv_obj_add_event_cb(b, bt_dev_click_cb, LV_EVENT_SHORT_CLICKED, (void *)d);
    if (d->paired)
        lv_obj_add_event_cb(b, bt_dev_longpress_cb, LV_EVENT_LONG_PRESSED, (void *)d);
    lv_obj_add_event_cb(b, group_key_cb,    LV_EVENT_KEY,     NULL);
}

/* Fill the dynamic tail of the BT list: status line + Paired/Available sections.
 * Split out so a device-change refresh can rebuild just these rows, leaving the
 * screen chrome (header, gradients, list, toggle/scan buttons) untouched. */
static void bt_fill_devices(lv_obj_t *list)
{
    if (s_bt_scan_val) lv_label_set_text(s_bt_scan_val, s_bt_scan_tm ? "Scanning..." : "");
    /* Last error/status from bt_changed_cb; nothing when all is well. */
    if (s_bt_msg[0]) list_hint(list, s_bt_msg);

    /* Two sections, split on the device's Paired flag: previously-paired
     * devices first, then live scan results. */
    const bt_devtable_t *tab = ui_bt_devices();
    int n_paired = 0, n_avail = 0;
    for (int i = 0; i < tab->n; i++) {
        if (tab->dev[i].paired) n_paired++; else n_avail++;
    }

    if (n_paired) {   /* iOS hides "My Devices" until something is paired */
        lv_list_add_text(list, "My Devices");
        for (int i = 0; i < tab->n; i++)
            if (tab->dev[i].paired) bt_add_device_row(list, &tab->dev[i]);
    }

    if (n_avail == 0) {
        /* Big icon only when nothing is paired: then just two rows sit above
         * it. Under a My Devices list, a compact line pair instead. */
        list_empty(list, n_paired ? NULL : UI_ICON_BT,
                   s_bt_scan_tm ? "Searching..." : n_paired ? "No other devices" : "No devices found",
                   s_bt_scan_tm ? "Make sure your device is in pairing mode."
                                : "Put Bluetooth headphones in pairing mode, then press Scan.");
    } else {
        lv_list_add_text(list, "Other Devices");
        for (int i = 0; i < tab->n; i++)
            if (!tab->dev[i].paired) bt_add_device_row(list, &tab->dev[i]);
    }
}

/* In-place refresh: delete the dynamic tail (everything past the static
 * toggle+scan buttons) and refill it. Turns the per-RSSI-event cost from a full
 * screen teardown+rebuild into a few list rows -- the fix for the CPU spike. */
static void bt_refresh_devices(void)
{
    lv_obj_t *list = s_bt_list;
    if (!list) return;

    /* Preserve focus across the refill. A focused *static* widget (toggle/scan)
     * is restored by index. A focused device row is about to be deleted, so
     * remember WHICH DEVICE it showed (bluez object path -- the stable key) and
     * re-focus that device's new row. Restoring by index instead would break the
     * moment the table compacts or a device flips Available->Paired; falling back
     * to child 0 -- the old behaviour -- threw the cursor to the top of the page
     * on EVERY refresh, i.e. every 700ms for the whole 60s scan window, which
     * made the page unnavigable while scanning. */
    lv_group_t *g   = ui_screen_group(s_bt_scr);
    lv_obj_t   *foc = g ? lv_group_get_focused(g) : NULL;
    long fidx = (foc && lv_obj_get_parent(foc) == list) ? lv_obj_get_index(foc) : -1;
    char fpath[BT_PATH_MAX] = "";
    if (foc && fidx >= (long)s_bt_static_n) {
        const bt_device_t *fd = lv_obj_get_user_data(foc);   /* NULL on section labels */
        if (fd) lv_snprintf(fpath, sizeof fpath, "%s", fd->path);
    }

    for (uint32_t c = lv_obj_get_child_count(list); c > s_bt_static_n; c--)
        lv_obj_delete(lv_obj_get_child(list, c - 1));   /* LVGL drops it from the group too */

    bt_fill_devices(list);

    if (!g) return;
    lv_obj_t *want = NULL;
    if (fpath[0]) {
        uint32_t n = lv_obj_get_child_count(list);
        for (uint32_t c = s_bt_static_n; c < n && !want; c++) {
            lv_obj_t *ch = lv_obj_get_child(list, c);
            const bt_device_t *d = lv_obj_get_user_data(ch);
            if (d && !strcmp(d->path, fpath)) want = ch;   /* same device, new row */
        }
    }
    if (!want) {   /* static widget, or the device is gone: index, else the toggle */
        long keep = (fidx >= 0 && fidx < (long)s_bt_static_n) ? fidx : 0;
        want = lv_obj_get_child(list, keep);
    }
    lv_group_focus_obj(want);
}

static void bt_scan_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_bt_scan_tm = NULL;   /* one-shot */
    ui_bt_scan_stop();   /* the Scan row's "Scanning..." clears on this refresh */
    if (s_bt_scr && ui_nav_top(&s_nav) == s_bt_scr) bt_refresh_devices();
}

/* Start discovery and arm its auto-stop. Restarts the window if called again
 * (e.g. the user presses Scan mid-window). */
static void bt_scan_begin(void)
{
    ui_bt_scan_start();
    if (s_bt_scan_tm) lv_timer_delete(s_bt_scan_tm);
    s_bt_scan_tm = lv_timer_create(bt_scan_timeout_cb, BT_SCAN_SECS * 1000, NULL);
    if (s_bt_scan_tm) lv_timer_set_repeat_count(s_bt_scan_tm, 1);
    s_bt_msg[0] = 0;   /* a fresh scan supersedes the last error */
    if (s_bt_scr && ui_nav_top(&s_nav) == s_bt_scr) bt_refresh_devices();   /* show "Scanning..." now */
}

lv_obj_t *scr_bluetooth(void)
{
    lv_obj_t *scr = ui_screen_base_kind("Bluetooth", UI_SCR_BT);
    s_bt_scr = scr;
    s_bt_list = NULL;   /* set below only when BT is on and the device list exists */
    s_bt_scan_val = NULL;
    lv_obj_add_event_cb(scr, bt_scr_del_cb, LV_EVENT_DELETE, scr);
    lv_obj_t *list = ui_screen_list(scr);

    int on = ui_bt_is_on();
    s_bt_shown_on = on;
    lv_obj_t *bt = lv_list_add_button(list, NULL, "Bluetooth");
    row_value(bt, on ? "On" : "Off");
    ui_focus_add(bt); ui_apply_focus_style(bt);
    lv_obj_add_event_cb(bt, bt_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bt, group_key_cb, LV_EVENT_KEY,     NULL);

    if (!on) {
        list_empty(list, UI_ICON_BT, "Bluetooth is off", "Turn on to use Bluetooth headphones,\na Bluetooth speaker or a game controller.");
        lv_group_focus_obj(bt);
        ui_bt_set_changed_cb(bt_changed_cb);
        return scr;
    }

    lv_obj_t *bs = lv_list_add_button(list, NULL, "Scan for Devices");
    s_bt_scan_val = row_value(bs, "");   /* bt_fill_devices sets it */
    ui_focus_add(bs); ui_apply_focus_style(bs);
    lv_obj_add_event_cb(bs, bt_scan_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bs, group_key_cb,     LV_EVENT_KEY,     NULL);

    /* Everything above (toggle + Scan) is static across a device refresh; the
     * tail bt_fill_devices adds is the dynamic part bt_refresh_devices rebuilds. */
    s_bt_list = list;
    s_bt_static_n = lv_obj_get_child_count(list);
    bt_fill_devices(list);

    lv_group_focus_obj(bt);
    ui_bt_set_changed_cb(bt_changed_cb);
    return scr;
}
