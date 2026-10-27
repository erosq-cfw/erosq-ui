#ifndef EROSQ_STRTAB_H
#define EROSQ_STRTAB_H
/* Growable string table: every string in one malloc'd blob, plus an offset
 * array. Browse lists (thousands of artists/albums/files) live here, on the
 * libc heap, instead of as LVGL objects in the fixed 384 KB LVGL pool.
 * Dependency-free; tests/test_mpd.c. */

typedef struct {
    char *buf;  int len, cap;    /* NUL-separated strings */
    int  *off;  int n,   ncap;   /* start of string i in buf */
    int   max;                   /* entry limit (0 = STRTAB_MAX) */
} strtab_t;

#define STRTAB_MAX 50000         /* ponytail: bound so a corrupt reply can't eat RAM */

/* Append s[0..len) (len < 0: strlen). Index, or -1 (out of memory / at max). */
int  strtab_add(strtab_t *t, const char *s, int len);
static inline const char *strtab_get(const strtab_t *t, int i) { return t->buf + t->off[i]; }
void strtab_free(strtab_t *t);

/* Sort case-insensitively and merge entries that differ only in ASCII case,
 * keeping the spelling with the fewest capitals (mpd_fold_case's rule).
 * Returns the new count, or -1 on allocation failure (table unchanged). */
int  strtab_fold_case(strtab_t *t);

#endif
