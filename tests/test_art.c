#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_art.h"
#include "ui_mpd.h"

static unsigned char *slurp(const char *f, int *n)
{
    FILE *fp = fopen(f, "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); *n = (int)ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(*n); assert(fread(b, 1, *n, fp) == (size_t)*n); fclose(fp); return b;
}
static int red(uint16_t p)  { return (p >> 11) > 24 && ((p >> 5) & 0x3f) < 12 && (p & 0x1f) < 6; }
static int blue(uint16_t p) { return (p & 0x1f) > 24 && ((p >> 5) & 0x3f) < 12 && (p >> 11) < 6; }

static void check(const char *f, int T)
{
    int n; unsigned char *j = slurp(f, &n);
    uint16_t *px = calloc(T * T, 2);
    assert(ui_art_decode(j, n, px, T) == 0);
    /* left half red, right half blue, every row -- proves subsampling covers the tile */
    for (int y = 0; y < T; y++) {
        assert(red(px[y * T + 2]));
        assert(blue(px[y * T + T - 3]));
    }
    free(px); free(j);
}

int main(void)
{
    /* Both live tiles (Home preview + Now Playing) are ui_art_create(.., 120):
     * a 1 px border each side leaves a 118 px decode. */
    check("tests/art_600.jpg", 118);   /* big cover -> shrink to the tile */
    check("tests/art_600.jpg", 94);    /* non-tile odd size: shrink still covers every row */
    check("tests/art_50.jpg", 118);    /* tiny cover -> upscale to the tile */
    /* progressive JPEG: tjpgd refuses -> caller keeps the placeholder */
    int n; unsigned char *j = slurp("tests/art_prog.jpg", &n);
    uint16_t px[16];
    assert(ui_art_decode(j, n, px, 4) != 0); free(j);
    /* garbage input */
    assert(ui_art_decode((const unsigned char *)"not a jpeg", 10, px, 4) != 0);

    /* MPD binary-reply header parser */
    int size = -1, nbin = -1;
    assert(mpd_bin_hdr("size: 51234", &size, &nbin) == 1 && size == 51234);
    assert(mpd_bin_hdr("type: image/jpeg", &size, &nbin) == 0);
    assert(mpd_bin_hdr("binary: 8192", &size, &nbin) == 2 && nbin == 8192);
    assert(mpd_bin_hdr("OK", &size, &nbin) == 3);
    assert(mpd_bin_hdr("ACK [50@0] {readpicture} No file exists", &size, &nbin) == -1);
    assert(mpd_bin_hdr("OKAY", &size, &nbin) == 0);   /* not a bare OK */
    puts("art OK");
    return 0;
}
