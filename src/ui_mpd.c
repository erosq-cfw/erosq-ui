#include "ui_mpd.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#ifndef MPD_PATH
#define MPD_PATH "/run/mpd.sock"   /* -D override for the hermetic fake-MPD test */
#endif
#define SHUFFLE_PATH    "/data/erosq-ui.shuffle"
#define SHUFFLE_DEFAULT 1     /* buckets shuffle unless the user says otherwise */

int mpd_shuffle_parse(const char *buf, int n)
{
    if (!buf || n <= 0) return SHUFFLE_DEFAULT;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '0') return 0;
        if (buf[i] == '1') return 1;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') continue;
        break;                      /* anything else: keep the default */
    }
    return SHUFFLE_DEFAULT;
}

static int s_shuffle = -1;          /* -1 = not yet loaded */

int mpd_shuffle_get(void)
{
    if (s_shuffle < 0) {
        s_shuffle = SHUFFLE_DEFAULT;
        int fd = open(SHUFFLE_PATH, O_RDONLY);
        if (fd >= 0) {
            char buf[12]; int n = (int)read(fd, buf, sizeof buf);
            close(fd);
            s_shuffle = mpd_shuffle_parse(buf, n);
        }
    }
    return s_shuffle;
}

void mpd_shuffle_set(int on)
{
    s_shuffle = on ? 1 : 0;
    int fd = open(SHUFFLE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;             /* ponytail: no /data -> just don't persist */
    char ch = s_shuffle ? '1' : '0';
    if (write(fd, &ch, 1) < 0) { /* ignore */ }
    close(fd);
}

static int mpd_open(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0); if (s < 0) return -1;
    /* 1s send/recv timeouts (like mpd_io in ui_bt.c): this is polled from LVGL
     * timers on the single UI thread, so a wedged-but-alive MPD must make the
     * reads fail, not hang the whole UI (including POWER handling) forever. */
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_un dst = { .sun_family = AF_UNIX };
    const char *path = MPD_PATH;
#ifdef EROSQ_HOST
    if (getenv("EROSQ_MPD_SOCK")) path = getenv("EROSQ_MPD_SOCK");   /* host preview: tests/fake_mpd_lib.py */
#endif
    snprintf(dst.sun_path, sizeof dst.sun_path, "%s", path);
    if (connect(s, (void *)&dst, sizeof dst) < 0) { close(s); return -1; }

    /* read+discard greeting line "OK MPD <ver>\n" */
    char g[64]; int gi = 0;
    while (gi < (int)sizeof g - 1) {
        char c; if (read(s, &c, 1) != 1) { close(s); return -1; }
        if (c == '\n') break;
        g[gi++] = c;
    }
    return s;
}

int mpd_reply_end(const char *buf, int len, int *scan)
{
    int p = *scan;
    for (int i = p; i < len; i++) {
        if (buf[i] != '\n') continue;
        int l = i - p;
        if ((l == 2 && !strncmp(buf + p, "OK", 2)) || (l >= 3 && !strncmp(buf + p, "ACK", 3))) {
            *scan = i + 1;
            return 1;
        }
        p = i + 1;
    }
    *scan = p;                       /* resume at the incomplete line */
    return 0;
}

static int mpd_send(const char *cmd)
{
    int s = mpd_open(); if (s < 0) return -1;
    char line[512]; int li = snprintf(line, sizeof line, "%s\n", cmd);
    if (li >= (int)sizeof line) li = (int)sizeof line - 1;   /* snprintf returns the untruncated length */
    if (write(s, line, li) < 0) { close(s); return -1; }
    return s;
}

int mpd_cmd(const char *cmd, char *reply, int rlen)
{
    int s = mpd_send(cmd); if (s < 0) return -1;
    /* read until a line is exactly "OK" or starts with "ACK" */
    int total = 0, done = 0, scan = 0;
    while (total < rlen - 1) {
        int n = read(s, reply + total, rlen - 1 - total);
        if (n <= 0) break;
        total += n; reply[total] = 0;
        if ((done = mpd_reply_end(reply, total, &scan))) break;
    }
    close(s);
    /* No terminator seen => the reply was cut short (buffer filled before OK, or
     * the socket stalled). Return -2 rather than a silently-truncated buffer, so
     * callers fail visibly (empty screen) instead of rendering a partial listing
     * as if it were the whole thing. Callers already treat <= 0 as "no data". */
    return done ? total : -2;
}

int mpd_cmd_dyn(const char *cmd, char **out, int cap)
{
    *out = NULL;
    int s = mpd_send(cmd); if (s < 0) return -1;
    int size = 16384, total = 0, done = 0, scan = 0;
    char *b = malloc(size);
    while (b) {
        if (total + 1 >= size) {                       /* full: grow, up to cap */
            if (size >= cap) break;
            int ns = size * 2 > cap ? cap : size * 2;
            char *nb = realloc(b, ns);
            if (!nb) break;
            b = nb; size = ns;
        }
        int n = read(s, b + total, size - 1 - total);
        if (n <= 0) break;
        total += n; b[total] = 0;
        if ((done = mpd_reply_end(b, total, &scan))) break;
    }
    close(s);
    if (!b) return -1;
    if (!done) { free(b); return -2; }                 /* cut short: fail visibly, as mpd_cmd */
    *out = b;
    return total;
}

int mpd_taglist_tab(const char *reply, const char *tag, strtab_t *t)
{
    size_t tl = strlen(tag); int n = 0;
    for (const char *p = reply; p && *p; ) {
        const char *e = strchr(p, '\n');
        if (strncmp(p, tag, tl) == 0 && p[tl] == ':' && p[tl+1] == ' ') {
            const char *v = p + tl + 2;
            if (strtab_add(t, v, (int)((e ? e : v + strlen(v)) - v)) < 0) return -1;
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

int mpd_listing_tab(const char *reply, strtab_t *t)
{
    int n = 0;
    char tmp[1024];
    for (const char *p = reply; p && *p; ) {
        const char *e = strchr(p, '\n'); int len = e ? (int)(e - p) : (int)strlen(p);
        const char *v = NULL; char type = 0;
        if (len > 11 && !strncmp(p, "directory: ", 11)) { type = 'd'; v = p + 11; }
        else if (len > 6 && !strncmp(p, "file: ", 6))  { type = 'f'; v = p + 6; }
        if (type) {
            int vl = (int)(p + len - v);
            if (vl > (int)sizeof tmp - 2) vl = (int)sizeof tmp - 2;   /* MPD paths are far shorter */
            tmp[0] = type; memcpy(tmp + 1, v, vl);
            if (strtab_add(t, tmp, vl + 1) < 0) return -1;
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

int mpd_ack_text(const char *reply, char *out, int n)
{
    if (strncmp(reply, "ACK", 3)) return 0;
    const char *m = strchr(reply, '}');
    m = m ? m + 1 : reply + 3;
    while (*m == ' ') m++;
    int l = (int)strcspn(m, "\n");
    if (l > n - 1) l = n - 1;
    memcpy(out, m, l); out[l] = 0;
    return 1;
}

int mpd_update_poll(const char *status, int *fails)
{
    char v[32];
    if (!status || !strncmp(status, "ACK", 3))
        return ++*fails >= MPD_UPD_MAX_FAILS ? MPD_UPD_FAILED : MPD_UPD_RUNNING;
    *fails = 0;
    return mpd_field(status, "updating_db", v, sizeof v) ? MPD_UPD_RUNNING : MPD_UPD_DONE;
}

unsigned mpd_update_job(const char *reply)
{
    char v[16];
    if (!reply || !mpd_field(reply, "updating_db", v, sizeof v)) return 0;
    return (unsigned)strtoul(v, NULL, 10);
}

int mpd_update_poll_job(const char *status, unsigned job, int *fails)
{
    int st = mpd_update_poll(status, fails);
    if (st != MPD_UPD_RUNNING || !job || !status) return st;
    unsigned cur = mpd_update_job(status);
    unsigned ahead = (cur - job) & 0x7fffu;          /* MPD's update ids wrap at 1 << 15 */
    return cur && ahead && ahead < 0x4000u ? MPD_UPD_DONE : MPD_UPD_RUNNING;
}

int mpd_field(const char *reply, const char *key, char *out, int outlen)
{
    size_t kl = strlen(key);
    for (const char *p = reply; p && *p; ) {
        if (strncmp(p, key, kl) == 0 && p[kl] == ':' && p[kl+1] == ' ') {
            const char *v = p + kl + 2;
            const char *e = strchr(v, '\n'); int n = e ? (int)(e - v) : (int)strlen(v);
            if (n > outlen - 1) n = outlen - 1;
            memcpy(out, v, n); out[n] = 0; return 1;
        }
        const char *e = strchr(p, '\n'); if (!e) break; p = e + 1;
    }
    return 0;
}

void mpd_quote(const char *s, char *out, int outlen)
{
    /* On overflow the value is truncated but the result stays a well-formed
     * quoted string: room for the closing quote is reserved up front, and an
     * escape pair is only emitted when both bytes fit. */
    int o = 0;
    if (outlen < 3) { if (outlen > 0) out[0] = 0; return; }
    out[o++] = '"';
    for (; *s && o < outlen - 2; s++) {
        if (*s == '"' || *s == '\\') {
            if (o >= outlen - 3) break;   /* no room for both escape bytes */
            out[o++] = '\\';
        }
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

int mpd_parse_listing(const char *reply, mpd_entry_t *out, int max)
{
    int n = 0;
    for (const char *p = reply; p && *p && n < max; ) {
        const char *e = strchr(p, '\n'); int len = e ? (int)(e - p) : (int)strlen(p);
        int type = -1; const char *v = NULL;
        if (len > 11 && strncmp(p, "directory: ", 11) == 0) { type = 0; v = p + 11; }
        else if (len > 6 && strncmp(p, "file: ", 6) == 0)    { type = 1; v = p + 6; }
        if (type >= 0) {
            int vl = (int)((e ? e : p + len) - v); if (vl > 255) vl = 255;
            out[n].type = type; memcpy(out[n].name, v, vl); out[n].name[vl] = 0; n++;
        }
        if (!e) break; p = e + 1;
    }
    return n;
}

int mpd_parse_taglist(const char *reply, const char *tag, char out[][128], int max)
{
    size_t tl = strlen(tag); int n = 0;
    for (const char *p = reply; p && *p && n < max; ) {
        const char *e = strchr(p, '\n');
        if (strncmp(p, tag, tl) == 0 && p[tl] == ':' && p[tl+1] == ' ') {
            const char *v = p + tl + 2;
            int vl = (int)((e ? e : v + strlen(v)) - v); if (vl > 127) vl = 127;
            memcpy(out[n], v, vl); out[n][vl] = 0; n++;
        }
        if (!e) break; p = e + 1;
    }
    return n;
}

static int caps_count(const char *s)
{
    int n = 0;
    for (; *s; s++) n += (*s >= 'A' && *s <= 'Z');
    return n;
}
static int cicmp(const void *a, const void *b) { return strcasecmp(a, b); }

/* Merge tag values that differ only in capitalisation ("TOOL"/"Tool"), keeping
 * the spelling with the fewest capitals. Sorts the list case-insensitively as
 * a side effect (so folded groups are adjacent AND the list reads naturally).
 * ponytail: ASCII-only fold -- non-ASCII bytes compare exact, so accented
 * variants stay separate; add UTF-8 casefold tables if that ever bites. */
/* Classify an MPD `status` error string into a short user-facing message and a
 * hint. sd_present = does the SD music root currently exist (caller checks
 * access("/media/sd/audio", F_OK) == 0) -- it separates "the card is gone" from
 * "this one file went bad", which read identically in MPD's own text. Returns 1
 * and fills msg/hint when err is a real (non-empty) error, else 0. The match set
 * is the fixed casing MPD emits (strerror()-derived + its decoder text), so a
 * plain strstr is enough -- no case-folding needed.
 * ponytail: exact-substring match; if a future MPD reworks these strings, the
 * generic branch still shows MPD's own text, so it degrades safely. */
int mpd_classify_error(const char *err, int sd_present,
                       char *msg, int msglen, char *hint, int hintlen)
{
    if (!err || !*err) return 0;
    const char *m, *h;
    if (!sd_present) {
        m = "SD card unavailable";
        h = "Reinsert the card, then press Play";
    } else if (strstr(err, "Input/output error") || strstr(err, "I/O")) {
        m = "Can't read the SD card";
        h = "Check the card is seated, then press Play";
    } else if (strstr(err, "No such file") || strstr(err, "not found")) {
        m = "Track not found";
        h = "The file may have moved -- update the library";
    } else if (strstr(err, "decode") || strstr(err, "Decoder")) {
        m = "Can't play this track";
        h = "The file may be corrupt or an unsupported format";
    } else {
        m = "Playback error";
        h = err;   /* unknown class: fall back to MPD's own text */
    }
    snprintf(msg,  msglen,  "%s", m);
    snprintf(hint, hintlen, "%s", h);
    return 1;
}

int mpd_fold_case(char (*vals)[128], int nv)
{
    qsort(vals, nv, sizeof *vals, cicmp);
    int m = 0;
    for (int i = 0; i < nv; i++) {
        if (m && strcasecmp(vals[m-1], vals[i]) == 0) {
            if (caps_count(vals[i]) < caps_count(vals[m-1]))
                memcpy(vals[m-1], vals[i], sizeof *vals);
        } else {
            if (m != i) memcpy(vals[m], vals[i], sizeof *vals);
            m++;
        }
    }
    return m;
}

/* ---- album art ------------------------------------------------------------
 * `readpicture` (tag-embedded) then `albumart` (cover.{jpg,png,webp} beside the
 * file). Binary replies look like:  size: N\ntype: image/jpeg\nbinary: n\n<n bytes>\nOK\n
 * and come in binary_limit-sized chunks addressed by offset, so mpd_cmd's
 * "scan for an OK line" reader can't be used -- the payload may contain one. */
int mpd_bin_hdr(const char *line, int *size, int *nbin)
{
    if (strncmp(line, "size: ", 6) == 0)   { *size = atoi(line + 6); return 1; }
    if (strncmp(line, "binary: ", 8) == 0) { *nbin = atoi(line + 8); return 2; }
    if (strcmp(line, "OK") == 0)           return 3;
    if (strncmp(line, "ACK", 3) == 0)      return -1;
    return 0;
}

static int read_line(int s, char *l, int cap)
{
    int i = 0;
    for (;;) {
        char c; if (read(s, &c, 1) != 1) return -1;
        if (c == '\n') { l[i] = 0; return i; }
        if (i < cap - 1) l[i++] = c;
    }
}

/* Streaming reader. The payload is read straight from the socket into the
 * caller's buffer (no chunk buffer at all), so a multi-MB embedded cover never
 * sits in our RAM. MPD re-reads the whole picture for every readpicture call,
 * so the chunk size is what bounds the call count: measured on the device for
 * a 3.67 MB cover, 8 KB (MPD's default) = 449 calls / 12.6 s, 128 KB = 29
 * calls / 0.9 s warm, 1.9 s cold. Bigger chunks are faster still but grow
 * MPD's own transient (it buffers one chunk on top of the decoded picture). */
#define PIC_CHUNK 131072
#define PIC_IO_TIMEOUT_S 10   /* per socket op: a cold first chunk took 1.9 s */
#define PIC_DEADLINE_S   60   /* whole fetch; it runs off the UI thread */

static long long mono_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec;
}

/* Request the chunk at offset p->got and parse its header; the payload is left
 * in the socket (p->left bytes). 1 = chunk ready, 0 = no picture this way
 * (ACK / bare OK), -1 = protocol/IO error or deadline. */
static int pic_chunk(mpd_pic_t *p)
{
    char line[300];
    if (mono_s() > p->deadline) return -1;
    int li = snprintf(line, sizeof line, "%s %s %d\n", p->verb, p->q, p->got);
    if (li >= (int)sizeof line || write(p->s, line, li) < 0) return -1;
    int size = -1, nbin = -1;
    for (;;) {
        if (read_line(p->s, line, sizeof line) < 0) return -1;
        int k = mpd_bin_hdr(line, &size, &nbin);
        if (k < 0 || k == 3) return 0;            /* ACK, or OK before any binary */
        if (k == 2) break;
    }
    if (size <= 0 || size > p->cap || nbin <= 0 || nbin > PIC_CHUNK || p->got + nbin > size) return -1;
    if (p->size < 0) p->size = size;
    else if (size != p->size) return -1;          /* picture changed under us */
    p->left = nbin; p->got += nbin; p->trailer = 1; p->chunks++;
    return 1;
}

/* After a chunk's payload: the "\n" ending the blob, then "OK". */
static int pic_trailer(mpd_pic_t *p)
{
    char line[16];
    p->trailer = 0;
    return read_line(p->s, line, sizeof line) < 0 ||
           read_line(p->s, line, sizeof line) < 0 || strcmp(line, "OK") ? -1 : 0;
}

static int pic_open(mpd_pic_t *p, const char *uri, int cap, int embedded)
{
    memset(p, 0, sizeof *p);
    p->size = -1; p->cap = cap;
    if ((p->s = mpd_open()) < 0) return -1;
    struct timeval tv = { PIC_IO_TIMEOUT_S, 0 };
    setsockopt(p->s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(p->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    p->deadline = mono_s() + PIC_DEADLINE_S;
    mpd_quote(uri, p->q, sizeof p->q);
    char r[128];   /* NB the command is "binarylimit"; "binary_limit" is ACKed as unknown */
    if (write(p->s, "binarylimit 131072\n", 19) < 0 || read_line(p->s, r, sizeof r) < 0) goto fail;
    p->verb = embedded ? "readpicture" : "albumart";
    int k = pic_chunk(p);
    if (k == 0 && embedded) {                      /* no embedded picture: cover.* beside it */
        p->verb = "albumart";
        k = pic_chunk(p);
    }
    if (k <= 0) goto fail;
    return p->size;
fail:
    mpd_pic_close(p);
    return -1;
}
int mpd_pic_open(mpd_pic_t *p, const char *uri, int cap)       { return pic_open(p, uri, cap, 1); }
int mpd_pic_open_cover(mpd_pic_t *p, const char *uri, int cap) { return pic_open(p, uri, cap, 0); }

int mpd_pic_read(mpd_pic_t *p, unsigned char *dst, int n)
{
    unsigned char sink[512];
    int done = 0;
    while (done < n && p->s >= 0) {
        if (p->left == 0) {
            if (p->trailer && pic_trailer(p) < 0) break;
            if (p->got >= p->size || pic_chunk(p) <= 0) break;   /* end, or a failed fetch */
        }
        int k = n - done; if (k > p->left) k = p->left;
        if (!dst && k > (int)sizeof sink) k = (int)sizeof sink;
        int r = (int)read(p->s, dst ? dst + done : sink, k);
        if (r <= 0) break;
        p->left -= r; done += r;
    }
    return done;
}

void mpd_pic_close(mpd_pic_t *p)
{
    if (p->s >= 0) close(p->s);
    p->s = -1;
}

int mpd_readpicture(const char *uri, unsigned char **out, int cap)
{
    *out = NULL;
    mpd_pic_t p;
    int n = mpd_pic_open(&p, uri, cap);
    if (n > 0 && (*out = malloc(n)) && mpd_pic_read(&p, *out, n) != n) { free(*out); *out = NULL; }
    mpd_pic_close(&p);
    return *out ? n : -1;
}
