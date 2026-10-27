/* test_art_stream -- ui_art_decode_stream pulling from a chunked source (like
 * mpd_pic_read: refills every `step` bytes, but always satisfies the whole
 * request until EOF, as tjpgd treats a short read as an error) must give the
 * same tile as the in-memory decode; a truncated stream must fail cleanly. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_art.h"

typedef struct { const unsigned char *p; int len, pos, step; } src_t;

static size_t dribble(void *s, uint8_t *buf, size_t n)   /* copies in `step`-sized refills */
{
    src_t *c = s;
    size_t done = 0;
    while (done < n && c->pos < c->len) {
        int k = c->step;
        if (k > (int)(n - done)) k = (int)(n - done);
        if (k > c->len - c->pos) k = c->len - c->pos;
        if (buf) memcpy(buf + done, c->p + c->pos, k);
        c->pos += k; done += k;
    }
    return done;
}

static unsigned char *slurp(const char *f, int *n)
{
    FILE *fp = fopen(f, "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); *n = (int)ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(*n); assert(fread(b, 1, *n, fp) == (size_t)*n); fclose(fp); return b;
}

int main(void)
{
    const char *files[] = { "tests/art_600.jpg", "tests/art_50.jpg" };
    const int steps[] = { 1, 7, 512, 1 << 20 };
    const int T = 118;
    for (int f = 0; f < 2; f++) {
        int n; unsigned char *j = slurp(files[f], &n);
        uint16_t *ref = calloc(T * T, 2), *got = calloc(T * T, 2);
        assert(ui_art_decode(j, n, ref, T) == 0);
        for (int s = 0; s < 4; s++) {
            memset(got, 0, T * T * 2);
            src_t c = { j, n, 0, steps[s] };
            assert(ui_art_decode_stream(dribble, &c, got, T) == 0);
            assert(memcmp(got, ref, T * T * 2) == 0);
        }
        /* stream that dies half way (MPD hiccup mid-cover): error, no crash */
        src_t c = { j, n / 2, 0, 4096 };
        assert(ui_art_decode_stream(dribble, &c, got, T) != 0);
        free(ref); free(got); free(j);
    }
    puts("art_stream: ok");
    return 0;
}
