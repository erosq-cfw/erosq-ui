#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/input.h>
#include "ui_input.h"
#include "ui_power.h"
#include "ui_settings_logic.h" /* Screen-Off Buttons mode */
#include "ui_screens.h"   /* ui_blank_active / ui_blank_wake: gate input while blanked */
#include "ui_mpd.h"       /* mpd_cmd: AVRCP media buttons drive MPD directly */
#include "ui_bt.h"        /* ui_bt_audio_active: don't auto-pause when BT is the sink */
#include "ui_wake_logic.h" /* time-based lazy-open rescans */

/* Lazy-open rescans for nodes that come and go (AVRCP, jacks, BT pad): about
 * once a second by the clock, not by main-loop iterations -- the loop sleeps
 * far longer than 16 ms while the screen is blanked (main.c). */
#define UI_RESCAN_MS 1000

/* Resolve an evdev node by device name via /proc/bus/input/devices, so a kernel
 * probe-order change (e.g. gpio-keys going interrupt-driven) can't swap us onto
 * the wrong eventN. Writes "/dev/input/eventN" into out; returns 0 on success. */
int ui_evdev_by_name(const char *name, char *out, size_t n)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return -1;
    char line[256]; int match = 0; int rc = -1;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'N') match = (strstr(line, name) != NULL);
        else if (match && line[0] == 'H') {
            char *ev = strstr(line, "event");
            if (ev) { int num = atoi(ev + 5);
                      snprintf(out, n, "/dev/input/event%d", num); rc = 0; break; }
        }
    }
    fclose(f);
    return rc;
}

uint32_t ui_keymap(int evcode)
{
    switch (evcode) {
    case 165: return UI_KEY_PREVTRACK; /* KEY_PREVIOUSSONG -> skip track (wheel does focus nav) */
    case 163: return UI_KEY_NEXTTRACK; /* KEY_NEXTSONG -> skip track */
    case 164: return LV_KEY_ENTER;   /* KEY_PLAYPAUSE */
    case 158: return LV_KEY_ESC;     /* KEY_BACK */
    case 139: return UI_KEY_HOME;    /* KEY_MENU */
    case 115: return UI_KEY_VOLUP;
    case 114: return UI_KEY_VOLDOWN;
    case 116: return UI_KEY_POWER;
    default:  return 0;
    }
}

/* --- Keys that act while blanked ------------------------------------------
 * The screen sleeps during pocket playback, so the transport controls must work
 * blind rather than spend the press waking a panel nobody is looking at. Pure
 * mappings, host-tested; the caller applies them and deliberately skips the wake.
 * ENTER is the physical Play button (ui_keymap: evcode 164 KEY_PLAYPAUSE). It is
 * only ambiguous when awake -- there ENTER also means "click the focused row",
 * which is why np_key_cb is screen-local -- but blanked nothing is focused and
 * no widget may be clicked, so here it unambiguously means play/pause. */
int ui_blanked_vol_step(uint32_t key)
{
    if (key == UI_KEY_VOLUP)   return +UI_VOL_STEP;
    if (key == UI_KEY_VOLDOWN) return -UI_VOL_STEP;
    return 0;
}

/* Send a transport command; the play/pause toggle goes through mpd_play_cmd()
 * so it also STARTS a stopped player. */
#define SD_RESUME_PATH "/run/sd-resume"   /* written by sd-hotplug on card removal */
void ui_media_cmd(const char *cmd)
{
    char r[512], rec[32] = "", seek[96];
    /* A truncated reply (-2) still decides: MPD prints "state:" well before
     * "error:", and a decoder error naming the file twice pushes status past
     * 512 bytes -- treating that as "no reply" sent a bare pause, a no-op when
     * stopped (HITL 10-21 SD swap). */
    if (!strcmp(cmd, "pause")) {
        int fd = open(SD_RESUME_PATH, O_RDONLY);
        if (fd >= 0) {
            ssize_t k = read(fd, rec, sizeof rec - 1);
            rec[k > 0 ? k : 0] = 0;
            close(fd);
        }
        r[0] = 0;
        mpd_cmd("status", r, sizeof r);
        cmd = mpd_resume_cmd(r, rec[0] ? rec : NULL, seek, sizeof seek);
        if (cmd == seek) {
            /* card still out: playing now would fail every song and walk the
             * queue -- the very skip the record exists to undo */
            if (access("/media/sd/audio", F_OK) != 0) {
                ui_error_popup("SD card unavailable", "Reinsert the card, then press Play");
                return;
            }
            unlink(SD_RESUME_PATH);   /* one-shot */
        }
    }
    mpd_cmd(cmd, r, sizeof r);
}

const char *ui_blanked_media_cmd(uint32_t key)
{
    switch (key) {
    case UI_KEY_PREVTRACK: return "previous";
    case UI_KEY_NEXTTRACK: return "next";
    case LV_KEY_ENTER:     return "pause";   /* MPD "pause" with no arg = toggle */
    default:               return NULL;
    }
}

/* The indev is keypad type. We stash the fd + last event in user_data.
 * App pseudo-keys (HOME/VOL/POWER) are still delivered to the group as data->key;
 * ui_screens installs a group key handler that acts on them. */
struct evk { int fd; uint32_t key; lv_indev_state_t state; };
static struct evk *g_evk;		/* ui_input_flush needs the fd */

/* Drain events queued while the UI was paused in the extras launcher's
 * waitpid: buttons pressed INSIDE the child (e.g. Doom's quit-confirm)
 * otherwise replay into LVGL on resume and re-activate the still-focused
 * row -- the game instantly relaunches. Clear the latch too so a mid-press
 * state can't click through on the next read. */
void ui_input_flush(void)
{
    struct input_event ev;
    if (!g_evk || g_evk->fd < 0) return;
    while (read(g_evk->fd, &ev, sizeof ev) == sizeof ev)
        ;
    g_evk->key = 0;
    g_evk->state = LV_INDEV_STATE_RELEASED;
}

static int power_held;                  /* POWER currently down (ui_input_quiet) */

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t power_press_tick;   /* lv_tick at last POWER press */
    struct evk *e = lv_indev_get_user_data(indev);
    struct input_event ev;
    int power_released = 0;             /* POWER key-up edge this read */
    uint32_t released_key = 0;          /* any key-up edge this read (for wake) */
    if (e->fd >= 0) {
        ssize_t n;
        while ((n = read(e->fd, &ev, sizeof ev)) == sizeof ev) {
            if (ev.type != EV_KEY) continue;
            if (ev.code == 116) {       /* KEY_POWER: hold=shutdown, tap=blank/wake */
                if (ev.value) { power_press_tick = lv_tick_get(); power_held = 1; }
                else          { power_held = 0; power_released = 1; }
            }
            uint32_t k = ui_keymap(ev.code);
            if (!k) continue;
            e->key = k;
            e->state = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            if (!ev.value) released_key = k;   /* key-up edge */
            /* Deliver ONE key edge per read: draining the whole batch latched
             * only the last event, so a press+release landing in the same poll
             * lost the press. continue_reading makes LVGL call straight back
             * for the rest of the batch. */
            data->continue_reading = true;
            break;
        }
    }
    /* POWER held >= 1.5s -> shutdown CONFIRM screen (not an immediate poweroff:
     * a 1.5s press in a pocket/bag shouldn't kill the device). Wake the backlight
     * first so the confirm is visible if we were blanked. Fires once (power_held
     * cleared); the later release is a no-op (elaps >= 1500 skips the tap path). */
    if (power_held && lv_tick_elaps(power_press_tick) >= 1500) {
        power_held = 0;
        if (ui_blank_active()) ui_blank_wake(1);
        ui_goto(scr_poweroff);
        e->key = 0;       e->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;    data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    /* POWER short tap -> toggle backlight (blank when on, wake when blanked).
     * Handled as one unit on RELEASE so the press doesn't blank and then the
     * release immediately wake. Never delivered to a widget. */
    if (power_released && lv_tick_elaps(power_press_tick) < 1500) {
        ui_blank_power_tap();
        e->key = 0;       e->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;    data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    /* Screen blanked: swallow the ENTIRE keypress (press + release) so it never
     * reaches a widget, and wake on the RELEASE edge -- waking on press would
     * unblank mid-press and the latched press would then click through on the
     * next read. POWER is handled above; other keys wake per the policy. */
    if (ui_blank_active()) {
        /* Display > Screen-Off Buttons: a key the mode doesn't allow falls
         * through to the wake policy below, like any other key. */
        int mode = ui_power_blkeys_get();
        int vstep = ui_blkeys_volume(mode) ? ui_blanked_vol_step(released_key) : 0;
        const char *mcmd = (vstep || !ui_blkeys_media(mode)) ? NULL : ui_blanked_media_cmd(released_key);
        if (vstep) {
            ui_volume(vstep);            /* act, stay dark */
        } else if (mcmd) {
            ui_media_cmd(mcmd);
        } else if (released_key && released_key != UI_KEY_POWER) {
            ui_blank_wake(0);            /* everything else still wakes */
        }
        e->key = 0;       e->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;    data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    /* Awake: POWER never reaches a widget (handled above); deliver other keys. */
    if (e->key == UI_KEY_POWER) { e->key = 0; e->state = LV_INDEV_STATE_RELEASED; }
    data->key = e->key;
    data->state = e->state;
}

void ui_input_init(struct ui_ctx *ctx, const char *evdev_path)
{
    static struct evk e = { .fd = -1 };   /* -1 sentinel: first call must not close fd 0 */
    if (e.fd >= 0) close(e.fd);   /* close a prior fd on re-init (launcher reacquire) */
    e.fd = open(evdev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (e.fd < 0) fprintf(stderr, "erosq-ui: no input %s (%s)\n", evdev_path, strerror(errno));
    e.key = 0;                    /* clear stale latch so re-init can't inject a phantom key */
    e.state = LV_INDEV_STATE_RELEASED;
    g_evk = &e;

    if (ctx->group) lv_group_delete(ctx->group);   /* free prior indev group on re-init */
    ctx->group = lv_group_create();
    /* Deliberately NOT a default group: lv_button/textarea have group_def=TRUE, so
     * a default group auto-adds every freshly-created row to it. Each add focuses
     * the sole member -> LV_EVENT_FOCUSED -> scroll-to-view, and since ui_focus_add
     * then moves the row to its per-screen group, the LAST row built wins and scrolls
     * long lists (Artists/Albums/Songs/Genres) to the bottom on open. Every focusable
     * widget is added to its per-screen group explicitly (ui_focus_add), so no auto-add
     * is needed. set_active_group() puts the right group on the indevs. */

    ctx->indev = lv_indev_create();
    lv_indev_set_type(ctx->indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(ctx->indev, keypad_read);
    lv_indev_set_user_data(ctx->indev, &e);
    lv_indev_set_group(ctx->indev, ctx->group);
}

/* Headphone media buttons. When BT headphones connect, bluetoothd (AVRCP target)
 * injects passthrough commands as key events on a uinput node named "<name> (AVRCP)"
 * that appears on connect and vanishes on disconnect. We drive MPD straight from
 * these -- deliberately NOT through the LVGL keypad/group -- so play/pause/next/prev
 * always control playback regardless of what the UI has focused. BlueZ emits the
 * CD-style codes (avctp.c key_map): PLAYCD/PAUSECD/STOPCD + NEXT/PREVIOUSSONG.
 * Call once per main-loop tick; no-ops on host (no node, no mpd socket). */

/* Pure map: AVRCP passthrough keycode -> MPD command, or NULL to ignore.
 * play/pause is a toggle (mpd "pause"), matching the UI's center key. */
const char *ui_avrcp_cmd(int evcode)
{
    switch (evcode) {
    case 200: case 201: case 164: return "pause";    /* PLAYCD/PAUSECD/PLAYPAUSE */
    case 163: return "next";                          /* KEY_NEXTSONG */
    case 165: return "previous";                      /* KEY_PREVIOUSSONG */
    case 166: return "stop";                          /* KEY_STOPCD */
    default:  return 0;
    }
}

static int s_avrcp_fd = -1;
static struct ui_rescan s_avrcp_scan;   /* throttle the /proc scan while the node is absent */

void ui_avrcp_poll(void)
{
    struct input_event ev;
    char reply[64];                  /* mpd_cmd writes into this unconditionally */

    if (s_avrcp_fd < 0) {
        if (!ui_rescan_due(&s_avrcp_scan, lv_tick_get(), UI_RESCAN_MS)) return;
        char path[32];
        if (ui_evdev_by_name(" (AVRCP)", path, sizeof path)) return;
        s_avrcp_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (s_avrcp_fd < 0) return;
    }

    ssize_t n;
    while ((n = read(s_avrcp_fd, &ev, sizeof ev)) == sizeof ev) {
        if (ev.type != EV_KEY || ev.value != 1) continue;   /* key-down edges only */
        /* Bluetooth Buttons = Off: still drain the node (it is in the blanked
         * poll() set; unread events would wake the loop forever), just don't act. */
        const char *cmd = ui_btbuttons_filter(ui_power_btbuttons_get(), ui_avrcp_cmd(ev.code));
        if (cmd) ui_media_cmd(cmd);
    }
    /* -1/EAGAIN just means drained; any other error is the node going away on
     * headphone disconnect -- drop the fd so we reopen when they reconnect. */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(s_avrcp_fd); s_avrcp_fd = -1; }
}

/* --- Auto-pause on wired-headphone removal (BT-gated) -----------------------
 * The kernel publishes each wired jack as an EV_SW input node ("Headphone Jack",
 * "Line Out Jack"; SW_HEADPHONE_INSERT / SW_LINEOUT_INSERT, value 1 = plugged).
 * When the LAST wired plug is pulled we force-pause MPD -- but only when
 * Bluetooth is NOT the sink (on BT the jacks are empty anyway, and pausing would
 * stop BT playback). Mirrors the kernel's mute-on-all-unplugged policy: pulling
 * one plug of two keeps playing. "pause 1" is forced (never toggles), and we
 * don't auto-resume on replug. Like ui_avrcp_poll this bypasses LVGL and drives
 * MPD directly; call once per main-loop tick. */

/* Read the current level of EV_SW `code` on an evdev fd (0 if unknown). Byte
 * buffer sized to SW_MAX so EVIOCGSW works regardless of long width. */
static int jack_sw(int fd, int code)
{
    unsigned char bits[SW_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
    return (bits[code / 8] >> (code % 8)) & 1;
}

/* Pure: issue MPD "pause 1" this tick?  Fire on the transition to "no wired jack
 * occupied" (rising edge of all-empty), suppressed while BT is the active sink.
 * prev_all_empty is last tick's all-empty state (seeded from the initial read so
 * booting with nothing plugged doesn't pause). */
int ui_jack_should_pause(int hp_in, int lo_in, int bt_active, int prev_all_empty)
{
    int all_empty = !hp_in && !lo_in;
    return all_empty && !prev_all_empty && !bt_active;
}

/* Pure: MPD command for a BT-sink drop (MPD now on the wired DAC), or NULL.
 * No jack occupied -> pause, else playback runs on silently into the empty jack.
 * Jack occupied and MPD was stopped by the failed BT output -> carry on wired.
 * Always clear MPD's sticky error so a normal disconnect doesn't raise the
 * Now Playing error popup. */
const char *ui_bt_drop_cmd(int hp_in, int lo_in, int open_failed)
{
    if (!hp_in && !lo_in) return "clearerror\npause 1\n";
    return open_failed ? "clearerror\nplay\n" : 0;
}

static int hp_fd = -1, lo_fd = -1;
static struct ui_rescan s_jack_scan;

/* Current plug state straight from the jack nodes (EVIOCGSW), not the per-tick
 * cache: the BT drop handler runs off the D-Bus path. Unopened node reads as
 * empty, which errs towards pausing. */
void ui_jack_state(int *hp_in, int *lo_in)
{
    *hp_in = hp_fd >= 0 && jack_sw(hp_fd, SW_HEADPHONE_INSERT);
    *lo_in = lo_fd >= 0 && jack_sw(lo_fd, SW_LINEOUT_INSERT);
}

void ui_jack_poll(void)
{
    static int hp_in, lo_in;         /* last-known plug state (1 = inserted) */
    static int prev_all_empty = 1;
    static int seeded;               /* both fds open and initial state read */
    struct input_event ev;
    char reply[64];                  /* mpd_cmd writes into this unconditionally */

    /* Lazy-open both jack nodes; rescan ~1/s until both are present. The ASoC
     * jacks persist for the card's life, so this normally settles once at boot. */
    if (hp_fd < 0 || lo_fd < 0) {
        if (!ui_rescan_due(&s_jack_scan, lv_tick_get(), UI_RESCAN_MS)) return;
        char path[32];
        if (hp_fd < 0 && ui_evdev_by_name("Headphone Jack", path, sizeof path) == 0)
            hp_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (lo_fd < 0 && ui_evdev_by_name("Line Out Jack", path, sizeof path) == 0)
            lo_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (hp_fd < 0 || lo_fd < 0) return;
        /* Both open: seed from current state so we don't fire a phantom edge. */
        hp_in = jack_sw(hp_fd, SW_HEADPHONE_INSERT);
        lo_in = jack_sw(lo_fd, SW_LINEOUT_INSERT);
        prev_all_empty = !hp_in && !lo_in;
        seeded = 1;
    }

    ssize_t n;
    while ((n = read(hp_fd, &ev, sizeof ev)) == sizeof ev)
        if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) hp_in = ev.value;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(hp_fd); hp_fd = -1; seeded = 0; }

    while ((n = read(lo_fd, &ev, sizeof ev)) == sizeof ev)
        if (ev.type == EV_SW && ev.code == SW_LINEOUT_INSERT) lo_in = ev.value;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(lo_fd); lo_fd = -1; seeded = 0; }

    if (!seeded) return;             /* a node vanished mid-tick; reopen+reseed next */

    if (ui_power_discpause_get() &&       /* Settings > Pause on Disconnect */
        ui_jack_should_pause(hp_in, lo_in, ui_bt_audio_active(), prev_all_empty))
        mpd_cmd("pause 1", reply, sizeof reply);
    prev_all_empty = !hp_in && !lo_in;
}

/* Scroll wheel = rotary-encoder evdev (REL_X +1/-1). An LVGL ENCODER indev:
 * enc_diff (accumulated steps since last read) moves focus natively. No button. */
struct encdev { int fd; int diff; };

/* Screen-local wheel override (Now Playing scrub). Returns nonzero if it
 * consumed the detents; they then never reach LVGL focus nav. */
static int (*s_wheel_hook)(int diff);
void ui_wheel_set_hook(int (*fn)(int diff)) { s_wheel_hook = fn; }

static void wheel_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    struct encdev *w = lv_indev_get_user_data(indev);
    struct input_event ev;
    if (w->fd >= 0) {
        while (read(w->fd, &ev, sizeof ev) == sizeof ev) {
            if (ev.type == EV_REL && ev.code == REL_X)
                w->diff += ev.value;
        }
    }
    /* Screen blanked: wheel motion only (maybe) wakes it (wheel = non-power) and
     * is swallowed so it never moves focus behind a dark screen / on wake. */
    if (ui_blank_active()) {
        if (w->diff) ui_blank_wake(0);
        w->diff = 0;
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (w->diff && s_wheel_hook && s_wheel_hook(w->diff)) w->diff = 0;
    data->enc_diff = w->diff;
    w->diff = 0;
    data->state = LV_INDEV_STATE_RELEASED;   /* no wheel click */
}

static struct encdev s_wheel = { .fd = -1 };   /* file scope: ui_input_pollfds needs the fd */

void ui_wheel_init(struct ui_ctx *ctx, const char *evdev_path)
{
    if (s_wheel.fd >= 0) close(s_wheel.fd);
    s_wheel.fd = open(evdev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_wheel.fd < 0) fprintf(stderr, "erosq-ui: no wheel %s (%s)\n", evdev_path, strerror(errno));
    s_wheel.diff = 0;

    ctx->enc = lv_indev_create();
    lv_indev_set_type(ctx->enc, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(ctx->enc, wheel_read);
    lv_indev_set_user_data(ctx->enc, &s_wheel);
    lv_indev_set_group(ctx->enc, ctx->group);
}

/* --- BT gamepad (8bitdo, HoG/uhid) as a second keypad indev -----------------
 * Unlike gpio-keys/scrollwheel (always present), the pad's evdev node appears
 * and vanishes with the BT connection, so we open it lazily BY NAME from the
 * read cb (scan ~1/s while absent; drop the fd on read error so a reconnect
 * reattaches). Maps the d-pad to focus nav (PREV/NEXT/LEFT/RIGHT) and A/B/START
 * to activate/back/home. Shares ctx->group; set_active_group re-points it. */
#define UI_GAMEPAD_NAME "8BitDo"

static uint32_t ui_gamepad_map_key(int evcode)
{
    switch (evcode) {
    case 304: return LV_KEY_ENTER;   /* A     -> activate */
    case 305: return LV_KEY_ESC;     /* B     -> back     */
    case 315: return UI_KEY_HOME;    /* START -> home     */
    default:  return 0;
    }
}

struct gpad { int fd; uint32_t key; lv_indev_state_t state; struct ui_rescan scan; };
static struct gpad s_pad = { .fd = -1 };   /* file scope: blanked rescan + pollfds */

/* Lazy (re)open: the pad comes/goes with BT. Scan ~1/s while absent. Called from
 * the read cb (awake) and ui_input_blanked_rescan (read cbs paused while blanked). */
static void gamepad_try_open(struct gpad *g)
{
    if (g->fd >= 0 || !ui_rescan_due(&g->scan, lv_tick_get(), UI_RESCAN_MS)) return;
    char path[32];
    if (ui_evdev_by_name(UI_GAMEPAD_NAME, path, sizeof path) == 0)
        g->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

static void ui_gamepad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    struct gpad *g = lv_indev_get_user_data(indev);
    struct input_event ev;
    uint32_t released_key = 0;

    gamepad_try_open(g);             /* lazy (re)open: pad comes/goes with BT */

    if (g->fd >= 0) {
        ssize_t n;
        while ((n = read(g->fd, &ev, sizeof ev)) == sizeof ev) {
            uint32_t k = 0; lv_indev_state_t st = LV_INDEV_STATE_RELEASED;
            if (ev.type == EV_KEY) {
                if (ev.value == 2) continue;             /* autorepeat */
                k = ui_gamepad_map_key(ev.code);
                if (!k) continue;
                st = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
                if (!ev.value) released_key = k;
            } else if (ev.type == EV_ABS && ev.code == ABS_HAT0Y) {
                if (ev.value < 0)      { k = LV_KEY_PREV; st = LV_INDEV_STATE_PRESSED; }
                else if (ev.value > 0) { k = LV_KEY_NEXT; st = LV_INDEV_STATE_PRESSED; }
                else { k = g->key; released_key = k; }   /* centre -> release held */
            } else if (ev.type == EV_ABS && ev.code == ABS_HAT0X) {
                if (ev.value < 0)      { k = LV_KEY_LEFT;  st = LV_INDEV_STATE_PRESSED; }
                else if (ev.value > 0) { k = LV_KEY_RIGHT; st = LV_INDEV_STATE_PRESSED; }
                else { k = g->key; released_key = k; }
            } else continue;
            g->key = k; g->state = st;
            data->continue_reading = true;
            break;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(g->fd); g->fd = -1; }
    }

    /* Blanked: swallow input, wake on a release edge (mirrors keypad_read). */
    if (ui_blank_active()) {
        if (released_key) ui_blank_wake(0);
        g->key = 0; g->state = LV_INDEV_STATE_RELEASED;
        data->key = 0; data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->key = g->key;
    data->state = g->state;
}

void ui_gamepad_init(struct ui_ctx *ctx)
{
    ctx->pad = lv_indev_create();
    lv_indev_set_type(ctx->pad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(ctx->pad, ui_gamepad_read);
    lv_indev_set_user_data(ctx->pad, &s_pad);
    lv_indev_set_group(ctx->pad, ctx->group);
}

/* --- Blanked sleep support (main.c) ----------------------------------------
 * While the screen is blanked the main loop pauses the three indev read timers
 * and blocks in poll() on the input fds instead of waking every 16 ms to read
 * them. poll() never consumes evdev data: on any readable fd main.c resumes the
 * read timers (ready to run at once) and the unchanged read callbacks / polls
 * see the event exactly as before -- wake gate, blanked transport keys and the
 * POWER tap/long-press logic included. */

/* Input fds to sleep on: keys, wheel, BT pad, AVRCP, jacks (whichever are open). */
int ui_input_pollfds(struct pollfd *p, int max)
{
    int fds[6] = { g_evk ? g_evk->fd : -1, s_wheel.fd, s_pad.fd, s_avrcp_fd, hp_fd, lo_fd };
    int n = 0;
    for (int i = 0; i < 6 && n < max; i++)
        if (fds[i] >= 0) { p[n].fd = fds[i]; p[n].events = POLLIN; p[n].revents = 0; n++; }
    return n;
}

/* Run the pad's lazy-open while its read cb is paused (AVRCP/jacks rescan from
 * their own per-iteration polls, which keep running while blanked). */
void ui_input_blanked_rescan(void) { gamepad_try_open(&s_pad); }

/* ms until the next due lazy-open rescan of an absent node (UI_WAIT_NONE if all open). */
uint32_t ui_input_rescan_wait(void)
{
    uint32_t now = lv_tick_get(), t = UI_WAIT_NONE, x;
    if (s_avrcp_fd < 0 && (x = ui_rescan_wait(&s_avrcp_scan, now, UI_RESCAN_MS)) < t) t = x;
    if ((hp_fd < 0 || lo_fd < 0) && (x = ui_rescan_wait(&s_jack_scan, now, UI_RESCAN_MS)) < t) t = x;
    if (s_pad.fd < 0 && (x = ui_rescan_wait(&s_pad.scan, now, UI_RESCAN_MS)) < t) t = x;
    return t;
}

/* Nothing needs periodic reads: POWER held must keep the keypad timer running
 * (no event arrives during a hold; the 1.5 s long-press is timed by reads). */
int ui_input_quiet(void)
{
    return ui_input_idle(power_held, g_evk && g_evk->state == LV_INDEV_STATE_PRESSED,
                         s_pad.state == LV_INDEV_STATE_PRESSED);
}

/* Pause/resume the keypad, wheel and pad read timers. Resume marks them ready so
 * the very next lv_timer_handler() reads whatever woke poll(). */
void ui_input_reads_pause(struct ui_ctx *ctx, int pause)
{
    lv_indev_t *in[3] = { ctx->indev, ctx->enc, ctx->pad };
    for (int i = 0; i < 3; i++) {
        lv_timer_t *t = in[i] ? lv_indev_get_read_timer(in[i]) : NULL;
        if (!t) continue;
        if (pause) lv_timer_pause(t);
        else { lv_timer_resume(t); lv_timer_ready(t); }
    }
}
