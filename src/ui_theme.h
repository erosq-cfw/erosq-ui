#ifndef EROSQ_UI_THEME_H
#define EROSQ_UI_THEME_H
#include "lvgl.h"
#include "theme_logic.h"
/* iPod-Video palette. White list, black text; glossy blue gradients for the
 * title bar / selection live in ui_theme.c. UI_COL_ACCENT is the flat iPod blue
 * used by the transient toast + volume-overlay border. */
/* Runtime, so a Rockbox theme (Settings > Display > Theme) can recolour them;
 * the iPod values are the defaults. */
extern lv_color_t ui_col_bg, ui_col_accent, ui_col_text, ui_col_on_accent;
#define UI_COL_BG        ui_col_bg          /* white list background */
#define UI_COL_ACCENT    ui_col_accent      /* iPod blue */
#define UI_COL_TEXT      ui_col_text        /* black foreground */
#define UI_COL_ON_ACCENT ui_col_on_accent   /* white, text on the accent */
/* Status-bar geometry: theme_logic.h THEME_* (title FIXED 128 px). The
 * status font's battery / clock / play widths are measured when a theme font
 * loads (ui_theme.c status_font); with the iPod font they are battery 18 px
 * (every LV_SYMBOL_BATTERY_*), "12:00 PM" 61 px (widest clock over all formats
 * and minutes), play/pause 12 px: the right cluster then exactly meets the
 * title's 6 px gap. */
#define UI_TITLE_W THEME_TITLE_W
int ui_theme_clock_right(void);   /* clock's right offset for the active status font */
/* Fonts: the iPod set, or the active theme's (theme.txt font*), like UI_COL_*. */
extern const lv_font_t *ui_font, *ui_font_bold, *ui_font_large;
#define UI_FONT       ui_font         /* body, 14 px iPod */
#define UI_FONT_BOLD  ui_font_bold    /* title bar, headings */
#define UI_FONT_LARGE ui_font_large   /* clocks, big numbers */
unsigned ui_theme_font_gen(void);  /* bumps when a font changes: screens must be rebuilt */
void ui_theme_init(void);
/* Screen kinds: which side-panel sprite a themed screen shows (theme_logic.h
 * THEME_SPR_*), or FULL = no panel, full-width body (Now Playing, keyboards). */
enum { UI_SCR_DEFAULT = THEME_SPR_DEFAULT, UI_SCR_HOME = THEME_SPR_HOME,
       UI_SCR_MUSIC = THEME_SPR_MUSIC, UI_SCR_BT = THEME_SPR_BT, UI_SCR_FULL = THEME_SPR_N };
lv_obj_t *ui_screen_base(const char *title);                 /* UI_SCR_DEFAULT */
lv_obj_t *ui_screen_base_kind(const char *title, int kind);
lv_obj_t *ui_screen_list(lv_obj_t *scr);   /* lv_list filling the body */
int ui_screen_body_w(lv_obj_t *scr);       /* 320, or less beside a theme's side panel */
int ui_theme_panel_w(void);                /* active side panel width, 0 = none */
int ui_theme_panel_shown(void);
int ui_theme_row_dividers(void);           /* 1 = hairline under list rows (iPod), 0 = theme row_dividers = off */            /* panel on screen now (not a FULL screen, not hidden) */
void ui_theme_panel_clock(const char *time, const char *date);   /* 1 s tick; no-op without a panel */
lv_obj_t *ui_screen_body(lv_obj_t *screen);
lv_group_t *ui_screen_group(lv_obj_t *scr);
void ui_style_scroll_thumb(lv_obj_t *o);  /* a list scrollbar's look, for a standalone thumb */
void ui_apply_focus_style(lv_obj_t *o);   /* accent highlight on LV_STATE_FOCUSED */
void ui_style_scrubber(lv_obj_t *bar);    /* glossy-blue iPod position bar */
void ui_style_danger(lv_obj_t *btn);      /* red destructive-action button */
void ui_style_bar_text(lv_obj_t *o);      /* status-bar text over the title bar */
void ui_style_surface(lv_obj_t *o);       /* screen background (transparent over a backdrop) */
void ui_style_art_placeholder(lv_obj_t *tile, lv_obj_t *note);   /* missing-cover tile colours */
/* Themes by id, "" = the iPod look (formats: THEMES.md):
 *   "erosq:<name>"   = /media/sd/themes/<name>/theme.txt
 *   "rockbox:<name>" = /media/sd/.rockbox/themes/<name>.cfg */
#include "theme_logic.h"
void ui_theme_load(lv_display_t *d);      /* saved choice + LVGL default theme; before ui_build */
const char *ui_theme_current(void);
int  ui_theme_select(const char *id);     /* applies live + persists; -1 = unreadable, unchanged */
int  ui_theme_list(char (*ids)[THEME_ID_MAX], int max);   /* sorted by name */
#endif
