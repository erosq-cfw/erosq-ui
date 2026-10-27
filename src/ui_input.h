#ifndef EROSQ_UI_INPUT_H
#define EROSQ_UI_INPUT_H
#include "lvgl.h"
#include "ui.h"

#define UI_KEY_HOME      0x10001u
#define UI_KEY_VOLUP     0x10002u
#define UI_KEY_VOLDOWN   0x10003u
#define UI_KEY_POWER     0x10004u
#define UI_KEY_PREVTRACK 0x10005u   /* Prev button = skip track (was focus nav) */
#define UI_KEY_NEXTTRACK 0x10006u   /* Next button = skip track (was focus nav) */

/* Volume change for one button press. Lives here, with the key codes it is
 * keyed off, so both consumers see it: the awake path (group_key_cb in
 * ui_screens.c) and the blanked path (keypad_read below). Shared because this
 * value has already changed once (5% -> 2%) and the two must not drift. */
#define UI_VOL_STEP 2

uint32_t ui_keymap(int evcode);
const char *ui_avrcp_cmd(int evcode);   /* BT AVRCP keycode -> MPD command (NULL = ignore) */
/* Keys that ACT while the screen is blanked instead of waking it, so the device
 * works as a pocket player. Both pure (host-tested in test_keymap).
 *   ui_blanked_vol_step:  +/-UI_VOL_STEP for VOLUP/VOLDOWN, else 0
 *   ui_blanked_media_cmd: MPD command for Prev/Next/Play, else NULL
 * Mirrors ui_avrcp_cmd's shape: same command strings, NULL means "not mine". */
int         ui_blanked_vol_step(uint32_t key);
const char *ui_blanked_media_cmd(uint32_t key);
void ui_media_cmd(const char *cmd);   /* send it; "pause" also starts a stopped player */
int ui_evdev_by_name(const char *name, char *out, size_t n);  /* resolve eventN by device name */
void ui_input_init(struct ui_ctx *ctx, const char *evdev_path);
void ui_input_flush(void);   /* drain events queued while a launched child ran */
void ui_avrcp_poll(void);    /* poll BT headphone media buttons -> MPD (call per loop tick) */
int  ui_jack_should_pause(int hp_in, int lo_in, int bt_active, int prev_all_empty); /* pure */
void ui_jack_poll(void);     /* auto-pause MPD when wired jacks all unplugged (per tick) */
const char *ui_bt_drop_cmd(int hp_in, int lo_in, int open_failed);  /* pure: MPD cmd on BT-sink drop */
void ui_jack_state(int *hp_in, int *lo_in);         /* live wired-jack plug state */
void ui_wheel_init(struct ui_ctx *ctx, const char *evdev_path);  /* scroll-wheel encoder */
void ui_wheel_set_hook(int (*fn)(int diff));  /* screen-local wheel override; NULL clears */
void ui_gamepad_init(struct ui_ctx *ctx);                        /* BT gamepad keypad (lazy-opens by name) */
/* Blanked sleep (main.c): pause the indev read timers and poll() the input fds. */
struct pollfd;
int      ui_input_pollfds(struct pollfd *p, int max);  /* open input fds -> p[], returns count */
void     ui_input_blanked_rescan(void);   /* BT pad lazy-open while its read cb is paused */
uint32_t ui_input_rescan_wait(void);      /* ms to next absent-node rescan (UI_WAIT_NONE: none) */
int      ui_input_quiet(void);            /* 0 while POWER/a key/a pad button is held (needs reads) */
void     ui_input_reads_pause(struct ui_ctx *ctx, int pause);  /* resume also marks them ready */
#endif
