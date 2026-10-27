#include "podindex_logic.h"
#include "pod_name.h"
#include "pod_utf8.h"
#include <string.h>
#include <stdio.h>

int pi_url_ok(const char *url)
{
    if (!url) return 0;
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) return 0;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

int pi_filename_from_url(const char *url, char *out, size_t outlen)
{
    if (!url || !*url || outlen < 2) return 0;

    /* path = after the authority; stop at '?' or '#' */
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    const char *path = slash;
    size_t plen = strcspn(path, "?#");

    /* find the last two segments of path[0..plen) */
    const char *seg[2] = { NULL, NULL };
    size_t seglen[2] = { 0, 0 };
    size_t i = 0;
    while (i < plen) {
        while (i < plen && path[i] == '/') i++;
        size_t start = i;
        while (i < plen && path[i] != '/') i++;
        if (i > start) {
            seg[0] = seg[1]; seglen[0] = seglen[1];
            seg[1] = path + start; seglen[1] = i - start;
        }
    }
    if (!seg[1]) return 0;                     /* no segment at all */

    /* Each segment must consist ENTIRELY of whitelisted bytes: a segment that
     * needs bytes dropped is refused outright rather than silently mangled
     * into a shorter, different-looking name (e.g. "%2e%2e" must not become
     * the accidentally-valid "2e2e"). */
    for (int k = 0; k < 2; k++) {
        if (!seg[k]) continue;
        for (size_t j = 0; j < seglen[k]; j++)
            if (!pod_name_char_ok(seg[k][j])) return 0;
    }

    /* Refuse rather than silently truncate: a truncated name would collide. */
    size_t want = seglen[1] + (seg[0] ? seglen[0] + 1 : 0);
    if (want + 1 > outlen) return 0;

    size_t o = 0;
    if (seg[0]) {
        memcpy(out + o, seg[0], seglen[0]); o += seglen[0];
        out[o++] = '-';
    }
    memcpy(out + o, seg[1], seglen[1]); o += seglen[1];
    out[o] = 0;

    return pod_name_ok(out);
}

size_t pi_sanitise_title(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    int pending_space = 0;
    int truncated = 0;   /* did we stop because outlen ran out, not because
                           * the input ended? Only THEN is a dangling partial
                           * UTF-8 sequence possible -- a title that simply
                           * ends on a complete codepoint must be left alone. */
    if (outlen == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
            *p == '\f' || *p == '\v') {
            if (o) pending_space = 1;                 /* whitespace run -> one space */
            continue;
        }
        if (*p < 0x20 || *p == 0x7f) continue;   /* other control chars: dropped outright */
        if (pending_space) {
            if (o + 1 >= outlen) { truncated = 1; break; }
            out[o++] = ' ';
            pending_space = 0;
        }
        if (o + 1 >= outlen) { truncated = 1; break; }
        out[o++] = (char)*p;
    }
    if (truncated) {
        /* Back off any partial UTF-8 sequence left dangling by the length
         * cap, and any trailing space the cap left behind ("ab cd" capped at
         * 4 must be "ab", not "ab " -- fuzz-found). See pod_utf8_boundary()
         * for why a naive trailing-byte strip is not sufficient. */
        o = pod_utf8_boundary((const unsigned char *)out, o);
    }
    out[o] = 0;
    return o;
}

void pi_reset(pi_state *s)
{
    memset(s, 0, sizeof *s);
    s->item_depth = -1;
}

/* Only a DIRECT child of <item> counts: an <image><title> nested inside an
 * item must not overwrite the episode title. */
static int direct_child_of_item(const pi_state *s)
{
    return s->item_depth >= 0 && s->depth == s->item_depth + 1;
}

void pi_start(pi_state *s, const char *el, const char *const *attrs)
{
    s->depth++;
    if (strcmp(el, "item") == 0 && s->item_depth < 0) {
        s->item_depth = s->depth;
        s->title[0] = s->date[0] = s->url[0] = 0;
        s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0;
        return;
    }
    if (!direct_child_of_item(s)) return;
    /* raw[0]=0 alongside rawlen=0: an empty element (<title></title>) fires
     * no pi_chars() call at all, so without this the buffer still holds
     * whatever the PREVIOUS element (e.g. pubDate) left in it, and that
     * stale text gets sanitised into THIS element's output in pi_end(). */
    if (strcmp(el, "title") == 0)        { s->in_title = 1; s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0; }
    else if (strcmp(el, "pubDate") == 0) { s->in_date  = 1; s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0; }
    else if (strcmp(el, "enclosure") == 0) {
        if (s->url[0]) return;   /* RSS convention: first enclosure is the
                                   * episode payload -- first VALID one wins,
                                   * later enclosures (any format) are ignored. */
        for (const char *const *a = attrs; a && a[0] && a[1]; a += 2) {
            if (strcmp(a[0], "url") != 0) continue;
            if (pi_url_ok(a[1]) && strlen(a[1]) < PI_URL_MAX) {
                memcpy(s->url, a[1], strlen(a[1]) + 1);
            }
            break;
        }
    }
}

void pi_chars(pi_state *s, const char *data, int len)
{
    if (!s->in_title && !s->in_date) return;
    int i = 0;
    for (; i < len && s->rawlen + 1 < PI_RAW_MAX; i++)
        s->raw[s->rawlen++] = data[i];
    if (i < len) s->raw_trunc = 1;   /* input dropped -- tail may be mid-codepoint */
    s->raw[s->rawlen] = 0;
}

/* The raw accumulator is capped at PI_RAW_MAX-1 bytes and that cut can land
 * mid-codepoint. pi_sanitise_title backs off only when ITS OWN cap trips, so
 * without this a raw-side cut survives whitespace collapsing (which can shrink
 * 511 raw bytes to well under the 121-byte title cap) and the dangling lead
 * byte reaches index.tsv verbatim -- code review 2026-09-08, regression test
 * test_raw_truncation_boundary(). Trim here, at the one point where we know no
 * further character data is coming for this element. */
static void pi_raw_finish(pi_state *s)
{
    if (!s->raw_trunc) return;
    s->rawlen = pod_utf8_boundary((const unsigned char *)s->raw, s->rawlen);
    s->raw[s->rawlen] = 0;
}

int pi_end(pi_state *s, const char *el, char *row, size_t rowlen)
{
    int emitted = 0;

    if (s->in_title && strcmp(el, "title") == 0) {
        pi_raw_finish(s);
        pi_sanitise_title(s->raw, s->title, sizeof s->title);
        s->in_title = 0; s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0;
    } else if (s->in_date && strcmp(el, "pubDate") == 0) {
        pi_raw_finish(s);
        pi_sanitise_title(s->raw, s->date, sizeof s->date);
        s->in_date = 0; s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0;
    } else if (strcmp(el, "item") == 0 && s->depth == s->item_depth) {
        char name[PI_NAME_MAX];
        if (s->title[0] && s->url[0] &&
            pi_filename_from_url(s->url, name, sizeof name)) {
            int n = snprintf(row, rowlen, "%s\t%s\t%s\t%s",
                             name, s->date, s->title, s->url);
            emitted = (n > 0 && (size_t)n < rowlen);
        }
        s->item_depth = -1;
        s->title[0] = s->date[0] = s->url[0] = 0;
        s->in_title = s->in_date = 0; s->rawlen = 0; s->raw[0] = 0; s->raw_trunc = 0;
    }
    if (s->depth > 0) s->depth--;
    return emitted;
}
