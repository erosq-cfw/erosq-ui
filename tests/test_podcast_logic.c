#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui_podcast_logic.h"
#include "pod_name.h"

static void test_parse_feeds(void) {
    up_feed_t f[4];
    const char *txt =
        "# a comment\n"
        "\n"
        "Nothing Much Happens|https://feeds.megaphone.fm/SSM2868305742\n"
        "https://feeds.acast.com/public/shows/abc\n";
    int n = up_parse_feeds(txt, f, 4);
    assert(n == 2);
    assert(strcmp(f[0].name, "Nothing Much Happens") == 0);
    assert(strcmp(f[0].url, "https://feeds.megaphone.fm/SSM2868305742") == 0);
    /* bare URL -> name derived from the host */
    assert(strcmp(f[1].name, "feeds.acast.com") == 0);

    /* a name with path-hostile bytes is sanitised, not rejected */
    n = up_parse_feeds("A/B:C|https://h/f\n", f, 4);
    assert(n == 1);
    assert(strchr(f[0].name, '/') == NULL);

    /* non-http scheme rejected; over-long line does not overflow */
    assert(up_parse_feeds("X|ftp://h/f\n", f, 4) == 0);
    char big[4096];
    memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
    assert(up_parse_feeds(big, f, 4) == 0);

    /* max respected */
    assert(up_parse_feeds("A|https://h/1\nB|https://h/2\nC|https://h/3\n", f, 2) == 2);
}

static void test_page_math(void) {
    /* 508 episodes at 100/page -> 6 pages, last one short */
    assert(up_page_count(508, 100) == 6);
    assert(up_page_count(207, 100) == 3);
    assert(up_page_count(100, 100) == 1);
    assert(up_page_count(1, 100) == 1);
    /* an empty index has ZERO pages -- must never render "page 1 of 0" */
    assert(up_page_count(0, 100) == 0);
    assert(up_page_count(-1, 100) == 0);
    assert(up_page_count(100, 0) == 0);   /* defensive */

    assert(up_page_clamp(0, 6) == 0);
    assert(up_page_clamp(5, 6) == 5);
    assert(up_page_clamp(9, 6) == 5);     /* clamped to last */
    assert(up_page_clamp(-3, 6) == 0);
    assert(up_page_clamp(2, 0) == 0);     /* no pages -> page 0 */
}

/* Build a synthetic index file: row i has filename ep<i>.mp3 */
static char *make_index(int rows) {
    static char path[] = "/tmp/erosq-podtest-XXXXXX";
    char tmpl[] = "/tmp/erosq-podtest-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    for (int i = 0; i < rows; i++)
        fprintf(f, "ep%d.mp3\tTue, 11 Aug 2026 16:00:00 GMT\tEpisode %d\thttps://h/%d.mp3\n",
                i, i, i);
    fclose(f);
    strcpy(path, tmpl);
    return path;
}

static void test_index_window(void) {
    char *p = make_index(508);
    up_ep_t eps[UP_PAGE];
    int total = -1;

    FILE *f = fopen(p, "r");
    int n = up_index_window(f, 0, UP_PAGE, eps, UP_PAGE, &total);
    fclose(f);
    assert(n == 100);
    assert(total == 508);
    assert(strcmp(eps[0].file, "ep0.mp3") == 0);
    assert(strcmp(eps[0].title, "Episode 0") == 0);
    assert(strcmp(eps[99].file, "ep99.mp3") == 0);

    /* a middle page */
    f = fopen(p, "r");
    n = up_index_window(f, 3, UP_PAGE, eps, UP_PAGE, &total);
    fclose(f);
    assert(n == 100);
    assert(strcmp(eps[0].file, "ep300.mp3") == 0);

    /* the final short page: 508 items -> page 5 holds 8 rows */
    f = fopen(p, "r");
    n = up_index_window(f, 5, UP_PAGE, eps, UP_PAGE, &total);
    fclose(f);
    assert(n == 8);
    assert(strcmp(eps[7].file, "ep507.mp3") == 0);

    /* past the end: zero rows, no read off the end, total still correct */
    f = fopen(p, "r");
    n = up_index_window(f, 99, UP_PAGE, eps, UP_PAGE, &total);
    fclose(f);
    assert(n == 0);
    assert(total == 508);
    remove(p);
}

static void test_index_malformed(void) {
    char tmpl[] = "/tmp/erosq-podbad-XXXXXX";
    int fd = mkstemp(tmpl);
    FILE *f = fdopen(fd, "w");
    fputs("good.mp3\tdate\tTitle\thttps://h/a.mp3\n", f);
    fputs("only\ttwo\n", f);                       /* too few fields -> skipped */
    fputs("../evil.mp3\tdate\tT\thttps://h/b.mp3\n", f);  /* bad name -> skipped */
    fputs("tail.mp3\tdate\tTail\thttps://h/c.mp3", f);    /* no trailing newline */
    fclose(f);

    up_ep_t eps[8]; int total = 0;
    f = fopen(tmpl, "r");
    int n = up_index_window(f, 0, UP_PAGE, eps, 8, &total);
    fclose(f);
    assert(n == 2);                                /* good + tail */
    assert(strcmp(eps[0].file, "good.mp3") == 0);
    assert(strcmp(eps[1].file, "tail.mp3") == 0);  /* final line, no trailing newline */
    remove(tmpl);
}

/* up_index_window's internal line buffer is 1024 bytes (1023 usable bytes +
 * NUL). Fix-round-1 regression (review finding B): a buffer-filling read
 * with no newline in it can mean two very different things -- a genuinely
 * over-long line (more bytes still follow) or a valid final row that simply
 * ends exactly at EOF with no trailing newline -- and the old code treated
 * both as garbage, silently dropping a syntactically valid last row whenever
 * a truncated write or power-loss on the SD card happened to leave one
 * exactly buffer-sized with no closing newline. */
static void test_index_overlong(void) {
    /* Case 1: a genuinely over-long line (content continues past the 1023-
     * byte buffer, no trailing newline anywhere, then EOF). Must be
     * discarded in full: zero rows, zero total. */
    {
        char tmpl[] = "/tmp/erosq-podlong-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fputs("bad.mp3\tdate\t", f);
        for (int i = 0; i < 1100; i++) fputc('T', f);   /* pushes well past 1023 */
        fputs("\thttps://h/bad.mp3", f);                /* still no newline; then EOF */
        fclose(f);

        up_ep_t eps[8]; int total = -1;
        f = fopen(tmpl, "r");
        int n = up_index_window(f, 0, UP_PAGE, eps, 8, &total);
        fclose(f);
        assert(n == 0);
        assert(total == 0);
        remove(tmpl);
    }

    /* Case 2: the final row is EXACTLY 1023 bytes with no trailing newline,
     * and that is genuinely the whole file (EOF right after). This is the
     * case the old `l == sizeof line - 1` check could not tell apart from
     * case 1 -- it must now be kept and counted, not discarded. */
    {
        char tmpl[] = "/tmp/erosq-podexact-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        const char *prefix = "f.mp3\td\t";      /* 8 bytes */
        const char *suffix = "\thttps://h/u";   /* 12 bytes */
        int titlelen = 1023 - (int)strlen(prefix) - (int)strlen(suffix);  /* 1003 */
        fputs(prefix, f);
        for (int i = 0; i < titlelen; i++) fputc('T', f);
        fputs(suffix, f);                        /* NO trailing newline; then EOF */
        fclose(f);

        up_ep_t eps[8]; int total = -1;
        f = fopen(tmpl, "r");
        int n = up_index_window(f, 0, UP_PAGE, eps, 8, &total);
        fclose(f);
        assert(n == 1);
        assert(total == 1);
        assert(strcmp(eps[0].file, "f.mp3") == 0);
        remove(tmpl);
    }

    /* Case 3: an over-long line (with a trailing newline further out) is
     * immediately followed by a genuinely valid row. The valid row must
     * still parse correctly and the total must reflect only it -- the
     * discarded tail of the over-long line must never be mistaken for a
     * fresh row boundary. */
    {
        char tmpl[] = "/tmp/erosq-podlong2-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fputs("bad.mp3\tdate\t", f);
        for (int i = 0; i < 1200; i++) fputc('T', f);
        fputs("\thttps://h/bad.mp3\n", f);        /* over-long, but DOES end in \n */
        fputs("good.mp3\tdate\tGood Title\thttps://h/good.mp3\n", f);
        fclose(f);

        up_ep_t eps[8]; int total = -1;
        f = fopen(tmpl, "r");
        int n = up_index_window(f, 0, UP_PAGE, eps, 8, &total);
        fclose(f);
        assert(n == 1);
        assert(total == 1);
        assert(strcmp(eps[0].file, "good.mp3") == 0);
        remove(tmpl);
    }
}

static void test_status(void) {
    up_status_t s;
    assert(up_parse_status("download 1/2 3/10 42 some-episode.mp3", &s) == 1);
    assert(strcmp(s.state, "download") == 0);
    assert(s.fi == 1 && s.fn == 2 && s.ei == 3 && s.en == 10 && s.pct == 42);
    assert(strcmp(s.text, "some-episode.mp3") == 0);

    assert(up_parse_status("done 2/2 0/0 100 finished", &s) == 1);
    assert(strcmp(s.state, "done") == 0);

    /* text may contain spaces (feed names do) */
    assert(up_parse_status("refresh 1/2 0/0 0 Nothing Much Happens", &s) == 1);
    assert(strcmp(s.text, "Nothing Much Happens") == 0);

    /* garbage and truncation are rejected, not half-parsed */
    assert(up_parse_status("", &s) == 0);
    assert(up_parse_status("download", &s) == 0);
    assert(up_parse_status("download 1/2", &s) == 0);

    /* the status writer emits a text field that may be empty -- must still parse */
    assert(up_parse_status("download 1/2 3/10 42 ", &s) == 1);
    assert(strcmp(s.state, "download") == 0);
    assert(s.pct == 42);
    assert(s.text[0] == 0);
}

static void test_wanted(void) {
    char buf[256];

    strcpy(buf, "a.mp3\nb.mp3\n");
    assert(up_wanted_has(buf, "a.mp3") == 1);
    assert(up_wanted_has(buf, "b.mp3") == 1);
    assert(up_wanted_has(buf, "c.mp3") == 0);
    /* prefix must not false-positive */
    assert(up_wanted_has(buf, "a.mp") == 0);
    assert(up_wanted_has("aa.mp3\n", "a.mp3") == 0);

    /* toggle on */
    strcpy(buf, "a.mp3\n");
    assert(up_wanted_toggle(buf, sizeof buf, "c.mp3") == 1);
    assert(up_wanted_has(buf, "c.mp3") == 1);
    assert(up_wanted_has(buf, "a.mp3") == 1);
    /* adding twice is idempotent, not a duplicate line */
    assert(up_wanted_toggle(buf, sizeof buf, "c.mp3") == 0);
    assert(up_wanted_has(buf, "c.mp3") == 0);

    /* toggle off the middle entry leaves the others intact */
    strcpy(buf, "a.mp3\nb.mp3\nc.mp3\n");
    assert(up_wanted_toggle(buf, sizeof buf, "b.mp3") == 0);
    assert(strcmp(buf, "a.mp3\nc.mp3\n") == 0);

    /* removing the LAST line (no trailing newline after it) works */
    strcpy(buf, "a.mp3\nb.mp3");
    assert(up_wanted_toggle(buf, sizeof buf, "b.mp3") == 0);
    assert(strcmp(buf, "a.mp3\n") == 0);

    /* empty buffer, and a full buffer that cannot take another line */
    buf[0] = 0;
    assert(up_wanted_toggle(buf, sizeof buf, "a.mp3") == 1);
    assert(strcmp(buf, "a.mp3\n") == 0);
    char small[8];
    strcpy(small, "a.mp3\n");
    assert(up_wanted_toggle(small, sizeof small, "zzzzzzzz.mp3") == -1);  /* no room */
    assert(strcmp(small, "a.mp3\n") == 0);                                /* unchanged */
}

static void test_row_label(void) {
    up_ep_t e;
    strcpy(e.file, "x.mp3");
    strcpy(e.date, "Tue, 11 Aug 2026 16:00:00 GMT");
    strcpy(e.title, "The Most Dangerous Midway Game");
    char out[96];

    /* ASCII markers only: the iPod fonts may lack check/arrow glyphs */
    up_row_label(&e, UP_ST_AVAIL, out, sizeof out);
    assert(strncmp(out, "  ", 2) == 0);
    assert(strstr(out, "The Most Dangerous Midway Game") != NULL);
    assert(strstr(out, "11 Aug 2026") != NULL);

    up_row_label(&e, UP_ST_WANTED, out, sizeof out);
    assert(strncmp(out, "+ ", 2) == 0);

    up_row_label(&e, UP_ST_ONDISK, out, sizeof out);
    assert(strncmp(out, "* ", 2) == 0);

    /* a long title truncates without overflowing: the return value must be the
     * ACTUAL written length, capped at outlen-1, and match strlen exactly */
    memset(e.title, 'x', sizeof e.title - 1); e.title[sizeof e.title - 1] = 0;
    int n = up_row_label(&e, UP_ST_AVAIL, out, sizeof out);
    assert(n == (int)sizeof out - 1);          /* 95: filled to the cap */
    assert(strlen(out) == (size_t)n);          /* NUL-terminated at that length */
    assert(strncmp(out, "  xxx", 5) == 0);     /* marker survived the truncation */
    /* ...and the DATE survives too: the title is what gets cut, never the
     * suffix -- review found "11 Aug 2026" rendering as "11 Aug 202". */
    assert(strcmp(out + n - 13, "  11 Aug 2026") == 0);
    /* title cut that lands after a space: no double gap before the date */
    strcpy(e.date, "Tue, 11 Aug 2026 16:00:00 GMT");
    memset(e.title, 'y', 79); e.title[79] = ' '; e.title[80] = 'z'; e.title[81] = 0;
    n = up_row_label(&e, UP_ST_AVAIL, out, sizeof out);
    assert(strstr(out, "y  11 Aug 2026") != NULL && n == (int)strlen(out));
}

/* Minimal UTF-8 validator for the test's own use: every continuation byte run
 * matches the length its lead byte declares, and no truncated trailer. */
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

/* UTF-8-aware truncation: byte cap must never leave a sliced codepoint behind,
 * but must also never trim a title that ends cleanly (the pi_sanitise_title
 * bug class -- see task-4 controller ruling). */
static void test_row_label_utf8(void) {
    up_ep_t e;
    strcpy(e.file, "u.mp3");
    e.date[0] = 0;   /* no pubDate -> no "  DD Mon YYYY" suffix to reason about */

    /* Title is 50 repeats of U+00E9 (e-acute, 2-byte UTF-8 0xC3 0xA9) = 100
     * bytes. mark "  " (2 bytes) + one complete e-acute (2 bytes) = 4 bytes,
     * so an outlen of 6 caps after byte 5: "  " + e-acute + a lone lead byte
     * (0xC3) of the next, incomplete, e-acute. That lone lead byte must be
     * backed off. */
    {
        char *t = e.title;
        for (int i = 0; i < 50; i++) { t[0] = '\xC3'; t[1] = '\xA9'; t += 2; }
        *t = 0;
    }
    char small[6];
    int n = up_row_label(&e, UP_ST_AVAIL, small, sizeof small);
    assert(utf8_ok(small));
    assert(n == 4);
    assert(strlen(small) == 4);
    assert(memcmp(small, "  \xC3\xA9", 4) == 0);   /* partial trailing bytes gone */

    /* Regression test for the bug class: a SHORT title ending in a complete
     * multi-byte codepoint, in a buffer far larger than needed, must come
     * back byte-for-byte intact -- no back-off may fire when nothing was
     * truncated. */
    strcpy(e.title, "Caf\xC3\xA9");   /* "Café", ends on a complete codepoint */
    char big[96];
    n = up_row_label(&e, UP_ST_AVAIL, big, sizeof big);
    assert(utf8_ok(big));
    assert(n == 7);
    assert(strcmp(big, "  Caf\xC3\xA9") == 0);

    /* Fix-round-1 regression (review finding A): the truncation point can
     * land EXACTLY on a codepoint boundary -- after a complete multi-byte
     * sequence, with the next codepoint excluded in its entirety by
     * snprintf. A naive "strip trailing 10xxxxxx bytes" back-off cannot
     * distinguish that from a sequence cut mid-way, because a complete
     * sequence's own last byte has the identical bit pattern, so it wrongly
     * ate the last complete codepoint too. Four complete e-acute (2 bytes
     * each) then an ASCII 'X', with outlen=11 landing the cap exactly after
     * the 4th e-acute -- all four must survive; only 'X' (excluded by
     * snprintf itself, not by the back-off) is gone. */
    strcpy(e.title, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9X");
    char exact[11];
    n = up_row_label(&e, UP_ST_AVAIL, exact, sizeof exact);
    assert(utf8_ok(exact));
    assert(n == 10);
    assert(strcmp(exact, "  \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9") == 0);

    /* Fix-round-2 regression (review finding A, part 2): a run of raw
     * continuation bytes LONGER than 3 has no lead byte that could explain
     * it within any valid sequence's reach. The round-1 fix capped its
     * backward walk at 3 and gave up beyond that, leaving a lone
     * continuation byte behind -- itself invalid UTF-8. Reproduced with the
     * reviewer's exact bytes as the title: outlen=8 (mark "  " + 5 raw
     * continuation bytes fill the buffer, 'X' excluded by snprintf). The
     * whole run must be stripped, leaving just the mark. */
    strcpy(e.title, "\x81\x82\x83\x84\x85X");
    char purerun[8];
    n = up_row_label(&e, UP_ST_AVAIL, purerun, sizeof purerun);
    assert(utf8_ok(purerun));
    assert(n == 2);
    assert(strcmp(purerun, "  ") == 0);

    /* Same bug, swept across run lengths 1, 3, 4, 5, 8: the title is
     * nothing but that many raw continuation bytes plus a trailing 'X'
     * excluded by the byte cap. Only the mark ("  ") may survive -- the
     * result must be valid UTF-8, not just short. */
    {
        static const int lens[] = {1, 3, 4, 5, 8};
        for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
            char *t = e.title;
            for (int i = 0; i < lens[k]; i++) *t++ = (char)(0x81 + (i % 4));
            *t++ = 'X';
            *t = 0;
            char out[16];
            size_t outlen = (size_t)(2 /* mark */ + lens[k] + 1 /* cut before 'X' */);
            n = up_row_label(&e, UP_ST_AVAIL, out, outlen);
            assert(utf8_ok(out));
            assert(strcmp(out, "  ") == 0);
            (void)n;
        }
    }
}


/* ---- fix-wave (final review) tests ---- */

/* M4: the RFC-822 date. The weekday is OPTIONAL in RFC-822 and both shapes
 * occur in the wild; "everything after the first space" only handled one of
 * them and rendered "11 Aug 2026 16:00:00 GMT" as "Aug 2026 16". */
static void test_short_date(void) {
    char d[16];
    up_short_date("Tue, 11 Aug 2026 16:00:00 GMT", d, sizeof d);
    assert(strcmp(d, "11 Aug 2026") == 0);
    up_short_date("11 Aug 2026 16:00:00 GMT", d, sizeof d);
    assert(strcmp(d, "11 Aug 2026") == 0);
    /* single-digit day: no trailing space left behind */
    up_short_date("Sat, 1 Aug 2026 05:00:00 +0000", d, sizeof d);
    assert(strcmp(d, "1 Aug 2026") == 0);
    up_short_date("1 Aug 2026 05:00:00 +0000", d, sizeof d);
    assert(strcmp(d, "1 Aug 2026") == 0);
    /* exactly three tokens, nothing after them */
    up_short_date("11 Aug 2026", d, sizeof d);
    assert(strcmp(d, "11 Aug 2026") == 0);
    /* empty / whitespace-only / junk */
    up_short_date("", d, sizeof d);
    assert(d[0] == 0);
    up_short_date("   ", d, sizeof d);
    assert(d[0] == 0);
    up_short_date("junk", d, sizeof d);
    assert(strcmp(d, "junk") == 0);              /* passed through, not invented */
    up_short_date("Tue,", d, sizeof d);
    assert(d[0] == 0);                           /* weekday only -> nothing left */
    /* never writes past outlen */
    char tiny[4];
    up_short_date("Tue, 11 Aug 2026 16:00:00 GMT", tiny, sizeof tiny);
    assert(strlen(tiny) < sizeof tiny);
    up_short_date("x", tiny, 1);
    assert(tiny[0] == 0);
    /* and the row label uses it */
    up_ep_t e;
    memset(&e, 0, sizeof e);
    snprintf(e.file, sizeof e.file, "a.mp3");
    snprintf(e.title, sizeof e.title, "Ep");
    snprintf(e.date, sizeof e.date, "11 Aug 2026 16:00:00 GMT");
    char row[64];
    up_row_label(&e, UP_ST_AVAIL, row, sizeof row);
    assert(strcmp(row, "  Ep  11 Aug 2026") == 0);
}

/* Index age on the feed row: the spec's staleness signal. */
static void test_age_str(void) {
    char a[16];
    up_age_str(0, a, sizeof a);      assert(strcmp(a, "just now") == 0);
    up_age_str(59, a, sizeof a);     assert(strcmp(a, "just now") == 0);
    up_age_str(-500, a, sizeof a);   assert(strcmp(a, "just now") == 0);
    up_age_str(60, a, sizeof a);     assert(strcmp(a, "1m ago") == 0);
    up_age_str(3599, a, sizeof a);   assert(strcmp(a, "59m ago") == 0);
    up_age_str(3600, a, sizeof a);   assert(strcmp(a, "1h ago") == 0);
    up_age_str(86399, a, sizeof a);  assert(strcmp(a, "23h ago") == 0);
    up_age_str(86400, a, sizeof a);  assert(strcmp(a, "1d ago") == 0);
    up_age_str(3L * 86400, a, sizeof a); assert(strcmp(a, "3d ago") == 0);
    /* absurd future-proofing: clamped, never a 12-digit day count */
    up_age_str(100000L * 86400, a, sizeof a); assert(strcmp(a, "999d ago") == 0);
    assert(strlen(a) < sizeof a);
}

/* M12: a `done` line sits in /run for the rest of the boot, so entering
 * Podcasts hours later used to show "Finished" as the CURRENT state. */
static void test_status_stale(void) {
    assert(up_status_stale("done", UP_STATUS_STALE_SECS + 1) == 1);
    assert(up_status_stale("done", 0) == 0);
    assert(up_status_stale("done", UP_STATUS_STALE_SECS) == 0);
    /* Regression detector for the fix-2 finding: a refresh/download that
     * failed while the user was away ("no IP", "card full") must stay on
     * screen forever, not blank out after UP_STATUS_STALE_SECS like a
     * finished run does. Before the fix this returned 1 for a large age. */
    assert(up_status_stale("error", 0) == 0);
    assert(up_status_stale("error", UP_STATUS_STALE_SECS) == 0);
    assert(up_status_stale("error", UP_STATUS_STALE_SECS + 1) == 0);
    assert(up_status_stale("error", 100000) == 0);
    /* a live run can legitimately sit on one line for minutes (a 70 MB
     * episode), so the in-progress states are NEVER aged out */
    assert(up_status_stale("download", 100000) == 0);
    assert(up_status_stale("refresh", 100000) == 0);
    assert(up_status_stale("wifi", 100000) == 0);
    assert(up_status_stale(NULL, 0) == 1);
}

/* I3/I5: the lock's pid, which is what makes "busy" and "stale" decidable. */
static void test_parse_pid(void) {
    assert(up_parse_pid("1234\n") == 1234);
    assert(up_parse_pid("  7\n") == 7);
    assert(up_parse_pid("1234abc") == 1234);
    assert(up_parse_pid("") == 0);
    assert(up_parse_pid("\n") == 0);
    assert(up_parse_pid("abc") == 0);
    assert(up_parse_pid("-1") == 0);
    assert(up_parse_pid("99999999999999999999") == 0);
    assert(up_parse_pid(NULL) == 0);
}

/* I6(b): downloaded episodes the feed has aged out. */
static void test_disk_orphans(void) {
    up_disk_t d[5];
    memset(d, 0, sizeof d);
    snprintf(d[0].file, sizeof d[0].file, "keep1.mp3");   d[0].mtime = 100;
    snprintf(d[1].file, sizeof d[1].file, "orphan_a.mp3"); d[1].mtime = 300;
    snprintf(d[2].file, sizeof d[2].file, "keep2.mp3");   d[2].mtime = 200;
    snprintf(d[3].file, sizeof d[3].file, "orphan_b.mp3"); d[3].mtime = 500;
    snprintf(d[4].file, sizeof d[4].file, "orphan_c.mp3"); d[4].mtime = 500;

    FILE *f = tmpfile();
    assert(f);
    fputs("keep1.mp3\tTue, 1 Jan 2026 00:00:00 GMT\tOne\thttps://h/1\n", f);
    fputs("keep2.mp3\tTue, 2 Jan 2026 00:00:00 GMT\tTwo\thttps://h/2\n", f);
    /* a row for a file that is NOT on disk must not disturb anything */
    fputs("gone.mp3\tTue, 3 Jan 2026 00:00:00 GMT\tThree\thttps://h/3\n", f);
    rewind(f);
    assert(up_disk_mark_indexed(f, d, 5) == 2);
    fclose(f);

    int n = up_disk_keep_orphans(d, 5);
    assert(n == 3);
    up_disk_sort_mtime(d, n);
    /* newest first; the mtime tie broken by name so the order is stable */
    assert(strcmp(d[0].file, "orphan_b.mp3") == 0);
    assert(strcmp(d[1].file, "orphan_c.mp3") == 0);
    assert(strcmp(d[2].file, "orphan_a.mp3") == 0);

    /* an empty index means EVERY downloaded file is an orphan -- the case
     * where the picker has nothing else at all to show */
    memset(d, 0, sizeof d);
    snprintf(d[0].file, sizeof d[0].file, "solo.mp3");
    f = tmpfile();
    assert(f);
    rewind(f);
    assert(up_disk_mark_indexed(f, d, 1) == 0);
    fclose(f);
    assert(up_disk_keep_orphans(d, 1) == 1);

    /* an OVER-LONG index row is skipped, exactly as up_index_window() skips
     * it -- otherwise a file would be neither an index row nor an orphan and
     * would stay undeletable */
    memset(d, 0, sizeof d);
    snprintf(d[0].file, sizeof d[0].file, "long.mp3");
    f = tmpfile();
    assert(f);
    fputs("long.mp3\t", f);
    for (int i = 0; i < 2000; i++) fputc('x', f);
    fputc('\n', f);
    rewind(f);
    assert(up_disk_mark_indexed(f, d, 1) == 0);
    fclose(f);

    /* defensive: no entries, no file */
    assert(up_disk_mark_indexed(NULL, d, 1) == 0);
    assert(up_disk_keep_orphans(NULL, 3) == 0);
    up_disk_sort_mtime(NULL, 3);
    up_disk_sort_mtime(d, 0);
}

/* I1 + M11: the feed-DIRECTORY rule, single-sourced in pod_name.h. The
 * byte-for-byte agreement with podfetch.sh is checked by
 * tests/test_pod_dirname_agree.sh; this covers the C side's own contract. */
static void test_dir_name(void) {
    char out[UP_NAME_MAX];
    assert(pod_dir_name("Spaced Show ", 12, out, sizeof out) == 11);
    assert(strcmp(out, "Spaced Show") == 0);
    /* truncate to 63, THEN trim trailing spaces */
    char in64[65];
    memset(in64, 'B', 64); in64[64] = 0;
    assert(pod_dir_name(in64, 64, out, sizeof out) == 63);
    assert(strlen(out) == 63);
    char in63[64];
    memset(in63, 'A', 63); in63[63] = 0;
    assert(pod_dir_name(in63, 63, out, sizeof out) == 63);
    /* 62 C's + ' ' + 'x' == 64 bytes: the cut lands on the space, which is
     * then trimmed -- 62, not 63 */
    char sp[65];
    memset(sp, 'C', 62); sp[62] = ' '; sp[63] = 'x'; sp[64] = 0;
    assert(pod_dir_name(sp, 64, out, sizeof out) == 62);
    assert(out[61] == 'C');
    /* all-dots names resolve to the parent directory: rejected */
    assert(pod_dir_name(".", 1, out, sizeof out) == 0 && out[0] == 0);
    assert(pod_dir_name("..", 2, out, sizeof out) == 0 && out[0] == 0);
    assert(pod_dir_name("...", 3, out, sizeof out) == 0 && out[0] == 0);
    /* dots that are not the WHOLE name are fine */
    assert(pod_dir_name("..dots..", 8, out, sizeof out) == 8);
    /* empty, whitespace-only, and all-dropped-bytes */
    assert(pod_dir_name("", 0, out, sizeof out) == 0);
    assert(pod_dir_name("   ", 3, out, sizeof out) == 0);
    assert(pod_dir_name("\xc3\x9c", 2, out, sizeof out) == 0);
    /* path separators and quotes can never survive */
    assert(pod_dir_name("a/b\"c\\d", 7, out, sizeof out) == 4);
    assert(strcmp(out, "abcd") == 0);
    /* degenerate buffers */
    assert(pod_dir_name("abc", 3, out, 0) == 0);
    assert(pod_dir_name("abc", 3, NULL, sizeof out) == 0);

    /* and up_parse_feeds must DROP a feed it cannot name, never invent one */
    up_feed_t f[4];
    assert(up_parse_feeds("..|https://h/f\n", f, 4) == 0);
    assert(up_parse_feeds("   |https://h/f\n", f, 4) == 0);
    assert(up_parse_feeds("..|https://h/f\nOK|https://h/g\n", f, 4) == 1);
    assert(strcmp(f[0].name, "OK") == 0);
}

/* Finding 2 (code review 2026-09-08): up_row_label passed day[16] to
 * up_short_date, which cut the date mid-token. "Tue, 11 September 2026
 * 16:00:00 GMT" became "11 September 20" -- a plausible-looking WRONG YEAR,
 * the same failure mode test_row_label already asserts against for the
 * title side. Two fixes: the buffer is now big enough for a full-month-name
 * RFC822 date, and up_short_date drops a trailing PARTIAL token rather than
 * emitting a truncated one, because a missing date beats a wrong one. */
static void test_short_date_partial_token(void) {
    char d[24];

    /* Full month name now fits. */
    up_short_date("Tue, 11 September 2026 16:00:00 GMT", d, sizeof d);
    assert(strcmp(d, "11 September 2026") == 0);

    /* Too small for the year: drop that token, never emit a partial one. */
    up_short_date("Tue, 11 September 2026 16:00:00 GMT", d, 16);
    assert(strcmp(d, "11 September") == 0);

    /* Too small mid-month: drop the month too. */
    up_short_date("Tue, 11 September 2026 16:00:00 GMT", d, 8);
    assert(strcmp(d, "11") == 0);

    /* ISO 8601 has no spaces at all -- one token. It fits in 24... */
    up_short_date("2026-08-11T16:00:00Z", d, sizeof d);
    assert(strcmp(d, "2026-08-11T16:00:00Z") == 0);
    /* ...and when it does not, the whole token goes rather than showing a
     * truncated timestamp that reads like a real one. */
    up_short_date("2026-08-11T16:00:00Z", d, 16);
    assert(d[0] == 0);

    /* Unchanged short-date behaviour. */
    up_short_date("Tue, 11 Aug 2026 16:00:00 GMT", d, sizeof d);
    assert(strcmp(d, "11 Aug 2026") == 0);
}

/* Finding 4 (code review 2026-09-08): with outlen == 0, snprintf returns the
 * would-be length (>= 0), so neither the n < 0 nor the n < outlen branch
 * short-circuited, fixed + 1 <= 0 was false, and pod_utf8_boundary was
 * called with outlen - 1 == SIZE_MAX -- an unbounded backwards read followed
 * by an out-of-bounds store. Latent (every caller passes label[96]) but the
 * function already guarded outlen == 0 on the n < 0 path, so the omission
 * was accidental. */
static void test_row_label_zero_outlen(void) {
    up_ep_t e; memset(&e, 0, sizeof e);
    snprintf(e.title, sizeof e.title, "some title");
    snprintf(e.date, sizeof e.date, "Tue, 11 Aug 2026 16:00:00 GMT");
    char buf[8] = { 'x' };
    assert(up_row_label(&e, UP_ST_AVAIL, buf, 0) == 0);
    assert(buf[0] == 'x');                    /* nothing written */
    assert(up_row_label(&e, UP_ST_AVAIL, NULL, 0) == 0);
}

int main(void) {
    test_parse_feeds();
    test_page_math();
    test_index_window();
    test_index_malformed();
    test_index_overlong();
    test_status();
    test_wanted();
    test_row_label();
    test_row_label_utf8();
    test_short_date();
    test_age_str();
    test_status_stale();
    test_parse_pid();
    test_disk_orphans();
    test_dir_name();
    test_short_date_partial_token();
    test_row_label_zero_outlen();
    printf("podcast logic OK\n");
    return 0;
}
