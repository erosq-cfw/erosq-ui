/* ui_art_fetch.c -- the art worker body: MPD -> TJpgDec -> RGB565 tile.
 * Runs on a ui_art_job thread: no LVGL here, and nothing shared with the UI
 * thread except the job (see ui_art_job.h for the handover rules). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui_art.h"
#include "ui_art_job.h"
#include "ui_mpd.h"

#define ART_MAX_JPEG (4 * 1024 * 1024)   /* library survey: largest embedded cover 3.67MB (4000x4000) */

typedef struct { mpd_pic_t pic; art_job_t *job; } fsrc_t;

static size_t fetch_in(void *src, uint8_t *buf, size_t n)   /* tjpgd input <- MPD */
{
    fsrc_t *f = src;
    if (art_job_cancelled(f->job)) return 0;                 /* short read = tjpgd aborts */
    int r = mpd_pic_read(&f->pic, buf, (int)n);
    return r > 0 ? (size_t)r : 0;
}

int ui_art_fetch(art_job_t *j)
{
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    fsrc_t f = { .job = j };
    int n = mpd_pic_open(&f.pic, j->uri, ART_MAX_JPEG);
    if (n <= 0) { mpd_pic_close(&f.pic); return -1; }
    j->bytes = n;
    uint16_t *px = malloc((size_t)j->T * j->T * 2);
    int rc = px ? ui_art_decode_stream(fetch_in, &f, px, j->T) : -1;
    /* The embedded picture is one TJpgDec can't decode (progressive JPEG,
     * PNG): the folder's cover file may still be a baseline JPEG. Same tile
     * buffer, so no extra RAM; tjpgd gives up within the first chunk. */
    if (rc && px && !art_job_cancelled(j) && f.pic.verb && !strcmp(f.pic.verb, "readpicture")) {
        mpd_pic_close(&f.pic);
        if ((n = mpd_pic_open_cover(&f.pic, j->uri, ART_MAX_JPEG)) > 0) {
            j->bytes = n;
            rc = ui_art_decode_stream(fetch_in, &f, px, j->T);
        }
    }
    mpd_pic_close(&f.pic);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    j->ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    if (rc || art_job_cancelled(j)) { free(px); return -1; }
    j->px = px;
    return 0;
}
