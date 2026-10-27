#include "binfont_logic.h"

/* lv_font_conv bin layout: little-endian sections, each "u32 length (incl.
 * this 8-byte prefix), 4-char label, body": head, cmap, loca, glyf[, kern]. */

static uint32_t u8at(const uint8_t *b, size_t o)  { return b[o]; }
static uint32_t u16at(const uint8_t *b, size_t o) { return b[o] | (uint32_t)b[o + 1] << 8; }
static uint32_t u32at(const uint8_t *b, size_t o) { return u16at(b, o) | u16at(b, o + 2) << 16; }

/* Section at off with this label: its length (>= 8, inside the file), or 0. */
static size_t section(const uint8_t *b, size_t n, size_t off, const char *label)
{
    if (off > n || n - off < 8) return 0;
    uint32_t len = u32at(b, off);
    if (len < 8 || len > n - off) return 0;
    for (int i = 0; i < 4; i++) if (b[off + 4 + i] != (uint8_t)label[i]) return 0;
    return len;
}

/* MSB-first bit reader over glyf, as the loader's read_bits. */
static uint32_t bits(const uint8_t *b, size_t *bit, int nb)
{
    uint32_t v = 0;
    for (int i = 0; i < nb; i++, (*bit)++) v = v << 1 | (b[*bit / 8] >> (7 - *bit % 8) & 1);
    return v;
}

int binfont_check(const uint8_t *b, size_t n)
{
    if (!b || n > BINFONT_MAX_SIZE) return -1;

    /* head: font_header_bin_t, 40 bytes after the prefix */
    size_t hl = section(b, n, 0, "head");
    if (hl < 8 + 40) return -1;
    uint32_t tables = u16at(b, 12);
    int ascent = (int16_t)u16at(b, 16), descent = (int16_t)u16at(b, 18);
    uint32_t loc_fmt = u8at(b, 34), id_fmt = u8at(b, 35), bpp = u8at(b, 37);
    uint32_t xy_bits = u8at(b, 38), wh_bits = u8at(b, 39), adv_bits = u8at(b, 40), compr = u8at(b, 41);
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) return -1;
    if (compr != 0 || loc_fmt > 1 || id_fmt > 1) return -1;              /* LV_USE_FONT_COMPRESSED is off */
    if (xy_bits < 1 || xy_bits > 8 || wh_bits > 8 || adv_bits > 16) return -1;   /* glyph dsc field widths */
    if (ascent - descent < 1 || ascent - descent > BINFONT_MAX_LINE_H) return -1;

    /* loca + glyf first: the cmaps and kerning are checked against the glyph count */
    size_t co = hl, cl = section(b, n, co, "cmap");
    if (cl < 12) return -1;
    size_t lo = co + cl, ll = section(b, n, lo, "loca");
    if (ll < 12) return -1;
    uint32_t glyphs = u32at(b, lo + 8), esz = loc_fmt ? 4 : 2;
    if (glyphs < 1 || glyphs > BINFONT_MAX_GLYPHS || (ll - 12) / esz < glyphs) return -1;
    size_t go = lo + ll, gl = section(b, n, go, "glyf");
    if (!gl) return -1;
    uint32_t nbits = adv_bits + 2 * xy_bits + 2 * wh_bits;
    for (uint32_t i = 1; i < glyphs; i++) {          /* the loader zeroes glyph 0 */
        uint32_t off = esz == 4 ? u32at(b, lo + 12 + i * 4) : u16at(b, lo + 12 + i * 2);
        uint32_t next = i + 1 < glyphs ? (esz == 4 ? u32at(b, lo + 12 + (i + 1) * 4)
                                                   : u16at(b, lo + 12 + (i + 1) * 2)) : gl;
        if (off > gl || gl - off < (nbits + 7) / 8) return -1;   /* header bits inside glyf */
        size_t bit = (go + off) * 8 + adv_bits + 2 * xy_bits;
        uint32_t w = bits(b, &bit, wh_bits), h = bits(b, &bit, wh_bits);
        if (!w || !h) continue;
        if (next > gl || next < off + nbits / 8) return -1;
        uint32_t have = next - off - nbits / 8;        /* the loader's bmp_size */
        if (!have || have * 8 < w * h * bpp) return -1;
    }

    /* cmaps: every reachable glyph id below the glyph count */
    uint32_t ncm = u32at(b, co + 8);
    if (ncm > BINFONT_MAX_CMAPS || (cl - 12) / 16 < ncm) return -1;
    for (uint32_t i = 0; i < ncm; i++) {
        size_t t = co + 12 + i * 16;
        uint32_t data = u32at(b, t), len = u16at(b, t + 8), start = u16at(b, t + 10);
        uint32_t cnt = u16at(b, t + 12), type = u8at(b, t + 14);
        if (data > cl) return -1;
        size_t room = cl - data, d = co + data;
        if (type == 0) {                               /* FORMAT0_FULL: u8 offset per code point */
            if (cnt < len || room < cnt) return -1;
            for (uint32_t k = 0; k < len; k++) if (start + b[d + k] >= glyphs) return -1;
        } else if (type == 2) {                        /* FORMAT0_TINY: consecutive ids */
            if (len && start + len - 1 >= glyphs) return -1;
        } else if (type == 1 || type == 3) {           /* SPARSE_FULL / SPARSE_TINY */
            if (room / 2 < cnt * (type == 1 ? 2u : 1u)) return -1;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t id = start + (type == 1 ? u16at(b, d + cnt * 2 + k * 2) : k);
                if (id >= glyphs) return -1;
            }
        } else return -1;
    }

    /* kerning (only read with 4+ tables) */
    if (tables < 4) return 0;
    size_t ko = go + gl, kl = section(b, n, ko, "kern");
    if (kl < 12) return -1;
    uint32_t ktype = u8at(b, ko + 8);
    if (ktype == 0) {                                  /* sorted pairs: searched, never indexed by id */
        if (kl < 16) return -1;
        uint64_t pairs = u32at(b, ko + 12);
        if ((uint64_t)kl - 16 < pairs * (id_fmt ? 4 : 2) + pairs) return -1;
    } else if (ktype == 3) {                           /* class tables indexed by glyph id */
        if (kl < 16) return -1;
        uint32_t map = u16at(b, ko + 12), rows = u8at(b, ko + 14), cols = u8at(b, ko + 15);
        if (map < glyphs || kl - 16 < 2 * map + rows * cols) return -1;
        for (uint32_t k = 0; k < map; k++)
            if (b[ko + 16 + k] > rows || b[ko + 16 + map + k] > cols) return -1;
    } else return -1;
    return 0;
}
