#ifndef EROSQ_UI_NET_H
#define EROSQ_UI_NET_H
typedef struct { char ssid[33]; int signal; int secured; } ui_wifi_ap_t;
int  wpa_parse_scan(const char *results, ui_wifi_ap_t *out, int max);
int  wpa_unescape(const char *s, const char *end, char *out, int cap);
                                         /* decode wpa printf_encode escapes; returns len */
void to_hex(const char *s, char *out);   /* lowercase hex; out >= 2*strlen(s)+1 */
int  psk_ok(const char *p);              /* 1 if a valid 8-63 printable-ASCII WPA passphrase */
int  wpa_cmd(const char *cmd, char *reply, int rlen);
extern int (*wpa_cmd_hook)(const char *cmd, char *reply, int rlen);
                                         /* host tests: replaces the ctrl socket */
typedef struct { int id; char ssid[33]; int ssid_len; int disabled; int temp_disabled; int current; } wpa_net_t;
int  wpa_parse_networks(const char *list, wpa_net_t *out, int max);
                                         /* LIST_NETWORKS rows; returns count */
int  wifi_ssid_display(const char *raw, int len, char *out, int cap);
                                         /* raw SSID bytes -> label-safe UTF-8: invalid
                                          * UTF-8 and control chars become '?'. Output is
                                          * never longer than the input; returns its length */
int  wpa_status_parse(const char *status, char ssid[33], int *ssid_len, char ip[64], int *completed);
                                         /* STATUS reply -> current network id (or -1),
                                          * its decoded ssid, ip_address, wpa_state==COMPLETED */
/* Saved networks (LIST_NETWORKS), one row per SSID: duplicate blocks merge. */
enum { WIFI_SAVED_IDLE, WIFI_SAVED_JOINING, WIFI_SAVED_CONNECTED };
typedef struct {
    int  id;                  /* block a join SELECTs: the current one, else the first enabled */
    char ssid[33]; int ssid_len;   /* raw bytes: the match key */
    char name[33];            /* wifi_ssid_display() of ssid */
    int  disabled;            /* every block with this SSID is [DISABLED]: not joinable */
    int  state;               /* WIFI_SAVED_* */
} wifi_saved_t;
#define WIFI_SAVED_MAX 32
int  wifi_saved_build(const wpa_net_t *nets, int n, int cur_id, int completed,
                      wifi_saved_t *out, int max);
                                         /* rows: connected first, then joinable, then
                                          * disabled; a pending join's temporary state is
                                          * hidden (see ui_net.c). Returns the count */
int  wifi_saved_list(wifi_saved_t *out, int max);
                                         /* LIST_NETWORKS + STATUS -> rows; -1 wpa unreachable */
int  wpa_join_saved(int id);             /* join saved block id (same transaction as a new
                                          * join, minus ADD/REMOVE): 0 started, -1 refused */
int  wpa_forget(const char *ssid, int len, int *was_current);
                                         /* REMOVE_NETWORK every block with this SSID, then
                                          * SAVE_CONFIG. 0 done (or already gone), -1 wpa error.
                                          * *was_current = 1 if the connected block went
                                          * (wpa deauthenticated): call wifi_lease_drop() */
void wifi_lease_drop(void);              /* ASYNC: wifi_dhcp.sh check (stop udhcpc + deconfig
                                          * while not associated) then wifi_join.sh start
                                          * (fresh client once wpa joins another network) */
int  wpa_connect_open(const char *ssid); /* 0 join started; -1 wpa unreachable/refused */
int  wpa_connect_psk(const char *ssid, const char *psk);
                                         /* 0 join started; -1 wpa unreachable; -2 bad passphrase */
/* A join keeps every other saved network, and is saved only once it worked:
 * call wpa_join_poll() about once a second while wpa_join_pending().
 * Returns 1 joined + saved, -1 failed (nothing saved, others re-enabled),
 * 0 still joining / nothing pending. */
#define WPA_JOIN_POLLS 30
int  wpa_join_poll(void);
int  wpa_join_pending(void);
void wifi_status(char *out, int n, char ssid[33]);
                                         /* human status line; ssid = joined network or "" */
const char *wifi_strength(int dbm);      /* "Strong"/"Good"/"Weak" for a scan signal */
int  wifi_is_enabled(void);              /* GROUND TRUTH: power rfkill + wpa PING */
void wifi_set_enabled(int on);           /* ASYNC: backgrounds wifi_up/down.sh; poll
                                          * wifi_is_enabled() for the outcome */
int  wifi_get_country(char *cc);        /* cc[3]; saved choice or live regdomain, 0 on success */
void wifi_set_country(const char *cc);  /* iw reg set + persist; cc = 2-letter code */
void wifi_apply_saved_country(void);    /* re-apply persisted country (enable/startup) */
void wifi_pref_save(int on);            /* persist WiFi on/off choice to /data */
void wifi_pref_restore(void);           /* re-enable WiFi at boot if saved "on" */
#endif
