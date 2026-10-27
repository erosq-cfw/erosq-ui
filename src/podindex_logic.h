#ifndef EROSQ_PODINDEX_LOGIC_H
#define EROSQ_PODINDEX_LOGIC_H
#include <stddef.h>

#define PI_NAME_MAX   128
#define PI_TITLE_MAX  121   /* 120 display bytes + NUL */
#define PI_DATE_MAX   40
#define PI_URL_MAX    512
#define PI_RAW_MAX    512   /* character-data accumulator */

/* Derive the on-disk filename from an enclosure URL: strip query and fragment,
 * take the last two path segments joined with '-'. Each retained segment must
 * consist ENTIRELY of pod_name.h whitelist bytes -- a segment that would need
 * any byte dropped refuses the WHOLE name rather than mangling it (e.g.
 * "%2e%2e" is refused outright, never silently reduced to "2e2e"). Returns 1
 * and fills `out`, or 0 if the result would be unsafe or does not fit (empty,
 * leading '.', a segment with a disallowed byte, or truncation). */
int pi_filename_from_url(const char *url, char *out, size_t outlen);

/* Accept an enclosure URL only if it is http/https and free of control
 * characters (a tab or newline would desynchronise the TSV). */
int pi_url_ok(const char *url);

/* Sanitise feed text for display and for TSV safety: drop control characters,
 * collapse whitespace runs (including tabs and newlines) to one space, trim
 * both ends, truncate to outlen-1 bytes on a UTF-8 boundary. Returns the
 * length written. */
size_t pi_sanitise_title(const char *in, char *out, size_t outlen);

/* Item state machine. Driven by expat in podindex.c and by a hand-written
 * event stream in the host test, so item scoping is testable without expat. */
typedef struct {
    int    depth;        /* element nesting depth */
    int    item_depth;   /* depth of the open <item>, -1 if none */
    int    in_title;
    int    in_date;
    char   title[PI_TITLE_MAX];
    char   date[PI_DATE_MAX];
    char   url[PI_URL_MAX];
    char   raw[PI_RAW_MAX];
    size_t rawlen;
    int    raw_trunc;    /* pi_chars() dropped input: the accumulated tail may
                          * be a partial UTF-8 sequence. pi_sanitise_title only
                          * backs off when ITS OWN cap trips, so a raw-side cut
                          * needs its own trim (code review 2026-09-08). */
} pi_state;

void pi_reset(pi_state *s);
void pi_start(pi_state *s, const char *el, const char *const *attrs);
void pi_chars(pi_state *s, const char *data, int len);
/* Returns 1 when an <item> closed with a usable title+enclosure and `row` was
 * filled with a TSV line (no trailing newline); 0 otherwise. */
int  pi_end(pi_state *s, const char *el, char *row, size_t rowlen);

#endif
