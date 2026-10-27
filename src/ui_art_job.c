/* ui_art_job.c -- see ui_art_job.h. */
#include "ui_art_job.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct { pthread_mutex_t mu; int refs, cancelled, done; } jpriv_t;
typedef struct { art_job_t job; jpriv_t p; } jbox_t;   /* one allocation */

static jpriv_t *P(art_job_t *j) { return &((jbox_t *)j)->p; }

static void *worker(void *arg)
{
    art_job_t *j = arg;
#ifdef SYS_gettid
    /* single-core SoC: decoding a 4000x4000 cover must not starve the UI thread */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    j->rc = j->work(j);
    pthread_mutex_lock(&P(j)->mu);
    P(j)->done = 1;
    pthread_mutex_unlock(&P(j)->mu);
    art_job_release(j);
    return NULL;
}

art_job_t *art_job_start(unsigned gen, const char *uri, int T, art_work_fn work, void *ctx)
{
    jbox_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    art_job_t *j = &b->job;
    j->gen = gen; j->T = T; j->work = work; j->ctx = ctx; j->rc = -1;
    snprintf(j->uri, sizeof j->uri, "%s", uri);
    pthread_mutex_init(&b->p.mu, NULL);
    b->p.refs = 2;                                 /* tile + worker */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);      /* decode needs ~8 KB; touched pages only */
    pthread_t t;
    int err = pthread_create(&t, &a, worker, j);
    pthread_attr_destroy(&a);
    if (err) { pthread_mutex_destroy(&b->p.mu); free(b); return NULL; }
    return j;
}

int art_job_cancelled(art_job_t *j)
{
    pthread_mutex_lock(&P(j)->mu);
    int c = P(j)->cancelled;
    pthread_mutex_unlock(&P(j)->mu);
    return c;
}

void art_job_cancel(art_job_t *j)
{
    pthread_mutex_lock(&P(j)->mu);
    P(j)->cancelled = 1;
    pthread_mutex_unlock(&P(j)->mu);
}

int art_job_done(art_job_t *j)
{
    pthread_mutex_lock(&P(j)->mu);
    int d = P(j)->done;
    pthread_mutex_unlock(&P(j)->mu);
    return d;
}

uint16_t *art_job_take(art_job_t *j, unsigned cur_gen)
{
    uint16_t *px = NULL;
    pthread_mutex_lock(&P(j)->mu);
    if (P(j)->done && !P(j)->cancelled && j->gen == cur_gen && j->rc == 0) {
        px = j->px; j->px = NULL;
    }
    pthread_mutex_unlock(&P(j)->mu);
    return px;
}

void art_job_release(art_job_t *j)
{
    pthread_mutex_lock(&P(j)->mu);
    int last = --P(j)->refs == 0;
    pthread_mutex_unlock(&P(j)->mu);
    if (!last) return;
    pthread_mutex_destroy(&P(j)->mu);
    free(j->px);
    free(j);                                       /* job is the first member of the box */
}
