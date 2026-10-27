#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "ui_net.h"

/* Decode wpa_supplicant printf_encode escapes (\" \\ \e \n \r \t \xHH) from
 * [s,end) into out (cap chars + NUL). SSIDs arrive ESCAPED on the ctrl
 * interface; hex-encoding the escaped text on connect used to target a
 * different (nonexistent) SSID for any name with bytes outside plain ASCII. */
int wpa_unescape(const char *s, const char *end, char *out, int cap)
{
    int n = 0;
    while (s < end && n < cap) {
        char c = *s++;
        if (c == '\\' && s < end) {
            char e = *s++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'e': c = 0x1b; break;
            case 'x': {
                int v = 0, i = 0;
                while (i < 2 && s < end && isxdigit((unsigned char)*s)) {
                    unsigned char h = (unsigned char)*s++;
                    v = v * 16 + (h > '9' ? (tolower(h) - 'a' + 10) : h - '0');
                    i++;
                }
                c = (char)v;
                break;
            }
            default: c = e; break;   /* \" \\ and unknowns -> the literal char */
            }
        }
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

int wpa_parse_scan(const char *results, ui_wifi_ap_t *out, int max)
{
    int n = 0; const char *p = strchr(results, '\n');   /* skip header */
    if (!p) return 0; p++;
    while (*p && n < max) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        /* fields: bssid \t freq \t signal \t flags \t ssid */
        const char *f[5]; int fi = 0; const char *s = p;
        for (const char *q = p; q <= eol && fi < 5; q++) {
            if (q == eol || *q == '\t') { f[fi++] = s; s = q + 1; }
        }
        if (fi == 5) {
            const char *sig = f[2], *flags = f[3], *ssid = f[4];
            out[n].signal = atoi(sig);
            out[n].secured = (memmem(flags, ssid - flags, "WPA", 3) ||
                              memmem(flags, ssid - flags, "WEP", 3)) ? 1 : 0;
            wpa_unescape(ssid, eol, out[n].ssid, 32);   /* raw bytes; <= escaped len */
            if (out[n].ssid[0]) n++;
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return n;
}

#define WPA_PATH "/var/run/wpa_supplicant/wlan0"

int (*wpa_cmd_hook)(const char *cmd, char *reply, int rlen);   /* host tests */

int wpa_cmd(const char *cmd, char *reply, int rlen)
{
    if (wpa_cmd_hook) return wpa_cmd_hook(cmd, reply, rlen);
#ifdef EROSQ_HOST
    /* ponytail: EROSQ_FAKE_WIFI=1 answers like a live wpa_supplicant so the
     * WiFi page's on/connected states can be screenshotted; host-only. */
    if (getenv("EROSQ_FAKE_WIFI")) {
        const char *a = !strcmp(cmd, "PING") ? "PONG\n"
          : !strcmp(cmd, "STATUS") ? "bssid=00:11:22:33:44:55\nssid=HomeNet\nid=0\nwpa_state=COMPLETED\nip_address=192.168.1.42\n"
          /* saved: connected / in range / out of range / long escaped UTF-8 / disabled */
          : !strcmp(cmd, "LIST_NETWORKS") ? "network id / ssid / bssid / flags\n"
              "0\tHomeNet\tany\t[CURRENT]\n"
              "1\tCafe Free WiFi\tany\t\n"
              "2\tOffice\tany\t\n"
              "3\tMEGACORP WIRELESS GUEST NETWORK2\tany\t\n"
              "4\tOld Hotspot\tany\t[DISABLED]\n"
          : !strcmp(cmd, "SCAN_RESULTS") && !strcmp(getenv("EROSQ_FAKE_WIFI"), "empty")
              ? "bssid / frequency / signal level / flags / ssid\n"   /* =empty: scan finds nothing */
          : !strcmp(cmd, "SCAN_RESULTS") ? "bssid / frequency / signal level / flags / ssid\n"
              "00:11:22:33:44:55\t2412\t-48\t[WPA2-PSK-CCMP][ESS]\tHomeNet\n"
              "00:11:22:33:44:56\t2437\t-66\t[WPA2-PSK-CCMP][ESS]\tNeighbours Wireless Network 5GHz\n"
              "66:77:88:99:aa:bb\t2462\t-81\t[ESS]\tCafe Free WiFi\n"
          : "OK\n";
        snprintf(reply, rlen, "%s", a);
        return strlen(reply);
    }
#endif
    int s = socket(AF_UNIX, SOCK_DGRAM, 0); if (s < 0) return -1;
    /* 2s timeouts: this runs on the single LVGL thread (2s status poll while
     * the WiFi screen is up); wpa_supplicant dying between our write and its
     * reply must fail the read, not hang the UI forever. */
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* Bind an abstract-namespace endpoint (sun_path[0]='\0') so we can recv the
     * reply with no filesystem presence: no predictable /tmp path to pre-create
     * or symlink, and nothing to unlink. wpa replies to this source address. */
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    int ll = snprintf(local.sun_path + 1, sizeof local.sun_path - 1, "erosq-wpa-%d", getpid());
    socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 + ll;
    if (bind(s, (void *)&local, alen) < 0) { close(s); return -1; }
    struct sockaddr_un dst = { .sun_family = AF_UNIX };
    snprintf(dst.sun_path, sizeof dst.sun_path, "%s", WPA_PATH);
    if (connect(s, (void *)&dst, sizeof dst) < 0) { close(s); return -1; }
    if (write(s, cmd, strlen(cmd)) < 0) { close(s); return -1; }
    int n = read(s, reply, rlen - 1);
    if (n > 0) reply[n] = 0;
    close(s);
    return n;
}


/* Lowercase hex of a NUL-terminated string into out (>= 2*len+1). wpa_supplicant
 * takes an unquoted hex SSID, which avoids any quoting/injection in the ctrl
 * command for arbitrary over-the-air SSID bytes. */
void to_hex(const char *s, char *out)
{
    static const char h[] = "0123456789abcdef";
    while (*s) { unsigned char c = (unsigned char)*s++; *out++ = h[c >> 4]; *out++ = h[c & 0xf]; }
    *out = 0;
}

/* A WPA2 passphrase is 8..63 printable ASCII (0x20-0x7e); reject anything else
 * (esp. '"' and '\' that would break/inject the quoted ctrl command). */
int psk_ok(const char *p)
{
    size_t n = strlen(p);
    if (n < 8 || n > 63) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return 0;
    }
    return 1;
}

/* Run argv[0] (PATH-searched) to completion. argv is trusted (fixed verbs +
 * validated args), so no quoting concerns. */
static void run_wait(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) { execvp(argv[0], argv); _exit(127); }
    if (p > 0) { int s; waitpid(p, &s, 0); }
}

static void spawn_dhcp(void)
{
    /* One resident udhcpc (no -q/-n: it stays and RENEWS the lease — -q quit
     * after the first lease, so long sessions silently went IP-less at expiry;
     * and without a lease it retried forever). Kill any previous instance
     * first: a new association can mean a new subnet, and without the kill
     * every connect stacked another client. wifi_down.sh kills it on WiFi-off.
     * Double-fork so init reaps it: this UI never waits for it, and a plain
     * fork left one zombie per WiFi enable/connect for the UI's life. */
    char *kill_argv[] = { "killall", "udhcpc", NULL };
    run_wait(kill_argv);
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) { execl("/sbin/udhcpc", "udhcpc", "-i", "wlan0", (char *)NULL); _exit(127); }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);   /* reap the short-lived intermediate */
}

/* ADD_NETWORK, validated: a transport-level success can still carry a "FAIL"
 * reply (e.g. network-count limit), and atoi("FAIL")==0 used to silently drive
 * SET_NETWORK/SELECT_NETWORK against a network id that did not exist.
 * Returns the new id, or -1. */
static int wpa_add_network(void)
{
    char r[64];
    if (wpa_cmd("ADD_NETWORK", r, sizeof r) <= 0) return -1;
    if (r[0] < '0' || r[0] > '9') return -1;
    return atoi(r);
}

static int wpa_ok(const char *cmd)
{
    char r[64];
    return wpa_cmd(cmd, r, sizeof r) > 0 && strncmp(r, "OK", 2) == 0;
}

/* LIST_NETWORKS body -> out[]: "network id / ssid / bssid / flags" header, then
 * "<id>\t<ssid>\t<bssid>\t<flags>" rows; flags are [CURRENT][DISABLED]
 * [TEMP-DISABLED][P2P-PERSISTENT] (2.12 ctrl_iface.c). SSIDs are printf_encode
 * escaped, as in SCAN_RESULTS. Returns the row count (<= max). */
int wpa_parse_networks(const char *list, wpa_net_t *out, int max)
{
    int n = 0; const char *p = strchr(list, '\n');   /* skip header */
    if (!p) return 0;
    p++;
    while (*p && n < max) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        const char *f[4]; int fi = 0; const char *s = p;
        for (const char *q = p; q <= eol && fi < 4; q++)
            if (q == eol || *q == '\t') { f[fi++] = s; s = q + 1; }
        if (fi == 4 && f[0][0] >= '0' && f[0][0] <= '9') {
            out[n].id = atoi(f[0]);
            out[n].ssid_len = wpa_unescape(f[1], f[2] - 1, out[n].ssid, 32);
            out[n].current       = memmem(f[3], eol - f[3], "[CURRENT]", 9) != NULL;
            out[n].disabled      = memmem(f[3], eol - f[3], "[DISABLED]", 10) != NULL;
            out[n].temp_disabled = memmem(f[3], eol - f[3], "[TEMP-DISABLED]", 15) != NULL;
            n++;
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return n;
}

/* Joining a network must NOT touch the other saved networks. SELECT_NETWORK
 * (the user picked THIS one) disables every other network in wpa_supplicant's
 * memory, and a SAVE_CONFIG then would persist them as disabled=1 -- no more
 * auto-join. So the join is a small transaction, finished by wpa_join_poll():
 *   start:   ADD the new block, SELECT it; remember which networks were enabled
 *            and which older blocks carry the same SSID.
 *   success: (COMPLETED on the new id) REMOVE the same-SSID older blocks,
 *            ENABLE_NETWORK <id> no-connect the others (2.12: clears disabled,
 *            no scan/reconnect), then SAVE_CONFIG.
 *   failure: (wrong key -> [TEMP-DISABLED], or timeout) REMOVE the new block,
 *            re-ENABLE the others (wpa resumes auto-join), NO save -- the file
 *            on /data still holds exactly what it held before.
 * Nothing is saved until the join succeeded. */
#define JOIN_MAXNET 32
static struct {
    int active, id, polls;
    int fresh;                     /* id is a block WE added (new network): drop it on fail */
    int n_en, en[JOIN_MAXNET];     /* enabled before the SELECT */
    int n_old, old[JOIN_MAXNET];   /* older blocks with the same SSID */
} s_join;

static int join_is_old(int id)
{
    for (int i = 0; i < s_join.n_old; i++) if (s_join.old[i] == id) return 1;
    return 0;
}

/* Re-enable what SELECT disabled. connect=0 after a join: no-connect, so the
 * fresh link is left alone; skip_old: the same-SSID blocks were removed. */
static void join_restore(int connect, int skip_old)
{
    char cmd[48];
    for (int i = 0; i < s_join.n_en; i++) {
        if (skip_old && join_is_old(s_join.en[i])) continue;
        snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d%s", s_join.en[i],
                 connect ? "" : " no-connect");
        wpa_ok(cmd);
    }
}

static void join_fail(void)
{
    char cmd[32];
    snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", s_join.id);
    if (s_join.fresh) wpa_ok(cmd);   /* a saved network stays saved */
    join_restore(1, 0);
    s_join.active = 0;
}

int wpa_join_pending(void) { return s_join.active; }

int wpa_join_poll(void)
{
    if (!s_join.active) return 0;
    char r[4096];
    if (wpa_cmd("STATUS", r, sizeof r) <= 0) {   /* wpa gone (Wi-Fi off): its */
        s_join.active = 0;                       /* memory went with it; the  */
        return -1;                               /* file is untouched         */
    }
    const char *st = strstr(r, "wpa_state=");
    const char *id = (strncmp(r, "id=", 3) == 0) ? r : strstr(r, "\nid=");
    if (id && *id == '\n') id++;
    if (st && strncmp(st + 10, "COMPLETED", 9) == 0 && id && atoi(id + 3) == s_join.id) {
        char cmd[32];
        for (int i = 0; i < s_join.n_old; i++) {
            snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", s_join.old[i]);
            wpa_ok(cmd);
        }
        join_restore(0, 1);
        s_join.active = 0;
        wpa_ok("SAVE_CONFIG");   /* persist to /data conf (update_config=1) */
        return 1;
    }
    int temp = 0;
    if (wpa_cmd("LIST_NETWORKS", r, sizeof r) > 0) {
        wpa_net_t nets[JOIN_MAXNET];
        int n = wpa_parse_networks(r, nets, JOIN_MAXNET);
        for (int i = 0; i < n; i++)
            if (nets[i].id == s_join.id && nets[i].temp_disabled) temp = 1;
    }
    if (temp || ++s_join.polls >= WPA_JOIN_POLLS) { join_fail(); return -1; }
    return 0;
}

static int wpa_join_start(const char *ssid, const char *psk)   /* psk NULL = open */
{
    char cmd[200], hex[67], r[4096];   /* ssid <= 32 bytes -> <= 64 hex + NUL */
    if (s_join.active) join_fail();    /* a newer pick supersedes */
    if (wpa_cmd("LIST_NETWORKS", r, sizeof r) <= 0) return -1;
    wpa_net_t nets[JOIN_MAXNET];
    int n = wpa_parse_networks(r, nets, JOIN_MAXNET);
    int id = wpa_add_network();
    if (id < 0) return -1;
    to_hex(ssid, hex);
    int ok = 1;
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, hex);           ok &= wpa_ok(cmd);
    if (psk) snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk \"%s\"", id, psk);
    else     snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id);
    ok &= wpa_ok(cmd);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d scan_ssid 1", id);  ok &= wpa_ok(cmd);  /* find hidden */
    if (!ok) {
        snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id); wpa_ok(cmd);
        return -1;
    }
    s_join.id = id; s_join.fresh = 1; s_join.polls = 0; s_join.n_en = 0; s_join.n_old = 0;
    for (int i = 0; i < n; i++) {
        if (!nets[i].disabled) s_join.en[s_join.n_en++] = nets[i].id;
        if (strcmp(nets[i].ssid, ssid) == 0) s_join.old[s_join.n_old++] = nets[i].id;
    }
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);   /* enables it, disables the rest */
    if (!wpa_ok(cmd)) {
        snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id); wpa_ok(cmd);
        join_restore(1, 0);
        return -1;
    }
    s_join.active = 1;
    if (!wpa_cmd_hook) spawn_dhcp();   /* host tests must not killall the host's udhcpc */
    return 0;
}

int wpa_connect_open(const char *ssid)
{
    return wpa_join_start(ssid, NULL);
}

int wpa_connect_psk(const char *ssid, const char *psk)
{
    if (!psk_ok(psk)) return -2;   /* malformed/injecting passphrase (distinct
                                    * from -1 = wpa_supplicant unreachable, so
                                    * the UI blames the right thing) */
    return wpa_join_start(ssid, psk);
}

/* Join a SAVED network: the same transaction as a new join, minus ADD and
 * the same-SSID cleanup. SELECT it (wpa disables the others in memory); on
 * COMPLETED wpa_join_poll re-enables the others no-connect and saves; on
 * failure the others are re-enabled with connect and nothing is saved -- the
 * selected block itself stays saved (fresh = 0). A [DISABLED] block is refused:
 * it is not joinable from the saved list (see wifi_saved_build). */
int wpa_join_saved(int id)
{
    char cmd[48], r[4096];
    if (s_join.active) join_fail();    /* a newer pick supersedes */
    if (wpa_cmd("LIST_NETWORKS", r, sizeof r) <= 0) return -1;
    wpa_net_t nets[JOIN_MAXNET];
    int n = wpa_parse_networks(r, nets, JOIN_MAXNET), found = 0;
    s_join.id = id; s_join.fresh = 0; s_join.polls = 0; s_join.n_en = 0; s_join.n_old = 0;
    for (int i = 0; i < n; i++) {
        if (nets[i].id == id) { found = !nets[i].disabled; continue; }
        if (!nets[i].disabled) s_join.en[s_join.n_en++] = nets[i].id;
    }
    if (!found) return -1;
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
    if (!wpa_ok(cmd)) { join_restore(1, 0); return -1; }
    s_join.active = 1;
    if (!wpa_cmd_hook) spawn_dhcp();   /* new network, maybe a new subnet */
    return 0;
}

/* Forget = every block carrying this SSID (a legacy conf can hold duplicates;
 * leaving one would let wpa -- or wifi_join.sh's REASSOCIATE -- rejoin the
 * "forgotten" network at once). Removing the CURRENT block makes wpa 2.12
 * deauthenticate (DEAUTH_LEAVING) without setting its `disconnected` flag, so
 * it goes on to auto-join another enabled saved network, if any. A pending
 * join is settled first: SAVE_CONFIG mid-join would persist SELECT's temporary
 * disabled=1 on every other network. */
/* After the UI dropped the association (Forget of the connected network):
 * udhcpc keeps its lease when wpa deauthenticates -- busybox deconfigs on
 * SIGTERM only with -R -- so the old IPv4 stayed on wlan0. erosq-br's
 * `wifi_dhcp.sh check` stops the client and runs its deconfig while wpa is
 * not COMPLETED; the watcher restart then gets a fresh client as soon as wpa
 * completes on another saved network (it calls `check` on COMPLETED).
 * Backgrounded like wifi_set_enabled: the stop can take up to ~5 s. */
void wifi_lease_drop(void)
{
#ifndef EROSQ_HOST
    if (wpa_cmd_hook) return;   /* host tests */
    if (system("(/usr/bin/wifi_dhcp.sh check; /usr/bin/wifi_join.sh start)"
               " </dev/null >/dev/null 2>&1 &")) { /* ignore */ }
#endif
}

int wpa_forget(const char *ssid, int len, int *was_current)
{
    char cmd[32], r[4096];
    *was_current = 0;
    if (s_join.active) join_fail();
    if (wpa_cmd("LIST_NETWORKS", r, sizeof r) <= 0) return -1;
    wpa_net_t nets[JOIN_MAXNET];
    int n = wpa_parse_networks(r, nets, JOIN_MAXNET), removed = 0, err = 0;
    for (int i = 0; i < n; i++) {
        if (nets[i].ssid_len != len || memcmp(nets[i].ssid, ssid, len)) continue;
        snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", nets[i].id);
        if (wpa_ok(cmd)) { removed++; *was_current |= nets[i].current; } else err = 1;
    }
    if (!removed) return err ? -1 : 0;   /* already gone: nothing to save */
    return wpa_ok("SAVE_CONFIG") ? 0 : -1;
}

/* Label-safe copy of raw SSID bytes: well-formed UTF-8 passes through, every
 * other byte (bad/overlong/surrogate sequences, C0/C1 controls, DEL) becomes
 * '?'. A '?' replaces at least one byte, so out never outgrows the input. */
int wifi_ssid_display(const char *raw, int len, char *out, int cap)
{
    const unsigned char *s = (const unsigned char *)raw;
    int i = 0, o = 0;
    while (i < len && o < cap - 1) {
        unsigned c = s[i];
        int k = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3
              : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        int ok = k > 0 && i + k <= len;
        if (ok && k > 1) {
            unsigned lo = 0x80, hi = 0xBF, c1 = s[i + 1];   /* second-byte range */
            if (c == 0xE0) lo = 0xA0;            /* overlong */
            else if (c == 0xED) hi = 0x9F;       /* surrogates */
            else if (c == 0xF0) lo = 0x90;       /* overlong */
            else if (c == 0xF4) hi = 0x8F;       /* > U+10FFFF */
            ok = c1 >= lo && c1 <= hi;
            for (int j = 2; ok && j < k; j++) ok = (s[i + j] & 0xC0) == 0x80;
            if (ok && c == 0xC2 && c1 < 0xA0) ok = 0;   /* C1 controls */
        }
        if (ok && k == 1 && (c < 0x20 || c == 0x7F)) ok = 0;
        if (!ok) { out[o++] = '?'; i++; continue; }
        if (o + k > cap - 1) break;
        memcpy(out + o, s + i, k); o += k; i += k;
    }
    out[o] = 0;
    return o;
}

/* STATUS: "ssid=" and "id=" appear only once associated (>= ASSOCIATED); the
 * ssid is printf_encode-escaped like LIST_NETWORKS'. */
int wpa_status_parse(const char *r, char ssid[33], int *ssid_len, char ip[64], int *completed)
{
    int id = -1, sl = 0;
    ssid[0] = 0; ip[0] = 0; *completed = 0;
    for (const char *p = r; *p; ) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        if (!strncmp(p, "ssid=", 5)) sl = wpa_unescape(p + 5, eol, ssid, 32);
        else if (!strncmp(p, "id=", 3) && p[3] >= '0' && p[3] <= '9') id = atoi(p + 3);
        else if (!strncmp(p, "wpa_state=COMPLETED", 19) && eol - p == 19) *completed = 1;
        else if (!strncmp(p, "ip_address=", 11)) {
            int l = eol - p - 11; if (l > 63) l = 63;
            memcpy(ip, p + 11, l); ip[l] = 0;
        }
        p = *eol ? eol + 1 : eol;
    }
    if (ssid_len) *ssid_len = sl;
    return id;
}

static int join_was_enabled(int id)
{
    for (int i = 0; i < s_join.n_en; i++) if (s_join.en[i] == id) return 1;
    return 0;
}

/* One row per SSID, in three bands: connected, joinable, disabled; wpa's list
 * order within a band. While a join is pending the list is shown as it will
 * be if the join fails (= as it was): a fresh block is not saved yet, so it
 * is hidden, and the others' SELECT-time [DISABLED] is ignored. */
int wifi_saved_build(const wpa_net_t *nets, int n, int cur_id, int completed,
                     wifi_saved_t *out, int max)
{
    wifi_saved_t t[WIFI_SAVED_MAX];
    int m = 0;
    for (int i = 0; i < n; i++) {
        const wpa_net_t *w = &nets[i];
        int pend = s_join.active && w->id == s_join.id;
        if (pend && s_join.fresh) continue;
        if (w->ssid_len <= 0) continue;          /* no name: nothing to show or match */
        int dis = w->disabled && !pend && !(s_join.active && join_was_enabled(w->id));
        int st = (completed && w->id == cur_id) ? WIFI_SAVED_CONNECTED
               : (w->current || pend) ? WIFI_SAVED_JOINING : WIFI_SAVED_IDLE;
        int j = 0;
        while (j < m && !(t[j].ssid_len == w->ssid_len && !memcmp(t[j].ssid, w->ssid, w->ssid_len))) j++;
        if (j == m) {
            if (m == WIFI_SAVED_MAX) continue;
            memset(&t[m], 0, sizeof t[m]);
            t[m].id = w->id; t[m].disabled = dis; t[m].state = st;
            memcpy(t[m].ssid, w->ssid, w->ssid_len); t[m].ssid_len = w->ssid_len;
            wifi_ssid_display(t[m].ssid, t[m].ssid_len, t[m].name, sizeof t[m].name);
            m++;
            continue;
        }
        /* duplicate SSID: the live/selected block wins, else the first enabled */
        if (st > t[j].state || (st == t[j].state && t[j].disabled && !dis)) t[j].id = w->id;
        if (st > t[j].state) t[j].state = st;
        t[j].disabled = t[j].disabled && dis;
    }
    int k = 0;
    for (int band = 0; band < 3; band++)
        for (int j = 0; j < m && k < max; j++) {
            int b = t[j].state == WIFI_SAVED_CONNECTED ? 0 : t[j].disabled ? 2 : 1;
            if (b == band) out[k++] = t[j];
        }
    return k;
}

int wifi_saved_list(wifi_saved_t *out, int max)
{
    char r[4096], ssid[33], ip[64];
    int completed;
    if (wpa_cmd("STATUS", r, sizeof r) <= 0) return -1;
    int cur = wpa_status_parse(r, ssid, NULL, ip, &completed);
    if (wpa_cmd("LIST_NETWORKS", r, sizeof r) <= 0) return -1;
    wpa_net_t nets[WIFI_SAVED_MAX];
    int n = wpa_parse_networks(r, nets, WIFI_SAVED_MAX);
    return wifi_saved_build(nets, n, cur, completed, out, max);
}

void wifi_status(char *out, int n, char ssid[33])
{
    ssid[0] = 0;
    char r[1024]; if (wpa_cmd("STATUS", r, sizeof r) <= 0) { snprintf(out, n, "Not connected"); return; }
    char ip[64], name[33]; int sl, completed;
    wpa_status_parse(r, ssid, &sl, ip, &completed);
    wifi_ssid_display(ssid, sl, name, sizeof name);
    if (ssid[0]) snprintf(out, n, "Connected to %s \xe2\x80\xa2 %s", name, ip[0] ? ip : "getting address...");
    else snprintf(out, n, "Not connected");
}

/* Plain-word signal for the network list (raw dBm meant nothing to users).
 * ponytail: fixed brcmfmac-dBm thresholds; tune if a different chip reports
 * on another scale. */
const char *wifi_strength(int dbm)
{
    return dbm >= -60 ? "Strong" : dbm >= -72 ? "Good" : "Weak";
}

/* Locate the WiFi POWER-GATE rfkill's "soft" control file by NAME
 * (erosq-wifi-power, the pwrseq hard gate: WL_REG_ON). Matching by type=wlan
 * was fragile: once the driver is bound, cfg80211's phy0 rfkill is ALSO type
 * wlan, and readdir order decided which one we read -- the wrong pick would
 * report soft-kill state instead of actual chip power. Returns 0 + fills out. */
static int wlan_rfkill_soft(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/rfkill");
    if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "rfkill", 6) != 0) continue;
        char tp[300], buf[32];   /* d_name can be NAME_MAX; size to avoid truncation */
        snprintf(tp, sizeof tp, "/sys/class/rfkill/%s/name", e->d_name);
        int fd = open(tp, O_RDONLY); if (fd < 0) continue;
        int r = read(fd, buf, sizeof buf - 1); close(fd);
        if (r <= 0) continue; buf[r] = 0;
        if (strncmp(buf, "erosq-wifi-power", 16) == 0 &&
            (buf[16] == '\n' || buf[16] == 0)) {
            snprintf(out, n, "/sys/class/rfkill/%s/soft", e->d_name); rc = 0; break;
        }
    }
    closedir(d);
    return rc;
}

int wifi_is_enabled(void)
{
    /* "Enabled" must mean the stack is actually usable, not just that the radio
     * is powered. The rfkill soft-state alone reported On while wpa_supplicant
     * was dead (chip powered, daemon gone) -- a trap: the UI showed On, Scan
     * found nothing, and no toggle ever re-ran bringup. So: radio must be
     * unblocked AND wpa_supplicant must answer on its control socket. */
    char soft[300];
    if (wlan_rfkill_soft(soft, sizeof soft) == 0) {   /* radio blocked -> off */
        char b[4]; int fd = open(soft, O_RDONLY);
        if (fd >= 0) { int r = read(fd, b, sizeof b - 1); close(fd);
                       if (r > 0) { b[r] = 0; if (atoi(b)) return 0; } }   /* soft=1 -> blocked */
    }
    /* Ground truth that wpa is alive and responsive: PING -> PONG. A dead
     * daemon makes wpa_cmd fail (no listener) even if a stale socket file
     * lingers, so this also rejects the stale-socket case. */
    char r[16];
    return (wpa_cmd("PING", r, sizeof r) > 0 && strncmp(r, "PONG", 4) == 0) ? 1 : 0;
}

/* Record the user's most-recent WiFi power INTENT (/run/wifi-want, "1"/"0").
 * The backgrounded scripts check it after taking the power mutex, so a fast
 * off<->on converges on the LAST intent regardless of which script wins the
 * lock (same scheme as /run/bt-want; see bt_up.sh). */
static void wifi_want_save(int on)
{
    int fd = open("/run/wifi-want", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, on ? "1" : "0", 1) < 0) { /* ignore */ }
    close(fd);
}

void wifi_set_enabled(int on)
{
    /* BACKGROUNDED, like the BT toggle: a cold bring-up takes ~5-15s (chip
     * power-up + SDIO enumerate + firmware + wpa_supplicant) and this runs on
     * the single LVGL thread -- foreground it froze the whole UI including
     * the power button. wifi_up.sh/wifi_down.sh own the power gate, regdomain,
     * wpa_supplicant and udhcpc lifecycles (mutexed + last-intent, honest exit
     * codes); the UI reads the RESULTING state via wifi_is_enabled() (rfkill +
     * PING ground truth), so no optimistic flag can wedge stuck-ON. */
    wifi_want_save(on);
    if (system(on ? "/usr/bin/wifi_up.sh >/dev/null 2>&1 &"
                  : "/usr/bin/wifi_down.sh >/dev/null 2>&1 &")) { /* ignore */ }
}

/* The chosen country is persisted here so it survives the kernel dropping the
 * regdomain to default whenever the wiphy goes down (WiFi disabled) and across
 * reboots. This file is the UI's source of truth for the country; wifi_up.sh
 * re-applies it on every bring-up. MUST live on /data: the old /etc path was
 * on the RO squashfs, so the save failed silently and the boot re-apply was a
 * no-op -- the country reverted to default on every reboot. */
#define COUNTRY_FILE "/data/erosq-wifi-country"

static int country_load(char *cc)   /* cc[3]; 0 on success */
{
    int fd = open(COUNTRY_FILE, O_RDONLY); if (fd < 0) return -1;
    int r = read(fd, cc, 2); close(fd);
    if (r == 2 && cc[0] > ' ' && cc[1] > ' ') { cc[2] = 0; return 0; }
    return -1;
}

int wifi_get_country(char *cc)   /* cc[3] */
{
    if (country_load(cc) == 0) return 0;   /* prefer the user's saved choice */
    cc[0] = 0;
    FILE *f = popen("iw reg get 2>/dev/null", "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "country ");
        if (p && p[8] && p[9]) { cc[0] = p[8]; cc[1] = p[9]; cc[2] = 0; break; }
    }
    pclose(f);
    return cc[0] ? 0 : -1;
}

void wifi_set_country(const char *cc)
{
    char code[3] = { cc[0], cc[1], 0 };   /* exactly 2 chars, from our fixed list */
    char *a[] = { "iw", "reg", "set", code, NULL };
    run_wait(a);
    int fd = open(COUNTRY_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if (write(fd, code, 2) < 0) { /* ignore */ } close(fd); }
}

/* Re-apply the saved country to the kernel (after enabling WiFi / at startup),
 * since the regdomain resets when the wiphy goes down. */
void wifi_apply_saved_country(void)
{
    char cc[3];
    if (country_load(cc) != 0) return;
    char *a[] = { "iw", "reg", "set", cc, NULL };
    run_wait(a);
}

/* Remember the user's WiFi on/off choice across reboots. One char, 0/1; mirrors
 * the one-value-per-file convention in ui_power.c (silent skip if /data absent). */
#define WIFI_PREF_PATH "/data/erosq-ui.wifi"

void wifi_pref_save(int on)
{
    int fd = open(WIFI_PREF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* no /data -> just don't persist */
    if (write(fd, on ? "1" : "0", 1) < 0) { /* ignore */ }
    close(fd);
}

/* Apply the saved choice at boot. Only acts on a saved "on": WiFi is deferred
 * (radio off) at boot, so "off" and "never toggled" are already satisfied.
 * ponytail: assumes boot default is radio-off; if that ever changes, add an
 * else branch -> wifi_set_enabled(0). */
void wifi_pref_restore(void)
{
    int fd = open(WIFI_PREF_PATH, O_RDONLY);
    if (fd < 0) return;
    char b; int r = read(fd, &b, 1); close(fd);
    if (r == 1 && b == '1') wifi_set_enabled(1);
}

