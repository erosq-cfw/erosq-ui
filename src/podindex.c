/* podindex -- stream an RSS feed on stdin, emit a TSV episode index on stdout.
 *
 * Streaming by design: the target feeds are 1.3-2.2 MB and this runs on a
 * 23.6 MB device, so the document is never held in memory. Peak RSS is the
 * expat parser plus one fixed buffer, independent of feed length.
 *
 * All parsing rules live in podindex_logic.c so they are host-testable without
 * expat (the dev box has no expat headers; the target has libexpat via dbus). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>
#include "podindex_logic.h"

#define CHUNK 8192

static pi_state g_state;
static long     g_emitted, g_max = 2000, g_dropped, g_skipped;

static void XMLCALL on_start(void *ud, const XML_Char *el, const XML_Char **attrs)
{
    (void)ud;
    pi_start(&g_state, el, (const char *const *)attrs);
}

static void XMLCALL on_chars(void *ud, const XML_Char *s, int len)
{
    (void)ud;
    pi_chars(&g_state, s, len);
}

static void XMLCALL on_end(void *ud, const XML_Char *el)
{
    (void)ud;
    char row[900];
    int is_item = (strcmp(el, "item") == 0);
    if (!pi_end(&g_state, el, row, sizeof row)) {
        /* pi_end returns 0 for almost every close (it only does real work on
         * </title>, </pubDate>, </item>) -- only an </item> close that failed
         * to emit represents a dropped episode (missing title, missing or
         * invalid enclosure, or a URL whose filename pi_filename_from_url
         * refused). Counting any other non-emitting close would report a
         * nonsense number in the thousands. */
        if (is_item) g_skipped++;
        return;
    }
    if (g_emitted >= g_max) { g_dropped++; return; }
    puts(row);
    g_emitted++;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "-n") == 0) {
        g_max = strtol(argv[2], NULL, 10);
        if (g_max <= 0) { fprintf(stderr, "podindex: bad -n\n"); return 2; }
    } else if (argc != 1) {
        fprintf(stderr, "usage: podindex [-n max] < feed.xml > index.tsv\n");
        return 2;
    }

    pi_reset(&g_state);
    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) { fprintf(stderr, "podindex: no parser\n"); return 1; }
    XML_SetElementHandler(p, on_start, on_end);
    XML_SetCharacterDataHandler(p, on_chars);

    char buf[CHUNK];
    size_t n;
    int rc = 0;
    do {
        n = fread(buf, 1, sizeof buf, stdin);
        if (XML_Parse(p, buf, (int)n, n == 0) == XML_STATUS_ERROR) {
            /* Most likely an undefined entity outside CDATA. Fail loudly and
             * non-zero so podfetch.sh keeps the previous index rather than
             * replacing it with a truncated one. */
            fprintf(stderr, "podindex: parse error at line %lu: %s\n",
                    XML_GetCurrentLineNumber(p),
                    XML_ErrorString(XML_GetErrorCode(p)));
            rc = 1;
            break;
        }
    } while (n > 0);
    XML_ParserFree(p);

    if (g_dropped)
        fprintf(stderr, "podindex: %ld episodes dropped past the -n %ld bound\n",
                g_dropped, g_max);
    if (g_skipped)
        fprintf(stderr, "podindex: %ld items skipped (no title, or unusable "
                        "enclosure URL)\n", g_skipped);
    if (rc == 0 && g_emitted == 0) {
        fprintf(stderr, "podindex: no episodes found\n");
        rc = 1;
    }
    /* Every row so far went through puts() into a buffered stdout. On a full SD
     * card the writes fail INSIDE stdio, and without this check main() returns 0
     * with an empty/short index -- podfetch.sh would then mv that over a good
     * 127 KB index.tsv and destroy it (the spec guarantees a failed refresh
     * leaves the previous index intact). fflush forces the buffered tail out so
     * a late ENOSPC is seen here; ferror catches an error that already latched
     * on an earlier flush. Both, because either alone misses a case. */
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "podindex: write failed\n");
        rc = 1;
    }
    return rc;
}
