#ifndef UI_ART_JOB_H
#define UI_ART_JOB_H
#include <stdint.h>
/* One album-art fetch+decode running on its own detached worker thread, so a
 * big cover (seconds of MPD readpicture + TJpgDec) never blocks the LVGL
 * thread. Pure pthreads, no LVGL/MPD (host-tested in test_art_job.c).
 *
 * Ownership: the job is refcounted -- one ref for the tile (UI thread), one
 * for the worker. Whoever drops the last ref frees it, including any pixels
 * nobody took. The UI thread polls art_job_done() from an LVGL timer and
 * takes the result there, so no LVGL call ever happens off the LVGL thread.
 *
 * Generations: each tile bumps its counter on every song change; a result is
 * only handed over if its generation still matches and it wasn't cancelled,
 * so a slow fetch for the previous song can never paint over the current one. */
typedef struct art_job art_job_t;

/* Worker body: fills job->px (malloc'd T*T*2) and returns 0, or nonzero on
 * failure/cancel. Should poll art_job_cancelled() to stop early. */
typedef int (*art_work_fn)(art_job_t *j);

struct art_job {
    /* set at start, read-only afterwards */
    unsigned gen; int T; char uri[256]; art_work_fn work; void *ctx;
    /* written by the worker before it marks the job done */
    uint16_t *px; int rc, bytes, ms;
    /* private: guarded by the job's mutex */
    void *priv;
};

art_job_t *art_job_start(unsigned gen, const char *uri, int T, art_work_fn work, void *ctx);
int        art_job_cancelled(art_job_t *j);   /* worker: stop early? */
void       art_job_cancel(art_job_t *j);      /* UI: song changed / tile gone */
int        art_job_done(art_job_t *j);        /* UI: worker finished? */
/* UI: take the pixels if the job finished OK, wasn't cancelled and `cur_gen`
 * is still its generation; NULL otherwise. Caller owns the returned buffer. */
uint16_t  *art_job_take(art_job_t *j, unsigned cur_gen);
void       art_job_release(art_job_t *j);     /* drop one ref (UI or worker) */
#endif
