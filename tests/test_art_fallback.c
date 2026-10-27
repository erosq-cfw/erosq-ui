/* The art worker (ui_art_fetch) against tests/fake_mpd.py: an embedded picture
 * TJpgDec can't decode (progressive) falls back to the folder's cover file;
 * with no cover file the job fails (the tile keeps its placeholder) instead of
 * showing a half-decoded buffer. argv: baseline jpeg, expect ("cover"|"none"). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui_art.h"
#include "ui_art_job.h"

int main(int argc, char **argv)
{
    assert(argc == 3);
    enum { T = 118 };
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *jpg = malloc(n); assert(fread(jpg, 1, n, f) == (size_t)n); fclose(f);
    static uint16_t ref[T * T];
    assert(ui_art_decode_rect(jpg, (int)n, ref, T, T) == 0);   /* what the cover should look like */

    art_job_t *j = art_job_start(1, "a/b.flac", T, ui_art_fetch, NULL);
    assert(j);
    for (int i = 0; i < 1000 && !art_job_done(j); i++) usleep(10000);
    assert(art_job_done(j));
    uint16_t *px = art_job_take(j, 1);
    if (!strcmp(argv[2], "cover")) {
        assert(px && memcmp(px, ref, sizeof ref) == 0);
        assert(j->bytes == n);                                   /* the cover's size, not the embedded one's */
    } else {
        assert(px == NULL);
    }
    free(px); art_job_release(j); free(jpg);
    printf("art_fallback: %s OK\n", argv[2]);
    return 0;
}
