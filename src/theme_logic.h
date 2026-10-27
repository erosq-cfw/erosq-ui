#ifndef EROSQ_THEME_LOGIC_H
#define EROSQ_THEME_LOGIC_H
/* Format-neutral theme model. Importers (rbtheme_logic.h: Rockbox .cfg) fill a
 * struct theme_model; ui_theme.c only ever applies the model, so another theme
 * format is one more importer. Dependency-free, host-tested in
 * tests/test_rbtheme.c. Phase 1: colours + backdrop; fonts/layout come later. */
#include <stdint.h>
#include <stdio.h>

#define THEME_NAME_MAX 64
#define THEME_ID_MAX   80     /* "<importer tag>:<name>", what Settings persists */

/* Per-screen panel sprite slots (theme.txt sprite, sprite_home, ...). */
enum { THEME_SPR_DEFAULT, THEME_SPR_HOME, THEME_SPR_MUSIC, THEME_SPR_BT, THEME_SPR_N };
/* Font roles (theme.txt font, font_bold, font_large), standing in for the
 * iPod look's 14 px body, 14 px bold and 18 px bold. */
enum { THEME_FONT_BODY, THEME_FONT_BOLD, THEME_FONT_LARGE, THEME_FONT_N };

struct theme_model {          /* 0xRRGGBB colours */
    uint32_t bg, fg;          /* screen background, text */
    uint32_t sel0, sel1;      /* selection (+ title bar unless has_titlebar): top / bottom */
    uint32_t sel_text;        /* text on sel0..sel1 */
    int sel_grad;             /* 1: vertical sel0 -> sel1 gradient, 0: flat sel0 */
    char backdrop[256];       /* absolute path of a full-screen BMP, "" = none */
    /* Optional title-bar colours; has_titlebar = 0 keeps the selection's. */
    int has_titlebar;
    uint32_t titlebar, titlebar_text, titlebar_line;
    /* Side panel (clock, date, sprite) right of the lists: width in px, 0 = none. */
    int panel_w;
    char sprite[THEME_SPR_N][256];   /* absolute BMP paths, "" = none; magenta = transparent */
    /* LVGL binary fonts (.bin), "" = the iPod font. bold defaults to body,
     * large to bold, so one font key restyles everything. */
    char font[THEME_FONT_N][256];
    /* List rows: height in px (0 = the iPod look's 8 px pads), dividers off. */
    int row_h, no_dividers;
};

#define THEME_ROW_MIN 16
#define THEME_ROW_MAX 64

/* Vertical pads giving a single-line row of row_h px: line height + border
 * + top + bottom. Odd remainders go to the bottom; a row_h below the text's
 * own height gives 0 pads (the row can't be shorter than its text). */
void theme_row_pads(int row_h, int line_h, int border, int *top, int *bottom);

/* Side panel text y (relative to the panel, whose top = the list's top):
 * with row_h set, the clock (line i=0) and the date (i=1) sit level with the
 * list's first and second row text; otherwise, or when the date would reach
 * the sprite box (THEME_PANEL_TEXT_H), the fixed 10 / 34 px. */
#define THEME_PANEL_TEXT_H 56
int theme_panel_text_y(int i, int row_h, int clock_lh, int date_lh, int border);

#define THEME_PANEL_MIN 80
#define THEME_PANEL_MAX 140
#define THEME_PANEL_DEFAULT 100

/* Status bar beside the title, which is FIXED at 128 px centred (x 96..224):
 * battery right-aligned THEME_BATT_RIGHT px from the edge, the clock
 * THEME_STATUS_GAP left of it, the play icon at x THEME_PLAY_X, and a
 * THEME_STATUS_GAP gap between each cluster and the title. Widths are the
 * status font's, measured at runtime. 1 = everything fits. */
#define THEME_TITLE_W    128
#define THEME_BATT_RIGHT 5
#define THEME_PLAY_X     6
#define THEME_STATUS_GAP 6
int theme_status_fits(int batt_w, int clock_max_w, int play_w);
/* Right offset of the clock: battery edge + battery + gap. */
int theme_clock_right(int batt_w);

/* Largest integer scale k >= 1 with a w x h sprite fitting box_w x box_h;
 * 0 if it doesn't fit even at 1:1. Pixel art never scales by fractions. */
int theme_sprite_scale(int box_w, int box_h, int w, int h);

/* RGB565 sprite with magenta (#ff00ff) as the transparent key -> alpha plane
 * (0 or 255 per pixel). */
#define THEME_KEY565 0xf81f
void theme_key_alpha(const uint16_t *px, int n, uint8_t *alpha);

/* Integer nearest-neighbour upscale of an RGB565A8 image (RGB565 plane, then
 * A8 plane) by k into out (same layout, (w*k) x (h*k)). Exact pixel art. */
void theme_scale565a8(const uint8_t *src, int w, int h, int k, uint8_t *out);

/* Secondary tones as fg-over-bg tints, matching the iPod look's greys on
 * white (#efefef header, #6a6a6a header text, #9a9a9a scrollbar). */
struct theme_tints { uint32_t hdr_bg, hdr_text, line, scroll, track; };
void theme_tints(const struct theme_model *m, struct theme_tints *t);

/* a*pct% + b*(100-pct)% per channel. */
uint32_t theme_mix(uint32_t a, uint32_t b, int pct);

/* Perceived luminance below mid-grey: use LVGL's dark default theme. */
int theme_is_dark(uint32_t rgb);

/* Safe to persist and to splice into a path: non-empty, no '/', no control
 * chars, no leading '.'. */
int theme_name_ok(const char *name);

/* "rockbox:Midnight" -> tag "rockbox", name "Midnight". 0 = ok. */
int theme_id_split(const char *id, char *tag, size_t tn, char *name, size_t nn);

/* Uncompressed BMP (1/4/8-bit paletted, 24- or 32-bit) -> RGB565, row-major,
 * stride *w. Images larger than max_w x max_h are rejected. Streams one row at
 * a time, so the only buffer is the caller's output. 0 = ok, -1 = bad/unsupported. */
int theme_bmp_load(FILE *f, uint16_t *out, int max_w, int max_h, int *w, int *h);

/* Backdrop as horizontal bands: rows y0..y1-1 filled with one RGB565 colour,
 * plus (x1 > x0) the image's own pixels in columns x0..x1-1. Scrolling then
 * redraws a flat band as a write-only fill instead of copying the whole image
 * every frame (device: a full-screen image cost +28% scroll CPU).
 * Returns the band count, or -1 when the image is too detailed for bands to
 * help (more than max bands, or detail spans over half the image): draw it whole. */
struct theme_band { int16_t y0, y1, x0, x1; uint16_t color; };
int theme_bands(const uint16_t *px, int w, int h, struct theme_band *out, int max);

#endif
