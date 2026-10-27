#include "strtab.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int strtab_add(strtab_t *t, const char *s, int len)
{
    if (len < 0) len = (int)strlen(s);
    if (t->n >= (t->max ? t->max : STRTAB_MAX)) return -1;
    if (t->len + len + 1 > t->cap) {
        int c = t->cap ? t->cap : 4096;
        while (c < t->len + len + 1) c *= 2;
        char *b = realloc(t->buf, c);
        if (!b) return -1;
        t->buf = b; t->cap = c;
    }
    if (t->n == t->ncap) {
        int c = t->ncap ? t->ncap * 2 : 256;
        int *o = realloc(t->off, c * sizeof *o);
        if (!o) return -1;
        t->off = o; t->ncap = c;
    }
    memcpy(t->buf + t->len, s, len);
    t->buf[t->len + len] = 0;
    t->off[t->n] = t->len;
    t->len += len + 1;
    return t->n++;
}

void strtab_free(strtab_t *t)
{
    free(t->buf); free(t->off);
    memset(t, 0, sizeof *t);
}

static const strtab_t *s_sort;   /* qsort context: single-threaded UI */
static int cmp_ci(const void *a, const void *b)
{
    return strcasecmp(strtab_get(s_sort, *(const int *)a), strtab_get(s_sort, *(const int *)b));
}
static int caps(const char *s)
{
    int n = 0;
    for (; *s; s++) n += (*s >= 'A' && *s <= 'Z');
    return n;
}

int strtab_fold_case(strtab_t *t)
{
    if (t->n < 2) return t->n;
    int *ix = malloc(t->n * sizeof *ix);
    strtab_t out = { .max = t->max };
    if (!ix) return -1;
    for (int i = 0; i < t->n; i++) ix[i] = i;
    s_sort = t;
    qsort(ix, t->n, sizeof *ix, cmp_ci);
    for (int i = 0; i < t->n; ) {
        int best = ix[i], j = i + 1;
        for (; j < t->n && !strcasecmp(strtab_get(t, ix[j]), strtab_get(t, best)); j++)
            if (caps(strtab_get(t, ix[j])) < caps(strtab_get(t, best))) best = ix[j];
        if (strtab_add(&out, strtab_get(t, best), -1) < 0) { free(ix); strtab_free(&out); return -1; }
        i = j;
    }
    free(ix);
    strtab_free(t);
    *t = out;
    return t->n;
}
