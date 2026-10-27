#include "ui_theme.h"
/* lv_theme_t is opaque in LVGL 9; a custom child theme needs the struct (same
 * include LVGL's own lv_example_style_14 uses). */
#include "src/themes/lv_theme_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop */
#include "rbtheme_logic.h"
#include "nativetheme_logic.h"
#include "binfont_logic.h"
#include "ui_clock.h"
#include "src/misc/lv_text_private.h"   /* lv_text_get_width: private since 9.6 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Saved Settings > Display > Theme: a theme id ("erosq:<name>" or
 * "rockbox:<name>"), "" = iPod look. */
#define THEME_PATH "/data/erosq-ui.theme"

lv_color_t ui_col_bg, ui_col_accent, ui_col_text, ui_col_on_accent;
const lv_font_t *ui_font = &lv_font_ipod_14, *ui_font_bold = &lv_font_ipod_14b,
                *ui_font_large = &lv_font_ipod_18b;

/* iPod-Video look: white list, black Helvetica-metric text (Arimo), and the
 * signature glossy vertical-blue gradients on the title bar and the selection
 * highlight. The "gloss" is a 4-stop gradient with a bright top and a hard
 * specular kink near the middle (needs LV_GRADIENT_MAX_STOPS >= 4). */
static lv_style_t s_bg, s_bar, s_sel;
static lv_style_t s_list, s_list_sb, s_list_hdr, s_list_row;
static lv_grad_dsc_t bar_grad, sel_grad, danger_grad;
static lv_grad_dsc_t s_flat_grad = { .dir = LV_GRAD_DIR_NONE };   /* "no gradient" for a flat title bar */
static lv_style_t s_bar_text;   /* status-bar glyphs over the title bar */
static lv_style_t s_surface;    /* panels on a screen (Home preview pane) */
static lv_style_t s_art, s_art_note;   /* missing-cover tile + its note glyph */
static lv_theme_t s_theme;   /* child of the default theme: list framing */

/* Active theme. s_rb = 0: the iPod look, every value below is the original
 * constant (default path is pixel-identical to the pre-theme build). Otherwise
 * s_m is whatever an importer produced; nothing here knows its file format. */
static char s_id[THEME_ID_MAX];
static int s_rb;
static struct theme_model s_m;
static struct theme_tints s_t;
static lv_display_t *s_disp;
#define BD_MAX_BANDS 32
static lv_obj_t *s_bd;               /* backdrop container on lv_layer_bottom, NULL = none */
static uint16_t *s_bd_px[BD_MAX_BANDS];   /* RGB565 pixels behind each image */
static lv_image_dsc_t s_bd_dsc[BD_MAX_BANDS];
static int s_bd_n;
static uint32_t s_track;             /* scrubber track */
/* Side panel (theme.txt layout = panel): one set of widgets on lv_layer_top,
 * shown beside the narrowed body of every non-FULL screen. */
static lv_obj_t *s_panel, *s_panel_clock, *s_panel_date, *s_panel_spr;
static uint16_t *s_spr_px[THEME_SPR_N];    /* 1:1 sprites, RGB565 + A8 planes, one buffer each */
static int s_spr_w[THEME_SPR_N], s_spr_h[THEME_SPR_N], s_spr_scale[THEME_SPR_N];
static uint8_t *s_spr_big;                 /* the shown sprite, upscaled exactly (~33 KB at 3x) */
static lv_image_dsc_t s_spr_dsc;
static int s_spr_cur = -1;
/* Theme fonts (lv_binfont, theme.txt font*): NULL = the iPod font for that
 * role. Roles may share one font. A switch rebuilds every screen (font_gen),
 * but the old screens are disposed asynchronously, so replaced fonts wait in
 * s_font_old until the next switch. */
static lv_font_t *s_font[THEME_FONT_N], *s_font_old[THEME_FONT_N];
static char s_font_path[THEME_FONT_N][256];
static unsigned s_font_gen;
static const lv_font_t *const s_ipod_font[THEME_FONT_N] = { &lv_font_ipod_14, &lv_font_ipod_14b, &lv_font_ipod_18b };
static int s_clock_right = 29;       /* theme_clock_right(18): the iPod font */

/* Row text inset. Matches the default theme's list_btn pad (PAD_SMALL, 8px at
 * this DPI) so section headers line up with the row labels. */
#define UI_LIST_PAD 8

/* Every lv_list_create gets the Home list's framing: edge-to-edge, no card
 * radius/border/outer pad, so dividers and the focus highlight both span the
 * full width. Runs after the parent (default) theme, so these win. Flat fills
 * only -- no gradients/shadows/opacity layers (all drawing is CPU). */
static void list_apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &s_list, 0);
        lv_obj_add_style(obj, &s_list_sb, LV_PART_SCROLLBAR);
    } else if (lv_obj_check_type(obj, &lv_list_text_class)) {
        lv_obj_add_style(obj, &s_list_hdr, 0);
    } else if (lv_obj_check_type(obj, &lv_list_button_class)) {
        lv_obj_add_style(obj, &s_list_row, 0);
    }
}

/* Vertical 4-stop gradient: c0 at top .. c3 at bottom, with a specular kink
 * between c1@f1 and c2@f2 (frac 0-255). */
static void grad4(lv_grad_dsc_t *g, uint32_t c0, uint32_t c1, uint32_t c2,
                  uint32_t c3, uint8_t f1, uint8_t f2)
{
    uint32_t col[4] = { c0, c1, c2, c3 };
    uint8_t  frc[4] = { 0, f1, f2, 255 };
    g->dir = LV_GRAD_DIR_VER;
    g->stops_count = 4;
    for (int i = 0; i < 4; i++) {
        g->stops[i].color = lv_color_hex(col[i]);
        g->stops[i].opa   = LV_OPA_COVER;
        g->stops[i].frac  = frc[i];
    }
}

/* Set a style prop, or drop it so the parent (default) theme's value shows --
 * the iPod look never set these, and must stay identical. */
#define SET_OR_DROP(st, prop, on, setter) \
    do { if (on) setter; else lv_style_remove_prop(st, prop); } while (0)

/* Vertical sel0 -> sel1 gradient, or LV_GRAD_DIR_NONE so LVGL takes the
 * cheaper flat bg_color path. */
static void grad2(lv_grad_dsc_t *g, uint32_t c0, uint32_t c1, int on)
{
    if (!on) { g->dir = LV_GRAD_DIR_NONE; return; }
    grad4(g, c0, c0, c1, c1, 0, 255);
    g->dir = LV_GRAD_DIR_VER;
}

/* UI_COL_* + the gradients, from the active theme. */
static void theme_vars(void)
{
    if (!s_rb) {
        ui_col_bg = lv_color_hex(0xffffff); ui_col_text = lv_color_hex(0x000000);
        ui_col_accent = lv_color_hex(0x2f6fce); ui_col_on_accent = lv_color_white();
        grad4(&bar_grad, 0xb0ccee, 0x6e9bdd, 0x4276c6, 0x2e5fb4, 118, 128);  /* title bar */
        grad4(&sel_grad, 0x86b4f4, 0x4a8cef, 0x2c75e8, 0x1e63d8, 118, 128);  /* selection */
        s_track = 0xd9d9d9;
    } else {
        ui_col_bg = lv_color_hex(s_m.bg); ui_col_text = lv_color_hex(s_m.fg);
        ui_col_accent = lv_color_hex(s_m.sel0); ui_col_on_accent = lv_color_hex(s_m.sel_text);
        grad2(&bar_grad, s_m.sel0, s_m.sel1, s_m.sel_grad);
        grad2(&sel_grad, s_m.sel0, s_m.sel1, s_m.sel_grad);
        s_track = s_t.track;
    }
}

static void status_font(void);

/* Every themed style colour. Runs at init and on a theme switch; LVGL styles
 * are shared by reference, so lv_obj_report_style_change repaints every live
 * object that uses them. */
static void theme_colors(void)
{
    int bd = s_bd != NULL;
    theme_vars();

    /* Screens + bodies; transparent over a backdrop (lv_layer_bottom). */
    lv_style_set_bg_color(&s_bg, UI_COL_BG);
    lv_style_set_bg_opa(&s_bg, bd ? LV_OPA_TRANSP : LV_OPA_COVER);
    lv_style_set_text_color(&s_bg, UI_COL_TEXT);

    int tb = s_rb && s_m.has_titlebar;   /* own flat title-bar colours */
    lv_style_set_bg_color(&s_bar, tb ? lv_color_hex(s_m.titlebar) : UI_COL_ACCENT);   /* flat when grad dir is NONE */
    lv_style_set_bg_grad(&s_bar, tb ? &s_flat_grad : &bar_grad);
    lv_style_set_text_color(&s_bar, tb ? lv_color_hex(s_m.titlebar_text) : UI_COL_ON_ACCENT);
    lv_style_set_border_color(&s_bar, lv_color_hex(tb ? s_m.titlebar_line : s_rb ? s_m.sel1 : 0x274f8e));
    lv_style_set_text_color(&s_bar_text, tb ? lv_color_hex(s_m.titlebar_text) : UI_COL_ON_ACCENT);
    lv_style_set_text_font(&s_bar, UI_FONT_BOLD);
    status_font();
    lv_style_set_bg_color(&s_surface, UI_COL_BG);
    lv_style_set_bg_opa(&s_surface, bd ? LV_OPA_TRANSP : LV_OPA_COVER);
    SET_OR_DROP(&s_surface, LV_STYLE_TEXT_COLOR, s_rb, lv_style_set_text_color(&s_surface, UI_COL_TEXT));

    lv_style_set_bg_color(&s_sel, UI_COL_ACCENT);
    lv_style_set_text_color(&s_sel, UI_COL_ON_ACCENT);

    SET_OR_DROP(&s_list, LV_STYLE_BG_COLOR, s_rb, lv_style_set_bg_color(&s_list, UI_COL_BG));
    SET_OR_DROP(&s_list, LV_STYLE_BG_OPA,   s_rb, lv_style_set_bg_opa(&s_list, bd ? LV_OPA_TRANSP : LV_OPA_COVER));
    SET_OR_DROP(&s_list_row, LV_STYLE_TEXT_COLOR,   s_rb, lv_style_set_text_color(&s_list_row, UI_COL_TEXT));
    SET_OR_DROP(&s_list_row, LV_STYLE_BORDER_COLOR, s_rb, lv_style_set_border_color(&s_list_row, lv_color_hex(s_t.line)));
    /* row_height / row_dividers; unset, the default theme's 8 px pads and 1 px divider show */
    int nodiv = s_rb && s_m.no_dividers, rh = s_rb ? s_m.row_h : 0, top, bot;
    theme_row_pads(rh, lv_font_get_line_height(UI_FONT), nodiv ? 0 : 1, &top, &bot);
    SET_OR_DROP(&s_list_row, LV_STYLE_PAD_TOP,      rh,    lv_style_set_pad_top(&s_list_row, top));
    SET_OR_DROP(&s_list_row, LV_STYLE_PAD_BOTTOM,   rh,    lv_style_set_pad_bottom(&s_list_row, bot));
    SET_OR_DROP(&s_list_row, LV_STYLE_BORDER_WIDTH, nodiv, lv_style_set_border_width(&s_list_row, 0));
    lv_style_set_border_width(&s_list_hdr, nodiv ? 0 : 1);
    lv_style_set_bg_color(&s_list_sb, lv_color_hex(s_rb ? s_t.scroll : 0x9a9a9a));
    lv_style_set_bg_color(&s_list_hdr, lv_color_hex(s_rb ? s_t.hdr_bg : 0xefefef));
    lv_style_set_text_color(&s_list_hdr, lv_color_hex(s_rb ? s_t.hdr_text : 0x6a6a6a));
    lv_style_set_border_color(&s_list_hdr, lv_color_hex(s_rb ? s_t.line : 0xd0d0d0));
    /* Home preview pane hairline + missing-cover tile: the iPod greys, or the
     * same tints the list headers use (light greys were glaring on dark themes) */
    lv_style_set_border_color(&s_surface, lv_color_hex(s_rb ? s_t.line : 0xc0c0c0));
    lv_style_set_bg_color(&s_art, lv_color_hex(s_rb ? s_t.hdr_bg : 0xeaeaea));
    lv_style_set_bg_grad_color(&s_art, lv_color_hex(s_rb ? s_t.line : 0xc2c2c2));
    lv_style_set_border_color(&s_art, lv_color_hex(s_rb ? s_t.scroll : 0xa8a8a8));
    lv_style_set_text_color(&s_art_note, lv_color_hex(s_rb ? s_t.hdr_text : 0x808080));
}

void ui_theme_init(void)
{
    grad4(&danger_grad, 0xf49090, 0xe85050, 0xd83030, 0xc42020, 118, 128); /* destructive, focused */

    lv_style_init(&s_bg);
    lv_style_set_border_width(&s_bg, 0);
    lv_style_set_pad_all(&s_bg, 0);

    lv_style_init(&s_bar);                 /* glossy blue title bar, bold white centered text */
    lv_style_set_bg_grad(&s_bar, &bar_grad);
    lv_style_set_bg_opa(&s_bar, LV_OPA_COVER);
    lv_style_set_pad_hor(&s_bar, 6);
    lv_style_set_border_width(&s_bar, 1);
    lv_style_set_border_side(&s_bar, LV_BORDER_SIDE_BOTTOM);

    lv_style_init(&s_sel);                 /* focused list item: glossy blue, white text */
    lv_style_set_bg_grad(&s_sel, &sel_grad);
    lv_style_set_bg_opa(&s_sel, LV_OPA_COVER);

    lv_style_init(&s_list);                /* flat, edge-to-edge list frame */
    lv_style_set_radius(&s_list, 0);
    lv_style_set_border_width(&s_list, 0);
    lv_style_set_pad_all(&s_list, 0);
    lv_style_set_clip_corner(&s_list, false);

    /* Narrow iPod-style bar hugging the right edge (x 316-318). Rows keep their
     * 8px right pad, so right-aligned chevrons end at x 312 and never collide.
     * Opaque: skips the default 40% blend. */
    lv_style_init(&s_list_sb);
    lv_style_set_width(&s_list_sb, 3);
    lv_style_set_pad_right(&s_list_sb, 1);
    lv_style_set_pad_ver(&s_list_sb, 2);
    lv_style_set_radius(&s_list_sb, 0);
    lv_style_set_bg_opa(&s_list_sb, LV_OPA_COVER);

    /* Unfocused rows: no fill of their own. The default theme paints every row
     * opaque white on top of the (opaque white, edge-to-edge) list, so each
     * scroll frame filled the whole list area twice -- the solid fill was ~22%
     * of erosq-ui's CPU while scrolling (on-device PC sampling). Same pixels:
     * the list shows through. The focused row keeps its opaque gloss (s_sel,
     * LV_STATE_FOCUSED beats this state-0 style). */
    lv_style_init(&s_list_row);
    lv_style_set_bg_opa(&s_list_row, LV_OPA_TRANSP);

    /* Section header (lv_list_add_text): subdued light-grey band, grey text at
     * the row-label inset, hairline divider like the rows. The default theme's
     * transform_width grow is zeroed so the band stays inside the list. */
    lv_style_init(&s_list_hdr);
    lv_style_set_bg_opa(&s_list_hdr, LV_OPA_COVER);
    lv_style_set_pad_hor(&s_list_hdr, UI_LIST_PAD);
    lv_style_set_pad_ver(&s_list_hdr, 3);
    lv_style_set_transform_width(&s_list_hdr, 0);
    lv_style_set_border_width(&s_list_hdr, 1);
    lv_style_set_border_side(&s_list_hdr, LV_BORDER_SIDE_BOTTOM);

    lv_style_init(&s_bar_text);
    lv_style_init(&s_surface);
    lv_style_init(&s_art);
    lv_style_init(&s_art_note);
    theme_colors();

    /* Chain once onto the display's (default) theme; ui_build re-runs this on
     * every UI rebuild. */
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *cur = d ? lv_display_get_theme(d) : NULL;
    if (cur && cur != &s_theme) {
        s_theme = *cur;
        lv_theme_set_parent(&s_theme, cur);
        lv_theme_set_apply_cb(&s_theme, list_apply_cb);
        lv_display_set_theme(d, &s_theme);
    }
}

void ui_style_scroll_thumb(lv_obj_t *o) { lv_obj_add_style(o, &s_list_sb, 0); }

void ui_apply_focus_style(lv_obj_t *o)
{
    lv_obj_add_style(o, &s_sel, LV_STATE_FOCUSED);
}

/* Destructive-action button: flat red, the same glossy highlight in red when
 * focused. Local styles, so they beat the blue FOCUSED selection style. */
void ui_style_danger(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xd32f2f), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad(btn, &danger_grad, LV_STATE_FOCUSED);
}

/* iPod position bar: glossy-blue indicator on a recessed gray track. */
void ui_style_scrubber(lv_obj_t *bar)
{
    lv_obj_set_style_bg_color(bar, lv_color_hex(s_track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(bar, &sel_grad, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
}

static void panel_show(int kind);

static void screen_load_cb(lv_event_t *e)
{
    panel_show((int)(intptr_t)lv_event_get_user_data(e));
}

lv_obj_t *ui_screen_base(const char *title) { return ui_screen_base_kind(title, UI_SCR_DEFAULT); }

lv_obj_t *ui_screen_base_kind(const char *title, int kind)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &s_bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_add_style(bar, &s_bar, 0);
    lv_obj_set_size(bar, 320, 22);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, UI_TITLE_W);          /* clears the status widgets, see ui_theme.h */
    /* One line tall: LONG_DOT only truncates a label with a fixed height. With
     * the default content height a long title wrapped to several lines,
     * centred and clipped by the 22px bar (no "...", stray glyph tops). */
    lv_obj_set_height(t, lv_font_get_line_height(UI_FONT_BOLD));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);   /* iPod: centered title */

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_add_style(body, &s_bg, 0);
    /* beside a theme's side panel the body (and every list in it) narrows */
    lv_obj_set_size(body, kind == UI_SCR_FULL ? 320 : 320 - ui_theme_panel_w(), 218);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_group_t *g = lv_group_create();
    lv_obj_set_user_data(scr, g);
    if (s_panel)
        lv_obj_add_event_cb(scr, screen_load_cb, LV_EVENT_SCREEN_LOAD_START, (void *)(intptr_t)kind);
    return scr;
}

lv_obj_t *ui_screen_list(lv_obj_t *scr)
{
    lv_obj_t *list = lv_list_create(ui_screen_body(scr));
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    return list;
}

int ui_screen_body_w(lv_obj_t *scr)
{
    return lv_obj_get_style_width(ui_screen_body(scr), LV_PART_MAIN);
}

lv_obj_t *ui_screen_body(lv_obj_t *screen){ return lv_obj_get_child(screen, 1); }

lv_group_t *ui_screen_group(lv_obj_t *scr){ return lv_obj_get_user_data(scr); }

/* Shared, so a theme switch recolours them in place. */
void ui_style_bar_text(lv_obj_t *o) { lv_obj_add_style(o, &s_bar_text, 0); }
void ui_style_surface(lv_obj_t *o)  { lv_obj_add_style(o, &s_surface, 0); }
void ui_style_art_placeholder(lv_obj_t *tile, lv_obj_t *note)
{
    lv_obj_add_style(tile, &s_art, 0);
    lv_obj_add_style(note, &s_art_note, 0);
}

/* ---- Themes: importers fill a theme_model (theme_logic.h) ----------------- */

static const char *sd_root(void)
{
    const char *r = getenv("EROSQ_SD_ROOT");   /* host screenshots */
    return r && *r ? r : "/media/sd";
}

/* Whole small file -> NUL-terminated buf; bytes read or -1. */
static int read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

static void backdrop_free(void)
{
    if (s_bd) {
        lv_obj_delete(s_bd);
        s_bd = NULL;
        lv_obj_remove_local_style_prop(lv_layer_bottom(), LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(lv_layer_bottom(), LV_STYLE_BG_OPA, 0);
    }
    for (int i = 0; i < s_bd_n; i++) {
        lv_image_cache_drop(&s_bd_dsc[i]);   /* dsc addresses are reused by the next backdrop */
        free(s_bd_px[i]);
    }
    s_bd_n = 0;
}

#define UI_COL_BG_RAW() lv_color_hex(s_m.bg)   /* UI_COL_* is set after theme_load */

/* RGB565 -> lv_color_t; bit replication makes the display's 565 conversion exact. */
static lv_color_t c565(uint16_t c)
{
    int r = c >> 11, g = (c >> 5) & 0x3f, b = c & 0x1f;
    return lv_color_make((uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2));
}

static lv_obj_t *bd_obj(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

/* Takes ownership of px (w x h RGB565). */
static void bd_image(lv_obj_t *parent, int x, int y, int w, int h, uint16_t *px)
{
    int i = s_bd_n++;
    s_bd_px[i] = px;
    s_bd_dsc[i] = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = (uint32_t)w, .h = (uint32_t)h, .stride = (uint32_t)w * 2 },
        .data_size = (uint32_t)(w * h * 2),
        .data = (const uint8_t *)px,
    };
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_pos(img, x, y);
    lv_image_set_src(img, &s_bd_dsc[i]);
}

/* Backdrop on the bottom layer; themed screens are transparent over it, so
 * every redraw under a list starts at lv_layer_bottom. That layer has an
 * opaque theme background of its own, which LVGL fills first -- one full pass
 * per scroll frame before the backdrop (device: most of a +21-28% scroll
 * cost). So the layer's own fill IS the backdrop's main colour: flat rows in
 * that colour cost nothing extra, other flat rows are fills, and only the
 * detail is kept as small images (theme_bands). A too-detailed image (photo,
 * gradient) is drawn whole over a transparent layer -- documented cost.
 * A missing or unsupported file just means no backdrop. */
static void backdrop_load(const char *path)
{
    if (!*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint16_t *px = malloc(320 * 240 * sizeof *px);
    int w, h;
    int rc = px ? theme_bmp_load(f, px, 320, 240, &w, &h) : -1;
    fclose(f);
    if (rc) { free(px); return; }

    struct theme_band b[BD_MAX_BANDS];
    int n = theme_bands(px, w, h, b, BD_MAX_BANDS);
    lv_obj_t *lb = lv_layer_bottom();
    s_bd = bd_obj(lb, 0, 0, w, h);
    if (n < 0) {
        /* whole image: a full-screen one hides the layer, so skip its fill */
        int full = w == 320 && h == 240;
        lv_obj_set_style_bg_color(lb, UI_COL_BG_RAW(), 0);
        lv_obj_set_style_bg_opa(lb, full ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        bd_image(s_bd, 0, 0, w, h, px);
        return;
    }
    /* the layer's fill = the colour covering the most rows */
    int main_i = 0;
    for (int i = 1; i < n; i++)
        if (b[i].y1 - b[i].y0 > b[main_i].y1 - b[main_i].y0) main_i = i;
    uint16_t main_c = b[main_i].color;
    lv_obj_set_style_bg_color(lb, c565(main_c), 0);
    lv_obj_set_style_bg_opa(lb, LV_OPA_COVER, 0);
    for (int i = 0; i < n; i++) {
        int bh = b[i].y1 - b[i].y0, sw = b[i].x1 - b[i].x0;
        lv_obj_t *band = bd_obj(s_bd, 0, b[i].y0, w, bh);
        if (b[i].color != main_c) {
            lv_obj_set_style_bg_color(band, c565(b[i].color), 0);
            lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        }
        if (!sw) continue;
        uint16_t *crop = malloc((size_t)sw * bh * sizeof *crop);
        if (!crop) continue;   /* ponytail: the band still shows its fill colour */
        for (int y = 0; y < bh; y++)
            memcpy(crop + (size_t)y * sw, px + (size_t)(b[i].y0 + y) * w + b[i].x0, (size_t)sw * 2);
        bd_image(band, b[i].x0, 0, sw, bh, crop);
    }
    free(px);
}

int ui_theme_panel_w(void) { return s_panel ? s_m.panel_w : 0; }
int ui_theme_row_dividers(void) { return !(s_rb && s_m.no_dividers); }
int ui_theme_panel_shown(void) { return s_panel && !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN); }

static void panel_free(void)
{
    if (s_panel) { lv_obj_delete(s_panel); s_panel = NULL; }
    if (s_spr_big) { lv_image_cache_drop(&s_spr_dsc); free(s_spr_big); s_spr_big = NULL; }
    s_spr_cur = -1;
    for (int i = 0; i < THEME_SPR_N; i++) { free(s_spr_px[i]); s_spr_px[i] = NULL; }
}

/* 1:1 sprite BMP (magenta = transparent) -> RGB565A8 planes; scale = the
 * largest integer that fits box_w x box_h. 0 = loaded. */
static int sprite_load(int i, const char *path, int box_w, int box_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    enum { MAXW = THEME_PANEL_MAX, MAXH = 218 };
    uint16_t *px = malloc((size_t)MAXW * MAXH * 3);   /* RGB565 plane + A8 plane, trimmed below */
    int w, h;
    int rc = px ? theme_bmp_load(f, px, MAXW, MAXH, &w, &h) : -1;
    fclose(f);
    int k = rc ? 0 : theme_sprite_scale(box_w, box_h, w, h);
    if (!k) { free(px); return -1; }
    uint16_t *t = realloc(px, (size_t)w * h * 3);
    if (t) px = t;
    theme_key_alpha(px, w * h, (uint8_t *)(px + w * h));
    s_spr_px[i] = px;
    s_spr_w[i] = w; s_spr_h[i] = h; s_spr_scale[i] = k;
    return 0;
}

/* Clock + date top, sprite bottom-centred. On lv_layer_top at the bottom of
 * its z-order so toasts and the volume overlay stay above it. Lists never
 * reach under it (the body is narrowed), so scrolling never redraws it. */
static void panel_create(void)
{
    int pw = s_m.panel_w;
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_panel, 320 - pw, 22);
    lv_obj_set_size(s_panel, pw, 218);
    lv_obj_move_to_index(s_panel, 0);
    lv_obj_set_style_text_color(s_panel, lv_color_hex(s_m.fg), 0);   /* UI_COL_* not set yet */

    s_panel_clock = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_panel_clock, UI_FONT_LARGE, 0);
    lv_label_set_text(s_panel_clock, "");
    /* level with the list's first two rows (same pads as s_list_row) */
    int border = s_m.no_dividers ? 0 : 1;
    int clh = lv_font_get_line_height(UI_FONT_LARGE), dlh = lv_font_get_line_height(UI_FONT);
    lv_obj_align(s_panel_clock, LV_ALIGN_TOP_MID, 0, theme_panel_text_y(0, s_m.row_h, clh, dlh, border));
    s_panel_date = lv_label_create(s_panel);
    lv_label_set_text(s_panel_date, "");
    lv_obj_align(s_panel_date, LV_ALIGN_TOP_MID, 0, theme_panel_text_y(1, s_m.row_h, clh, dlh, border));

    /* sprite box: below the date, 4 px margins */
    int box_w = pw - 8, box_h = 218 - 56 - 4;
    for (int i = 0; i < THEME_SPR_N; i++)
        if (s_m.sprite[i][0]) sprite_load(i, s_m.sprite[i], box_w, box_h);
    s_panel_spr = lv_image_create(s_panel);
    lv_obj_add_flag(s_panel_spr, LV_OBJ_FLAG_HIDDEN);
}

static void panel_show(int kind)
{
    if (!s_panel) return;
    if (kind == UI_SCR_FULL) { lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    int i = s_spr_px[kind] ? kind : THEME_SPR_DEFAULT;      /* per-screen, else the default */
    if (!s_spr_px[i]) { lv_obj_add_flag(s_panel_spr, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_panel_spr, LV_OBJ_FLAG_HIDDEN);
    if (i == s_spr_cur) return;                             /* same sprite: no redraw */
    /* upscale by pixel replication (exact; LVGL's transform samples off-grid) */
    int k = s_spr_scale[i], w = s_spr_w[i] * k, h = s_spr_h[i] * k;
    uint8_t *big = malloc((size_t)w * h * 3);
    if (!big) { lv_obj_add_flag(s_panel_spr, LV_OBJ_FLAG_HIDDEN); return; }
    theme_scale565a8((const uint8_t *)s_spr_px[i], s_spr_w[i], s_spr_h[i], k, big);
    if (s_spr_big) { lv_image_cache_drop(&s_spr_dsc); free(s_spr_big); }
    s_spr_big = big;
    s_spr_cur = i;
    s_spr_dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,
                    .w = (uint32_t)w, .h = (uint32_t)h, .stride = (uint32_t)w * 2 },
        .data_size = (uint32_t)(w * h * 3),
        .data = big,
    };
    lv_image_set_src(s_panel_spr, &s_spr_dsc);
    lv_obj_align(s_panel_spr, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void lbl_change(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt)) lv_label_set_text(l, txt);   /* redraw only on change */
}

void ui_theme_panel_clock(const char *time, const char *date)
{
    if (!s_panel) return;
    lbl_change(s_panel_clock, time);
    lbl_change(s_panel_date, date);
}

static void fonts_free(lv_font_t **f)
{
    for (int i = 0; i < THEME_FONT_N; i++) {
        if (!f[i]) continue;
        for (int j = i + 1; j < THEME_FONT_N; j++) if (f[j] == f[i]) f[j] = NULL;   /* shared roles */
        lv_binfont_destroy(f[i]);
        f[i] = NULL;
    }
}

static int font_in(const lv_font_t *f, lv_font_t *const *set)
{
    for (int i = 0; i < THEME_FONT_N; i++) if (set[i] == f) return 1;
    return 0;
}

/* lv_binfont_create on a file binfont_check accepts; NULL if unreadable,
 * malformed or too big. */
static lv_font_t *font_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(BINFONT_MAX_SIZE + 1);
    size_t n = b ? fread(b, 1, BINFONT_MAX_SIZE + 1, f) : 0;
    fclose(f);
    int ok = b && binfont_check(b, n) == 0;
    free(b);
    if (!ok) return NULL;
    char p[260];
    snprintf(p, sizeof p, "P:%s", path);   /* LV_FS_POSIX_LETTER */
    return lv_binfont_create(p);
}

/* m NULL = the iPod fonts. A font that fails to load falls back to the iPod
 * font for its role; glyphs a theme font lacks (LV_SYMBOL_*) come from the
 * iPod body font. */
static void fonts_load(const struct theme_model *m)
{
    lv_font_t *f[THEME_FONT_N] = { 0 };
    for (int i = 0; i < THEME_FONT_N; i++) {
        const char *path = m ? m->font[i] : "";
        if (!*path) continue;
        int j = 0;
        while (j < i && strcmp(path, m->font[j])) j++;
        if (j < i) { f[i] = f[j]; continue; }                 /* same file as an earlier role */
        for (j = 0; j < THEME_FONT_N && !f[i]; j++)        /* unchanged: keep it */
            if (s_font[j] && !strcmp(path, s_font_path[j])) f[i] = s_font[j];
        if (f[i]) continue;
        f[i] = font_open(path);
        if (f[i]) f[i]->fallback = &lv_font_ipod_14;
        else LV_LOG_WARN("theme font %s unreadable, iPod font instead", path);
    }
    if (!memcmp(f, s_font, sizeof f)) return;
    fonts_free(s_font_old);   /* replaced a switch ago: their screens are gone by now */
    for (int i = 0; i < THEME_FONT_N; i++)
        if (s_font[i] && !font_in(s_font[i], f) && !font_in(s_font[i], s_font_old)) s_font_old[i] = s_font[i];
    for (int i = 0; i < THEME_FONT_N; i++) {
        s_font[i] = f[i];
        snprintf(s_font_path[i], sizeof s_font_path[i], "%s", f[i] ? m->font[i] : "");
    }
    ui_font       = f[THEME_FONT_BODY]  ? f[THEME_FONT_BODY]  : s_ipod_font[THEME_FONT_BODY];
    ui_font_bold  = f[THEME_FONT_BOLD]  ? f[THEME_FONT_BOLD]  : s_ipod_font[THEME_FONT_BOLD];
    ui_font_large = f[THEME_FONT_LARGE] ? f[THEME_FONT_LARGE] : s_ipod_font[THEME_FONT_LARGE];
    s_font_gen++;
}

unsigned ui_theme_font_gen(void) { return s_font_gen; }

static int glyphs_w(const lv_font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) w += lv_font_get_glyph_width(f, (unsigned char)*s, (unsigned char)s[1]);
    return w;
}

static int text_w(const lv_font_t *f, const char *s)
{
    lv_text_attributes_t a;
    lv_text_attributes_init(&a);
    return lv_text_get_width(s, strlen(s), f, &a);
}

/* Status-bar font: the theme's body font when its battery, clock and play
 * glyphs fit beside the fixed 128 px title, else the iPod font. The iPod
 * font's widths are the ones measured in ui_theme.h, so it isn't re-measured
 * (that would cost every boot). */
static void status_font(void)
{
    const lv_font_t *f = UI_FONT;
    int batt = 18, clock = 61, play = 12;
    if (f != &lv_font_ipod_14) {
        static const char *const batts[] = { LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1,
            LV_SYMBOL_BATTERY_2, LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL };
        batt = 0;
        for (unsigned i = 0; i < sizeof batts / sizeof *batts; i++)
            if (text_w(f, batts[i]) > batt) batt = text_w(f, batts[i]);
        play = text_w(f, LV_SYMBOL_PLAY) > text_w(f, LV_SYMBOL_PAUSE) ? text_w(f, LV_SYMBOL_PLAY)
                                                                       : text_w(f, LV_SYMBOL_PAUSE);
        /* every minute of a local day, every format (2021-01-01: after the
         * clock's 2020 sanity floor) */
        clock = 0;
        for (int fmt = 0; fmt < UI_CLOCK_FMT_N; fmt++)
            for (int min = 0; min < 1440; min++) {
                char b[UI_CLOCK_BUF];
                ui_clock_format((time_t)1609459200 + min * 60, fmt, b, sizeof b);
                int w = glyphs_w(f, b);
                if (w > clock) clock = w;
            }
        if (!theme_status_fits(batt, clock, play)) {
            LV_LOG_WARN("theme font too wide for the status bar, iPod font there");
            f = &lv_font_ipod_14; batt = 18;
        }
    }
    lv_style_set_text_font(&s_bar_text, f);
    s_clock_right = theme_clock_right(batt);
}

int ui_theme_clock_right(void) { return s_clock_right; }

/* "" = the iPod look. On failure the iPod look is active and -1 returned. */
static int theme_load(const char *id)
{
    backdrop_free();
    panel_free();
    s_rb = 0;
    s_id[0] = 0;
    if (!id || !*id) { fonts_load(NULL); return 0; }
    char tag[16], name[THEME_NAME_MAX];
    if (theme_id_split(id, tag, sizeof tag, name, sizeof name)) return -1;
    int rc = !strcmp(tag, "erosq")   ? nt_import(sd_root(), name, &s_m)
           : !strcmp(tag, "rockbox") ? rbt_import(sd_root(), name, &s_m)
           : -1;
    if (rc) return -1;
    theme_tints(&s_m, &s_t);
    s_rb = 1;
    snprintf(s_id, sizeof s_id, "%s", id);
    backdrop_load(s_m.backdrop);
    fonts_load(&s_m);                       /* before the panel: its clock uses UI_FONT_LARGE */
    if (s_m.panel_w) panel_create();
    return 0;
}

/* LVGL's default theme draws the stock widgets (sliders, switches, buttons):
 * accent = the selector colour, dark mode for dark backgrounds. A re-init
 * restyles live objects itself. */
static void default_theme_init(void)
{
    int dark = s_rb && theme_is_dark(s_m.bg);
    lv_theme_t *th = lv_theme_default_init(s_disp, UI_COL_ACCENT, UI_COL_ACCENT,
                                           dark, UI_FONT);
    if (lv_display_get_theme(s_disp) != &s_theme) lv_display_set_theme(s_disp, th);
    /* lv_layer_top predates the theme, so its children (panel, toasts, popups)
     * fall back to LV_FONT_DEFAULT, the iPod body font, unless told otherwise */
    lv_obj_set_style_text_font(lv_display_get_layer_top(s_disp), UI_FONT, 0);
}

void ui_theme_load(lv_display_t *d)
{
    s_disp = d;
    char name[THEME_ID_MAX] = "";
    const char *env = getenv("EROSQ_THEME");   /* host screenshots */
    if (env) snprintf(name, sizeof name, "%s", env);
    else if (read_file(THEME_PATH, name, sizeof name) > 0) name[strcspn(name, "\r\n")] = 0;
    /* ponytail: an SD card not mounted yet at UI start falls back to the iPod
     * look for this boot (the saved choice is kept); retry on sd-mount if it bites. */
    theme_load(name);
    theme_vars();
    default_theme_init();
}

const char *ui_theme_current(void) { return s_id; }

int ui_theme_select(const char *id)
{
    char prev[THEME_ID_MAX];
    snprintf(prev, sizeof prev, "%s", s_id);
    int rc = theme_load(id);
    if (rc) theme_load(prev);   /* unreadable: keep what is on screen (and saved) */
    theme_colors();
    default_theme_init();
    lv_obj_report_style_change(NULL);
    if (!rc) {
        int fd = open(THEME_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (write(fd, s_id, strlen(s_id)) < 0) { /* best effort, like the other settings */ }
            close(fd);
        }
    }
    return rc;
}

/* By name, then tag ("erosq" before "rockbox"). */
static int id_cmp(const void *a, const void *b)
{
    int c = strcasecmp(strchr(a, ':') + 1, strchr(b, ':') + 1);
    return c ? c : strcmp(a, b);
}

int ui_theme_list(char (*ids)[THEME_ID_MAX], int max)
{
    char names[64][THEME_NAME_MAX];   /* 4 KB, stack: only while the picker builds */
    int n = 0, k;
    k = nt_list(sd_root(), names, max - n < 64 ? max - n : 64);
    for (int i = 0; i < k; i++) snprintf(ids[n++], THEME_ID_MAX, "erosq:%s", names[i]);
    k = rbt_list(sd_root(), names, max - n < 64 ? max - n : 64);
    for (int i = 0; i < k; i++) snprintf(ids[n++], THEME_ID_MAX, "rockbox:%s", names[i]);
    qsort(ids, (size_t)n, sizeof *ids, id_cmp);
    return n;
}
