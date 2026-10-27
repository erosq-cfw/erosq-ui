/* ui_art_decode_rect: a 320x240 baseline JPEG (tests/art_rect_320x240.jpg, a
 * synthetic gradient with flat colour patches centred on the reference points)
 * decodes at full size with the right channel order (TJpgDec emits B,G,R).
 * Tolerance covers IDCT rounding + RGB565. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_art.h"

static const int ref[][5] = {   /* x, y, r, g, b: 5x5 means (flat-ish areas) */
    {  40,  60, 185, 199, 205 },
    { 160, 120, 217, 223, 237 },
    { 250,  40, 135, 107, 115 },
    { 300, 220,  62,  71,  61 },
    { 100, 200,  84,  47,  20 },   /* red >> blue: catches an R/B swap */
    {  10,  10, 149, 155, 150 },
    {  60, 180,  26, 164, 122 },   /* blue >> red */
};

static int near(int a, int b) { return abs(a - b) <= 10; }

int main(void)
{
    FILE *f = fopen("tests/art_rect_320x240.jpg", "rb");
    assert(f);
    static unsigned char jpg[256 * 1024];
    int len = (int)fread(jpg, 1, sizeof jpg, f);
    fclose(f);
    assert(len > 1000 && len < 64 * 1024);           /* small asset, baseline */

    static uint16_t px[320 * 240];
    memset(px, 0xAA, sizeof px);
    assert(ui_art_decode_rect(jpg, len, px, 320, 240) == 0);
    for (unsigned i = 0; i < sizeof ref / sizeof ref[0]; i++) {
        int r = 0, g = 0, b = 0;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                uint16_t v = px[(ref[i][1] + dy) * 320 + ref[i][0] + dx];
                r += (v >> 11) << 3; g += ((v >> 5) & 0x3f) << 2; b += (v & 0x1f) << 3;
            }
        r = (r + 12) / 25; g = (g + 12) / 25; b = (b + 12) / 25;
        if (!(near(r, ref[i][2]) && near(g, ref[i][3]) && near(b, ref[i][4]))) {
            fprintf(stderr, "px (%d,%d) = %d,%d,%d want %d,%d,%d\n", ref[i][0], ref[i][1],
                    r, g, b, ref[i][2], ref[i][3], ref[i][4]);
            return 1;
        }
    }
    /* every output pixel written (no 0xAAAA hole at the right/bottom edge) */
    int holes = 0;
    for (int i = 0; i < 320 * 240; i++) holes += px[i] == 0xAAAA;
    assert(holes < 50);   /* a real 0xAAAA colour is possible, a missing column is 240+ */

    (void)ui_art_decode_rect(jpg, len / 3, px, 320, 240);   /* truncated: must not crash (ASan) */
    unsigned char junk[64] = { 0xff, 0xd8, 0xff, 0x00 };
    assert(ui_art_decode_rect(junk, sizeof junk, px, 320, 240) != 0);
    printf("art_rect OK\n");
    return 0;
}
