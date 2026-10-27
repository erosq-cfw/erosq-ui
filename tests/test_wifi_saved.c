/* Saved Wi-Fi networks: the list, joining a saved network, and Forget.
 * Same in-memory wpa_supplicant as test_wpa_join.c (2.12 ctrl semantics,
 * ctrl_iface.c / wpa_supplicant.c), plus what this feature relies on:
 *   SELECT_NETWORK n also clears n's temp-disable (wpas_clear_temp_disabled);
 *   REMOVE_NETWORK of the CURRENT block deauthenticates (DEAUTH_LEAVING, no
 *   `disconnected` flag) -> STATUS drops ssid/id, state DISCONNECTED;
 *   LIST_NETWORKS prints printf_encode-escaped SSIDs (\xNN, \", \\, \e ...). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_net.h"

#define MAXN 16
static struct net { int used, id, disabled, temp; char ssid[40]; int slen; } N[MAXN];
static int next_id, saves, wpa_up = 1;
static char state[32] = "SCANNING"; static int cur_id = -1;
static struct net saved[MAXN]; static int n_saved;
static char log_[8192];

static struct net *find(int id)
{
    for (int i = 0; i < MAXN; i++) if (N[i].used && N[i].id == id) return &N[i];
    return NULL;
}

/* wpa's printf_encode, so the parser sees what the real daemon prints */
static int enc(char *o, const char *s, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c == 0x1b) { o[k++] = '\\'; o[k++] = 'e'; }
        else if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
        else if (c == '\t') { o[k++] = '\\'; o[k++] = 't'; }
        else if (c >= 32 && c <= 126) o[k++] = (char)c;
        else k += sprintf(o + k, "\\x%02x", c);
    }
    o[k] = 0;
    return k;
}

static int fake(const char *cmd, char *r, int rlen)
{
    if (!wpa_up) return -1;
    strcat(log_, cmd); strcat(log_, "\n");
    int id; char a[80], b[80];
    if (!strcmp(cmd, "ADD_NETWORK")) {
        for (int i = 0; i < MAXN; i++) if (!N[i].used) {
            memset(&N[i], 0, sizeof N[i]); N[i].used = 1; N[i].id = next_id++; N[i].disabled = 1;
            return snprintf(r, rlen, "%d\n", N[i].id);
        }
        return snprintf(r, rlen, "FAIL\n");
    }
    if (sscanf(cmd, "SET_NETWORK %d %79s %79[^\n]", &id, a, b) == 3) {
        struct net *n = find(id); if (!n) return snprintf(r, rlen, "FAIL\n");
        if (!strcmp(a, "ssid")) {   /* the UI sends hex */
            n->slen = 0;
            for (int j = 0; b[2*j] && b[2*j+1] && j < 32; j++) { unsigned v; sscanf(b + 2*j, "%2x", &v); n->ssid[n->slen++] = (char)v; }
        }
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "SELECT_NETWORK %d", &id) == 1) {
        struct net *s = find(id); if (!s) return snprintf(r, rlen, "FAIL\n");
        for (int i = 0; i < MAXN; i++) if (N[i].used) N[i].disabled = (N[i].id != id);
        s->temp = 0;
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "ENABLE_NETWORK %d", &id) == 1) {
        struct net *n = find(id); if (!n) return snprintf(r, rlen, "FAIL\n");
        n->disabled = 0; n->temp = 0;
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "REMOVE_NETWORK %d", &id) == 1) {
        struct net *n = find(id); if (!n) return snprintf(r, rlen, "FAIL\n");
        n->used = 0;
        if (id == cur_id) { cur_id = -1; strcpy(state, "DISCONNECTED"); }   /* deauth */
        return snprintf(r, rlen, "OK\n");
    }
    if (!strcmp(cmd, "SAVE_CONFIG")) {
        saves++; n_saved = 0;
        for (int i = 0; i < MAXN; i++) if (N[i].used) saved[n_saved++] = N[i];
        return snprintf(r, rlen, "OK\n");
    }
    if (!strcmp(cmd, "LIST_NETWORKS")) {
        int k = snprintf(r, rlen, "network id / ssid / bssid / flags\n");
        for (int i = 0; i < MAXN; i++) if (N[i].used) {
            char e[160]; enc(e, N[i].ssid, N[i].slen);
            k += snprintf(r + k, rlen - k, "%d\t%s\tany\t%s%s%s\n", N[i].id, e,
                          N[i].id == cur_id ? "[CURRENT]" : "",
                          N[i].disabled ? "[DISABLED]" : "", N[i].temp ? "[TEMP-DISABLED]" : "");
        }
        return k;
    }
    if (!strcmp(cmd, "STATUS")) {
        struct net *c = find(cur_id);
        if (c) {
            char e[160]; enc(e, c->ssid, c->slen);
            return snprintf(r, rlen, "bssid=00:11:22:33:44:55\nfreq=2412\nssid=%s\nid=%d\nmode=station\n"
                            "wpa_state=%s\nip_address=10.0.0.7\naddress=00:00:00:00:00:01\n", e, cur_id, state);
        }
        return snprintf(r, rlen, "wpa_state=%s\naddress=00:00:00:00:00:01\n", state);
    }
    return snprintf(r, rlen, "OK\n");
}

static int seed_n(const char *ssid, int len, int disabled)
{
    for (int i = 0; i < MAXN; i++) if (!N[i].used) {
        memset(&N[i], 0, sizeof N[i]); N[i].used = 1; N[i].id = next_id++;
        N[i].disabled = disabled; memcpy(N[i].ssid, ssid, len); N[i].slen = len;
        return N[i].id;
    }
    return -1;
}
static int seed(const char *ssid, int disabled) { return seed_n(ssid, (int)strlen(ssid), disabled); }

static void reset(void)
{
    while (wpa_join_pending()) { wpa_up = 0; wpa_join_poll(); wpa_up = 1; }
    memset(N, 0, sizeof N); next_id = 0; saves = 0; n_saved = 0; wpa_up = 1;
    strcpy(state, "SCANNING"); cur_id = -1; log_[0] = 0;
}
static void connect_to(int id) { cur_id = id; strcpy(state, "COMPLETED"); }
static int in_saved(const char *ssid)
{
    int c = 0;
    for (int i = 0; i < n_saved; i++) c += saved[i].slen == (int)strlen(ssid) && !memcmp(saved[i].ssid, ssid, saved[i].slen);
    return c;
}
static const wifi_saved_t *row(const wifi_saved_t *s, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (!strcmp(s[i].name, name)) return &s[i];
    return NULL;
}

int main(void)
{
    wpa_cmd_hook = fake;
    char o[64];
    wifi_saved_t s[WIFI_SAVED_MAX];
    int n;

    /* --- display sanitiser ------------------------------------------------ */
    assert(wifi_ssid_display("Caf\xc3\xa9", 5, o, sizeof o) == 5 && !strcmp(o, "Caf\xc3\xa9"));
    assert(wifi_ssid_display("\xe2\x80\x94\xf0\x9f\x93\xb6", 7, o, sizeof o) == 7);   /* 3- and 4-byte */
    assert(wifi_ssid_display("a\x1b[2Jb", 6, o, sizeof o) == 6 && !strcmp(o, "a?[2Jb"));  /* escape seq */
    assert(wifi_ssid_display("a\nb\tc\x7f", 6, o, sizeof o) && !strcmp(o, "a?b?c?"));
    assert(wifi_ssid_display("x\0y", 3, o, sizeof o) == 3 && !strcmp(o, "x?y"));          /* NUL inside */
    assert(wifi_ssid_display("\xff\xfe", 2, o, sizeof o) == 2 && !strcmp(o, "??"));       /* not UTF-8 */
    assert(wifi_ssid_display("\xc0\xaf", 2, o, sizeof o) && !strcmp(o, "??"));            /* overlong '/' */
    assert(wifi_ssid_display("\xe0\x80\xaf", 3, o, sizeof o) && !strcmp(o, "???"));       /* overlong */
    assert(wifi_ssid_display("\xed\xa0\x80", 3, o, sizeof o) && !strcmp(o, "???"));       /* surrogate */
    assert(wifi_ssid_display("\xf4\x90\x80\x80", 4, o, sizeof o) && !strcmp(o, "????"));  /* > U+10FFFF */
    assert(wifi_ssid_display("\xc2\x85", 2, o, sizeof o) && !strcmp(o, "??"));            /* C1 NEL */
    assert(wifi_ssid_display("\xc2\xa0z", 3, o, sizeof o) == 3);                          /* NBSP is fine */
    assert(wifi_ssid_display("ab\xc3", 3, o, sizeof o) == 3 && !strcmp(o, "ab?"));        /* cut sequence */
    assert(wifi_ssid_display("\xc3\xa9\xc3\xa9", 4, o, 4) == 2 && !strcmp(o, "\xc3\xa9")); /* no half char */
    { char raw[32]; memset(raw, 0xff, 32);                                                /* 32 bad bytes */
      assert(wifi_ssid_display(raw, 32, o, 33) == 32); }

    /* --- STATUS parse: escaped ssid, id, COMPLETED, ip -------------------- */
    { char ss[33], ip[64]; int sl, c;
      assert(wpa_status_parse("bssid=00:11:22:33:44:55\nssid=Caf\\xc3\\xa9 \\\"Q\\\"\nid=3\nwpa_state=COMPLETED\nip_address=10.1.2.3\n",
                              ss, &sl, ip, &c) == 3);
      assert(!strcmp(ss, "Caf\xc3\xa9 \"Q\"") && sl == 9 && c == 1 && !strcmp(ip, "10.1.2.3"));
      assert(wpa_status_parse("wpa_state=ASSOCIATING\nssid=x\nid=1\n", ss, &sl, ip, &c) == 1 && c == 0);
      assert(wpa_status_parse("wpa_state=DISCONNECTED\naddress=00:00:00:00:00:01\n", ss, &sl, ip, &c) == -1 && !ss[0] && !ip[0]);
      assert(wpa_status_parse("", ss, &sl, ip, &c) == -1); }

    /* --- LIST_NETWORKS: escapes, hex, [CURRENT], length ------------------- */
    { wpa_net_t w[4];
      assert(wpa_parse_networks("network id / ssid / bssid / flags\n"
                                "0\ta\\x00b\tany\t[CURRENT]\n"
                                "1\tq\\\\\\\"\tany\t[DISABLED]\n", w, 4) == 2);
      assert(w[0].ssid_len == 3 && !memcmp(w[0].ssid, "a\0b", 3) && w[0].current && !w[0].disabled);
      assert(w[1].ssid_len == 3 && !strcmp(w[1].ssid, "q\\\"") && w[1].disabled && !w[1].current); }

    /* --- 1. the list: connected first, joinable, then disabled; dupes merge */
    reset();
    int home = seed("HomeNet", 0), off = seed("Office", 0), old = seed("Old Hotspot", 1);
    int dup = seed("Office", 1);                 /* legacy duplicate block, disabled */
    int cafe = seed_n("Caf\xc3\xa9\x1b", 6, 0); /* UTF-8 + a control byte */
    connect_to(home);
    n = wifi_saved_list(s, WIFI_SAVED_MAX);
    assert(n == 4);
    assert(!strcmp(s[0].name, "HomeNet") && s[0].state == WIFI_SAVED_CONNECTED && s[0].id == home);
    assert(!strcmp(s[1].name, "Office") && !s[1].disabled && s[1].id == off && s[1].state == WIFI_SAVED_IDLE);
    assert(!strcmp(s[2].name, "Caf\xc3\xa9?") && s[2].ssid_len == 6 && s[2].id == cafe);
    assert(!strcmp(s[3].name, "Old Hotspot") && s[3].disabled && s[3].id == old);
    (void)dup;
    strcpy(state, "ASSOCIATING");                /* current but not COMPLETED */
    n = wifi_saved_list(s, WIFI_SAVED_MAX);
    assert(row(s, n, "HomeNet")->state == WIFI_SAVED_JOINING);
    wpa_up = 0; assert(wifi_saved_list(s, WIFI_SAVED_MAX) == -1); wpa_up = 1;
    assert(wifi_saved_list(s, 2) == 2);          /* max honoured */

    /* --- 2. join a saved network: SELECT, then the others no-connect + save */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0); old = seed("Old Hotspot", 1);
    connect_to(home);
    assert(wpa_join_saved(off) == 0 && wpa_join_pending());
    assert(!strstr(log_, "ADD_NETWORK") && strstr(log_, "SELECT_NETWORK 1\n"));
    cur_id = -1; strcpy(state, "SCANNING");
    /* mid-join, the list shows what is saved: HomeNet still enabled (SELECT's
     * in-memory [DISABLED] is hidden), Office joining, Old Hotspot disabled */
    n = wifi_saved_list(s, WIFI_SAVED_MAX);
    assert(n == 3 && !row(s, n, "HomeNet")->disabled && row(s, n, "Office")->state == WIFI_SAVED_JOINING);
    assert(row(s, n, "Old Hotspot")->disabled);
    assert(wpa_join_poll() == 0 && saves == 0);
    connect_to(off);
    assert(wpa_join_poll() == 1 && !wpa_join_pending() && saves == 1);
    assert(strstr(log_, "ENABLE_NETWORK 0 no-connect"));
    assert(!strstr(log_, "ENABLE_NETWORK 2"));   /* the disabled one stays disabled */
    assert(!strstr(log_, "REMOVE_NETWORK"));     /* nothing dropped */
    assert(n_saved == 3 && find(home)->disabled == 0 && find(old)->disabled == 1);
    assert(strstr(log_, "SAVE_CONFIG") > strstr(log_, "ENABLE_NETWORK 0 no-connect"));

    /* --- 3. saved join fails (stored key now wrong): kept, others re-enabled
     *        WITH connect, nothing saved */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0);
    assert(wpa_join_saved(off) == 0);
    find(off)->temp = 1;
    assert(wpa_join_poll() == -1 && !wpa_join_pending());
    assert(saves == 0 && find(off) && find(home)->disabled == 0);
    assert(strstr(log_, "ENABLE_NETWORK 0\n") && !strstr(log_, "REMOVE_NETWORK"));

    /* --- 4. saved join out of range: times out, same rollback ------------- */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0);
    assert(wpa_join_saved(off) == 0);
    { int r, p = 0; while ((r = wpa_join_poll()) == 0) assert(++p < 100);
      assert(r == -1 && p == WPA_JOIN_POLLS - 1 && saves == 0 && find(off) && !find(home)->disabled); }

    /* --- 5. disabled / unknown blocks are not joinable; wpa down = -1 ------ */
    reset();
    old = seed("Old Hotspot", 1);
    assert(wpa_join_saved(old) == -1 && !wpa_join_pending() && !strstr(log_, "SELECT"));
    assert(wpa_join_saved(9) == -1 && !wpa_join_pending());
    wpa_up = 0; assert(wpa_join_saved(old) == -1); wpa_up = 1;

    /* --- 6. a saved pick supersedes a pending NEW join (that one rolled back) */
    reset();
    home = seed("HomeNet", 0);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);           /* id 1, fresh */
    n = wifi_saved_list(s, WIFI_SAVED_MAX);
    assert(n == 1 && !row(s, n, "Loft"));                        /* not saved yet: hidden */
    assert(wpa_join_saved(home) == 0);
    assert(!find(1) && wpa_join_pending());
    connect_to(home);
    assert(wpa_join_poll() == 1 && n_saved == 1 && in_saved("HomeNet"));

    /* --- 7. Forget the connected network: removed, saved, deauthenticated;
     *        nothing left that a REASSOCIATE could rejoin under that SSID */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0); dup = seed("HomeNet", 1);
    connect_to(home);
    { int c; assert(wpa_forget("HomeNet", 7, &c) == 0 && c == 1); }   /* -> lease drop */
    assert(!find(home) && !find(dup) && find(off));
    assert(saves == 1 && !in_saved("HomeNet") && in_saved("Office") == 1);
    assert(strstr(log_, "SAVE_CONFIG") > strstr(log_, "REMOVE_NETWORK 2"));
    assert(!strstr(log_, "DISCONNECT") && !strstr(log_, "SELECT"));   /* no `disconnected` flag:
                                                                      * wpa may auto-join Office */
    assert(cur_id == -1 && !strcmp(state, "DISCONNECTED"));
    { char st[96], j[33]; wifi_status(st, sizeof st, j); assert(!strcmp(st, "Not connected") && !j[0]); }
    n = wifi_saved_list(s, WIFI_SAVED_MAX);
    assert(n == 1 && !strcmp(s[0].name, "Office") && !s[0].disabled);   /* the watcher's
                                                                     * "enabled net" = Office only */

    /* --- 8. Forget an out-of-range / disabled one: only that SSID goes ----- */
    reset();
    home = seed("HomeNet", 0); old = seed("Old Hotspot", 1);
    connect_to(home);
    { int c; assert(wpa_forget("Old Hotspot", 11, &c) == 0 && c == 0); } assert(!find(old) && find(home) && cur_id == home);
    assert(saves == 1 && n_saved == 1);
    /* raw-byte match: a NUL-containing SSID is not confused with its prefix */
    reset();
    int a0 = seed_n("a\0b", 3, 0), a1 = seed("a", 0);
    { int c; assert(wpa_forget("a\0b", 3, &c) == 0 && c == 0); } assert( !find(a0) && find(a1));

    /* --- 9. already gone: OK, nothing saved; wpa down: -1 ------------------ */
    reset();
    seed("HomeNet", 0);
    { int c; assert(wpa_forget("Nope", 4, &c) == 0 && c == 0 && saves == 0); }
    { int c; wpa_up = 0; assert(wpa_forget("HomeNet", 7, &c) == -1 && c == 0); wpa_up = 1; }

    /* --- 10. Forget during a pending join: the join is settled FIRST, so the
     *         save never persists SELECT's temporary disabled=1 ------------- */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0); old = seed("Old Hotspot", 0);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);           /* others disabled in memory */
    assert(find(home)->disabled && find(off)->disabled);
    { int c; assert(wpa_forget("Office", 6, &c) == 0 && c == 0); } assert(!wpa_join_pending());
    assert(saves == 1 && n_saved == 2 && in_saved("HomeNet") && in_saved("Old Hotspot") && !in_saved("Loft"));
    for (int i = 0; i < n_saved; i++) assert(saved[i].disabled == 0);
    /* same, pending SAVED join of the network being forgotten */
    reset();
    home = seed("HomeNet", 0); off = seed("Office", 0);
    assert(wpa_join_saved(off) == 0);
    { int c; assert(wpa_forget("Office", 6, &c) == 0 && c == 0); } assert(!wpa_join_pending());
    assert(n_saved == 1 && in_saved("HomeNet") && saved[0].disabled == 0);
    assert(wpa_join_poll() == 0);                                /* nothing left to finish */

    /* --- 11. wifi_status decodes the escaped SSID (was printed escaped) ----- */
    reset();
    cafe = seed_n("Caf\xc3\xa9", 5, 0); connect_to(cafe);
    { char st[96], j[33]; wifi_status(st, sizeof st, j);
      assert(!strcmp(j, "Caf\xc3\xa9") && !strcmp(st, "Connected to Caf\xc3\xa9 \xe2\x80\xa2 10.0.0.7")); }
    puts("test_wifi_saved: ALL CHECKS PASSED");
    return 0;
}
