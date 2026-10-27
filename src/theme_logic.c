#include "theme_logic.h"
#include <stdlib.h>
#include <string.h>

uint32_t theme_mix(uint32_t a, uint32_t b, int pct)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        r |= (uint32_t)((ca * pct + cb * (100 - pct) + 50) / 100) << sh;
    }
    return r;
}

int theme_is_dark(uint32_t c)
{
    return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000 < 128;
}

void theme_tints(const struct theme_model *m, struct theme_tints *t)
{
    t->hdr_bg   = theme_mix(m->fg, m->bg, 6);
    t->hdr_text = theme_mix(m->fg, m->bg, 58);
    t->line     = theme_mix(m->fg, m->bg, 10);
    t->scroll   = theme_mix(m->fg, m->bg, 40);
    t->track    = theme_mix(m->fg, m->bg, 15);   /* scrubber track (#d9d9d9 on white) */
}

int theme_name_ok(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (!len || len >= THEME_NAME_MAX || s[0] == '.') return 0;
    for (; *s; s++)
        if (*s == '/' || (unsigned char)*s < 0x20) return 0;
    return 1;
}

int theme_id_split(const char *id, char *tag, size_t tn, char *name, size_t nn)
{
    const char *c = id ? strchr(id, ':') : NULL;
    if (!c || c == id || (size_t)(c - id) >= tn || strlen(c + 1) >= nn) return -1;
    memcpy(tag, id, (size_t)(c - id));
    tag[c - id] = 0;
    strcpy(name, c + 1);
    return theme_name_ok(name) ? 0 : -1;
}

void theme_row_pads(int row_h, int line_h, int border, int *top, int *bottom)
{
    int pad = row_h - line_h - border;
    if (pad < 0) pad = 0;
    *top = pad / 2;
    *bottom = pad - pad / 2;
}

int theme_panel_text_y(int i, int row_h, int clock_lh, int date_lh, int border)
{
    int ct, dt, b;
    theme_row_pads(row_h, clock_lh, border, &ct, &b);
    theme_row_pads(row_h, date_lh, border, &dt, &b);
    if (!row_h || row_h + dt + date_lh > THEME_PANEL_TEXT_H) return i ? 34 : 10;
    return i ? row_h + dt : ct;
}

int theme_clock_right(int batt_w) { return THEME_BATT_RIGHT + batt_w + THEME_STATUS_GAP; }

int theme_status_fits(int batt_w, int clock_max_w, int play_w)
{
    int side = (320 - THEME_TITLE_W) / 2 - THEME_STATUS_GAP;   /* 90 px each side */
    return theme_clock_right(batt_w) + clock_max_w <= side
        && THEME_PLAY_X + play_w <= side;
}

int theme_sprite_scale(int box_w, int box_h, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    int kx = box_w / w, ky = box_h / h;
    return kx < ky ? kx : ky;
}

void theme_key_alpha(const uint16_t *px, int n, uint8_t *alpha)
{
    for (int i = 0; i < n; i++) alpha[i] = px[i] == THEME_KEY565 ? 0 : 255;
}

void theme_scale565a8(const uint8_t *src, int w, int h, int k, uint8_t *out)
{
    const uint16_t *sc = (const uint16_t *)src;
    const uint8_t *sa = src + (size_t)w * h * 2;
    int ow = w * k, oh = h * k;
    uint16_t *oc = (uint16_t *)out;
    uint8_t *oa = out + (size_t)ow * oh * 2;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            size_t si = (size_t)(y / k) * w + x / k, oi = (size_t)y * ow + x;
            oc[oi] = sc[si];
            oa[oi] = sa[si];
        }
}

/* Majority colour of a row (Boyer-Moore vote) or -1 if no colour has > w/2. */
static int row_major(const uint16_t *r, int w)
{
    uint16_t c = r[0];
    int n = 0;
    for (int x = 0; x < w; x++) {
        if (!n) { c = r[x]; n = 1; }
        else n += r[x] == c ? 1 : -1;
    }
    n = 0;
    for (int x = 0; x < w; x++) n += r[x] == c;
    return n * 2 > w ? c : -1;
}

int theme_bands(const uint16_t *px, int w, int h, struct theme_band *out, int max)
{
    int n = 0;
    long detail = 0;
    for (int y = 0; y < h; y++) {
        const uint16_t *r = px + (size_t)y * w;
        int c = row_major(r, w);
        if (c < 0) return -1;
        int x0 = w, x1 = 0;
        for (int x = 0; x < w; x++)
            if (r[x] != c) { if (x < x0) x0 = x; x1 = x + 1; }
        if (x0 >= x1) x0 = x1 = 0;
        struct theme_band *b = n ? &out[n - 1] : NULL;
        /* extend the band while the fill colour holds and the row is either
         * flat or the band already carries an image span (union it) */
        if (b && b->color == c && ((x0 == x1 && b->x0 == b->x1) || (x0 != x1 && b->x0 != b->x1))) {
            if (x0 != x1) {
                detail -= (long)(b->x1 - b->x0) * (b->y1 - b->y0);
                if (x0 < b->x0) b->x0 = (int16_t)x0;
                if (x1 > b->x1) b->x1 = (int16_t)x1;
                detail += (long)(b->x1 - b->x0) * (y + 1 - b->y0);
            }
            b->y1 = (int16_t)(y + 1);
            continue;
        }
        if (n == max) return -1;
        out[n++] = (struct theme_band){ (int16_t)y, (int16_t)(y + 1), (int16_t)x0, (int16_t)x1, (uint16_t)c };
        detail += x1 - x0;
    }
    return detail * 2 > (long)w * h ? -1 : n;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rgb565(int r, int g, int b) { return (uint16_t)((r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3); }

int theme_bmp_load(FILE *f, uint16_t *out, int max_w, int max_h, int *pw, int *ph)
{
    uint8_t hd[54];
    if (fread(hd, 1, sizeof hd, f) != sizeof hd || hd[0] != 'B' || hd[1] != 'M') return -1;
    uint32_t off = le32(hd + 10), dib = le32(hd + 14);
    int32_t w = (int32_t)le32(hd + 18), h = (int32_t)le32(hd + 22);
    int bpp = hd[28] | hd[29] << 8;
    uint32_t comp = le32(hd + 30), ncol = le32(hd + 46);
    int topdown = h < 0;
    if (topdown) h = -h;
    if (dib < 40 || w <= 0 || h <= 0 || w > max_w || h > max_h) return -1;
    /* BI_RGB; BI_BITFIELDS only for 32-bit (almost always plain BGRA masks) */
    if (!(comp == 0 || (comp == 3 && bpp == 32))) return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return -1;

    uint16_t pal[256];
    if (bpp <= 8) {
        uint32_t max = 1u << bpp;
        if (!ncol || ncol > max) ncol = max;
        if (fseek(f, 14 + (long)dib, SEEK_SET)) return -1;
        for (uint32_t i = 0; i < ncol; i++) {
            uint8_t q[4];
            if (fread(q, 1, 4, f) != 4) return -1;
            pal[i] = rgb565(q[2], q[1], q[0]);
        }
        for (uint32_t i = ncol; i < max; i++) pal[i] = 0;
    }

    size_t stride = ((size_t)w * bpp + 31) / 32 * 4;
    uint8_t *row = malloc(stride);
    if (!row) return -1;
    int rc = fseek(f, (long)off, SEEK_SET) ? -1 : 0;
    for (int y = 0; !rc && y < h; y++) {
        if (fread(row, 1, stride, f) != stride) { rc = -1; break; }
        uint16_t *o = out + (size_t)(topdown ? y : h - 1 - y) * w;
        for (int x = 0; x < w; x++) {
            switch (bpp) {
            case 24: o[x] = rgb565(row[3 * x + 2], row[3 * x + 1], row[3 * x]); break;
            case 32: o[x] = rgb565(row[4 * x + 2], row[4 * x + 1], row[4 * x]); break;
            case 8:  o[x] = pal[row[x]]; break;
            case 4:  o[x] = pal[(row[x / 2] >> (x & 1 ? 0 : 4)) & 0xf]; break;
            default: o[x] = pal[(row[x / 8] >> (7 - (x & 7))) & 1]; break;
            }
        }
    }
    free(row);
    if (!rc) { *pw = w; *ph = h; }
    return rc;
}
