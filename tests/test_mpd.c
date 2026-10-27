#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_mpd.h"

/* Browse listings at the sizes the lists must handle in full: 2,000 albums
 * and artists, 20,000 songs, 500 entries in one folder (Leslie's library:
 * 356 albums, 4,277 songs). The old fixed buffers capped these at 256 / 200 /
 * 128, or showed nothing past 16 KB / 8 KB of reply. */
static char *big_reply(const char *fmt, int n, int *len)
{
    char *r = malloc((size_t)n * 128 + 8);
    int o = 0;
    for (int i = 0; i < n; i++) o += sprintf(r + o, fmt, i, i % 7);
    o += sprintf(r + o, "OK\n");
    *len = o;
    return r;
}

static void test_browse_sizes(void)
{
    int len, scan;
    /* 2,000 albums: every one parsed, in order, full length */
    char *r = big_reply("Album: Album number %05d of the library, disc %d\n", 2000, &len);
    scan = 0; assert(mpd_reply_end(r, len, &scan) == 1 && scan == len);
    strtab_t t = { 0 };
    assert(mpd_taglist_tab(r, "Album", &t) == 2000 && t.n == 2000);
    assert(!strcmp(strtab_get(&t, 0), "Album number 00000 of the library, disc 0"));
    assert(!strcmp(strtab_get(&t, 1999), "Album number 01999 of the library, disc 4"));
    strtab_free(&t); free(r);

    /* 20,000 songs, as `search any "" window` returns them (metadata between) */
    r = big_reply("file: Artist/Album/%05d Track.flac\nTitle: t%d\nTime: 200\n", 20000, &len);
    assert(mpd_listing_tab(r, &t) == 20000);
    assert(!strcmp(strtab_get(&t, 19999), "fArtist/Album/19999 Track.flac"));
    strtab_free(&t); free(r);

    /* 500 entries in one folder, directories and files mixed */
    r = big_reply("directory: Music/dir %03d\nfile: Music/song%d.mp3\nLast-Modified: x\n", 250, &len);
    assert(mpd_listing_tab(r, &t) == 500);
    assert(strtab_get(&t, 0)[0] == 'd' && strtab_get(&t, 1)[0] == 'f');
    assert(!strcmp(strtab_get(&t, 498), "dMusic/dir 249"));
    strtab_free(&t); free(r);

    /* the entry bound holds: a reply can't grow the table without limit */
    r = big_reply("Artist: a%d %d\n", 100, &len);
    strtab_t lim = { .max = 64 };
    assert(mpd_taglist_tab(r, "Artist", &lim) == -1 && lim.n == 64);
    strtab_free(&lim); free(r);
}

/* The terminator scan must find OK / ACK however read() splits the reply,
 * and never mistake a value that merely starts with OK for the end. */
static void test_reply_end(void)
{
    const char *rep = "Album: OK Computer\nfile: OK\nAlbum: ACKnowledged\nOK\n";
    int len = (int)strlen(rep);
    for (int cut = 0; cut <= len; cut++) {
        int scan = 0, done = mpd_reply_end(rep, cut, &scan);
        assert(done == (cut == len));
        if (!done) assert(mpd_reply_end(rep, len, &scan) == 1);        /* resume finds it */
        assert(scan == len);
    }
    int scan = 0;
    assert(mpd_reply_end("ACK [50@0] {lsinfo} No such directory\n", 39, &scan) == 1);
    scan = 0;
    assert(mpd_reply_end("Album: x\nOKAY\n", 15, &scan) == 0);
}

static void test_ack(void)
{
    char m[40];
    assert(mpd_ack_text("ACK [52@0] {update} already updating\n", m, sizeof m) && !strcmp(m, "already updating"));
    assert(mpd_ack_text("ACK [5@0] {} unknown command \"x\"\n", m, sizeof m) && !strcmp(m, "unknown command \"x\""));
    assert(!mpd_ack_text("updating_db: 3\nOK\n", m, sizeof m));
    assert(mpd_ack_text("ACK [1@0] {update} a very long reason that does not fit\n", m, 8) && !strcmp(m, "a very "));
}

static void test_fold_tab(void)
{
    strtab_t t = { 0 };
    const char *in[] = { "TOOL", "abba", "Tool", "ABBA", "tool", "Zappa" };
    for (int i = 0; i < 6; i++) strtab_add(&t, in[i], -1);
    assert(strtab_fold_case(&t) == 3);
    assert(!strcmp(strtab_get(&t, 0), "abba") && !strcmp(strtab_get(&t, 1), "tool"));
    assert(!strcmp(strtab_get(&t, 2), "Zappa"));
    strtab_free(&t);
}

/* Update/Rescan completion: keep polling while updating_db is there, report
 * done once it's gone, give up after MPD_UPD_MAX_FAILS silent polls in a row. */
static void test_update_poll(void)
{
    int f = 0;
    assert(mpd_update_poll("volume: 50\nupdating_db: 3\nstate: stop\nOK\n", &f) == MPD_UPD_RUNNING);
    assert(mpd_update_poll("volume: 50\nstate: stop\nOK\n", &f) == MPD_UPD_DONE);
    f = 0;
    for (int i = 1; i < MPD_UPD_MAX_FAILS; i++) assert(mpd_update_poll(NULL, &f) == MPD_UPD_RUNNING && f == i);
    assert(mpd_update_poll("updating_db: 3\nOK\n", &f) == MPD_UPD_RUNNING && f == 0);   /* answer resets */
    for (int i = 1; i < MPD_UPD_MAX_FAILS; i++) mpd_update_poll(NULL, &f);
    assert(mpd_update_poll(NULL, &f) == MPD_UPD_FAILED);
    f = MPD_UPD_MAX_FAILS - 1;
    assert(mpd_update_poll("ACK [5@0] {status} x\n", &f) == MPD_UPD_FAILED);
    /* a value that merely contains the key is not the key */
    f = 0;
    assert(mpd_update_poll("Title: updating_db: no\nOK\n", &f) == MPD_UPD_DONE);
}

/* 2e: the poll follows the job id `update` returned, so a job that finished
 * before the first poll, or was followed by a queued one, still reports. */
static void test_update_poll_job(void)
{
    int f = 0;
    assert(mpd_update_job("updating_db: 7\nOK\n") == 7);
    assert(mpd_update_job("OK\n") == 0 && mpd_update_job(NULL) == 0);
    assert(mpd_update_job("ACK [54@0] {update} already updating\n") == 0);
    /* ours running, or an earlier (queued-ahead) one still running */
    assert(mpd_update_poll_job("updating_db: 7\nOK\n", 7, &f) == MPD_UPD_RUNNING);
    assert(mpd_update_poll_job("updating_db: 6\nOK\n", 7, &f) == MPD_UPD_RUNNING);
    /* finished before the first poll: no updating_db at all */
    assert(mpd_update_poll_job("state: stop\nOK\n", 7, &f) == MPD_UPD_DONE);
    /* a later job (someone else's update queued behind ours) is running: ours is done */
    assert(mpd_update_poll_job("updating_db: 8\nOK\n", 7, &f) == MPD_UPD_DONE);
    /* id wrap: 32767 -> 1 */
    assert(mpd_update_poll_job("updating_db: 1\nOK\n", 32767, &f) == MPD_UPD_DONE);
    assert(mpd_update_poll_job("updating_db: 32767\nOK\n", 1, &f) == MPD_UPD_RUNNING);
    /* unknown job id: presence alone, like mpd_update_poll */
    assert(mpd_update_poll_job("updating_db: 9\nOK\n", 0, &f) == MPD_UPD_RUNNING);
    /* no answer: same fail counting / give-up as mpd_update_poll */
    f = 0;
    for (int i = 1; i < MPD_UPD_MAX_FAILS; i++) assert(mpd_update_poll_job(NULL, 7, &f) == MPD_UPD_RUNNING && f == i);
    assert(mpd_update_poll_job(NULL, 7, &f) == MPD_UPD_FAILED);
}

int main(void)
{
    test_update_poll();
    test_update_poll_job();
    test_browse_sizes();
    test_reply_end();
    test_fold_tab();
    test_ack();
    const char *status =
      "volume: 70\nstate: play\nelapsed: 83.0\nduration: 225.0\nsong: 0\nOK\n";
    char buf[256];
    assert(mpd_field(status, "state", buf, sizeof buf) == 1 && strcmp(buf, "play") == 0);
    assert(mpd_field(status, "elapsed", buf, sizeof buf) == 1 && strcmp(buf, "83.0") == 0);
    assert(mpd_field(status, "missing", buf, sizeof buf) == 0);
    /* the ':' + space guard: a query must match the whole key, not a prefix */
    const char *ids = "songid: 5\nsong: 0\nOK\n";
    assert(mpd_field(ids, "song", buf, sizeof buf) == 1 && strcmp(buf, "0") == 0);
    /* value truncated to fit a small out buffer (stays NUL-terminated) */
    char small[6];
    assert(mpd_field("Title: HelloWorld\n", "Title", small, sizeof small) == 1);
    assert(strcmp(small, "Hello") == 0);

    const char *song = "file: a/b.flac\nTitle: Hi\nArtist: Me\nAlbum: X\nOK\n";
    assert(mpd_field(song, "Title", buf, sizeof buf) == 1 && strcmp(buf, "Hi") == 0);
    assert(mpd_field(song, "Artist", buf, sizeof buf) == 1 && strcmp(buf, "Me") == 0);

    /* arg quoting */
    mpd_quote("plain", buf, sizeof buf);
    assert(strcmp(buf, "\"plain\"") == 0);
    mpd_quote("a\"b\\c", buf, sizeof buf);
    assert(strcmp(buf, "\"a\\\"b\\\\c\"") == 0);

    /* truncation stays a well-formed quoted string (closing quote reserved) */
    char tiny[8];
    mpd_quote("abcdefghij", tiny, sizeof tiny);
    assert(strcmp(tiny, "\"abcde\"") == 0);
    mpd_quote("ab\"cdef", tiny, sizeof tiny);      /* escape pair straddles the cut */
    assert(strcmp(tiny, "\"ab\\\"c\"") == 0);
    char tiny2[6];
    mpd_quote("ab\"cd", tiny2, sizeof tiny2);      /* no room for both escape bytes */
    assert(strcmp(tiny2, "\"ab\"") == 0);
    char one[1];
    mpd_quote("x", one, sizeof one);               /* degenerate: just NUL */
    assert(one[0] == 0);

    /* directory listing */
    const char *ls =
      "directory: Music/Rock\nfile: Music/song.flac\ndirectory: Music/Jazz\nOK\n";
    mpd_entry_t e[8];
    int n = mpd_parse_listing(ls, e, 8);
    assert(n == 3);
    assert(e[0].type == 0 && strcmp(e[0].name, "Music/Rock") == 0);
    assert(e[1].type == 1 && strcmp(e[1].name, "Music/song.flac") == 0);
    assert(e[2].type == 0 && strcmp(e[2].name, "Music/Jazz") == 0);

    /* listing respects the max cap: 3 entries, room for 2 -> returns 2 */
    assert(mpd_parse_listing(ls, e, 2) == 2);

    /* an over-long entry name is truncated to 255 bytes (buffer safety) */
    char big[320]; int bo = 0;
    bo += snprintf(big + bo, sizeof big - bo, "directory: ");
    for (int i = 0; i < 300; i++) big[bo++] = 'x';
    big[bo++] = '\n'; big[bo] = 0;
    assert(mpd_parse_listing(big, e, 8) == 1 && strlen(e[0].name) == 255);

    /* tag list */
    const char *al = "Artist: AC/DC\nArtist: Adele\nOK\n";
    char tags[8][128];
    int m = mpd_parse_taglist(al, "Artist", tags, 8);
    assert(m == 2 && strcmp(tags[0], "AC/DC") == 0 && strcmp(tags[1], "Adele") == 0);

    /* tag value over 127 bytes is truncated; max cap respected */
    char bigtag[200]; int to = 0;
    to += snprintf(bigtag + to, sizeof bigtag - to, "Album: ");
    for (int i = 0; i < 150; i++) bigtag[to++] = 'y';
    bigtag[to++] = '\n'; bigtag[to] = 0;
    assert(mpd_parse_taglist(bigtag, "Album", tags, 8) == 1 && strlen(tags[0]) == 127);

    /* case-fold grouping: dupes merge to the fewest-capitals spelling, and the
     * survivors come back sorted case-insensitively */
    char cf[8][128];
    strcpy(cf[0], "TOOL"); strcpy(cf[1], "Spiritbox"); strcpy(cf[2], "Tool");
    strcpy(cf[3], "SlipKnoT"); strcpy(cf[4], "The Offspring"); strcpy(cf[5], "Slipknot");
    assert(mpd_fold_case(cf, 6) == 4);
    assert(strcmp(cf[0], "Slipknot") == 0);        /* SlipKnoT folded away */
    assert(strcmp(cf[1], "Spiritbox") == 0);
    assert(strcmp(cf[2], "The Offspring") == 0);
    assert(strcmp(cf[3], "Tool") == 0);            /* TOOL folded, ci-sorted after The */
    /* no dupes: untouched except for the ci sort; empty list is fine */
    strcpy(cf[0], "b"); strcpy(cf[1], "A");
    assert(mpd_fold_case(cf, 2) == 2 && strcmp(cf[0], "A") == 0 && strcmp(cf[1], "b") == 0);
    assert(mpd_fold_case(cf, 0) == 0);
    /* three-way group keeps the single lowercase winner */
    strcpy(cf[0], "ABBA"); strcpy(cf[1], "abba"); strcpy(cf[2], "Abba");
    assert(mpd_fold_case(cf, 3) == 1 && strcmp(cf[0], "abba") == 0);

    /* playback-error classifier */
    char em[64], eh[128];
    assert(mpd_classify_error("", 1, em, sizeof em, eh, sizeof eh) == 0);   /* no error */
    assert(mpd_classify_error(NULL, 1, em, sizeof em, eh, sizeof eh) == 0);
    /* card gone: overrides whatever MPD's text says */
    assert(mpd_classify_error("Failed to open x: No such file or directory", 0,
                              em, sizeof em, eh, sizeof eh) == 1);
    assert(strcmp(em, "SD card unavailable") == 0);
    /* card present, I/O error -> read failure (the original SD-corruption bug) */
    assert(mpd_classify_error("Failed to open x: Input/output error", 1,
                              em, sizeof em, eh, sizeof eh) == 1);
    assert(strcmp(em, "Can't read the SD card") == 0);
    /* card present, file missing -> not found (not "card gone") */
    assert(mpd_classify_error("Failed to open x: No such file or directory", 1,
                              em, sizeof em, eh, sizeof eh) == 1);
    assert(strcmp(em, "Track not found") == 0);
    /* decode failure */
    assert(mpd_classify_error("Failed to decode a/b.flac", 1,
                              em, sizeof em, eh, sizeof eh) == 1);
    assert(strcmp(em, "Can't play this track") == 0);
    /* unknown class falls back to MPD's own text in the hint */
    assert(mpd_classify_error("Something weird happened", 1,
                              em, sizeof em, eh, sizeof eh) == 1);
    assert(strcmp(em, "Playback error") == 0 && strcmp(eh, "Something weird happened") == 0);

    /* Shuffle Playlists setting: the parse must never leave the flag undefined.
     * A missing/empty/truncated /data file is the normal case on a fresh unit,
     * and must fall back to the default rather than reading as "Off". */
    assert(mpd_shuffle_parse("1", 1)      == 1);
    assert(mpd_shuffle_parse("0", 1)      == 0);
    assert(mpd_shuffle_parse("1\n", 2)    == 1);
    assert(mpd_shuffle_parse("0\n", 2)    == 0);
    assert(mpd_shuffle_parse("  0", 3)    == 0);   /* leading blanks skipped */
    assert(mpd_shuffle_parse("", 0)       == 1);   /* empty file -> default On */
    assert(mpd_shuffle_parse(NULL, 0)     == 1);   /* no file    -> default On */
    assert(mpd_shuffle_parse("x", 1)      == 1);   /* junk       -> default On */
    assert(mpd_shuffle_parse("\0", 1)     == 1);   /* NUL byte   -> default On */
    return 0;
}
