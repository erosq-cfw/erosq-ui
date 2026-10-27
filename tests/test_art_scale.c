/* test_art_scale -- TJpgDec descale choice (ui_art_scale) and that every
 * descale path (1/2, 1/4, 1/8 and none) still fills the whole tile with the
 * right colours (left half red, right half blue fixture). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_art.h"

static unsigned char *slurp(const char *f, int *n)
{
    FILE *fp = fopen(f, "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); *n = (int)ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(*n); assert(fread(b, 1, *n, fp) == (size_t)*n); fclose(fp); return b;
}
static int red(uint16_t p)  { return (p >> 11) > 24 && ((p >> 5) & 0x3f) < 12 && (p & 0x1f) < 6; }
static int blue(uint16_t p) { return (p & 0x1f) > 24 && ((p >> 5) & 0x3f) < 12 && (p >> 11) < 6; }

int main(void)
{
    assert(ui_art_scale(4000, 4000, 118) == 3);   /* 500 px */
    assert(ui_art_scale(1425, 1425, 118) == 3);   /* 178 */
    assert(ui_art_scale(944, 944, 118) == 3);     /* 118 exactly */
    assert(ui_art_scale(943, 943, 118) == 2);
    assert(ui_art_scale(600, 600, 118) == 2);     /* 150 */
    assert(ui_art_scale(300, 300, 118) == 1);     /* 150 */
    assert(ui_art_scale(236, 236, 118) == 1);
    assert(ui_art_scale(235, 235, 118) == 0);
    assert(ui_art_scale(118, 118, 118) == 0);
    assert(ui_art_scale(50, 50, 118) == 0);       /* upscale */
    assert(ui_art_scale(4000, 200, 118) == 0);    /* the short side decides */
    assert(ui_art_scale(0, 0, 118) == 0);

    int n; unsigned char *j = slurp("tests/art_600.jpg", &n);
    int Ts[] = { 75, 118, 150, 300, 600 };          /* s = 3, 2, 2, 1, 0 */
    for (unsigned k = 0; k < sizeof Ts / sizeof *Ts; k++) {
        int T = Ts[k];
        uint16_t *px = calloc((size_t)T * T, 2);
        assert(ui_art_decode(j, n, px, T) == 0);
        for (int y = 0; y < T; y++) { assert(red(px[y * T + 2])); assert(blue(px[y * T + T - 3])); }
        free(px);
    }
    free(j);
    puts("art_scale OK");
    return 0;
}
