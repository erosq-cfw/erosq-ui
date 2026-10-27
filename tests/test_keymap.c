#include <assert.h>
#include <string.h>
#include "ui_input.h"

/* ui_input.c references ui_power_off(), the blank-state hooks
 * ui_blank_active/ui_blank_wake, and the nav hooks ui_goto/scr_poweroff
 * (long-press routes to the shutdown-confirm screen); this test only exercises
 * the pure ui_keymap() translator, so stub those symbols to link. */
void ui_power_off(void) {}
int  ui_blank_active(void) { return 0; }
void ui_blank_wake(int is_power_key) { (void)is_power_key; }
void ui_blank_power_tap(void) {}
void ui_goto(lv_obj_t *(*builder)(void)) { (void)builder; }
lv_obj_t *scr_poweroff(void) { return 0; }
/* ui_avrcp_poll()/ui_jack_poll() call mpd_cmd(); ui_jack_poll() also calls
 * ui_bt_audio_active(). Stub both so ui_input.c links here. */
int mpd_cmd(const char *cmd, char *reply, int rlen) { (void)cmd; (void)reply; (void)rlen; return 0; }
/* keypad_read's blanked path now calls ui_volume() (ui_screens.c) directly. */
void ui_volume(int delta) { (void)delta; }
int ui_bt_audio_active(void) { return 0; }
/* settings-batch1: the blanked path reads Screen-Off Buttons, ui_jack_poll()
 * reads Pause on Disconnect (both ui_power.c). Defaults: All / On. */
int ui_power_blkeys_get(void) { return 0; }
int ui_power_discpause_get(void) { return 1; }
int ui_power_btbuttons_get(void) { return 1; }   /* ui_avrcp_poll: Bluetooth Buttons On */
void ui_error_popup(const char *msg, const char *hint) { (void)msg; (void)hint; }   /* ui_media_cmd: SD gone */

int main(void)
{
    assert(ui_keymap(165) == UI_KEY_PREVTRACK); /* KEY_PREVIOUSSONG -> track skip (wheel does nav) */
    assert(ui_keymap(163) == UI_KEY_NEXTTRACK); /* KEY_NEXTSONG -> track skip */
    assert(ui_keymap(164) == LV_KEY_ENTER);  /* KEY_PLAYPAUSE */
    assert(ui_keymap(158) == LV_KEY_ESC);    /* KEY_BACK */
    assert(ui_keymap(139) == UI_KEY_HOME);   /* KEY_MENU */
    assert(ui_keymap(115) == UI_KEY_VOLUP);
    assert(ui_keymap(114) == UI_KEY_VOLDOWN);
    assert(ui_keymap(116) == UI_KEY_POWER);
    assert(ui_keymap(999) == 0);

    /* BT headphone AVRCP passthrough -> MPD (bluetoothd emits CD-style codes). */
    assert(strcmp(ui_avrcp_cmd(200), "pause") == 0);    /* KEY_PLAYCD  -> toggle */
    assert(strcmp(ui_avrcp_cmd(201), "pause") == 0);    /* KEY_PAUSECD -> toggle */
    assert(strcmp(ui_avrcp_cmd(164), "pause") == 0);    /* KEY_PLAYPAUSE */
    assert(strcmp(ui_avrcp_cmd(163), "next") == 0);     /* KEY_NEXTSONG */
    assert(strcmp(ui_avrcp_cmd(165), "previous") == 0); /* KEY_PREVIOUSSONG */
    assert(strcmp(ui_avrcp_cmd(166), "stop") == 0);     /* KEY_STOPCD */
    assert(ui_avrcp_cmd(999) == 0);

    /* Auto-pause: fire only on the edge to all-jacks-empty, never on BT.
     * args: (hp_in, lo_in, bt_active, prev_all_empty) */
    assert(ui_jack_should_pause(0, 0, 0, 0) == 1);  /* last plug pulled -> pause */
    assert(ui_jack_should_pause(1, 0, 0, 0) == 0);  /* HP still in -> no pause */
    assert(ui_jack_should_pause(0, 1, 0, 0) == 0);  /* line-out still in -> no pause */
    assert(ui_jack_should_pause(0, 0, 0, 1) == 0);  /* already empty (no edge) -> no pause */
    assert(ui_jack_should_pause(0, 0, 1, 0) == 0);  /* BT is the sink -> suppressed */
    assert(ui_jack_should_pause(1, 1, 0, 0) == 0);  /* both in -> no pause */
    /* The requirement: pull one of two, still playing; then pull the last -> pause. */
    assert(ui_jack_should_pause(1, 0, 0, 0) == 0);  /* two -> one (HP left) : no pause */
    assert(ui_jack_should_pause(0, 0, 0, 0) == 1);  /* one -> zero            : pause */

    /* BT sink dropped: MPD falls back to the wired DAC. args: (hp_in, lo_in,
     * open_failed = status carried "Failed to open audio output", i.e. the drop
     * hit mid-play). HW 2026-10-05: the old unconditional clearerror+play ran
     * playback on into the empty jack, 73:01 -> 73:15 with nothing audible. */
    assert(strcmp(ui_bt_drop_cmd(0, 0, 1), "clearerror\npause 1\n") == 0); /* mid-play, nothing plugged */
    assert(strcmp(ui_bt_drop_cmd(0, 0, 0), "clearerror\npause 1\n") == 0); /* MPD hasn't failed yet: still pause */
    assert(strcmp(ui_bt_drop_cmd(1, 0, 1), "clearerror\nplay\n") == 0);    /* headphones in: carry on */
    assert(strcmp(ui_bt_drop_cmd(0, 1, 1), "clearerror\nplay\n") == 0);    /* line-out in: carry on */
    assert(ui_bt_drop_cmd(1, 1, 0) == 0);                                   /* paused drop, jack in: leave it */
    assert(ui_bt_drop_cmd(1, 0, 0) == 0);

    /* Keys that ACT while blanked instead of waking the screen. The two mappers
     * must be disjoint and must claim ONLY these five keys -- anything else has
     * to fall through to the wake path, or buttons stop waking the screen. */
    assert(ui_blanked_vol_step(UI_KEY_VOLUP)   == +UI_VOL_STEP);
    assert(ui_blanked_vol_step(UI_KEY_VOLDOWN) == -UI_VOL_STEP);
    assert(ui_blanked_vol_step(UI_KEY_POWER)   == 0);   /* POWER: handled earlier */
    assert(ui_blanked_vol_step(UI_KEY_HOME)    == 0);   /* must still wake */
    assert(ui_blanked_vol_step(LV_KEY_ESC)     == 0);   /* must still wake */
    assert(ui_blanked_vol_step(0)              == 0);   /* no release edge */

    assert(strcmp(ui_blanked_media_cmd(UI_KEY_PREVTRACK), "previous") == 0);
    assert(strcmp(ui_blanked_media_cmd(UI_KEY_NEXTTRACK), "next")     == 0);
    assert(strcmp(ui_blanked_media_cmd(LV_KEY_ENTER),     "pause")    == 0);
    assert(ui_blanked_media_cmd(UI_KEY_HOME)   == 0);   /* must still wake */
    assert(ui_blanked_media_cmd(LV_KEY_ESC)    == 0);   /* must still wake */
    assert(ui_blanked_media_cmd(UI_KEY_POWER)  == 0);   /* POWER: handled earlier */
    assert(ui_blanked_media_cmd(UI_KEY_VOLUP)  == 0);   /* volume is the other mapper */
    assert(ui_blanked_media_cmd(0)             == 0);   /* no release edge */

    /* The blanked commands must match what the BT remote already sends, so a
     * headset button and a body button do the same thing. */
    assert(strcmp(ui_blanked_media_cmd(UI_KEY_NEXTTRACK), ui_avrcp_cmd(163)) == 0);
    assert(strcmp(ui_blanked_media_cmd(UI_KEY_PREVTRACK), ui_avrcp_cmd(165)) == 0);
    assert(strcmp(ui_blanked_media_cmd(LV_KEY_ENTER),     ui_avrcp_cmd(164)) == 0);
    return 0;
}
