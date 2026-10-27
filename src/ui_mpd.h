#ifndef UI_MPD_H
#define UI_MPD_H

#include "strtab.h"

typedef struct { int type; char name[256]; } mpd_entry_t;   /* 0=dir, 1=file */

int  mpd_cmd(const char *cmd, char *reply, int rlen);
/* Whole reply into a malloc'd buffer (*out, caller frees) that grows to cap
 * bytes: byte count, -1 no MPD / no memory, -2 longer than cap or cut short
 * (*out NULL). For listings whose size grows with the library. */
int  mpd_cmd_dyn(const char *cmd, char **out, int cap);
/* Reply terminator: a line that is exactly "OK" or starts with "ACK". Scans
 * buf[*scan..len) and leaves *scan at the incomplete last line, so a reader
 * calling it after every read() scans each byte once. 1 = complete. */
int  mpd_reply_end(const char *buf, int len, int *scan);
/* Every "<tag>: value" into t; "directory:"/"file:" lines into t as "d<path>" /
 * "f<path>". Count added, or -1 when t is full / out of memory. */
int  mpd_taglist_tab(const char *reply, const char *tag, strtab_t *t);
int  mpd_listing_tab(const char *reply, strtab_t *t);
/* An "ACK [code@n] {cmd} message" reply: 1 and the message into out, else 0. */
int  mpd_ack_text(const char *reply, char *out, int n);
/* One poll of a running update/rescan. status: the `status` reply, or NULL when
 * MPD didn't answer (busy scanning: tolerated MPD_UPD_MAX_FAILS-1 times in a
 * row; *fails counts them). RUNNING while updating_db is present. */
enum { MPD_UPD_RUNNING = 0, MPD_UPD_DONE = 1, MPD_UPD_FAILED = -1 };
#define MPD_UPD_MAX_FAILS 5
int  mpd_update_poll(const char *status, int *fails);
/* The job id an `update`/`rescan` reply hands back ("updating_db: N"), 0 if none. */
unsigned mpd_update_job(const char *reply);
/* mpd_update_poll for one job: also DONE once status shows a LATER job than
 * ours (MPD runs queued jobs in id order, so ours has finished; ids wrap at
 * 2^15). job 0 = unknown: falls back to "updating_db present" alone. */
int  mpd_update_poll_job(const char *status, unsigned job, int *fails);
int  mpd_field(const char *reply, const char *key, char *out, int outlen);
void mpd_quote(const char *s, char *out, int outlen);
int  mpd_parse_listing(const char *reply, mpd_entry_t *out, int max);
int  mpd_parse_taglist(const char *reply, const char *tag, char out[][128], int max);
int  mpd_fold_case(char (*vals)[128], int nv);
int  mpd_classify_error(const char *err, int sd_present,
                        char *msg, int msglen, char *hint, int hintlen);

/* Play button: MPD "pause" (toggle) is a no-op while STOPPED -- e.g. after a
 * playback error stopped the player, so "Reinsert the card, then press Play"
 * did nothing (HITL 10-20). Map a `status` reply to the command to send: play
 * (clearing the sticky error) when stopped, else toggle. Pure, inline so the
 * stub-linked keymap test needs nothing new. */
#include <string.h>
static inline const char *mpd_play_cmd(const char *status)
{
    int stopped = status && (!strncmp(status, "state: stop\n", 12) ||
                             strstr(status, "\nstate: stop\n"));
    return stopped ? "command_list_begin\nclearerror\nplay\ncommand_list_end" : "pause";
}

/* Play with a card-removal resume record (sd-hotplug writes "<songid> <secs>"
 * to /run/sd-resume when the card is pulled mid-song, then stops MPD before its
 * error handling can skip ahead). Stopped AND the status's current songid is the
 * recorded one -> seekid there (starts playback at that position) into buf;
 * otherwise exactly mpd_play_cmd. A record for a song that's no longer current
 * (queue replaced since: MPD never reuses ids) or a malformed one is ignored. */
#include <stdio.h>
#include <stdlib.h>
static inline const char *mpd_resume_cmd(const char *status, const char *rec,
                                         char *buf, int n)
{
    const char *c = mpd_play_cmd(status);
    unsigned id, secs;
    char tail;
    if (c[0] != 'c' || !rec) return c;               /* not stopped, or no record */
    int k = sscanf(rec, "%u %u%c", &id, &secs, &tail);
    if (k < 2 || (k == 3 && tail != '\n')) return c;  /* malformed */
    const char *p = !strncmp(status, "songid: ", 8) ? status : strstr(status, "\nsongid: ");
    if (!p) return c;
    p = strchr(p, ':') + 2;
    char *e;
    unsigned long cur = strtoul(p, &e, 10);
    if (e == p || *e != '\n' || cur != id) return c;
    snprintf(buf, n, "command_list_begin\nclearerror\nseekid %u %u\ncommand_list_end", id, secs);
    return buf;
}

/* "Shuffle Playlists" setting, persisted to /data. Governs the bucket-style
 * play actions (playlists and genres); albums always play in order. The parse
 * step is split out pure so it's covered by the host test. */
int  mpd_shuffle_parse(const char *buf, int n);   /* -> 0/1; junk keeps default */
int  mpd_shuffle_get(void);
void mpd_shuffle_set(int on);

/* Album art: readpicture (embedded) then albumart (cover.* beside the file).
 * *out is malloc'd; returns byte count or <=0 for none. Images > cap are refused. */
int  mpd_readpicture(const char *uri, unsigned char **out, int cap);

/* Same source, streamed straight off the socket (nothing buffered here):
 * open -> total picture size (or <=0: none / > cap / error); read -> bytes
 * copied (dst NULL = skip; short on end, error, or the fetch deadline);
 * close always. Long socket timeouts: call it off the UI thread. */
typedef struct {
    int s, size, cap, got;          /* socket, picture size, size cap, bytes requested */
    int left, trailer, chunks;      /* payload left in this chunk, OK pending, calls */
    long long deadline;             /* CLOCK_MONOTONIC seconds */
    const char *verb;               /* "readpicture" or the "albumart" fallback */
    char q[600];                    /* quoted uri */
} mpd_pic_t;
int  mpd_pic_open(mpd_pic_t *p, const char *uri, int cap);
/* `albumart` only: the folder's cover file. MPD 0.24.15 tries cover.png,
 * cover.jpg, cover.webp in the song's directory, in that order, and nothing
 * else -- no folder.jpg / front.jpg (src/command/FileCommands.cxx,
 * find_stream_art). */
int  mpd_pic_open_cover(mpd_pic_t *p, const char *uri, int cap);
int  mpd_pic_read(mpd_pic_t *p, unsigned char *dst, int n);
void mpd_pic_close(mpd_pic_t *p);
int  mpd_bin_hdr(const char *line, int *size, int *nbin);   /* 1 size,2 binary,3 OK,-1 ACK,0 other */

#endif
