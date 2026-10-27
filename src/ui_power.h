#ifndef EROSQ_UI_POWER_H
#define EROSQ_UI_POWER_H

#define UI_AUTOOFF_DEFAULT 15            /* minutes, used when no config file */
#define UI_BLTIMEOUT_DEFAULT 30          /* seconds, used when no config file */
#define UI_BLWAKE_DEFAULT    1           /* 0=power only, 1=auto, 2=any button */
#define UI_CHGLIMIT_OFF      100         /* percent; 100 = charge to full (default) */

void ui_power_off(void);                 /* stop MPD then poweroff; does not return */
int  ui_power_autooff_get(void);         /* minutes; 0 = disabled */
void ui_power_autooff_set(int minutes);  /* persist to /data + update cache */
void ui_power_autooff_init(void);        /* start the idle auto-off timer */
int  ui_power_bltimeout_get(void);       /* seconds; 0=off */
void ui_power_bltimeout_set(int secs);   /* persist to /data + update cache */
int  ui_power_blwake_get(void);          /* 0/1/2; 0=power only, 1=auto, 2=any button */
void ui_power_blwake_set(int mode);      /* persist to /data + update cache */
int  ui_power_refresh_get(void);         /* UI_REFRESH_* (ui_refresh_logic.h) */
void ui_power_refresh_set(int mode);     /* apply live + persist to /data */
void ui_power_refresh_apply(void);       /* set the display refresh timer from the saved mode */
int  ui_power_chglimit_get(void);        /* percent to stop charging at; 100 = off */
void ui_power_chglimit_set(int pct);     /* persist to /data + push to the kernel */
void ui_power_chglimit_apply(void);      /* re-push the saved limit at startup */
int  ui_power_critbatt_get(void);        /* critical-battery power-off percent; 0 = off */
void ui_power_critbatt_set(int pct);     /* persist to /data + update cache */
int  ui_power_critbatt_due(void);        /* one debounced gauge poll; 1 = power off now */
void ui_power_critbatt_warn(void);       /* 5 s notice, then power off unless charging */
int  ui_power_blkeys_get(void);          /* UI_BLKEYS_* (ui_settings_logic.h) */
void ui_power_blkeys_set(int mode);      /* persist to /data + update cache */
int  ui_power_vollimit_get(void);        /* max volume percent; 0 = no limit */
void ui_power_vollimit_set(int pct);     /* persist (caller re-applies the cap) */
int  ui_power_discpause_get(void);       /* 1 = pause when the audio output goes away */
void ui_power_discpause_set(int on);     /* persist to /data + update cache */
int  ui_power_btbuttons_get(void);       /* 1 = act on Bluetooth headphones/speaker buttons */
void ui_power_btbuttons_set(int on);     /* persist to /data + update cache */
int  ui_power_saved_volume(void);        /* last saved wired volume 0..100, -1 = none */
void ui_power_save_volume(int pct);      /* write only if changed */
int  ui_power_saved_brightness(void);    /* last saved backlight level 1..255, -1 = none */
void ui_power_save_brightness(int level);/* write only if changed */

#endif
