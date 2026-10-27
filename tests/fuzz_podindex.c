/* Dependency-free random-mutation fuzzer for the podcast logic: the only
 * hand-rolled code that sees bytes from the network (the XML itself is expat;
 * these functions get what expat hands back) plus the row formatter that
 * reads those bytes back off the SD card. Runs under run.sh's sanitizer
 * flags: every output buffer is malloc(outlen) so ASan's redzone sits exactly
 * at the cap, and this file supplies hostile inputs plus the invariants each
 * function promises in its header -- including a few POSITIVE ones, so a
 * stub that returns nothing cannot pass.
 *
 * ponytail: plain xorshift + biased alphabets instead of libFuzzer -- the host
 * has gcc only, and the run.sh rule is "dep-free host cc". Deterministic: a
 * failure prints SEED and iteration; rerun with SEED=<n> ITERS=<i+1>.
 * Upgrade path if it ever finds nothing for a year: delete it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pod_name.h"
#include "pod_utf8.h"
#include "podindex_logic.h"
#include "ui_podcast_logic.h"

static uint64_t rng, seed;
static uint32_t iter;
static uint32_t rnd(void)
{   /* xorshift64* */
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "\nFUZZ FAIL SEED=0x%llx iter=%u: %s\n", \
                      (unsigned long long)seed, iter, #c); abort(); } } while (0)

/* Bytes the parsers branch on, over-represented on purpose. */
static const char interesting[] =
    "//??##..--__%%::  \t\n\r\f\v\x7f\x01\x1f"
    "\x80\xbf\xc2\xdf\xe0\xef\xf0\xf4\xf8\xff"
    "aZ09h";

static unsigned char rnd_byte(void)
{
    switch (below(4)) {
    case 0:  return (unsigned char)interesting[below(sizeof interesting - 1)];
    case 1:  return (unsigned char)(0x20 + below(0x5f));       /* printable */
    default: return (unsigned char)(1 + below(255));           /* any non-NUL */
    }
}

static void rnd_bytes(char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] = (char)rnd_byte();
    buf[n] = 0;
}

/* Valid UTF-8: random 1..4-byte codepoints with the odd whitespace. */
static void rnd_utf8(char *buf, size_t maxbytes)
{
    size_t o = 0;
    while (o + 4 < maxbytes && below(40)) {
        uint32_t cp;
        switch (below(4)) {
        case 0:  cp = 0x20 + below(0x5f); break;
        case 1:  cp = 0x80 + below(0x780); break;
        case 2:  cp = 0x800 + below(0xf800); if (cp >= 0xd800 && cp < 0xe000) cp = 0xe000; break;
        default: cp = 0x10000 + below(0x100000); break;
        }
        if (cp < 0x80)          buf[o++] = (char)cp;
        else if (cp < 0x800)  { buf[o++] = (char)(0xc0 | cp >> 6);  buf[o++] = (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000){ buf[o++] = (char)(0xe0 | cp >> 12); buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); buf[o++] = (char)(0x80 | (cp & 0x3f)); }
        else                  { buf[o++] = (char)(0xf0 | cp >> 18); buf[o++] = (char)(0x80 | ((cp >> 12) & 0x3f)); buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); buf[o++] = (char)(0x80 | (cp & 0x3f)); }
        if (below(6) == 0) buf[o++] = " \t\n"[below(3)];
    }
    buf[o] = 0;
}

/* Structured URL: scheme + host + 0..6 segments, each segment mostly
 * whitelist bytes with the odd hostile one, optional ?query#frag. */
static void rnd_url(char *buf, size_t cap)
{
    size_t o = 0;
    const char *scheme = (const char *[]){ "https://", "http://", "ftp://", "", "https:/", "://" }[below(6)];
    o += (size_t)snprintf(buf + o, cap - o, "%s", scheme);
    for (uint32_t i = below(6); i && o + 2 < cap; i--)
        buf[o++] = "abc.xyz09"[below(9)];
    for (uint32_t s = below(7); s && o + 2 < cap; s--) {
        buf[o++] = '/';
        for (uint32_t k = below(300); k && o + 2 < cap; k--)
            buf[o++] = below(20) ? "aZ09._-"[below(7)] : (char)rnd_byte();
    }
    if (below(3) == 0 && o + 2 < cap) { buf[o++] = "?#"[below(2)]; buf[o++] = 'q'; }
    buf[o] = 0;
}

static int is_ctrl(unsigned char c) { return c < 0x20 || c == 0x7f; }

/* Does s[0..n) END on a codepoint boundary? Only the tail is judged: a
 * complete lead+continuations, an ASCII byte, or empty. Bytes before the
 * tail may be garbage (hostile input is passed through byte-for-byte). */
static int tail_on_boundary(const char *s, size_t n)
{
    if (n == 0) return 1;
    size_t run = 0;
    while (run < n && ((unsigned char)s[n - 1 - run] & 0xc0) == 0x80) run++;
    if (run == n) return 0;
    unsigned char lead = (unsigned char)s[n - 1 - run];
    int declared = (lead & 0x80) == 0 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                   (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
    return declared == (int)run + 1;
}

/* Whitespace-DENSE character data: long blank runs with the odd codepoint.
 * pi_sanitise_title collapses each run to a single space, so ~500 raw bytes
 * can shrink to a handful -- the shape that lets a raw-side mid-codepoint cut
 * (pi_chars caps at PI_RAW_MAX-1) slip past the title cap without ever setting
 * `truncated`. rnd_utf8() only sprinkles whitespace 1-in-6 codepoints, which
 * never compresses enough to reach that path: mutation-tested 2026-09-08, the
 * tail_on_boundary assert ALONE did not catch the bug it was added for. */
static size_t rnd_spacey(char *buf, size_t maxbytes)
{
    size_t o = 0;
    while (o + 8 < maxbytes && below(30)) {
        size_t run = 1 + below(24);
        while (run-- && o + 8 < maxbytes) buf[o++] = " \t\n"[below(3)];
        if (below(3)) buf[o++] = 'p';
        else { buf[o++] = (char)0xc3; buf[o++] = (char)0xa9; }   /* straddles the cap */
    }
    buf[o] = 0;
    return o;
}

/* Raw-accumulator truncation, finding 1 of the 2026-09-08 code review.
 *
 * The invariant under test is narrow and deliberate: VALID UTF-8 in must give
 * boundary-clean output, even when pi_chars() drops input at PI_RAW_MAX-1 and
 * whitespace collapsing then hides that cut from pi_sanitise_title's own cap.
 * It is NOT asserted for arbitrary bytes -- pi_* passes hostile input through
 * verbatim by design, so a feed whose <title> genuinely ends mid-sequence is
 * expected to round-trip unchanged. Asserting boundary-cleanliness
 * unconditionally fails on exactly that legitimate case (found by running it).
 *
 * Feeding only valid UTF-8 also removes the need for a well-formed enclosure:
 * the check runs on s.title directly rather than on an emitted row, because
 * emitting needs a valid URL and that path is too narrow to reach this bug
 * (mutation-tested: the row-level assert never fired in 20k iterations). */
static void fuzz_raw_truncation(void)
{
    pi_state s;
    char chunk[700], row[512];
    pi_reset(&s);
    pi_start(&s, "item", NULL);
    pi_start(&s, "title", NULL);
    int chunks = 1 + (int)below(8);
    while (chunks--) {
        size_t n;
        if (below(2)) { rnd_utf8(chunk, sizeof chunk - 1); n = strlen(chunk); }
        else          { n = rnd_spacey(chunk, sizeof chunk - 1); }
        pi_chars(&s, chunk, (int)n);
    }
    pi_end(&s, "title", row, sizeof row);
    CHECK(tail_on_boundary(s.title, strlen(s.title)));
    CHECK(strnlen(s.title, PI_TITLE_MAX) < PI_TITLE_MAX);
}

static void fuzz_filename(void)
{
    char url[1200];
    if (below(2)) rnd_url(url, sizeof url); else rnd_bytes(url, below(sizeof url - 1));
    size_t outlen = 1 + below(PI_NAME_MAX + 1);
    char *out = malloc(outlen);
    int ok = pi_filename_from_url(url, out, outlen);
    if (ok) {
        CHECK(strlen(out) < outlen);
        CHECK(pod_name_ok(out));           /* the load-bearing invariant: no '/', no '"', no '\\' */
    }
    free(out);
    pi_url_ok(url);
    (void)pi_url_ok(NULL);

    /* Positive: a clean two-segment URL that fits MUST yield "seg1-seg2". */
    char a[20], b[20], want[41], got[64];
    size_t la = 1 + below(16), lb = 1 + below(16);
    for (size_t i = 0; i < la; i++) a[i] = "aZ09_-"[below(6)];
    for (size_t i = 0; i < lb; i++) b[i] = "aZ09_-"[below(6)];
    a[la] = b[lb] = 0;
    snprintf(url, sizeof url, "https://h.example/%s/%s%s", a, b, below(2) ? "?x=1#f" : "");
    snprintf(want, sizeof want, "%s-%s", a, b);
    CHECK(pi_filename_from_url(url, got, sizeof got) == 1 && strcmp(got, want) == 0);
}

static void fuzz_sanitise(void)
{
    char in[1200], full[1200];
    int valid = below(2);
    if (valid) rnd_utf8(in, sizeof in - 1); else rnd_bytes(in, below(sizeof in - 1));
    size_t outlen = 1 + below(PI_TITLE_MAX + 64);
    char *out = malloc(outlen);
    size_t n = pi_sanitise_title(in, out, outlen);
    CHECK(n < outlen);
    CHECK(out[n] == 0 && strlen(out) == n);
    if (n) { CHECK(out[0] != ' ' && out[n - 1] != ' '); }
    for (size_t i = 0; i < n; i++) {
        CHECK(!is_ctrl((unsigned char)out[i]));
        CHECK(!(out[i] == ' ' && out[i + 1] == ' '));
    }
    /* Truncated (the uncapped result is longer)? Then the tail must be on a
     * codepoint boundary, hostile bytes or not -- that is the header's
     * promise and the fix that took three review rounds. */
    size_t nfull = pi_sanitise_title(in, full, sizeof full);
    if (nfull > n) CHECK(tail_on_boundary(out, n));
    /* Untruncated valid input: the output is a prefix-free rewrite, so it
     * must equal the full result byte-for-byte. */
    if (nfull <= n) CHECK(nfull == n && memcmp(out, full, n) == 0);
    /* Idempotent: clean output re-sanitised at full width is unchanged. */
    char again[1200];
    size_t m = pi_sanitise_title(out, again, sizeof again);
    CHECK(m == n && memcmp(out, again, n) == 0);
    free(out);
    (void)pi_sanitise_title(NULL, again, sizeof again);

    /* Positive: clean single-spaced ASCII that fits round-trips exactly. */
    size_t la = below(60);
    for (size_t i = 0; i < la; i++) in[i] = (i && i + 1 < la && below(6) == 0 && in[i - 1] != ' ') ? ' ' : "aZ09.,!"[below(7)];
    in[la] = 0;
    if (la && in[la - 1] == ' ') in[la - 1] = 'q';
    CHECK(pi_sanitise_title(in, again, 64) == la && strcmp(in, again) == 0);
}

static void fuzz_dirname(void)
{
    char in[300], again[80];
    size_t inlen = below(sizeof in);
    for (size_t i = 0; i < inlen; i++) in[i] = (char)rnd_byte();   /* NULs allowed: length-delimited */
    size_t outlen = 1 + below(79);
    char *out = malloc(outlen);
    int n = pod_dir_name(in, inlen, out, outlen);
    CHECK(n >= 0 && (size_t)n < outlen);
    if (n == 0) { CHECK(out[0] == 0); free(out); return; }
    CHECK(strlen(out) == (size_t)n && out[n - 1] != ' ');
    int alldots = 1;
    for (int i = 0; i < n; i++) {
        CHECK(pod_name_char_ok(out[i]) || out[i] == ' ');
        if (out[i] != '.') alldots = 0;
    }
    CHECK(!alldots);
    CHECK(pod_dir_name(out, (size_t)n, again, sizeof again) == n && strcmp(out, again) == 0);
    free(out);
}

/* up_row_label: the SD-card read-back side of the same bytes. */
static void fuzz_row_label(void)
{
    up_ep_t e;
    memset(&e, 0, sizeof e);
    strcpy(e.file, "x.mp3");
    if (below(2)) rnd_utf8(e.title, sizeof e.title - 1); else rnd_bytes(e.title, below(sizeof e.title - 1));
    if (below(3)) strcpy(e.date, below(2) ? "Tue, 11 Aug 2026 16:00:00 GMT" : "11 Aug 2026");
    char day[16];
    up_short_date(e.date, day, sizeof day);
    int state = (int)below(3);
    size_t outlen = 1 + below(130);
    char *out = malloc(outlen);
    int n = up_row_label(&e, state, out, outlen);
    CHECK(n >= 0 && (size_t)n < outlen && strlen(out) == (size_t)n);
    size_t fixed = 2 + (day[0] ? 2 + strlen(day) : 0);
    int truncated = fixed + strlen(e.title) >= outlen;
    if (fixed + 1 <= outlen) {
        /* Mark and date are never cut. An untruncated title is verbatim,
         * garbage and all; a truncated one ends on a codepoint boundary. */
        const char *marks[] = { "  ", "+ ", "* " };
        CHECK(strncmp(out, marks[state], 2) == 0);
        CHECK((size_t)n >= fixed);
        if (day[0]) {
            size_t dl = strlen(day);
            CHECK(strcmp(out + n - dl, day) == 0 && out[n - dl - 1] == ' ' && out[n - dl - 2] == ' ');
        }
        if (truncated) CHECK(tail_on_boundary(out + 2, (size_t)n - fixed));
        else CHECK((size_t)n == fixed + strlen(e.title) && strncmp(out + 2, e.title, strlen(e.title)) == 0);
    }
    free(out);
}

static const char *const elems[] = { "rss", "channel", "item", "title", "pubDate",
                                     "enclosure", "image", "description", "link", "x" };

static void fuzz_state_machine(void)
{
    pi_state s;
    pi_reset(&s);
    char url[600], garbage[40], chunk[700];
    const char *stack[64]; int sp = 0;
    int balanced = below(4) != 0;      /* expat is always balanced; 1-in-4 streams aren't */
    size_t rowlen = below(3) ? 1300 : 1 + below(64);
    char *row = malloc(rowlen);
    int rows = 0;

    for (uint32_t ev = below(200); ev; ev--) {
        const char *el = below(10) ? elems[below(sizeof elems / sizeof *elems)]
                                   : (rnd_bytes(garbage, below(sizeof garbage - 1)), garbage);
        switch (below(3)) {
        case 0: {                                        /* start */
            const char *attrs[7] = { 0 };
            if (below(2)) {
                if (below(2)) rnd_url(url, sizeof url); else rnd_bytes(url, below(sizeof url - 1));
                int k = 0;
                if (below(3) == 0) { attrs[k++] = "type"; attrs[k++] = "audio/mpeg"; }
                attrs[k++] = below(8) ? "url" : "URL"; attrs[k++] = url;
                if (below(3) == 0) { attrs[k++] = "length"; attrs[k++] = "1"; }
            }
            pi_start(&s, el, below(5) ? attrs : NULL);
            if (sp < 64) stack[sp++] = el == garbage ? "x" : el;
            break;
        }
        case 1: {                                        /* chars, chunked */
            size_t n = below(sizeof chunk - 1);
            switch (below(3)) {
            case 0:  rnd_bytes(chunk, n); break;
            case 1:  rnd_utf8(chunk, sizeof chunk - 1); n = strlen(chunk); break;
            default: n = rnd_spacey(chunk, sizeof chunk - 1); break;
            }
            pi_chars(&s, chunk, (int)n);
            break;
        }
        default: {                                       /* end */
            if (balanced) { if (!sp) break; el = stack[--sp]; }
            int emitted = pi_end(&s, el, row, rowlen);
            if (emitted) {
                rows++;
                size_t n = strlen(row);
                CHECK(n < rowlen);
                int tabs = 0;
                for (size_t i = 0; i < n; i++) {
                    CHECK(row[i] != '\n' && row[i] != '\r');
                    tabs += row[i] == '\t';
                }
                CHECK(tabs == 3);
                char *f1 = row, *f2 = strchr(f1, '\t'), *f3 = strchr(f2 + 1, '\t'), *f4 = strchr(f3 + 1, '\t');
                *f2 = *f3 = *f4 = 0;
                CHECK(pod_name_ok(f1));                  /* filename field */
                CHECK(f3[1] != 0);                       /* title non-empty */
                CHECK(pi_url_ok(f4 + 1));                /* url field */
            }
            break;
        }
        }
        /* Every string field stays NUL-terminated inside its array. */
        CHECK(strnlen(s.title, PI_TITLE_MAX) < PI_TITLE_MAX);
        CHECK(strnlen(s.date, PI_DATE_MAX) < PI_DATE_MAX);
        CHECK(strnlen(s.url, PI_URL_MAX) < PI_URL_MAX);
        CHECK(s.rawlen < PI_RAW_MAX && s.raw[s.rawlen] == 0);
    }
    free(row);

    /* Positive: a textbook item MUST produce a row (guards against a stub
     * that never emits, which every check above would wave through). */
    pi_reset(&s);
    const char *attrs[] = { "url", "https://h.example/ep/final.mp3", NULL };
    char r[600];
    pi_start(&s, "rss", NULL); pi_start(&s, "channel", NULL); pi_start(&s, "item", NULL);
    pi_start(&s, "title", NULL); pi_chars(&s, "Ep 1", 4); CHECK(pi_end(&s, "title", r, sizeof r) == 0);
    pi_start(&s, "enclosure", attrs); CHECK(pi_end(&s, "enclosure", r, sizeof r) == 0);
    CHECK(pi_end(&s, "item", r, sizeof r) == 1 && strcmp(r, "ep-final.mp3\t\tEp 1\thttps://h.example/ep/final.mp3") == 0);
}

int main(void)
{
    const char *e;
    seed = (e = getenv("SEED")) ? strtoull(e, 0, 0) : 0x5eed5eed5eedULL;
    uint32_t iters = (e = getenv("ITERS")) ? (uint32_t)strtoul(e, 0, 0) : 20000;
    for (iter = 0; iter < iters; iter++) {
        rng = seed ^ ((uint64_t)iter * 0x9E3779B97F4A7C15ULL); rng |= 1;
        fuzz_filename();
        fuzz_raw_truncation();
        fuzz_sanitise();
        fuzz_dirname();
        fuzz_row_label();
        fuzz_state_machine();
    }
    printf("podindex fuzz OK (%u iters, seed 0x%llx)\n", iters, (unsigned long long)seed);
    return 0;
}
