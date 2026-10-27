/* test_art_job -- worker handover rules for album art (src/ui_art_job.c):
 * generation check, cancel, the tile letting go before the worker finishes,
 * and no leak/race in any order. Run under ASan (run.sh) and TSan:
 *   cc -fsanitize=thread -g -Isrc tests/test_art_job.c src/ui_art_job.c -pthread */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ui_art_job.h"

/* A gate the test opens to let a fake worker finish. */
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int open, saw_cancel; } gate_t;
static void gate_init(gate_t *g) { pthread_mutex_init(&g->mu, NULL); pthread_cond_init(&g->cv, NULL); g->open = 0; g->saw_cancel = 0; }
static void gate_open(gate_t *g) { pthread_mutex_lock(&g->mu); g->open = 1; pthread_cond_broadcast(&g->cv); pthread_mutex_unlock(&g->mu); }

static int fake_work(art_job_t *j)           /* waits for the gate, polling cancel like the decoder does */
{
    gate_t *g = j->ctx;
    for (;;) {
        if (art_job_cancelled(j)) { pthread_mutex_lock(&g->mu); g->saw_cancel = 1; pthread_mutex_unlock(&g->mu); return -1; }
        pthread_mutex_lock(&g->mu); int o = g->open; pthread_mutex_unlock(&g->mu);
        if (o) break;
        usleep(1000);
    }
    j->px = malloc((size_t)j->T * j->T * 2);
    for (int i = 0; i < j->T * j->T; i++) j->px[i] = (uint16_t)(j->gen + i);
    j->bytes = 1234;
    return 0;
}
static int fail_work(art_job_t *j) { (void)j; return -1; }

static void wait_done(art_job_t *j) { while (!art_job_done(j)) usleep(500); }

int main(void)
{
    gate_t g;

    /* 1. normal: current generation gets the pixels, exactly once */
    gate_init(&g);
    art_job_t *j = art_job_start(7, "a.flac", 4, fake_work, &g);
    assert(j);
    assert(!art_job_done(j) && art_job_take(j, 7) == NULL);   /* nothing before done */
    gate_open(&g); wait_done(j);
    uint16_t *px = art_job_take(j, 7);
    assert(px && px[0] == 7 && px[15] == 7 + 15 && j->bytes == 1234);
    assert(art_job_take(j, 7) == NULL);                       /* ownership moved */
    free(px); art_job_release(j);

    /* 2. stale generation (song changed meanwhile): result dropped, freed by release */
    gate_init(&g);
    j = art_job_start(8, "b.flac", 4, fake_work, &g);
    gate_open(&g); wait_done(j);
    assert(art_job_take(j, 9) == NULL);
    art_job_release(j);

    /* 3. cancel while the worker runs: it stops early, nothing handed over */
    gate_init(&g);
    j = art_job_start(10, "c.flac", 4, fake_work, &g);
    art_job_cancel(j); wait_done(j);
    assert(g.saw_cancel && art_job_take(j, 10) == NULL);
    art_job_release(j);

    /* 4. tile deleted first (UI drops its ref while the worker still runs):
     *    the worker's release must free everything -- LSan checks the leak */
    gate_init(&g);
    j = art_job_start(11, "d.flac", 4, fake_work, &g);
    art_job_cancel(j); art_job_release(j);                    /* j is the worker's now */
    gate_open(&g);
    usleep(50000);

    /* 4b. same, but the worker completes successfully after the UI let go */
    gate_t g2; gate_init(&g2);
    j = art_job_start(12, "e.flac", 4, fake_work, &g2);
    art_job_release(j);
    gate_open(&g2);
    usleep(50000);

    /* 5. failed fetch: done, nothing to take */
    j = art_job_start(13, "f.flac", 4, fail_work, NULL);
    wait_done(j);
    assert(art_job_take(j, 13) == NULL);
    art_job_release(j);

    /* 6. churn: rapid song changes, each abandoning the previous job */
    gate_t gs[32];
    art_job_t *prev = NULL;
    for (int i = 0; i < 32; i++) {
        gate_init(&gs[i]);
        if (prev) { art_job_cancel(prev); art_job_release(prev); }
        prev = art_job_start(100 + i, "x", 8, fake_work, &gs[i]);
        assert(prev);
        if (i % 2) gate_open(&gs[i]);
    }
    gate_open(&gs[31]); wait_done(prev);
    px = art_job_take(prev, 131);
    assert(px && px[0] == 131);
    free(px); art_job_release(prev);
    for (int i = 0; i < 32; i++) gate_open(&gs[i]);
    usleep(100000);                                           /* let abandoned workers exit */
    puts("art_job: ok");
    return 0;
}
