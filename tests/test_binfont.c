/* LVGL binary-font check (src/binfont_logic.c). Host cc, assert only. Uses the
 * Pixel Operator fonts the Pixel example theme ships; run from the repo root. */
#include "binfont_logic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    assert(f);
    static uint8_t buf[2][BINFONT_MAX_SIZE];
    static int k;
    uint8_t *b = buf[k++ & 1];
    *n = fread(b, 1, sizeof buf[0], f);
    fclose(f);
    return b;
}

static void put16(uint8_t *b, size_t o, unsigned v) { b[o] = v & 0xff; b[o + 1] = v >> 8; }
static void put32(uint8_t *b, size_t o, unsigned v) { put16(b, o, v & 0xffff); put16(b, o + 2, v >> 16); }
static unsigned get32(const uint8_t *b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24; }

int main(void)
{
    size_t n, nb;
    {   /* the Pixel theme's shipped fonts, checked first: slurp's two buffers are reused (16 px = Pixel Operator's own grid) */
        size_t n16, nb16;
        const uint8_t *r16 = slurp("docs/themes/Pixel/PixelOperator16.bin", &n16);
        const uint8_t *b16 = slurp("docs/themes/Pixel/PixelOperator16-Bold.bin", &nb16);
        assert(binfont_check(r16, n16) == 0 && binfont_check(b16, nb16) == 0);
    }
    const uint8_t *reg = slurp("docs/themes/Pixel/PixelOperator20.bin", &n);
    const uint8_t *bold = slurp("docs/themes/Pixel/PixelOperator20-Bold.bin", &nb);
    assert(binfont_check(reg, n) == 0 && binfont_check(bold, nb) == 0);
    assert(binfont_check(NULL, 0) == -1);

    static uint8_t m[BINFONT_MAX_SIZE];
    for (size_t len = 0; len < n; len++) {             /* every truncation */
        memcpy(m, reg, len);
        assert(binfont_check(m, len) == -1);
    }

    size_t co = get32(reg, 0), lo = co + get32(reg, co), go = lo + get32(reg, lo);
#define BAD(edit) do { memcpy(m, reg, n); edit; assert(binfont_check(m, n) == -1); } while (0)
    BAD(m[37] = 3);                                     /* bpp */
    BAD(m[41] = 1);                                     /* compressed */
    BAD(m[38] = 0);                                     /* xy_bits 0: signed read shifts by -1 */
    BAD(m[39] = 9);                                     /* box wider than the u8 field */
    BAD(put16(m, 16, 200));                             /* line height 203 */
    BAD(m[4] = 'H');                                    /* label */
    BAD(put32(m, co + 8, 100000));                      /* cmap count: huge allocation */
    BAD(put16(m, co + 12 + 10, 60000));                 /* first cmap's glyph_id_start */
    BAD(put32(m, lo + 8, 100000));                      /* glyph count past loca */
    BAD(put32(m, go, 0x7fffffff));                      /* glyf length past the file */
    size_t g1 = go + (reg[lo + 14] | reg[lo + 15] << 8);    /* glyph 1 (16-bit loca); glyph 0 is ignored */
    BAD(m[g1 + 1] = 0xff; m[g1 + 2] = 0xff);            /* a glyph's box bigger than its bitmap */

    /* random corruption never crashes the checker (ASan/UBSan in run.sh) */
    srand(1);
    int ok = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(m, rand() & 1 ? reg : bold, n < nb ? n : nb);
        size_t len = n < nb ? n : nb;
        for (int k = 1 + rand() % 8; k > 0; k--) m[rand() % len] = rand();
        ok += binfont_check(m, len) == 0;
    }
    printf("ok (%d of 20000 mutants pass the check)\n", ok);
    return 0;
}
