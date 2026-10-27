/* test_picstream -- mpd_pic_* against a strict fake MPD (fake_mpd_strict.py)
 * with a picture several chunks long: odd-sized reads and skips must cross
 * chunk boundaries exactly; the 128 KB binarylimit must actually be in force
 * (call count); and a slow (cold) first chunk must not time out. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_mpd.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); int n = (int)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *ref = malloc(n); assert(fread(ref, 1, n, f) == (size_t)n); fclose(f);
    assert(n > 2 * 131072);                       /* fixture spans >= 3 chunks */

    mpd_pic_t p;
    assert(mpd_pic_open(&p, "a/b \"quoted\".flac", 4 * 1024 * 1024) == n);
    unsigned char *got = malloc(n); int pos = 0, k = 0;
    while (pos < n) {
        int want = 1 + (k * 7919) % 40000;        /* odd sizes */
        if (pos + want > n) want = n - pos;
        if (k % 5 == 3) {                         /* every 5th: skip (tjpgd buf=NULL) */
            assert(mpd_pic_read(&p, NULL, want) == want);
            memcpy(got + pos, ref + pos, want);
        } else {
            assert(mpd_pic_read(&p, got + pos, want) == want);
        }
        pos += want; k++;
    }
    assert(memcmp(got, ref, n) == 0);
    assert(mpd_pic_read(&p, got, 10) == 0);       /* past the end: short, not stuck */
    assert(p.chunks == (n + 131071) / 131072);    /* binarylimit honoured: 4 calls, not 49 */
    mpd_pic_close(&p);

    /* size cap still refuses big pictures */
    assert(mpd_pic_open(&p, "x", 1000) <= 0);
    mpd_pic_close(&p);
    free(got); free(ref);
    printf("picstream: %d bytes in %d reads OK\n", n, k);
    return 0;
}
