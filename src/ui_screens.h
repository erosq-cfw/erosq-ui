#ifndef EROSQ_UI_SCREENS_H
#define EROSQ_UI_SCREENS_H
#include "lvgl.h"
#include "ui.h"

extern struct ui_ctx *g_ctx;

void ui_build(struct ui_ctx *ctx);
void ui_goto(lv_obj_t *(*builder)(void));
void ui_back(void);
void ui_home(void);
void ui_focus_add(lv_obj_t *item);
void ui_screen_keysink(lv_obj_t *scr);  /* display-only screens: body catches ESC/HOME */
/* Empty-state message + optional hint (NULL = none); also installs the keysink. */
void ui_empty_state(lv_obj_t *scr, const char *msg, const char *hint);
void ui_volume(int delta);              /* nudge volume by delta %, with overlay */
void ui_volume_sync(void);              /* seed overlay value from the hardware mixer */
void ui_volume_boot(void);              /* startup: restore the saved wired level (capped) */
void ui_volume_persist(void);           /* save the wired level to /data if it changed */
void ui_volume_enforce_limit(void);     /* pull DAC + BT down to Volume Limit */
void ui_brightness_restore(void);       /* startup: restore the saved backlight level */
void ui_brightness_persist(void);       /* save the current backlight level if changed */
void ui_volume_set_from_bt(int pct);    /* set overlay from a BT-reported volume; no push-back */

/* Shared list-row key handler: ESC=back, HOME=home, transport + volume keys.
 * Exported so ui_podcast.c's rows behave identically to every other list row. */
void group_key_cb(lv_event_t *e);

/* Destructive-action confirm: question + centred `action` / `cancel` (focused)
 * buttons; danger=1 draws the action button red. on_confirm gets the question
 * label as user data; cancel = ui_back. */
lv_obj_t *ui_confirm_screen(const char *title, const char *question,
                            const char *action, const char *cancel,
                            lv_event_cb_t on_confirm, int danger);

lv_obj_t *scr_podcasts(void);      /* top-level: podcast feeds */

lv_obj_t *scr_menu(void);          /* root */
/* forward decls for menu targets (defined across Tasks 5,7,8) */
lv_obj_t *scr_now_playing(void);
lv_obj_t *scr_music(void);         /* iPod "Music" submenu (incl. Folders) */
lv_obj_t *scr_extras(void);
lv_obj_t *scr_settings(void);
lv_obj_t *scr_about(void);
lv_obj_t *scr_network(void);
lv_obj_t *scr_playback(void);   /* Settings > Playback submenu */
lv_obj_t *scr_library(void);    /* Settings > Library submenu */
lv_obj_t *scr_system(void);     /* Settings > System submenu */
lv_obj_t *scr_usb_transfer(void);  /* USB file-transfer mode (mass-storage) */
lv_obj_t *scr_wifi(void);
lv_obj_t *scr_bluetooth(void);
/* Backlight blank state, queried/driven by the input layer (ui_input.c) so a
 * wake press is consumed at the source rather than acting on a widget. */
int  ui_blank_active(void);
void ui_blank_wake(int is_power_key);
void ui_error_popup(const char *msg, const char *hint);   /* replaces any current one */
void ui_blank_power_tap(void);   /* POWER tap: blank<->wake toggle */
void ui_blank_force_wake(void);  /* wake regardless of policy (critical notices) */

lv_obj_t *scr_display(void);    /* Display submenu: brightness + backlight */
lv_obj_t *scr_brightness(void);
lv_obj_t *scr_bltimeout(void);  /* backlight timeout selector */
lv_obj_t *scr_blwake(void);     /* backlight wake mode selector */
lv_obj_t *scr_blkeys(void);     /* Buttons When Off selector */
lv_obj_t *scr_repeat(void);     /* Repeat Off/All/One (MPD repeat+single) */
lv_obj_t *scr_vollimit(void);   /* Volume Limit selector */
lv_obj_t *scr_discpause(void);  /* Disconnect Pause On/Off */
lv_obj_t *scr_btbuttons(void);  /* Bluetooth Buttons On/Off */
lv_obj_t *scr_refresh(void);    /* display refresh rate selector */
lv_obj_t *scr_theme(void);      /* Rockbox theme selector (colours + backdrop) */
lv_obj_t *scr_power(void);      /* Power submenu: auto power-off + power off */
lv_obj_t *scr_poweroff(void);   /* confirm-shutdown screen */
lv_obj_t *scr_autooff(void);    /* auto power-off timeout selector */
lv_obj_t *scr_plshuffle(void);  /* Shuffle Playlists on/off */
lv_obj_t *scr_critbatt(void);   /* low-battery power-off threshold selector */
lv_obj_t *scr_chglimit(void);   /* charge ceiling selector (stop charging at N%) */
lv_obj_t *scr_datetime(void);   /* Date & Time submenu: timezone + clock format */
lv_obj_t *scr_timezone(void);   /* timezone selector (Casio city list) */
lv_obj_t *scr_clockfmt(void);   /* clock format selector */
lv_obj_t *scr_update(void);     /* SD firmware update: scan/confirm/swupdate/reboot */
#endif
