#ifndef EROSQ_UI_PODCAST_LOGIC_H
#define EROSQ_UI_PODCAST_LOGIC_H
#include <stddef.h>
#include <stdio.h>

#define UP_PAGE        100    /* rows per picker page; 500+ LVGL rows at once
                               * would blow the object pool on this box */
#define UP_FEEDS_MAX   16
#define UP_NAME_MAX    64
#define UP_URL_MAX     512
#define UP_FILE_MAX    128
#define UP_DATE_MAX    40
#define UP_TITLE_MAX   121

typedef struct { char name[UP_NAME_MAX]; char url[UP_URL_MAX]; } up_feed_t;
typedef struct {
    char file[UP_FILE_MAX];
    char date[UP_DATE_MAX];
    char title[UP_TITLE_MAX];
} up_ep_t;

/* Episode state, as rendered in the picker. */
enum { UP_ST_AVAIL = 0, UP_ST_WANTED = 1, UP_ST_ONDISK = 2 };

/* Parse podcasts.txt: "Name|URL" or a bare URL (name derived from the host).
 * '#' comments and blank lines ignored; names sanitised to [A-Za-z0-9 ._-];
 * non-http(s) URLs and over-long lines rejected. Returns feeds written. */
int up_parse_feeds(const char *text, up_feed_t *out, int max);

/* Read index.tsv rows [page*per_page, +per_page) into `out`. Rows with fewer
 * than 4 fields or an unsafe filename are skipped. `*total` receives the count
 * of VALID rows in the whole file (the stream is read to the end). Returns the
 * number of rows written to `out`. */
int up_index_window(FILE *f, int page, int per_page, up_ep_t *out, int max, int *total);

/* Pages needed for `total` rows. Zero rows -> zero pages, so a caller never
 * renders "page 1 of 0". Defensive on negative/zero inputs. */
int up_page_count(int total, int per_page);
/* Clamp a page number into [0, pages-1]; returns 0 when pages <= 0. */
int up_page_clamp(int page, int pages);

typedef struct {
    char state[16];
    int  fi, fn, ei, en, pct;
    char text[64];
} up_status_t;
/* Parse one /run/podfetch.status line. Returns 1 on success, 0 on garbage or a
 * truncated line (the poller then leaves the previous status on screen). */
int up_parse_status(const char *line, up_status_t *out);

/* wanted.txt as a newline-separated text buffer. Whole-line matching only, so
 * "a.mp" never matches "a.mp3". */
int up_wanted_has(const char *buf, const char *file);
/* Toggle membership in place. Returns 1 if now present, 0 if removed,
 * -1 if it could not be added because `buf` has no room (buf unchanged). */
int up_wanted_toggle(char *buf, size_t buflen, const char *file);

/* Format a picker row: "* ", "+ " or "  " marker, then title, then the date's
 * "DD Mon YYYY". ASCII markers only -- the iPod fonts may lack check/arrow
 * glyphs. Returns the length written. */
int up_row_label(const up_ep_t *ep, int state, char *out, size_t outlen);

/* Reduce an RFC-822 pubDate to "DD Mon YYYY". Handles BOTH shapes seen in the
 * wild -- with the optional weekday ("Tue, 11 Aug 2026 16:00:00 GMT") and
 * without it ("11 Aug 2026 16:00:00 GMT"). A date with fewer than three
 * whitespace-separated tokens is passed through (capped), and an empty or
 * whitespace-only date yields an empty string. Exposed for testing. */
void up_short_date(const char *in, char *out, size_t outlen);

/* Coarse "how old is this index" string for the feed row: "just now", "5m ago",
 * "2h ago", "3d ago". Negative ages (clock skew, a card written by a PC with a
 * future clock) read "just now" rather than a negative number. The spec makes
 * this load-bearing: signed episode URLs expire, so an index older than a few
 * hours must be refreshed before downloading. Returns the length written. */
int up_age_str(long secs, char *out, size_t outlen);

/* A terminal status ("done"/"error") that is older than UP_STATUS_STALE_SECS
 * describes a run that finished long ago, so the UI must show idle instead of
 * advertising "Finished" on entry hours later. Returns 1 when the caller should
 * treat the status file as idle. */
#define UP_STATUS_STALE_SECS 120
int up_status_stale(const char *state, long age_secs);

/* Parse /run/podfetch.lock/pid. Returns the pid, or 0 if the buffer holds no
 * plausible pid. The caller checks /proc/<pid> -- a lock whose owner is gone is
 * stale, which is exactly the rule podfetch.sh itself applies. */
int up_parse_pid(const char *buf);

/* ---- on-disk episodes no longer in the index ----
 *
 * A feed that has aged an episode out leaves the file on the card with no index
 * row, so it is invisible in the picker AND undeletable -- permanently orphaned
 * space. These three let the picker list them: readdir fills `up_disk_t`, the
 * index stream marks the ones still present, the rest are orphans. */
#define UP_DISK_MAX    256   /* readdir candidates held at once (~36 KB) */
#define UP_ORPHAN_ROWS 12    /* orphan rows appended to the last page */
typedef struct {
    char file[UP_FILE_MAX];
    long mtime;
    int  in_index;
} up_disk_t;

/* Set ->in_index on every entry whose filename is field 1 of a row in `f`.
 * Over-long rows are skipped, matching up_index_window() exactly, so a row that
 * the picker cannot render is consistently treated as absent. Returns the count
 * marked. */
int up_disk_mark_indexed(FILE *f, up_disk_t *d, int n);
/* Compact `d` to only the entries with in_index == 0, preserving order.
 * Returns the new count. */
int up_disk_keep_orphans(up_disk_t *d, int n);
/* Newest first, ties broken by name so the order is deterministic. */
void up_disk_sort_mtime(up_disk_t *d, int n);

#endif
