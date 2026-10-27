#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pod_name.h"
#include "podindex_logic.h"

/* The two real enclosure shapes, measured 2026-08-20. */
#define NMH_URL "https://pscrb.fm/rss/p/tracking.swap.fm/track/hQzRUk5FlBqiODHZblrZ/" \
                "clrtpod.com/m/traffic.megaphone.fm/SSM8553983690.mp3"
#define DAD_URL "https://sphinx.acast.com/p/acast/s/dungeons-and-daddies/e/" \
                "6a7ab0b6cfc0396cc1d561e1/media.mp3"

static void test_name_ok(void) {
    assert(pod_name_ok("a.mp3") == 1);
    assert(pod_name_ok("A-b_c.9.mp3") == 1);
    assert(pod_name_ok("") == 0);          /* empty */
    assert(pod_name_ok(".hidden") == 0);   /* leading dot */
    assert(pod_name_ok("..") == 0);
    assert(pod_name_ok("a/b") == 0);       /* the invariant: no slash survives */
    assert(pod_name_ok("a b") == 0);       /* space is legal in dir names, not files */
    assert(pod_name_ok("a\"b") == 0);      /* would break MPD protocol quoting */
    assert(pod_name_ok("a\nb") == 0);

    /* Fix-1 root cause, documented here because it cannot be exercised where
     * the actual bug lives: pod_name_ok() ALONE does not distinguish our own
     * control files from a real episode -- both of these are legal filenames
     * by this check, exactly as a crafted enclosure URL (single path segment,
     * name "index.tsv" or "wanted.txt") would produce via
     * pi_filename_from_url(). ep_longpress_cb() in src/ui_podcast.c used to
     * gate a row's delete-confirmation ONLY on pod_name_ok(), so a feed could
     * make its own index or download queue show up as a deletable row. The
     * fix adds an is_media() check there (extension whitelist: .mp3/.m4a/
     * .ogg/.opus), the same check collect_orphans() already applies a few
     * lines below in that file. is_media() and ep_longpress_cb() are not
     * reachable from a dep-free host test: ep_longpress_cb() is an LVGL
     * LV_EVENT_LONG_PRESSED handler keyed off lv_event_t and module-static
     * picker/navigation state (s_page_eps, s_feed, ui_goto()), and
     * ui_podcast.c is not linked into any CMake test target (it pulls in
     * LVGL), so there is no seam to drive it without stubbing LVGL -- a much
     * bigger change than this fix warrants. There is also no shell-level
     * equivalent to cover instead: podfetch.sh never deletes anything: only
     * the UI's long-press/confirm flow does. This assertion is the closest
     * dep-free proof available: it pins down the exact gap the fix closes. */
    assert(pod_name_ok("index.tsv") == 1);
    assert(pod_name_ok("wanted.txt") == 1);
}

static void test_filename_from_url(void) {
    char n[128];
    assert(pi_filename_from_url(NMH_URL, n, sizeof n) == 1);
    assert(strcmp(n, "traffic.megaphone.fm-SSM8553983690.mp3") == 0);

    assert(pi_filename_from_url(DAD_URL, n, sizeof n) == 1);
    assert(strcmp(n, "6a7ab0b6cfc0396cc1d561e1-media.mp3") == 0);

    /* Acast reuses media.mp3 for EVERY episode: the episode id must disambiguate. */
    char m[128];
    assert(pi_filename_from_url("https://sphinx.acast.com/p/acast/s/x/e/DEADBEEF/media.mp3",
                                m, sizeof m) == 1);
    assert(strcmp(n, m) != 0);

    /* query and fragment stripped */
    assert(pi_filename_from_url("https://h/a/b.mp3?key=1&t=2", n, sizeof n) == 1);
    assert(strcmp(n, "a-b.mp3") == 0);
    assert(pi_filename_from_url("https://h/a/b.mp3#frag", n, sizeof n) == 1);
    assert(strcmp(n, "a-b.mp3") == 0);

    /* single segment is fine */
    assert(pi_filename_from_url("https://h/only.mp3", n, sizeof n) == 1);
    assert(strcmp(n, "only.mp3") == 0);

    /* traversal cannot escape: '/' never survives the whitelist */
    assert(pi_filename_from_url("https://h/../../data/erosq-ui.chglimit", n, sizeof n) == 1);
    assert(strchr(n, '/') == NULL);
    assert(n[0] != '.');

    /* hostile / degenerate inputs are refused, not mangled into something odd */
    assert(pi_filename_from_url("https://h/", n, sizeof n) == 0);        /* no segment */
    assert(pi_filename_from_url("https://h/%2e%2e/", n, sizeof n) == 0); /* empties out */
    assert(pi_filename_from_url("", n, sizeof n) == 0);

    /* truncation must not produce a name ending mid-escape or overflow */
    char tiny[8];
    assert(pi_filename_from_url(NMH_URL, tiny, sizeof tiny) == 0);
}

static void test_url_ok(void) {
    assert(pi_url_ok("https://h/a.mp3") == 1);
    assert(pi_url_ok("http://h/a.mp3") == 1);
    assert(pi_url_ok("ftp://h/a.mp3") == 0);
    assert(pi_url_ok("file:///etc/passwd") == 0);
    assert(pi_url_ok("https://h/a\nb.mp3") == 0);   /* control char */
    assert(pi_url_ok("https://h/a\tb.mp3") == 0);   /* would desync the TSV */
    assert(pi_url_ok("") == 0);
}

/* Minimal UTF-8 validator: every continuation byte belongs to a run whose
 * length matches its lead byte's declaration; no lone/orphaned continuation
 * bytes and no truncated trailer. */
static int utf8_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int need;
        if (*p < 0x80) need = 0;
        else if ((*p & 0xE0) == 0xC0) need = 1;
        else if ((*p & 0xF0) == 0xE0) need = 2;
        else if ((*p & 0xF8) == 0xF0) need = 3;
        else return 0;
        p++;
        for (int i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) return 0;
            p++;
        }
    }
    return 1;
}

static void test_sanitise_title(void) {
    char t[121];
    pi_sanitise_title("Plain Title", t, sizeof t);
    assert(strcmp(t, "Plain Title") == 0);

    /* tab and newline would desynchronise the TSV -> collapsed to a space */
    pi_sanitise_title("A\tB\nC", t, sizeof t);
    assert(strcmp(t, "A B C") == 0);

    /* whitespace runs collapse; ends trimmed */
    pi_sanitise_title("  A   \t  B  ", t, sizeof t);
    assert(strcmp(t, "A B") == 0);

    /* other control characters are dropped outright */
    pi_sanitise_title("A\x01\x7f" "B", t, sizeof t);
    assert(strcmp(t, "AB") == 0);

    /* Truncation landing right after a collapsed space (fuzz-found): the
     * space fitted, the next word did not -> must still be end-trimmed. */
    assert(pi_sanitise_title("ab cd", t, 4) == 2);
    assert(strcmp(t, "ab") == 0);

    /* Fuzz/review-found boundary bugs: a lead byte followed by MORE
     * continuation bytes than it declares keeps the complete sequence and
     * drops the orphans; a run > 3 after a lead byte must not leave that
     * lead byte bare. */
    assert(pi_sanitise_title("A\xe0\x80\x80\x80X", t, 6) == 4);
    assert(memcmp(t, "A\xe0\x80\x80", 5) == 0);
    assert(pi_sanitise_title("\xc2\x80\x80X", t, 4) == 2);
    assert(memcmp(t, "\xc2\x80", 3) == 0);
    assert(pi_sanitise_title("A\xf0\x80\x80\x80\x80X", t, 7) == 5);
    assert(utf8_ok(t) && memcmp(t, "A\xf0\x80\x80\x80", 6) == 0);

    /* Genuine mid-codepoint truncation: buffer holds exactly "ab" + the lead
     * byte of 'e' (0xc3), with no room for its continuation byte (0xa9).
     * The dangling lead byte must be backed off, not left as invalid UTF-8. */
    char lead_cut[4];                    /* 3 usable bytes: 'a','b',0xc3 */
    pi_sanitise_title("ab\xc3\xa9zz", lead_cut, sizeof lead_cut);
    assert(strcmp(lead_cut, "ab") == 0);
    assert((unsigned char)lead_cut[strlen(lead_cut) - 1] < 0x80);

    /* Mid-codepoint truncation that cuts through continuation bytes: a 4-byte
     * emoji with only its lead byte fitting -- the back-off loop must strip
     * BOTH orphaned continuation bytes, not just one. */
    char mid_cut[7];                     /* 6 usable bytes: 'h','i',' ',0xf0,0x9f,0x98 */
    pi_sanitise_title("hi \xf0\x9f\x98\x80", mid_cut, sizeof mid_cut);
    assert(strcmp(mid_cut, "hi") == 0);  /* and the now-trailing space goes too */

    /* REGRESSION (the back-off must NOT run when the string simply ends on a
     * complete codepoint, with room to spare): a trailing accented char in a
     * generously-sized buffer must survive intact, byte for byte. */
    pi_sanitise_title("caf\xc3\xa9", t, sizeof t);
    assert(strcmp(t, "caf\xc3\xa9") == 0);

    /* REGRESSION, worst case: a title that is ONLY a non-ASCII codepoint must
     * not collapse to empty -- an empty title makes pi_end() silently drop
     * the whole episode from the index. */
    assert(pi_sanitise_title("\xc3\xa9", t, sizeof t) > 0);
    assert(strcmp(t, "\xc3\xa9") == 0);

    /* Fix-round-1 regression (review finding A): the truncation point can
     * land EXACTLY on a codepoint boundary -- after a complete multi-byte
     * sequence, with the next codepoint excluded in its entirety. A naive
     * "strip trailing 10xxxxxx bytes" back-off cannot distinguish that from
     * a sequence cut mid-way, because a complete sequence's own last byte
     * has the identical bit pattern, so it wrongly ate the last complete
     * codepoint too. Four complete e-acute (2 bytes each) then an ASCII
     * 'X', with outlen=9 landing the cap exactly after the 4th e-acute --
     * all four must survive; only 'X' (excluded by the copy loop itself,
     * not by the back-off) is gone. */
    char exact[9];
    pi_sanitise_title("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9X", exact, sizeof exact);
    assert(strcmp(exact, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9") == 0);

    /* Fix-round-2 regression (review finding A, part 2): a run of raw
     * continuation bytes LONGER than 3 has no lead byte that could explain
     * it within any valid sequence's reach. The round-1 fix capped its
     * backward walk at 3 and gave up beyond that, leaving a lone
     * continuation byte behind -- itself invalid UTF-8, exactly what this
     * routine exists to prevent. Reproduced with the reviewer's exact
     * bytes: outlen=6, "\x81\x82\x83\x84\x85X" (5 raw continuation bytes
     * fill the buffer, 'X' excluded by the copy loop). The whole run must
     * now be stripped -- an all-garbage title legitimately collapses to
     * empty, which is correct: pi_end() only emits an index row when
     * s->title[0] is non-zero, so a title that decodes to nothing simply
     * drops that one corrupted item, exactly like the pre-existing
     * "empty stays empty" case below. That is not a new failure mode. */
    assert(pi_sanitise_title("\x81\x82\x83\x84\x85X", t, 6) == 0);
    assert(t[0] == 0);

    /* Same bug, swept across run lengths 1, 3, 4, 5, 8 -- each with an
     * ASCII prefix so the assertion is meaningful (valid UTF-8 AND a
     * specific expected survivor), not just "happens to be empty". Every
     * continuation byte in the run must be gone; only "hi" may survive. */
    {
        static const int lens[] = {1, 3, 4, 5, 8};
        for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
            char in[16] = "hi";
            int n = 2;
            for (int i = 0; i < lens[k]; i++) in[n++] = (char)(0x81 + (i % 4));
            in[n++] = 'X';
            in[n] = 0;
            char out[16];
            pi_sanitise_title(in, out, (size_t)n);   /* outlen = n: room for
                                                        * "hi"+run, not 'X' */
            assert(utf8_ok(out));
            assert(strcmp(out, "hi") == 0);
        }
    }

    /* empty stays empty (caller treats as "skip this item") */
    assert(pi_sanitise_title("   ", t, sizeof t) == 0);
}

/* Drive the state machine with a hand-written event stream -- the same calls
 * expat will make, so item scoping is testable without expat on the host. */
static void ev_start(pi_state *s, const char *el, const char *a0, const char *a1) {
    const char *attrs[3];
    attrs[0] = a0; attrs[1] = a1; attrs[2] = NULL;
    pi_start(s, el, a0 ? attrs : attrs + 2);
}

static void test_item_scoping(void) {
    pi_state s; pi_reset(&s);
    char row[900];

    /* channel-level title and <image><title> must NOT emit rows: this is the
     * measured 510-titles-for-508-enclosures bug. */
    ev_start(&s, "rss", NULL, NULL);
    ev_start(&s, "channel", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Channel Name", 12);
    assert(pi_end(&s, "title", row, sizeof row) == 0);
    ev_start(&s, "image", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Image Title", 11);
    assert(pi_end(&s, "title", row, sizeof row) == 0);
    assert(pi_end(&s, "image", row, sizeof row) == 0);

    /* a real item emits exactly one row on </item> */
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Ep One", 6);
    assert(pi_end(&s, "title", row, sizeof row) == 0);
    ev_start(&s, "pubDate", NULL, NULL);
    pi_chars(&s, "Tue, 11 Aug 2026 16:00:00 GMT", 29);
    assert(pi_end(&s, "pubDate", row, sizeof row) == 0);
    ev_start(&s, "enclosure", "url", DAD_URL);
    assert(pi_end(&s, "enclosure", row, sizeof row) == 0);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strcmp(row, "6a7ab0b6cfc0396cc1d561e1-media.mp3\t"
                       "Tue, 11 Aug 2026 16:00:00 GMT\tEp One\t" DAD_URL) == 0);

    /* a nested <image><title> INSIDE an item must not overwrite the episode
     * title: only a direct child of <item> counts */
    pi_reset(&s);
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Real", 4);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "image", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Nested", 6);
    pi_end(&s, "title", row, sizeof row);
    pi_end(&s, "image", row, sizeof row);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strstr(row, "\tReal\t") != NULL);
}

static void test_incomplete_items(void) {
    pi_state s; char row[900];

    /* no enclosure -> skipped entirely, not half-emitted */
    pi_reset(&s);
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "No Audio", 8);
    pi_end(&s, "title", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 0);

    /* no title -> skipped */
    pi_reset(&s);
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 0);

    /* rejected URL scheme -> skipped */
    pi_reset(&s);
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Bad Scheme", 10);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "enclosure", "url", "file:///etc/passwd");
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 0);

    /* state must be clean for the NEXT item after a skip */
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Good", 4);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strstr(row, "Bad Scheme") == NULL);
    assert(strstr(row, "\tGood\t") != NULL);
}

static void test_chunked_chars(void) {
    /* expat delivers character data in arbitrary pieces, including one call per
     * CDATA section -- the accumulator must stitch them. */
    pi_state s; pi_reset(&s); char row[900];
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Intro ", 6);
    pi_chars(&s, "& ", 2);          /* expat has already decoded &amp; */
    pi_chars(&s, "Chapter 1", 9);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strstr(row, "\tIntro & Chapter 1\t") != NULL);
}

static void test_multiple_enclosures(void) {
    /* RSS convention: the FIRST enclosure is the episode payload. With two
     * valid <enclosure> elements in one <item>, the first one must win and
     * the second must be ignored, not overwrite it. */
    pi_state s; pi_reset(&s); char row[900];
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "Two Enclosures", 14);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    ev_start(&s, "enclosure", "url", NMH_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strstr(row, DAD_URL) != NULL);
    assert(strstr(row, NMH_URL) == NULL);
}

/* Fix-4 regression detector: pi_start()/pi_end() used to reset s->rawlen to 0
 * WITHOUT clearing s->raw[0], so a truly empty element (no pi_chars() call at
 * all) fell through pi_end() with whatever the PREVIOUS element left in the
 * accumulator. Item 1 ends on a <pubDate>, so that text is the last thing
 * written into raw[]; item 2's <title></title> is the very next element and
 * has no text nodes of its own, so pre-fix it silently inherited item 1's
 * pubDate as its title -- turning a should-be-skipped item into an indexed
 * one with borrowed text and a blank date. */
static void test_empty_title_after_pubdate(void) {
    pi_state s; pi_reset(&s); char row[900];

    /* item 1: normal, title + pubDate + enclosure -- last thing written into
     * raw[] before item 2 starts is this pubDate's text. */
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    pi_chars(&s, "First Episode", 13);
    pi_end(&s, "title", row, sizeof row);
    ev_start(&s, "pubDate", NULL, NULL);
    pi_chars(&s, "Mon, 01 Jan 2026 00:00:00 GMT", 29);
    pi_end(&s, "pubDate", row, sizeof row);
    ev_start(&s, "enclosure", "url", DAD_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 1);
    assert(strstr(row, "\tFirst Episode\t") != NULL);

    /* item 2: <title></title> is empty and comes first, with no pi_chars()
     * call at all -- it must NOT borrow item 1's pubDate text, and an item
     * with no title must be skipped, not indexed. */
    ev_start(&s, "item", NULL, NULL);
    ev_start(&s, "title", NULL, NULL);
    assert(pi_end(&s, "title", row, sizeof row) == 0);
    ev_start(&s, "pubDate", NULL, NULL);
    pi_chars(&s, "Tue, 02 Jan 2026 00:00:00 GMT", 29);
    pi_end(&s, "pubDate", row, sizeof row);
    ev_start(&s, "enclosure", "url", NMH_URL);
    pi_end(&s, "enclosure", row, sizeof row);
    assert(pi_end(&s, "item", row, sizeof row) == 0);
}

/* Finding 1 (code review 2026-09-08): the boundary back-off in
 * pi_sanitise_title only ran when the TITLE cap tripped, but pi_chars()
 * already truncates the raw accumulator at PI_RAW_MAX-1 = 511 bytes, and
 * that cut can land mid-codepoint. When whitespace collapsing then shrinks
 * the text below PI_TITLE_MAX, `truncated` stays 0, the back-off never runs,
 * and the dangling lead byte is written verbatim into index.tsv -- exactly
 * the garbage the boundary fix exists to prevent, reaching LVGL by a path
 * the fix did not cover.
 *
 * Repro: 507 spaces + "ppp" puts 0xC3 at raw byte 510 (the last byte
 * pi_chars accepts), so its 0xA9 continuation is dropped. Leading whitespace
 * collapses away entirely, so the sanitised title is only 4 bytes -- far
 * under the 121-byte cap -- and the old code emitted "ppp\xC3". */
static void test_raw_truncation_boundary(void) {
    char raw[600];
    size_t n = 0;
    for (; n < 507; n++) raw[n] = ' ';
    raw[n++] = 'p'; raw[n++] = 'p'; raw[n++] = 'p';
    raw[n++] = (char)0xC3; raw[n++] = (char)0xA9;   /* 'e-acute', split by the cap */
    for (int i = 0; i < 20; i++) raw[n++] = 'z';
    raw[n] = 0;

    pi_state st; pi_reset(&st);
    pi_start(&st, "item", NULL);
    pi_start(&st, "title", NULL);
    pi_chars(&st, raw, (int)n);
    char row[512];
    pi_end(&st, "title", row, sizeof row);

    /* The raw accumulator must have filled and been cut. */
    assert(strlen(st.title) > 0);
    /* No dangling lead byte: the last byte must not be a lead or a
     * continuation byte -- with this input the title is pure ASCII. */
    size_t tl = strlen(st.title);
    unsigned char last = (unsigned char)st.title[tl - 1];
    assert((last & 0x80) == 0);
    assert(strcmp(st.title, "ppp") == 0);
}

int main(void) {
    test_name_ok();
    test_filename_from_url();
    test_url_ok();
    test_sanitise_title();
    test_item_scoping();
    test_incomplete_items();
    test_chunked_chars();
    test_multiple_enclosures();
    test_empty_title_after_pubdate();
    test_raw_truncation_boundary();
    printf("podindex logic OK\n");
    return 0;
}
