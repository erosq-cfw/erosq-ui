/* Joining a Wi-Fi network from the UI must keep every other saved network,
 * enabled, and save only after the join worked. Drives ui_net.c's join
 * transaction against an in-memory wpa_supplicant that models the 2.12 ctrl
 * semantics the code relies on (ctrl_iface.c / wpa_supplicant.c):
 *   ADD_NETWORK -> new block, disabled=1;  SELECT_NETWORK n -> n enabled, every
 *   other block disabled=1;  ENABLE_NETWORK n [no-connect] -> disabled=0;
 *   REMOVE_NETWORK n;  SAVE_CONFIG -> snapshot (what lands in the /data file);
 *   LIST_NETWORKS flags [DISABLED]/[TEMP-DISABLED]; STATUS wpa_state + id=. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_net.h"

#define MAXN 16
static struct net { int used, id, disabled, temp; char ssid[80], key[80]; } N[MAXN];
static int next_id, saves, wpa_up = 1;
static char state[32] = "SCANNING"; static int cur_id = -1;
static struct net saved[MAXN]; static int n_saved;
static char log_[8192];

static struct net *find(int id)
{
    for (int i = 0; i < MAXN; i++) if (N[i].used && N[i].id == id) return &N[i];
    return NULL;
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
        if (!strcmp(a, "ssid")) snprintf(n->ssid, sizeof n->ssid, "%s", b);
        else if (!strcmp(a, "psk") || !strcmp(a, "key_mgmt")) snprintf(n->key, sizeof n->key, "%s", b);
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "SELECT_NETWORK %d", &id) == 1) {
        if (!find(id)) return snprintf(r, rlen, "FAIL\n");
        for (int i = 0; i < MAXN; i++) if (N[i].used) N[i].disabled = (N[i].id != id);
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "ENABLE_NETWORK %d", &id) == 1) {
        struct net *n = find(id); if (!n) return snprintf(r, rlen, "FAIL\n");
        n->disabled = 0; n->temp = 0;
        return snprintf(r, rlen, "OK\n");
    }
    if (!strcmp(cmd, "REMOVE_NETWORK all")) {
        for (int i = 0; i < MAXN; i++) N[i].used = 0;
        return snprintf(r, rlen, "OK\n");
    }
    if (sscanf(cmd, "REMOVE_NETWORK %d", &id) == 1) {
        struct net *n = find(id); if (!n) return snprintf(r, rlen, "FAIL\n");
        n->used = 0; return snprintf(r, rlen, "OK\n");
    }
    if (!strcmp(cmd, "SAVE_CONFIG")) {
        saves++; n_saved = 0;
        for (int i = 0; i < MAXN; i++) if (N[i].used) saved[n_saved++] = N[i];
        return snprintf(r, rlen, "OK\n");
    }
    if (!strcmp(cmd, "LIST_NETWORKS")) {
        int k = snprintf(r, rlen, "network id / ssid / bssid / flags\n");
        for (int i = 0; i < MAXN; i++) if (N[i].used) {
            /* the fake stores the hex ssid the UI sends; list shows it decoded */
            char s[40] = ""; const char *h = N[i].ssid;
            if (h[0] != '"') for (int j = 0; h[2*j] && h[2*j+1] && j < 32; j++) {
                unsigned v; sscanf(h + 2*j, "%2x", &v); s[j] = (char)v; s[j+1] = 0;
            } else snprintf(s, sizeof s, "%.*s", (int)strlen(h) - 2, h + 1);
            k += snprintf(r + k, rlen - k, "%d\t%s\tany\t%s%s%s\n", N[i].id, s,
                          N[i].id == cur_id ? "[CURRENT]" : "",
                          N[i].disabled ? "[DISABLED]" : "", N[i].temp ? "[TEMP-DISABLED]" : "");
        }
        return k;
    }
    if (!strcmp(cmd, "STATUS")) {
        if (cur_id >= 0) return snprintf(r, rlen, "bssid=00:11:22:33:44:55\nfreq=2412\nssid=x\nid=%d\nmode=station\nwpa_state=%s\n", cur_id, state);
        return snprintf(r, rlen, "wpa_state=%s\naddress=00:00:00:00:00:01\n", state);
    }
    return snprintf(r, rlen, "OK\n");
}

/* seed one saved network block as if read from the conf at start-up */
static int seed(const char *quoted_ssid, int disabled)
{
    for (int i = 0; i < MAXN; i++) if (!N[i].used) {
        memset(&N[i], 0, sizeof N[i]); N[i].used = 1; N[i].id = next_id++;
        N[i].disabled = disabled; snprintf(N[i].ssid, sizeof N[i].ssid, "%s", quoted_ssid);
        snprintf(N[i].key, sizeof N[i].key, "\"oldkey12\"");
        return N[i].id;
    }
    return -1;
}

static void reset(void)
{
    memset(N, 0, sizeof N); next_id = 0; saves = 0; n_saved = 0; wpa_up = 1;
    strcpy(state, "SCANNING"); cur_id = -1; log_[0] = 0;
    while (wpa_join_pending()) { wpa_up = 0; wpa_join_poll(); wpa_up = 1; }
}

static struct net *saved_ssid(const char *hexssid)   /* in the SAVE_CONFIG snapshot */
{
    for (int i = 0; i < n_saved; i++) if (!strcmp(saved[i].ssid, hexssid)) return &saved[i];
    return NULL;
}
static int count_saved(const char *ssid) { int c = 0; for (int i = 0; i < n_saved; i++) c += !strcmp(saved[i].ssid, ssid); return c; }

int main(void)
{
    wpa_cmd_hook = fake;
    char hex[67];

    /* LIST_NETWORKS parser: flags + escaped SSIDs */
    wpa_net_t nets[8];
    const char *l = "network id / ssid / bssid / flags\n"
                    "0\tCafe Guest\tany\t[CURRENT]\n"
                    "1\tthrowaway\tany\t[DISABLED]\n"
                    "2\tCaf\\xc3\\xa9\tany\t[TEMP-DISABLED]\n";
    assert(wpa_parse_networks(l, nets, 8) == 3);
    assert(nets[0].id == 0 && !strcmp(nets[0].ssid, "Cafe Guest") && !nets[0].disabled && !nets[0].temp_disabled);
    assert(nets[1].id == 1 && nets[1].disabled);
    assert(nets[2].id == 2 && !strcmp(nets[2].ssid, "Caf\xc3\xa9") && nets[2].temp_disabled && !nets[2].disabled);
    assert(wpa_parse_networks("network id / ssid / bssid / flags\n", nets, 8) == 0);
    assert(wpa_parse_networks(l, nets, 1) == 1);

    /* 1. join Loft while Cafe Guest is saved: Cafe Guest stays saved AND enabled;
     *    nothing is saved before the join completes */
    reset();
    int a = seed("\"Cafe Guest\"", 0);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);
    assert(!strstr(log_, "REMOVE_NETWORK all"));
    assert(saves == 0 && wpa_join_pending());
    assert(find(a)->disabled == 1);                   /* SELECT disabled it -- in memory */
    assert(wpa_join_poll() == 0 && saves == 0);       /* still scanning: no save */
    to_hex("Loft", hex);
    int b = -1; for (int i = 0; i < MAXN; i++) if (N[i].used && !strcmp(N[i].ssid, hex)) b = N[i].id;
    assert(b >= 0 && b != a);
    strcpy(state, "COMPLETED"); cur_id = b;
    assert(wpa_join_poll() == 1 && !wpa_join_pending());
    assert(saves == 1 && n_saved == 2);
    assert(saved_ssid("\"Cafe Guest\"") && saved_ssid("\"Cafe Guest\"")->disabled == 0);   /* no disabled=1 */
    assert(saved_ssid(hex) && saved_ssid(hex)->disabled == 0);
    assert(strstr(log_, "ENABLE_NETWORK 0 no-connect"));   /* fresh link left alone */
    assert(strstr(log_, "SAVE_CONFIG") > strstr(log_, "ENABLE_NETWORK 0 no-connect"));

    /* 2. re-joining a saved SSID (new password) replaces ONLY that entry */
    reset();
    seed("\"Cafe Guest\"", 0); int old = seed("\"Loft\"", 0);
    assert(wpa_connect_psk("Loft", "newpass99") == 0);
    for (int i = 0; i < MAXN; i++) if (N[i].used && N[i].id >= 2) b = N[i].id;
    strcpy(state, "COMPLETED"); cur_id = b;
    assert(wpa_join_poll() == 1);
    to_hex("Loft", hex);
    assert(n_saved == 2 && count_saved(hex) == 1 && !saved_ssid("\"Loft\""));
    assert(!find(old));
    assert(!strcmp(saved_ssid(hex)->key, "\"newpass99\""));
    assert(saved_ssid("\"Cafe Guest\"")->disabled == 0 && saved_ssid(hex)->disabled == 0);
    { char c[32]; snprintf(c, sizeof c, "ENABLE_NETWORK %d", old); assert(!strstr(log_, c)); }

    /* 3. wrong password ([TEMP-DISABLED] on the new block): nothing saved, the
     *    new block is dropped, the others are re-enabled WITH connect (auto-join) */
    reset();
    a = seed("\"Cafe Guest\"", 0);
    assert(wpa_connect_psk("Loft", "wrongpass") == 0);
    for (int i = 0; i < MAXN; i++) if (N[i].used && N[i].id != a) N[i].temp = 1;
    assert(wpa_join_poll() == -1 && !wpa_join_pending());
    assert(saves == 0 && !strstr(log_, "SAVE_CONFIG"));
    assert(find(a) && find(a)->disabled == 0 && next_id == 2 && !find(1));
    assert(strstr(log_, "ENABLE_NETWORK 0\n"));

    /* 4. never completes: gives up after WPA_JOIN_POLLS polls, same rollback */
    reset();
    a = seed("\"Cafe Guest\"", 0);
    assert(wpa_connect_open("CafeOpen") == 0);
    assert(strstr(log_, "key_mgmt NONE"));
    int r = 0, polls = 0;
    while ((r = wpa_join_poll()) == 0) { polls++; assert(polls < 100); }
    assert(r == -1 && polls == WPA_JOIN_POLLS - 1 && saves == 0 && find(a)->disabled == 0);

    /* 5. a user-disabled block stays disabled; it never blocks the enabled ones */
    reset();
    a = seed("\"Cafe Guest\"", 0); int t = seed("\"throwaway\"", 1);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);
    strcpy(state, "COMPLETED"); cur_id = 2;
    assert(wpa_join_poll() == 1);
    assert(saved_ssid("\"Cafe Guest\"")->disabled == 0 && saved_ssid("\"throwaway\"")->disabled == 1);
    { char c[32]; snprintf(c, sizeof c, "ENABLE_NETWORK %d", t); assert(!strstr(log_, c)); }

    /* 6. Wi-Fi turned off mid-join (wpa gone): abandoned, nothing saved */
    reset();
    seed("\"Cafe Guest\"", 0);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);
    wpa_up = 0;
    assert(wpa_join_poll() == -1 && !wpa_join_pending() && saves == 0);
    wpa_up = 1;

    /* 7. a second pick while the first is joining supersedes it (first rolled back) */
    reset();
    a = seed("\"Cafe Guest\"", 0);
    assert(wpa_connect_psk("Loft", "loftloft1") == 0);           /* id 1 */
    assert(wpa_connect_psk("Other", "otherpass1") == 0);         /* id 2 */
    assert(!find(1) && find(2) && find(a));
    strcpy(state, "COMPLETED"); cur_id = 2;
    assert(wpa_join_poll() == 1);
    assert(n_saved == 2 && saved_ssid("\"Cafe Guest\"")->disabled == 0);

    /* 8. wpa unreachable at the start: -1, no pending join; bad psk: -2 */
    reset();
    wpa_up = 0; assert(wpa_connect_psk("Loft", "loftloft1") == -1 && !wpa_join_pending()); wpa_up = 1;
    assert(wpa_connect_psk("Loft", "short") == -2 && !wpa_join_pending());

    /* 9. no join pending: poll is a no-op (no ctrl traffic) */
    reset(); log_[0] = 0;
    assert(wpa_join_poll() == 0 && log_[0] == 0);
    return 0;
}
