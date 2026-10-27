#ifndef EROSQ_POD_UTF8_H
#define EROSQ_POD_UTF8_H
#include <stddef.h>

/* Shared UTF-8 truncation-boundary fix, used by both podindex (writes
 * titles, RSS -> TSV) and erosq-ui (formats titles into fixed-width picker
 * rows). Header-only for the same reason as pod_name.h (whose own comment
 * points at bt_vol.h as the established idiom in this repo): a
 * byte-for-byte identical routine was chased across two files once already
 * on this branch (Task 1's pi_sanitise_title and Task 4's up_row_label both
 * truncate UTF-8 text and both needed the identical fix), and a second bug
 * in it would otherwise need two synchronised edits with no compiler
 * enforcement that they stay in sync.
 *
 * Given a buffer of `o` valid bytes that may have been byte-truncated (the
 * caller must only call this when truncation actually happened -- see each
 * call site), return the length to keep so the result ends on a UTF-8
 * codepoint boundary.
 *
 * A truncation point can land in exactly three places relative to the
 * multi-byte sequence it falls in: cleanly on a boundary (nothing to do),
 * inside a sequence with some of its continuation bytes present (genuinely
 * incomplete -- drop it), or exactly after the LAST continuation byte of a
 * COMPLETE sequence, whose final byte has the same 10xxxxxx bit pattern as
 * a dangling continuation byte and is otherwise indistinguishable from one
 * by inspecting only that last byte. Blindly stripping trailing 10xxxxxx
 * bytes cannot tell the second case from the third, and silently eats a
 * complete trailing codepoint whenever the cut lands exactly on a boundary
 * -- about 1-in-N of all truncations for an N-byte codepoint, not rare.
 * This walks back to find the lead byte, reads the length it actually
 * DECLARES, and compares that against how many bytes are present to decide
 * complete vs. incomplete.
 *
 * The run of trailing continuation bytes is measured UNCAPPED and then
 * compared against what the lead byte declares: fewer than declared is an
 * incomplete sequence (drop lead and run), exactly declared is complete
 * (keep), MORE than declared is a complete sequence followed by orphan
 * continuation bytes (keep the sequence, drop the orphans -- corrupted
 * title bytes on the SD card). Trailing spaces are trimmed as part of the
 * same pass, so a cap that lands right after a collapsed space is clean. No lead byte at all means the whole run is
 * dropped; the result can legitimately be the empty string. An earlier
 * version kept "declared or more" as complete and special-cased runs > 3,
 * which left orphan continuation bytes, or a bare lead byte, behind
 * (found by tests/fuzz_podindex.c). */
static inline size_t pod_rtrim_spaces(const char *s, size_t n)
{   /* trailing spaces after a byte cap; shared with up_short_date() */
    while (n && s[n - 1] == ' ') n--;
    return n;
}

static inline size_t pod_utf8_boundary(const unsigned char *out, size_t o)
{
    /* Iterate: dropping a sequence can expose a trailing space, trimming a
     * space can expose a bare lead byte, and corrupted input can stack
     * several bad tails (fuzz-found: ".. fb <cut>" left the fb). Each pass
     * only shrinks, so this terminates; it stops as soon as the tail is a
     * complete sequence, an ASCII byte, or empty. */
    for (;;) {
        size_t start = o;
        o = pod_rtrim_spaces((const char *)out, o);
        if (o == 0) return 0;
        size_t run = 0;                      /* trailing continuation bytes, UNCAPPED */
        while (run < o && (out[o - 1 - run] & 0xC0) == 0x80)
            run++;
        if (run == o) return 0;          /* nothing but continuation bytes: none survive */

        size_t lead_pos = o - 1 - run;
        unsigned char lead = out[lead_pos];
        size_t declared;
        if ((lead & 0x80) == 0x00)      declared = 1;   /* ASCII */
        else if ((lead & 0xE0) == 0xC0) declared = 2;
        else if ((lead & 0xF0) == 0xE0) declared = 3;
        else if ((lead & 0xF8) == 0xF0) declared = 4;
        else declared = 0;                              /* invalid lead byte value */

        if (declared == 0 || run + 1 < declared) o = lead_pos;   /* bad or incomplete: drop it */
        else o = lead_pos + declared;                   /* complete: keep it, drop orphans after */
        if (o == start) return o;
    }
}
#endif
