#include "ui_podcast_logic.h"
#include "pod_name.h"
#include "pod_utf8.h"
#include <string.h>
#include <stdlib.h>

/* ---- feeds ---- */

/* The sanitiser itself lives in pod_name.h as pod_dir_name(): podfetch.sh
 * implements the same rule in shell and the two MUST agree byte for byte, so
 * the C side is single-sourced in the header next to the filename rule rather
 * than kept here where only this file can see it. */

int up_parse_feeds(const char *text, up_feed_t *out, int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *line = p;
        p = nl ? nl + 1 : p + len;

        while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
        if (!len || *line == '#') continue;
        if (len >= UP_URL_MAX + UP_NAME_MAX) continue;      /* absurd line */

        const char *bar = memchr(line, '|', len);
        const char *url = bar ? bar + 1 : line;
        size_t urllen = bar ? len - (size_t)(bar - line) - 1 : len;
        if (urllen == 0 || urllen >= UP_URL_MAX) continue;
        if (!(urllen > 8 && memcmp(url, "https://", 8) == 0) &&
            !(urllen > 7 && memcmp(url, "http://", 7) == 0)) continue;

        up_feed_t *f = &out[n];
        memcpy(f->url, url, urllen);
        f->url[urllen] = 0;

        if (bar) {
            pod_dir_name(line, (size_t)(bar - line), f->name, sizeof f->name);
        } else {
            /* derive from the host: strip scheme, stop at the first '/' */
            const char *h = strstr(f->url, "://");
            h = h ? h + 3 : f->url;
            size_t hl = strcspn(h, "/");
            pod_dir_name(h, hl, f->name, sizeof f->name);
        }
        if (!f->name[0]) continue;   /* unusable name: skip, never invent one */
        n++;
    }
    return n;
}

/* ---- index windowing ---- */

static int split_row(char *line, up_ep_t *ep)
{
    char *f[4];
    int i = 0;
    for (char *s = line; i < 4; i++) {
        f[i] = s;
        char *t = strchr(s, '\t');
        if (!t) { i++; break; }
        *t = 0;
        s = t + 1;
    }
    if (i < 4) return 0;                     /* too few fields */
    if (!pod_name_ok(f[0])) return 0;        /* unsafe filename */
    snprintf(ep->file,  sizeof ep->file,  "%s", f[0]);
    snprintf(ep->date,  sizeof ep->date,  "%s", f[1]);
    snprintf(ep->title, sizeof ep->title, "%s", f[2]);
    return 1;
}

int up_index_window(FILE *f, int page, int per_page, up_ep_t *out, int max, int *total)
{
    char line[1024];
    int valid = 0, written = 0;
    /* per_page<=0 falls back to UP_PAGE here, unlike up_page_count/
     * up_page_clamp which return 0 for per_page<=0 -- deliberate: a bad
     * per_page must still let the caller page through the file with a sane
     * window rather than silently returning nothing. Not a bug to fix. */
    int from = (page < 0 ? 0 : page) * (per_page > 0 ? per_page : UP_PAGE);
    int to   = from + (per_page > 0 ? per_page : UP_PAGE);

    if (total) *total = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n') line[--l] = 0;
        else if (l == sizeof line - 1) {
            /* Buffer filled with no newline in it. Two different things can
             * cause this, and they must not be treated alike: a genuinely
             * over-long line (more bytes still follow), or a valid final
             * row that happens to end exactly at EOF with no trailing
             * newline. Peek one more byte to tell them apart. */
            int c = fgetc(f);
            if (c == EOF) {
                /* The whole row IS these 1023 bytes; fall through and treat
                 * `line` as a normal, complete row. */
            } else {
                /* More line follows: discard it (including the newline
                 * that ends it, if any) and skip this row entirely -- the
                 * discarded tail must never be mistaken for a fresh row. */
                while (c != '\n' && c != EOF) c = fgetc(f);
                continue;
            }
        }
        if (!l) continue;
        up_ep_t ep;
        if (!split_row(line, &ep)) continue;
        if (valid >= from && written < max && valid < to) out[written++] = ep;
        valid++;
    }
    if (total) *total = valid;
    return written;
}

int up_page_count(int total, int per_page)
{
    if (total <= 0 || per_page <= 0) return 0;
    return (total + per_page - 1) / per_page;
}

int up_page_clamp(int page, int pages)
{
    if (pages <= 0) return 0;
    if (page < 0) return 0;
    return page >= pages ? pages - 1 : page;
}

/* ---- status ---- */

int up_parse_status(const char *line, up_status_t *out)
{
    if (!line || !*line) return 0;
    char st[16];
    int fi, fn, ei, en, pct, consumed = 0;
    /* %n gives the offset of the free-text tail, which may contain spaces. */
    if (sscanf(line, "%15s %d/%d %d/%d %d %n",
               st, &fi, &fn, &ei, &en, &pct, &consumed) < 6) return 0;
    snprintf(out->state, sizeof out->state, "%s", st);
    out->fi = fi; out->fn = fn; out->ei = ei; out->en = en; out->pct = pct;
    out->text[0] = 0;
    if (consumed > 0) {
        snprintf(out->text, sizeof out->text, "%s", line + consumed);
        size_t l = strlen(out->text);
        while (l && (out->text[l - 1] == '\n' || out->text[l - 1] == '\r')) out->text[--l] = 0;
    }
    return 1;
}

/* ---- wanted list ---- */

static const char *find_line(const char *buf, const char *file)
{
    size_t fl = strlen(file);
    for (const char *p = buf; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l == fl && memcmp(p, file, fl) == 0) return p;
        if (!nl) break;
        p = nl + 1;
    }
    return NULL;
}

int up_wanted_has(const char *buf, const char *file)
{
    if (!buf || !file || !*file) return 0;
    return find_line(buf, file) != NULL;
}

int up_wanted_toggle(char *buf, size_t buflen, const char *file)
{
    if (!buf || !file || !*file) return -1;
    char *hit = (char *)find_line(buf, file);
    if (hit) {
        char *nl = strchr(hit, '\n');
        char *rest = nl ? nl + 1 : hit + strlen(hit);
        memmove(hit, rest, strlen(rest) + 1);
        return 0;
    }
    size_t used = strlen(buf), add = strlen(file) + 1;
    if (used + add + 1 > buflen) return -1;          /* no room; unchanged */
    memcpy(buf + used, file, add - 1);
    buf[used + add - 1] = '\n';
    buf[used + add] = 0;
    return 1;
}

/* ---- row label ---- */

/* RFC-822 makes the weekday OPTIONAL, and both target feeds' shapes appear in
 * the wild:
 *     "Tue, 11 Aug 2026 16:00:00 GMT"   and   "11 Aug 2026 16:00:00 GMT"
 * The old rule -- "everything after the first space, first 11 bytes" -- is only
 * correct for the first shape; the second rendered as "Aug 2026 16". Skip a
 * leading weekday token (RFC-822 spells it with a trailing comma), then keep at
 * most three whitespace-separated tokens: day, month, year. */
void up_short_date(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    int tok = 0;
    const char *p = in, *sp;

    if (!out || outlen == 0) return;
    out[0] = 0;
    if (!in) return;
    while (*p == ' ' || *p == '\t') p++;

    sp = p;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (sp > p && sp[-1] == ',') {            /* "Tue," -- optional weekday */
        p = sp;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* tok_start marks where the token being copied began, so a token the
     * buffer cannot hold in full is dropped entirely rather than emitted
     * truncated: "11 September 2026" cut to "11 September 20" reads as a real
     * date with the WRONG YEAR, and an ISO-8601 pubDate (one token, no spaces)
     * cut to "2026-08-11T16:0" reads as a real timestamp. A missing date is
     * honest; a plausible wrong one is not. Code review 2026-09-08. */
    size_t tok_start = 0;
    int cut = 0;
    while (*p && tok < 3) {
        if (*p == ' ' || *p == '\t') {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (++tok >= 3) break;
            if (o + 1 >= outlen) break;   /* no room for another token; what we
                                           * already have is COMPLETE -- keep it */
            out[o++] = ' ';
            tok_start = o;
            continue;
        }
        if (o + 1 >= outlen) { cut = 1; break; }   /* ran out mid-token */
        out[o++] = *p++;
    }
    if (cut) o = tok_start;                        /* drop the partial token */
    o = pod_rtrim_spaces(out, o);
    out[o] = 0;
}

int up_row_label(const up_ep_t *ep, int state, char *out, size_t outlen)
{
    const char *mark = state == UP_ST_ONDISK ? "* "
                     : state == UP_ST_WANTED ? "+ " : "  ";
    /* 24, not 16: "11 September 2026" is 17 bytes + NUL, and an ISO-8601
     * pubDate is a single 20-byte token. At 16 the date was silently cut. */
    char day[24];
    if (!out || outlen == 0) return 0;   /* else the truncation path below runs
                                          * pod_utf8_boundary(out, SIZE_MAX) */
    up_short_date(ep->date, day, sizeof day);
    int n = snprintf(out, outlen, "%s%s%s%s", mark, ep->title,
                     day[0] ? "  " : "", day);
    if (n < 0) { if (outlen) out[0] = 0; return 0; }
    if ((size_t)n < outlen) return n;

    /* Truncated. Cut the TITLE, never the date: callers pass label[96]
     * against a 120-byte title cap, so this is routine, and letting snprintf
     * cut the tail turned "11 Aug 2026" into the plausible-looking
     * "11 Aug 202" (found by code review). Titles are UTF-8 (curly quotes,
     * em-dashes, emoji), so the byte cap may slice a codepoint in half --
     * back off to a boundary, but ONLY here, because truncation actually
     * happened: a title that simply ends on a complete codepoint must never
     * be touched (pi_sanitise_title once ran this back-off unconditionally
     * and ate the final codepoint of every clean non-ASCII title). See
     * pod_utf8_boundary() for why a naive trailing-byte strip is not enough. */
    size_t fixed = strlen(mark) + (day[0] ? 2 + strlen(day) : 0);
    if (fixed + 1 <= outlen) {
        size_t room = outlen - 1 - fixed;   /* < strlen(title), since it did not fit */
        char t[UP_TITLE_MAX];
        memcpy(t, ep->title, room);
        t[pod_utf8_boundary((const unsigned char *)t, room)] = 0;
        return snprintf(out, outlen, "%s%s%s%s", mark, t, day[0] ? "  " : "", day);
    }
    /* Not even mark + date fit: keep what snprintf left, on a boundary. */
    size_t o = pod_utf8_boundary((const unsigned char *)out, outlen - 1);
    out[o] = 0;
    return (int)o;
}

/* ---- index age (feed row) ---- */

int up_age_str(long secs, char *out, size_t outlen)
{
    int n;
    if (!out || outlen == 0) return 0;
    if (secs < 60) {                       /* also covers a future mtime */
        n = snprintf(out, outlen, "just now");
    } else if (secs < 3600) {
        n = snprintf(out, outlen, "%ldm ago", secs / 60);
    } else if (secs < 86400) {
        n = snprintf(out, outlen, "%ldh ago", secs / 3600);
    } else {
        long d = secs / 86400;
        if (d > 999) d = 999;              /* keeps the row a fixed width */
        n = snprintf(out, outlen, "%ldd ago", d);
    }
    if (n < 0) { out[0] = 0; return 0; }
    return (size_t)n < outlen ? n : (int)outlen - 1;
}

/* ---- staleness / lock ownership ---- */

int up_status_stale(const char *state, long age_secs)
{
    if (!state) return 1;
    /* Only "done" goes stale, so a finished run's status line clears itself
     * after UP_STATUS_STALE_SECS instead of showing a stale "finished" as
     * current forever. "error" must NOT go stale: a refresh or download
     * that failed while the user was away (no IP, card full) has to stay
     * visible until something replaces it -- clearing it after 120s would
     * make the failure invisible. wifi/refresh/download are never judged on
     * age either: a live run rewrites the file every episode, so a slow
     * 70 MB download can legitimately sit on one line for minutes. */
    if (strcmp(state, "done") != 0) return 0;
    return age_secs > UP_STATUS_STALE_SECS;
}

int up_parse_pid(const char *buf)
{
    long v = 0;
    const char *p = buf;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
        if (v > 0x7fffffff) return 0;      /* nonsense; treat as no owner */
    }
    return (int)v;
}

/* ---- on-disk episodes no longer in the index ---- */

int up_disk_mark_indexed(FILE *f, up_disk_t *d, int n)
{
    char line[1024];
    int marked = 0;
    if (!f || !d || n <= 0) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        int overlong = 0;
        char *t;
        if (l && line[l - 1] == '\n') line[--l] = 0;
        else if (l == sizeof line - 1) {
            /* Same peek as up_index_window(): a buffer-filling read is either
             * a complete final row at EOF or a genuinely over-long row whose
             * tail must be discarded. Skipping the latter keeps the two
             * functions consistent -- a row the picker refuses to render must
             * also count as "not in the index" here, or the file would be
             * neither an index row nor an orphan and stay undeletable. */
            int c = fgetc(f);
            if (c != EOF) {
                while (c != '\n' && c != EOF) c = fgetc(f);
                overlong = 1;
            }
        }
        if (overlong || !l) continue;
        t = strchr(line, '\t');
        if (t) *t = 0;
        for (int i = 0; i < n; i++) {
            if (!d[i].in_index && strcmp(d[i].file, line) == 0) {
                d[i].in_index = 1;
                marked++;
                break;
            }
        }
    }
    return marked;
}

int up_disk_keep_orphans(up_disk_t *d, int n)
{
    int k = 0;
    if (!d) return 0;
    for (int i = 0; i < n; i++)
        if (!d[i].in_index) { if (k != i) d[k] = d[i]; k++; }
    return k;
}

void up_disk_sort_mtime(up_disk_t *d, int n)
{
    /* Insertion sort: n is bounded by UP_DISK_MAX and typically single digits,
     * so this is smaller and cheaper than pulling in qsort's comparator
     * indirection on a 23.6 MB box. */
    if (!d) return;
    for (int i = 1; i < n; i++) {
        up_disk_t key = d[i];
        int j = i - 1;
        while (j >= 0 && (d[j].mtime < key.mtime ||
                          (d[j].mtime == key.mtime &&
                           strcmp(d[j].file, key.file) > 0))) {
            d[j + 1] = d[j];
            j--;
        }
        d[j + 1] = key;
    }
}
