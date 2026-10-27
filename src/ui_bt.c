/* Dual-mode discovery (main.conf ControllerMode=dual; LE-only HOGP pads such as
 * the 8BitDo Ultimate 2 never appear under Transport=bredr). The LE beacon flood
 * that once filled the 32-slot table (79 objects in one 25s scan, rotating
 * random addresses) is kept out at ingest by bt_dev_listable(), and their RSSI
 * churn no longer refreshes the UI. DuplicateData=false stops bluez re-reporting
 * every advertisement. The filter is per-D-Bus-client; D-Bus preserves
 * per-connection ordering, so it is applied before StartDiscovery. */
#include "ui_bt.h"
#include "ui_dbus.h"
#include "ui_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef EROSQ_HOST
/* SDL preview: no bluez. Everything no-ops; the BT screen shows "unavailable". */
int  ui_bt_init(void) { return -1; }
void ui_bt_set_power(int on) { (void)on; }
void ui_bt_power_on_deferred(void) {}
/* ponytail: EROSQ_FAKE_BT=1 shows a canned device table so the BT page's on/
 * populated states can be screenshotted (cap-nav.sh); host-only, no D-Bus. */
static int fake_bt(void) { return getenv("EROSQ_FAKE_BT") != NULL; }
int  ui_bt_is_on(void) { return fake_bt(); }
int  ui_bt_ready(void) { return 0; }
void ui_bt_scan_start(void) {}
void ui_bt_scan_stop(void) {}
void ui_bt_connect(const char *p) { (void)p; }
void ui_bt_disconnect(const char *p) { (void)p; }
void ui_bt_remove(const char *p) { (void)p; }
static bt_devtable_t s_empty;
static bt_devtable_t s_fake = { .dev = {
    { .path = "/f/1", .name = "MEGABOOM 3", .battery = 80, .paired = true, .connected = true },
    { .path = "/f/2", .name = "Pixel Buds Pro with a long name", .battery = -1, .paired = true },
    { .path = "/f/3", .name = "JBL Flip 5", .battery = -1 },
    { .path = "/f/4", .addr = "00:11:22:AA:BB:CC", .battery = -1 },
}, .n = 4 };
/* EROSQ_FAKE_BT=empty / =paired: no devices / only the two paired ones. */
const bt_devtable_t *ui_bt_devices(void)
{
    if (!fake_bt()) return &s_empty;
    const char *m = getenv("EROSQ_FAKE_BT");
    s_fake.n = !strcmp(m, "empty") ? 0 : !strcmp(m, "paired") ? 2 : 4;
    return &s_fake;
}
void ui_bt_set_changed_cb(ui_bt_changed_cb cb) { (void)cb; }
int  ui_bt_audio_active(void) { return 0; }
void ui_bt_set_volume_pct(int pct) { (void)pct; }
void ui_bt_persist_volume(void) {}
void ui_bt_set_volume_cb(ui_bt_volume_cb cb) { (void)cb; }
#else
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "bt_vol.h"
#include "ui_power.h"          /* Volume Limit, Pause on Disconnect */
#include "ui_settings_logic.h"
#include "ui_screens.h"        /* ui_volume_sync after a BT drop */

#define BLUEZ       "org.bluez"
#define ADAPTER     "/org/bluez/hci0"
#define IF_ADAPTER  "org.bluez.Adapter1"
#define IF_DEVICE   "org.bluez.Device1"
#define IF_BATTERY  "org.bluez.Battery1"
#define IF_PROPS    "org.freedesktop.DBus.Properties"
#define IF_AGENTMGR "org.bluez.AgentManager1"
#define AGENT_PATH  "/com/erosq/agent"
#define MPD_SOCK    "/run/mpd.sock"
/* bluealsa: a separate bus name from org.bluez (see the codec-upgrade section
 * below for the full interface notes). Defined here, ahead of on_props/
 * ui_bt_init, so the speaker->UI volume subscription can use it. */
#define BLUEALSA         "org.bluealsa"
#define IF_BLUEALSA_PCM  "org.bluealsa.PCM1"

static bt_devtable_t    s_tab;
static ui_bt_changed_cb s_cb;
static int              s_powered;
static int              s_bluez_up;    /* org.bluez currently owned = bringup confirmed */
static guint            s_sub_added, s_sub_removed, s_sub_props, s_sub_ba_props, s_sub_ba_added, s_sub_ba_removed;
static guint            s_watch;       /* g_bus_watch_name id for org.bluez */
static guint            s_up_confirm;  /* one-shot timeout: revert optimistic ON if bluez never appears */
static int              s_routed_bt = -1;  /* -1 unknown, 0 jack, 1 bluealsa */
/* MPD follows the A2DP sink PCM, not bluez's Connected (HW capture 2026-10-16):
 * the PCM appears ~1.3 s AFTER Connected=true (enabling MPD's BT output before
 * it made MPD fail to open it -> error popup) and vanishes ~0.2 s BEFORE
 * Connected=false (MPD fails right then). s_bt_pcm, kept exact by discovery and
 * bluealsa InterfacesAdded/Removed, is the "PCM exists" signal. */
static int              s_disc_kicked;     /* discovery started for this connection */
static int              s_drop_deferred;   /* carried on at the PCM loss; pause call due at Connected=false */
static guint            s_drop_timer;      /* ...or after BT_DROP_DECIDE_SECS if the link stays up */
#define BT_DROP_DECIDE_SECS 3   /* bluez Connected=false came 0.21 s after the PCM loss (HW); a codec reconfigure re-adds sooner */

/* BT (A2DP) volume target: the active source PCM and its per-device key. The wheel
 * drives this when BT is the active sink (see ui_bt_audio_active). s_bt_pcm is
 * discovered during the codec-upgrade enumeration and cleared on disconnect; the
 * desired level lives on disk per device (see bt_vol_load/save) and is re-applied
 * the moment the PCM appears, so a fresh connection never sits at bluealsa's
 * full-scale default until the first wheel turn. */
static char             s_bt_pcm[BT_PATH_MAX];
/* Codec upgrades in flight (GetCodecs -> SelectCodec). SelectCodec reconfigures
 * the A2DP stream, so an MPD open that lands mid-switch fails with "Failed to
 * open audio output" (HITL 10-20, WF-1000XM5/LDAC, 1 of 3 connects). Route to
 * the sink only once this is back to 0; a counter, not a flag, because a
 * reconnect can start a second upgrade before the first one finishes. */
static int              s_bt_codec_busy;
/* A2DP SelectCodec recreates the transport: the reply comes back while the OLD
 * PCM still exists, and bluealsa removes it and adds the new one (same path)
 * only afterwards (HITL 10-21: routing on the reply raced that, 1 connect in 4
 * "Failed to open audio output"). So routing also waits until a sink PCM on the
 * chosen codec has been ADDED, or the fallback timer gives up. "" = no wait. */
static char             s_codec_wait[16];
static guint            s_codec_wait_timer;
#define BT_CODEC_WAIT_SECS 5
static char             s_bt_dev[BT_PATH_MAX]; /* org.bluez device path = per-device volume key */
static int              s_bt_vol_pct = -1;     /* live level for s_bt_dev; RAM-only, flushed to disk on
                                                * disconnect + graceful shutdown -- NOT on every change,
                                                * so a volume scrub doesn't churn NAND (the normal poweroff
                                                * syncs, so the on-disk value is current after it). */
static ui_bt_volume_cb  s_volume_cb;    /* speaker -> UI volume reporter (may be NULL) */

/* Per-device BT volume, remembered across reboots. Each headphone/speaker keeps
 * its OWN last level (keyed by org.bluez device path) so switching from quiet
 * headphones to a loud speaker restores each correctly -- and, crucially, no
 * device ever comes back at bluealsa's full-scale default and blows out ears.
 * One "<dev> <pct>\n" line per device. */
#define BT_VOL_PATH "/data/erosq-ui.btvol"
#define BT_VOL_DEFAULT_PCT 40   /* first time we ever see a device: safe, not max */

/* Slurp the whole (small) db. Returns bytes read (0 on absent/empty). */
static int bt_vol_slurp(char *buf, size_t cap) {
    int fd = open(BT_VOL_PATH, O_RDONLY);
    if (fd < 0) return 0;
    size_t n = 0; ssize_t r;                       /* loop: read() may return short */
    while (n < cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0) n += (size_t)r;
    close(fd);
    buf[n] = 0;
    return (int)n;
}

static int bt_vol_load(const char *dev) {
    if (!dev || !dev[0]) return -1;
    char buf[2048];
    bt_vol_slurp(buf, sizeof buf);
    return bt_vol_lookup(buf, dev);
}

/* Replace this device's line, keeping every other device's. Read-modify-write
 * so the file stays one line per device (append-only would outgrow the read
 * buffer under a wheel scrub). Idempotent: skips the write when unchanged, so a
 * scrub past the same value doesn't churn NAND. */
static void bt_vol_save(const char *dev, int pct) {
    if (!dev || !dev[0]) return;
    pct = bt_vol_clamp_pct(pct);
    char old[2048];
    bt_vol_slurp(old, sizeof old);
    if (bt_vol_lookup(old, dev) == pct) return;  /* unchanged -> no NAND write */

    size_t klen = strlen(dev);
    char out[2048]; size_t o = 0;
    for (const char *p = old; *p && o < sizeof out - 1; ) {         /* keep other devices */
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p + 1) : strlen(p);
        int mine = (strncmp(p, dev, klen) == 0 && p[klen] == ' ');
        if (!mine && o + linelen < sizeof out) { memcpy(out + o, p, linelen); o += linelen; }
        if (!eol) break;
        p = eol + 1;
    }
    /* snprintf returns would-have-written, not did-write; advance by the actual
     * (possibly truncated) length so a near-full buffer can't push o past out[]. */
    snprintf(out + o, sizeof out - o, "%s %d\n", dev, pct);
    o += strlen(out + o);

    /* Durable atomic replace: write .new, fsync the DATA, rename, fsync the DIR.
     * This is a battery DAP whose normal shutdown IS a hard power cut, and the
     * whole point of persisting is to survive it. Dropping the data fsync (an
     * earlier "reduce NAND churn" change) broke exactly this: UBIFS committed the
     * rename's dir-entry to flash while the new inode's data was still only in the
     * page cache -> after a power cut the name pointed at a zero-byte inode = empty
     * db (HITL-observed 2026-07-24). fsync(data) before the rename guarantees the
     * new bytes are on flash first; fsync(dir) after makes the rename itself
     * durable so the value just set survives the cut, not just the prior one.
     * ponytail: per-change fsync (~2 flushes/step during a scrub); if that ever
     * shows up as NAND wear or wheel lag, debounce to one write after the wheel
     * settles -- but keep the fsync, durability is the feature. */
    int fd = open(BT_VOL_PATH ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* no /data -> just don't persist */
    int ok = write(fd, out, o) == (ssize_t)o;
    if (ok && fsync(fd) != 0) ok = 0;         /* data on flash BEFORE the rename swaps it in */
    close(fd);
    if (ok && rename(BT_VOL_PATH ".new", BT_VOL_PATH) == 0) {
        int dfd = open("/data", O_RDONLY | O_DIRECTORY);   /* make the rename durable too */
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    } else {
        unlink(BT_VOL_PATH ".new");
    }
}

/* --- MPD output routing -----------------------------------------------------
 * MPD (bind_to_address /run/mpd.sock) defines two outputs in mpd.conf:
 *   output 0 = ES9018K2M (hw:0,0, the headphone jack)
 *   output 1 = Bluetooth (device "bluealsa", the A2DP source)
 * Audio follows the connected BT device: when one is connected route MPD to
 * bluealsa, otherwise to the jack. Indices are fixed by mpd.conf order. */

/* Send a command string to MPD's unix socket and optionally capture the reply
 * into out (read until "OK"/"ACK" or the 1s recv timeout). Best-effort: skips
 * silently if MPD is absent; the 1s socket timeouts keep a wedged MPD from
 * stalling the LVGL thread (this runs from the D-Bus pump on that thread). */
static void mpd_io(const char *cmds, char *out, int outsz) {
    if (out && outsz) out[0] = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", MPD_SOCK);
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        char drain[128];
        if (read(fd, drain, sizeof drain) < 0) {}   /* drain "OK MPD x.y.z" greeting */
        if (write(fd, cmds, strlen(cmds)) >= 0) {
            if (out && outsz > 1) {
                int n = 0, r;
                while (n < outsz - 1 && (r = read(fd, out + n, outsz - 1 - n)) > 0) {
                    n += r; out[n] = 0;
                    if (strstr(out, "\nOK\n") || strstr(out, "\nACK") ||
                        !strncmp(out, "OK\n", 3)) break;
                }
            } else {
                if (read(fd, drain, sizeof drain) < 0) {}  /* process before close */
            }
        }
    }
    close(fd);
}
static void mpd_cmd(const char *cmds) { mpd_io(cmds, NULL, 0); }

/* Defined below; also used here to (re)discover the A2DP PCM for the volume
 * wheel when a device is already connected at startup / auto-reconnect (no
 * UI-initiated Connect ran to cache it). */
void ui_bt_codec_upgrade(const char *dev_path);

/* Keep MPD's output in sync with the BT connection state: bluealsa when a
 * device is connected, the jack otherwise. On the fall-back to the jack, MPD
 * has paused with "Failed to open audio output" because the bluealsa sink died
 * mid-stream, so clear the error and resume; that error is only present when
 * audio was actually playing, so a stopped/idle player is left untouched. */
/* The pause call held back at a PCM-only loss: jack + Pause on Disconnect decide,
 * as for a plain disconnect. */
static void drop_decide(void) {
    if (s_drop_timer) { g_source_remove(s_drop_timer); s_drop_timer = 0; }
    if (!s_drop_deferred) return;
    s_drop_deferred = 0;
    int hp, lo;
    ui_jack_state(&hp, &lo);
    const char *c = ui_bt_drop_cmd(bt_drop_carry_on(hp, ui_power_discpause_get(), false), lo, 0);
    if (c) mpd_cmd(c);
}
/* A2DP gone but the link stays up (Bluetooth headphones switched to another
 * source): don't let MPD play on silently into an empty jack. */
static gboolean drop_timer_cb(gpointer u) {
    (void)u;
    s_drop_timer = 0;
    drop_decide();
    return G_SOURCE_REMOVE;
}

static void update_audio_route(void) {
    const char *conn_path = NULL;
    for (int i = 0; i < s_tab.n; i++)
        if (s_tab.dev[i].connected) { conn_path = s_tab.dev[i].path; break; }
    if (!conn_path) s_disc_kicked = 0;
    else if (!s_bt_pcm[0] && !s_disc_kicked) {   /* connected at startup / before the PCM: look for it */
        s_disc_kicked = 1;
        ui_bt_codec_upgrade(conn_path);          /* found -> on_bluealsa_objects re-runs this */
    }
    int any = bt_route_want_bt(conn_path != NULL,
                               s_bt_pcm[0] != 0 && !s_bt_codec_busy && !s_codec_wait[0]);
    if (!conn_path) drop_decide();           /* PCM went first; now the device is gone too */
    if (any == s_routed_bt) return;          /* no transition -> nothing to do */
    s_routed_bt = any;
    if (any) {
        s_drop_deferred = 0;      /* PCM back while still connected (codec reconfigure): no pause call */
        if (s_drop_timer) { g_source_remove(s_drop_timer); s_drop_timer = 0; }
        mpd_cmd(bt_route_cmds(true));
    } else {
        if (s_bt_dev[0] && s_bt_vol_pct >= 0)
            bt_vol_save(s_bt_dev, s_bt_vol_pct);   /* flush the departing device's level before we forget it */
        s_bt_pcm[0] = 0;          /* volume wheel reverts to the jack codec */
        s_bt_dev[0] = 0;          /* forget which device we were keyed to; next connect restores from disk */
        s_bt_vol_pct = -1;
        /* Back to the wired DAC, then pause or resume per the jacks (see
         * ui_bt_drop_cmd: playing on into an empty jack is silent). */
        char st[512];
        char cmds[160];
        snprintf(cmds, sizeof cmds, "%sstatus\n", bt_route_cmds(false));
        mpd_io(cmds, st, sizeof st);
        int hp, lo;
        ui_jack_state(&hp, &lo);
        /* Pause on Disconnect = Off, or only the PCM is gone so far: keep playing. */
        bool still = conn_path != NULL;
        s_drop_deferred = still;
        if (still && !s_drop_timer) s_drop_timer = g_timeout_add_seconds(BT_DROP_DECIDE_SECS, drop_timer_cb, NULL);
        const char *c = ui_bt_drop_cmd(bt_drop_carry_on(hp, ui_power_discpause_get(), still), lo,
                                       strstr(st, "Failed to open audio output") != NULL);
        if (c) mpd_cmd(c);
        ui_volume_sync();         /* the volume overlay held the BT level; the wheel now drives the DAC */
    }
}

static void notify(const char *msg) {
    update_audio_route();   /* keep MPD output in sync with BT connection state */
    if (s_cb) s_cb(msg);
}

/* One line to the kernel log (dmesg), the only log that survives on the
 * device: erosq-ui's stdout/stderr go to /dev/null. Rare events only (BT
 * power edges, reconnect attempts) -- never per frame or per poll. */
#include <stdarg.h>
static void bt_log(const char *fmt, ...) {
    char m[200];
    int n = snprintf(m, sizeof m, "<6>erosq-ui: bt: ");
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(m + n, sizeof m - n, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : (n + k >= (int)sizeof m - 1 ? (int)sizeof m - 2 : n + k);
    m[n++] = '\n';
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { fwrite(m, 1, n, stderr); return; }
    if (write(fd, m, n) < 0) { /* ignore */ }
    close(fd);
}

/* Off -> On reconnect (defined with connect/disconnect below). */
static void adapter_powered(int p);
static void reconnect_cancel(void);
static bt_rc_t s_rc;   /* Off -> On reconnect state (ui_bt_logic.c, host-tested) */

/* --- helpers ---------------------------------------------------------------- */

/* Set a bool property on an object via org.freedesktop.DBus.Properties.Set. */
static void set_bool_prop(const char *obj, const char *iface,
                          const char *prop, gboolean val) {
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, obj, IF_PROPS, "Set",
        g_variant_new("(ssv)", iface, prop, g_variant_new_boolean(val)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Pull a device's properties out of an a{sv} dict into the table row. */
static void apply_dev_props(int idx, GVariant *props) {
    bt_device_t *d = &s_tab.dev[idx];
    GVariantIter it; const char *k; GVariant *v;
    g_variant_iter_init(&it, props);
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
        if (!strcmp(k, "Address"))   snprintf(d->addr, sizeof d->addr, "%s", g_variant_get_string(v, NULL));
        /* ponytail: Alias is bluez's canonical display string (user alias, else
         * device Name, else the address). Take Name or Alias unconditionally: a
         * BLE name resolved after the row already exists is usually delivered as
         * an Alias-only PropertiesChanged, and the old "!d->name[0]" guard + a
         * MAC written into d->name made that update get dropped -> row stuck on
         * the MAC. Empty name is fine; the row renderer falls back to d->addr. */
        else if (!strcmp(k, "Name") || !strcmp(k, "Alias"))
                                     snprintf(d->name, sizeof d->name, "%s", g_variant_get_string(v, NULL));
        else if (!strcmp(k, "Paired"))    d->paired    = g_variant_get_boolean(v);
        else if (!strcmp(k, "Connected")) d->connected = g_variant_get_boolean(v);
        else if (!strcmp(k, "RSSI"))      d->rssi      = g_variant_get_int16(v);
        g_variant_unref(v);
    }
}

/* Pull org.bluez.Battery1.Percentage (byte 0..100) into the table row. */
static void apply_batt_props(int idx, GVariant *props) {
    GVariant *v = g_variant_lookup_value(props, "Percentage", G_VARIANT_TYPE_BYTE);
    if (v) { s_tab.dev[idx].battery = g_variant_get_byte(v); g_variant_unref(v); }
}

/* bt_dev_listable() over a Device1 property dict (full set, or a
 * PropertiesChanged delta: absent keys count as false/empty). */
static bool props_listable(GVariant *props) {
    gboolean paired = FALSE, connected = FALSE;
    const char *icon = NULL, **uuids = NULL;
    g_variant_lookup(props, "Paired", "b", &paired);
    g_variant_lookup(props, "Connected", "b", &connected);
    g_variant_lookup(props, "Icon", "&s", &icon);
    g_variant_lookup(props, "UUIDs", "^a&s", &uuids);
    bool ok = bt_dev_listable(paired, connected, icon, uuids,
                              uuids ? (int)g_strv_length((gchar **)uuids) : 0);
    g_free(uuids);
    return ok;
}

/* Add/refresh one device object from an "interfaces" a{sa{sv}} dict. Battery1
 * may arrive on its own (bluez adds it after connect, in a separate
 * InterfacesAdded) so handle it independently of Device1. */
static void ingest_object(const char *path, GVariant *ifaces) {
    GVariant *dev  = g_variant_lookup_value(ifaces, IF_DEVICE, NULL);
    GVariant *batt = g_variant_lookup_value(ifaces, IF_BATTERY, NULL);
    if (!dev && !batt) return;              /* not a device (adapter, etc.) */
    int idx = bt_dev_find(&s_tab, path);
    if (idx < 0 && dev && props_listable(dev)) idx = bt_dev_upsert(&s_tab, path);
    if (idx >= 0 && dev)  apply_dev_props(idx, dev);
    if (idx >= 0 && batt) apply_batt_props(idx, batt);
    if (dev)  g_variant_unref(dev);
    if (batt) g_variant_unref(batt);
}

/* --- signal handlers -------------------------------------------------------- */

static void on_added(GDBusConnection *c, const char *sender, const char *opath,
                     const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)opath;(void)iface;(void)sig;(void)u;
    const char *path; GVariant *ifaces;
    g_variant_get(params, "(&o@a{sa{sv}})", &path, &ifaces);
    ingest_object(path, ifaces);
    g_variant_unref(ifaces);
    notify(NULL);
}

static void on_removed(GDBusConnection *c, const char *sender, const char *opath,
                       const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)opath;(void)iface;(void)sig;(void)u;
    const char *path; GVariant *ifs;
    g_variant_get(params, "(&o@as)", &path, &ifs);
    g_variant_unref(ifs);
    bt_dev_remove(&s_tab, path);
    notify(NULL);
}

/* A device admitted by a PropertiesChanged delta may have had its Name/Alias in
 * an earlier, unlisted delta: fetch the full set once. */
static void on_getall(GObject *src, GAsyncResult *res, gpointer user_data) {
    char *path = user_data;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    int idx = bt_dev_find(&s_tab, path);
    if (reply && idx >= 0) {
        GVariant *props; g_variant_get(reply, "(@a{sv})", &props);
        apply_dev_props(idx, props);
        g_variant_unref(props);
        notify(NULL);
    }
    if (reply) g_variant_unref(reply);
    free(path);
}

static void on_props(GDBusConnection *c, const char *sender, const char *opath,
                     const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)iface;(void)sig;(void)u;
    const char *changed_iface; GVariant *changed; GVariant *inval;
    g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed, &inval);
    if (!strcmp(changed_iface, IF_ADAPTER)) {
        gboolean p;
        if (g_variant_lookup(changed, "Powered", "b", &p)) adapter_powered(p);
    } else if (!strcmp(changed_iface, IF_DEVICE)) {
        int idx = bt_dev_find(&s_tab, opath);
        if (idx < 0 && props_listable(changed)) {
            idx = bt_dev_upsert(&s_tab, opath);
            char *p = idx >= 0 ? strdup(opath) : NULL;
            if (p) g_dbus_connection_call(ui_dbus_conn(), BLUEZ, opath, IF_PROPS, "GetAll",
                       g_variant_new("(s)", IF_DEVICE), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                       NULL, on_getall, p);
        }
        if (idx < 0) goto out;   /* unlisted (beacon RSSI churn): no UI refresh */
        apply_dev_props(idx, changed);
    } else if (!strcmp(changed_iface, IF_BATTERY)) {
        int idx = bt_dev_find(&s_tab, opath);
        if (idx < 0) goto out;
        apply_batt_props(idx, changed);
    }
    notify(NULL);
out:
    g_variant_unref(changed); g_variant_unref(inval);
}

/* bluealsa PropertiesChanged: mirror the active A2DP sink's Volume into the UI.
 * Burst-safe -- ui_volume_set_from_bt early-outs on no change and only touches
 * the volume bar, never rebuilds a screen. */
static void apply_bt_volume(int pct);   /* below; on_ba_props enforces Volume Limit */
static void on_ba_props(GDBusConnection *c, const char *sender, const char *opath,
                        const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)iface;(void)sig;(void)u;
    const char *changed_iface; GVariant *changed; GVariant *inval;
    g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed, &inval);
    if (!strcmp(changed_iface, IF_BLUEALSA_PCM) && s_bt_pcm[0] &&
        !strcmp(opath, s_bt_pcm)) {
        guint16 v;
        if (g_variant_lookup(changed, "Volume", "q", &v)) {
            int pct = bt_vol_packed_to_pct(v);
            /* Volume Limit: a speaker-side (button) raise past the cap is pushed
             * back down. Our own Set then echoes at the cap -> no further action. */
            int cap = ui_vol_cap(pct, ui_power_vollimit_get());
            if (cap != pct) { apply_bt_volume(cap); pct = cap; }
            s_bt_vol_pct = pct;           /* device-side (button) change -> RAM; flushed on disconnect/shutdown */
            if (s_volume_cb) s_volume_cb(pct);
        }
    }
    g_variant_unref(changed); g_variant_unref(inval);
}

/* BlueALSA ObjectManager InterfacesAdded: the A2DP PCM for a device is created a
 * beat AFTER the bluez Connected edge, so ui_bt_audio_route()'s codec-upgrade can
 * run before the PCM exists (esp. on boot auto-reconnect) -> it finds nothing and
 * s_bt_pcm stays empty -> the volume wheel silently no-ops. Re-run the discovery
 * when the playback (Mode="sink") PCM actually appears; that also fires the
 * SoftVolume=false switch + per-device volume restore. Skip if we already have a
 * PCM cached. */
static void codec_wait_end(void);
static void on_ba_added(GDBusConnection *c, const char *sender, const char *opath,
                        const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)opath;(void)iface;(void)sig;(void)u;
    const char *added; GVariant *ifaces;
    g_variant_get(params, "(&o@a{sa{sv}})", &added, &ifaces);
    (void)added;   /* keyed off the Device property below */
    GVariant *pcm = g_variant_lookup_value(ifaces, IF_BLUEALSA_PCM, NULL);
    if (pcm && s_codec_wait[0]) {   /* the recreated PCM: the codec switch is over */
        GVariant *cv = g_variant_lookup_value(pcm, "Codec", G_VARIANT_TYPE_STRING);
        if (cv && !strcmp(g_variant_get_string(cv, NULL), s_codec_wait)) {
            codec_wait_end();
            /* same path as the old PCM: forget it so the rediscovery below re-applies
             * SoftVolume + volume to the new object (and routes). */
            s_bt_pcm[0] = 0;
        }
        if (cv) g_variant_unref(cv);
    }
    if (s_bt_pcm[0]) { if (pcm) g_variant_unref(pcm); g_variant_unref(ifaces); return; }
    if (pcm) {
        GVariant *dev_v  = g_variant_lookup_value(pcm, "Device", G_VARIANT_TYPE_OBJECT_PATH);
        GVariant *mode_v = g_variant_lookup_value(pcm, "Mode",   G_VARIANT_TYPE_STRING);
        if (dev_v && mode_v && !strcmp(g_variant_get_string(mode_v, NULL), "sink"))
            ui_bt_codec_upgrade(g_variant_get_string(dev_v, NULL));  /* PCM exists now */
        if (dev_v)  g_variant_unref(dev_v);
        if (mode_v) g_variant_unref(mode_v);
        g_variant_unref(pcm);
    }
    g_variant_unref(ifaces);
}

/* BlueALSA ObjectManager InterfacesRemoved: our A2DP sink is gone. Forget it, so
 * MPD goes to the DAC now, not at bluez's Connected=false ~0.2 s later. */
static void on_ba_removed(GDBusConnection *c, const char *sender, const char *opath,
                          const char *iface, const char *sig, GVariant *params, gpointer u) {
    (void)c;(void)sender;(void)opath;(void)iface;(void)sig;(void)u;
    const char *path; GVariant *ifs;
    g_variant_get(params, "(&o@as)", &path, &ifs);
    g_variant_unref(ifs);
    if (!s_bt_pcm[0] || strcmp(path, s_bt_pcm) != 0) return;
    s_bt_pcm[0] = 0;
    notify(NULL);
}

/* --- enumeration ------------------------------------------------------------ */

static void on_managed(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!reply) { g_clear_error(&err); return; }
    GVariant *objs; g_variant_get(reply, "(@a{oa{sa{sv}}})", &objs);
    GVariantIter it; const char *path; GVariant *ifaces;
    g_variant_iter_init(&it, objs);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *ad = g_variant_lookup_value(ifaces, IF_ADAPTER, NULL);
        if (ad) {
            gboolean p = FALSE;
            GVariant *pv = g_variant_lookup_value(ad, "Powered", G_VARIANT_TYPE_BOOLEAN);
            if (pv) { p = g_variant_get_boolean(pv); g_variant_unref(pv); }
            adapter_powered(p);
            g_variant_unref(ad);
        }
        ingest_object(path, ifaces);
        g_variant_unref(ifaces);
    }
    g_variant_unref(objs); g_variant_unref(reply);
    notify(NULL);
}

/* --- Agent1 (NoInputNoOutput just-works pairing) ----------------------------- */

/* Hand-written introspection XML for Agent1 (NoInputNoOutput subset). */
static const gchar AGENT_XML[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestConfirmation'>"
    "      <arg direction='in'  type='o' name='device'/>"
    "      <arg direction='in'  type='u' name='passkey'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg direction='in'  type='o' name='device'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg direction='in'  type='o' name='device'/>"
    "      <arg direction='in'  type='s' name='uuid'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void agent_method(GDBusConnection *c, const gchar *sender,
                         const gchar *opath, const gchar *iface,
                         const gchar *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer u) {
    (void)c;(void)sender;(void)opath;(void)iface;(void)params;(void)u;
    /* NoInputNoOutput: just-works — accept everything silently. */
    (void)method;
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable s_agent_vtable = {
    .method_call  = agent_method,
    .get_property = NULL,
    .set_property = NULL,
};

static guint s_agent_reg;

static void register_agent(void) {
    if (s_agent_reg) return;          /* already registered */

    GDBusConnection *conn = ui_dbus_conn();
    GError *err = NULL;

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(AGENT_XML, &err);
    if (!node) {
        notify(err ? err->message : "Agent XML parse failed");
        g_clear_error(&err);
        return;
    }

    s_agent_reg = g_dbus_connection_register_object(
        conn, AGENT_PATH,
        node->interfaces[0],
        &s_agent_vtable,
        NULL, NULL, &err);
    g_dbus_node_info_unref(node);

    if (!s_agent_reg) {
        notify(err ? err->message : "Agent register failed");
        g_clear_error(&err);
        return;
    }
}

/* Seed the device table + adapter power state from BlueZ's current objects.
 * Safe to call repeatedly; no-op if the bus connection is absent. */
static void seed_objects(void) {
    if (!ui_dbus_conn()) return;
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_managed, NULL);
}

/* Tell BlueZ about our exported Agent1 (capability NoInputNoOutput ->
 * just-works). Must run when bluez is actually on the bus, so it is driven
 * by the name watch below, not by init time. */
static void bluez_register_agent(void) {
    GDBusConnection *conn = ui_dbus_conn();
    if (!conn) return;
    g_dbus_connection_call(
        conn, BLUEZ, "/org/bluez", IF_AGENTMGR, "RegisterAgent",
        g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_call(
        conn, BLUEZ, "/org/bluez", IF_AGENTMGR, "RequestDefaultAgent",
        g_variant_new("(o)", AGENT_PATH),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Name watch: fires when bluetoothd claims/loses org.bluez. This is what makes
 * BT survive the boot race (erosq-ui starts before bluetoothd finishes patchram)
 * and any later bluetoothd restart. "appeared" also fires immediately if bluez
 * is already present, so the normal case seeds promptly. */
static void on_bluez_up(GDBusConnection *c, const gchar *name,
                        const gchar *owner, gpointer u) {
    (void)c;(void)name;(void)owner;(void)u;
    s_bluez_up = 1;
    bt_log("org.bluez up (%s)", owner);
    if (s_up_confirm) { g_source_remove(s_up_confirm); s_up_confirm = 0; }
    bluez_register_agent();
    seed_objects();
}
static void on_bluez_down(GDBusConnection *c, const gchar *name, gpointer u) {
    (void)c;(void)name;(void)u;
    s_bluez_up = 0;
    bt_log("org.bluez down");
    s_tab.n   = 0;        /* bluez gone -> nothing to show */
    adapter_powered(0);
    notify(NULL);
}

/* --- public API ------------------------------------------------------------- */

int ui_bt_init(void) {
    if (!ui_dbus_conn()) return -1;
    register_agent();
    if (!s_sub_added) {
        s_sub_added = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEZ, "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_added, NULL, NULL);
        s_sub_removed = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEZ, "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_removed, NULL, NULL);
        s_sub_props = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEZ, IF_PROPS, "PropertiesChanged", NULL, NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_props, NULL, NULL);
        /* Speaker-side volume comes from bluealsa (org.bluealsa, a different bus
         * name than org.bluez). arg0 = the PCM1 interface so dbus drops non-volume
         * signals before we're called. */
        s_sub_ba_props = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEALSA, IF_PROPS, "PropertiesChanged",
            NULL, IF_BLUEALSA_PCM, G_DBUS_SIGNAL_FLAGS_NONE, on_ba_props, NULL, NULL);
        /* Catch the A2DP PCM appearing after the connect edge (boot auto-reconnect
         * race) so the volume wheel's PCM path always gets cached. */
        s_sub_ba_added = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEALSA, "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_ba_added, NULL, NULL);
        s_sub_ba_removed = g_dbus_connection_signal_subscribe(
            ui_dbus_conn(), BLUEALSA, "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_ba_removed, NULL, NULL);
    }
    /* Drive seeding + agent registration off a name watch so it works no
     * matter when bluetoothd appears (boot race) or reappears (restart).
     * on_bluez_up fires immediately if bluez is already on the bus. */
    if (!s_watch)
        s_watch = g_bus_watch_name_on_connection(
            ui_dbus_conn(), BLUEZ, G_BUS_NAME_WATCHER_FLAGS_NONE,
            on_bluez_up, on_bluez_down, NULL, NULL);
    return 0;
}

int ui_bt_is_on(void) { return s_powered; }
int ui_bt_ready(void) { return s_bluez_up && s_powered; }

/* Honest power state: the toggle sets s_powered=1 optimistically (so it doesn't
 * snap back while the backgrounded scripts run), but bringup CAN fail -- no hci0
 * means bluetoothd never starts, so the name watch / PropertiesChanged that
 * normally confirm never fire and the UI would claim ON forever. Arm a one-shot
 * GLib timeout (same single-threaded pump as everything here): if org.bluez
 * still has no owner when it fires, stop claiming ON. A bringup that merely
 * finishes late self-corrects afterwards via the name watch + Powered event.
 * 60s ~ the worst LEGIT cold chain (bt_up ~35s + bt_audio_up ~25s of waits). */
#define BT_CONFIRM_SECS 60
static gboolean power_confirm_cb(gpointer u) {
    (void)u;
    s_up_confirm = 0;
    if (!s_bluez_up && s_powered) {
        s_powered = 0;
        notify("Bluetooth failed to start");
    }
    return G_SOURCE_REMOVE;
}
static void arm_power_confirm(void) {
    if (s_up_confirm) g_source_remove(s_up_confirm);
    s_up_confirm = g_timeout_add_seconds(BT_CONFIRM_SECS, power_confirm_cb, NULL);
}

/* Record the user's most-recent BT power INTENT (/run/bt-want, "1"/"0"). The UI
 * is the sole caller of the bringup/teardown scripts and taps serialize on the
 * LVGL thread, so this file's last write == the last toggle. Each script, after
 * acquiring the /run/bt-power.lock mutex, bails if its intent went stale -- so a
 * fast off<->on lands on the last intent no matter which script wins the lock. */
static void bt_write_want(int on) {
    int fd = open("/run/bt-want", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, on ? "1" : "0", 1) < 0) { /* ignore */ }
    close(fd);
}

void ui_bt_set_power(int on) {
    bt_write_want(on);                        /* latest intent, before launching the async scripts */
    bt_log("power %s (target '%s')", on ? "on" : "off", s_rc.target);
    if (on) {
        /* Cold bringup BACKGROUNDED (create hci0 + dbus/bluetoothd/bluealsa if
         * absent; both scripts idempotent + mutexed). Running these foreground
         * froze the LVGL thread -- ALL input, including the power button -- for
         * the whole bringup, ~35s+ when patchram wedges (the exact freeze class
         * the boot-restore fix removed). Nothing here needs bluez yet: the UI
         * attaches via the org.bluez name watch when bluetoothd appears, and
         * bt_audio_up.sh powers the adapter itself (bluetoothctl power on). */
        if (system("(/usr/bin/bt_up.sh && /usr/bin/bt_audio_up.sh) >/dev/null 2>&1 &")) {}
        ui_bt_init();                         /* (re)subscribe + name watch (no-op pre-bus/repeat) */
        arm_power_confirm();
    } else {
        /* Full power-down: power the adapter off cleanly via bluez, then tear the
         * stack down + cold the chip (bt_down.sh) so ~2MB (bluetoothd+bluealsa) is
         * freed and the radio is fully off. Backgrounded so the UI doesn't block
         * on the kills; on_bluez_down clears our state when bluetoothd exits. */
        /* Remember what was connected BEFORE the power-down disconnects it: the
         * clean disconnect means headphones won't page back after On, so the
         * Powered edge then reconnects it (adapter_powered). Nothing connected:
         * the last audio device (headphones switched off first), else keep a
         * still-pending target (fast Off -> On -> Off). */
        bt_rc_off(&s_rc, &s_tab, s_bt_dev);
        bt_log("off: audio '%s', last '%s', reconnect target '%s' (%d devices)",
               s_bt_dev, s_rc.last_audio, s_rc.target, s_tab.n);
        reconnect_cancel();
        set_bool_prop(ADAPTER, IF_ADAPTER, "Powered", FALSE);
        if (system("/usr/bin/bt_down.sh >/dev/null 2>&1 &")) {}
        if (s_up_confirm) { g_source_remove(s_up_confirm); s_up_confirm = 0; }
    }
    s_powered = on;                           /* optimistic; name watch / confirm timeout settle it */
    notify(on ? "Turning Bluetooth on..." : "Bluetooth off");
}

/* Boot-restore power-on. Kept as a named entry point for the restore call site;
 * since the manual toggle now backgrounds the bringup too, it is the same code.
 * At boot the bus may not exist yet -- ui_bt_init() inside no-ops and main.c's
 * on_bus_ready -> ui_bt_init name-watch attaches once the backgrounded
 * bt_audio_up.sh brings dbus up. */
void ui_bt_power_on_deferred(void) {
    ui_bt_set_power(1);
}

/* BR/EDR-only discovery. Without this filter a busy RF environment delivers
 * dozens of LE beacons (79 in one 25s scan here, and their random addresses
 * ROTATE so each rotation is a new object path) -- they filled the 32-slot
 * device table twice over, so bt_dev_upsert() returned -1 for the headphones
 * the user was actually trying to pair and the row never appeared. main.conf
 * sets ControllerMode=bredr, so an LE-only device can never be connected here
 * anyway: filtering them out at the source costs nothing and is the only bound
 * that holds (a bigger table or an eviction policy still loses to an unbounded
 * beacon flood). The filter is per-D-Bus-client, so it can't affect bluetoothd's
 * other users. Fire-and-forget like the rest of this file; D-Bus preserves
 * per-connection ordering, so the filter is applied before StartDiscovery. */
void ui_bt_scan_start(void) {
    GVariantBuilder f;
    g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("auto"));
    g_variant_builder_add(&f, "{sv}", "DuplicateData", g_variant_new_boolean(FALSE));
    g_dbus_connection_call(ui_dbus_conn(), BLUEZ, ADAPTER, IF_ADAPTER,
        "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_call(ui_dbus_conn(), BLUEZ, ADAPTER, IF_ADAPTER,
        "StartDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}
void ui_bt_scan_stop(void) {
    g_dbus_connection_call(ui_dbus_conn(), BLUEZ, ADAPTER, IF_ADAPTER,
        "StopDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

const bt_devtable_t *ui_bt_devices(void) { return &s_tab; }
void ui_bt_set_changed_cb(ui_bt_changed_cb cb) { s_cb = cb; }

/* --- connect / disconnect ---------------------------------------------------- */

/* Forward declaration: ui_bt_codec_upgrade is implemented in Task 5. */
void ui_bt_codec_upgrade(const char *path);

typedef struct { char *path; } ConnCtx;

/* Map a bluez/GDBus error to a short, human-readable phrase for the UI.
 * bluez messages arrive as "GDBus.Error:org.bluez.Error.Failed: <detail>"; the
 * common failures get a plain-language line, anything else falls back to the
 * detail tail (after the last ": ") rather than the full dotted prefix. */
static const char *bt_friendly_err(GError *err) {
    if (!err || !err->message) return "unknown error";
    const char *m = err->message;
    if (strstr(m, "page-timeout"))            return "device not found (is it on and nearby?)";
    if (strstr(m, "AuthenticationFailed"))    return "pairing rejected by the device";
    if (strstr(m, "ConnectionAttemptFailed")) return "connection failed, try again";
    if (strstr(m, "NotReady"))                return "Bluetooth not ready yet";
    if (strstr(m, "InProgress"))              return "already in progress";
    if (strstr(m, "NotAvailable"))            return "device not available";
    const char *tail = strrchr(m, ':');       /* strip the dotted error-name prefix */
    return (tail && tail[1] == ' ' && tail[2]) ? tail + 2 : m;
}

static void on_connect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    ConnCtx *ctx = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (reply) {
        g_variant_unref(reply);
        /* Trust what the user connected, not only what they paired here: a
         * device paired earlier (Pair -> AlreadyExists) stayed Trusted=false
         * (2e.2: the WF-1000XM5), and bluez wants Trusted for the device's own
         * reconnects. No-op (no NAND write) when already trusted. */
        set_bool_prop(ctx->path, IF_DEVICE, "Trusted", TRUE);
        bt_log("connect %s ok (trusted)", ctx->path);
        ui_bt_scan_stop();   /* connected: ongoing inquiry only costs RF bandwidth + battery */
        ui_bt_codec_upgrade(ctx->path);
    } else {
        char msg[256];
        snprintf(msg, sizeof msg, "Connect failed: %s", bt_friendly_err(err));
        bt_log("connect %s failed: %s", ctx->path, err && err->message ? err->message : "?");
        notify(msg);
        g_clear_error(&err);
    }
    free(ctx->path);
    free(ctx);
}

static void on_pair_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    ConnCtx *ctx = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (reply) {
        g_variant_unref(reply);
        /* Trust what the user paired: bluez only lets a trusted device reconnect
         * (HID pads, auto-reconnecting sinks) without an agent to authorise it. */
        set_bool_prop(ctx->path, IF_DEVICE, "Trusted", TRUE);
    } else {
        /* Ignore AlreadyExists — device was already paired, proceed to Connect. */
        if (!err || !strstr(err->message, "AlreadyExists")) {
            char msg[256];
            snprintf(msg, sizeof msg, "Pair failed: %s", bt_friendly_err(err));
            notify(msg);
            g_clear_error(&err);
            free(ctx->path);
            free(ctx);
            return;
        }
        g_clear_error(&err);
    }
    /* Proceed to Connect regardless of whether Pair was needed. */
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, ctx->path, IF_DEVICE,
        "Connect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
        on_connect_done, ctx);
}

void ui_bt_connect(const char *path) {
    ConnCtx *ctx = malloc(sizeof *ctx);
    if (!ctx) return;
    ctx->path = strdup(path);
    if (!ctx->path) { free(ctx); return; }
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, path, IF_DEVICE,
        "Pair", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
        on_pair_done, ctx);
}

/* --- Off -> On reconnect ----------------------------------------------------
 * Boot and the On toggle share one bringup (ui_bt_power_on_deferred ==
 * ui_bt_set_power(1)), and neither connects anything itself: after a reboot
 * the headphones page the player back on their own (link loss), but after Off
 * the adapter power-down disconnected them cleanly, so they wait to be called.
 * On the adapter's real Powered false->true edge (from bluez, not the toggle's
 * optimistic s_powered), Device1.Connect the device remembered at Off -- the
 * same call + completion as a UI connect (scan stop + codec upgrade). Retries
 * on a short schedule (bt_reconnect_delay_s: bluealsa registers its A2DP
 * endpoints a few seconds after power-on), stops as soon as the device is
 * connected by either side, toasts once if it gives up. */
static guint s_reconnect_timer;

static void reconnect_cancel(void) {
    if (s_reconnect_timer) { g_source_remove(s_reconnect_timer); s_reconnect_timer = 0; }
}
static gboolean reconnect_cb(gpointer u);
static void reconnect_arm(int d) {
    reconnect_cancel();
    if (d >= 0) s_reconnect_timer = g_timeout_add_seconds(d, reconnect_cb, NULL);
}
static void reconnect_gave_up(const char *path) {
    int i = bt_dev_find(&s_tab, path);
    char msg[64];
    snprintf(msg, sizeof msg, "Couldn't reconnect %.24s",
             i >= 0 && s_tab.dev[i].name[0] ? s_tab.dev[i].name : "Bluetooth device");
    bt_log("reconnect gave up: %s", path);
    notify(msg);
}
static void on_reconnect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    char *path = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    int ok = reply || (err && err->message && strstr(err->message, "AlreadyConnected"));
    if (reply) g_variant_unref(reply);
    bt_log("reconnect try %d %s: %s", s_rc.tries, path,
           reply ? "ok" : err && err->message ? err->message : "?");
    g_clear_error(&err);
    int superseded = strcmp(path, s_rc.target) != 0;
    int d = bt_rc_result(&s_rc, path, ok);
    if (ok && !superseded) {
        set_bool_prop(path, IF_DEVICE, "Trusted", TRUE);
        ui_bt_codec_upgrade(path);
    } else if (d == BT_RC_GIVE_UP) reconnect_gave_up(path);
    else reconnect_arm(d);
    free(path);
}
static gboolean reconnect_cb(gpointer u) {
    (void)u;
    s_reconnect_timer = 0;
    if (!s_rc.target[0] || !s_rc.adapter_on || !s_bluez_up) {
        bt_log("reconnect skipped: target '%s' adapter %d bluez %d", s_rc.target, s_rc.adapter_on, s_bluez_up);
        return G_SOURCE_REMOVE;
    }
    int i = bt_dev_find(&s_tab, s_rc.target);
    if (i >= 0 && s_tab.dev[i].connected) {   /* came back by itself */
        bt_log("reconnect: %s already connected", s_rc.target);
        bt_rc_result(&s_rc, s_rc.target, 1);
        return G_SOURCE_REMOVE;
    }
    bt_log("reconnect try %d: Connect %s (in table %d)", s_rc.tries, s_rc.target, i >= 0);
    char *p = strdup(s_rc.target);
    if (!p) return G_SOURCE_REMOVE;
    /* Inquiry and paging share the radio: a discovery the BT screen kicked at
     * power-on slows or fails the page. Stop it first; D-Bus keeps per-
     * connection order, so StopDiscovery reaches bluez before Connect. */
    ui_bt_scan_stop();
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, p, IF_DEVICE,
        "Connect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
        on_reconnect_done, p);
    return G_SOURCE_REMOVE;
}
/* Every bluez-reported adapter Powered value goes through here (and org.bluez
 * vanishing = 0): a new bluetoothd's 0->1 edge arms the first attempt. */
static void adapter_powered(int p) {
    s_powered = p;
    int was = s_rc.adapter_on;
    int d = bt_rc_powered(&s_rc, p);
    if (p != was) bt_log("adapter Powered %d, reconnect target '%s'", p, s_rc.target);
    if (!p) { reconnect_cancel(); return; }
    if (d >= 0) reconnect_arm(d);
}

static void on_disconnect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    char *path = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (reply) {
        g_variant_unref(reply);
    } else {
        char msg[256];
        snprintf(msg, sizeof msg, "Disconnect failed: %s",
                 err ? err->message : "unknown");
        notify(msg);
        g_clear_error(&err);
    }
    free(path);
}

void ui_bt_disconnect(const char *path) {
    bt_rc_drop(&s_rc, path);   /* explicit UI disconnect: no auto-reconnect after Off -> On */
    bt_log("ui disconnect %s: reconnect target '%s'", path, s_rc.target);
    char *p = strdup(path);
    if (!p) return;
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, p, IF_DEVICE,
        "Disconnect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_disconnect_done, p);
}

static void on_remove_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    char *path = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (reply) {
        /* Success needs no action: bluez emits InterfacesRemoved and
         * on_removed() drops the device + refreshes the UI. */
        g_variant_unref(reply);
    } else {
        char msg[256];
        snprintf(msg, sizeof msg, "Forget failed: %s", bt_friendly_err(err));
        notify(msg);
        g_clear_error(&err);
    }
    free(path);
}

/* Unpair: RemoveDevice deletes the device object AND its link keys. Works on
 * connected devices too (bluez disconnects first). */
void ui_bt_remove(const char *path)
{
    bt_rc_drop(&s_rc, path);   /* forgotten: never auto-reconnect it */
    bt_log("ui forget %s: reconnect target '%s'", path, s_rc.target);
    char *p = strdup(path);
    if (!p) return;
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEZ, ADAPTER, IF_ADAPTER,
        "RemoveDevice", g_variant_new("(o)", p), NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        on_remove_done, p);
}

/* --- codec upgrade via org.bluealsa ---------------------------------------- */
/* BlueALSA 4.x names (pending HITL introspection confirmation):
 *   service:    org.bluealsa
 *   ObjectManager object: /
 *   PCM interface: org.bluealsa.PCM1
 *   PCM properties: Device (o), Mode (s "source"/"sink")
 *   GetCodecs() → a{sa{sv}}   (codec-name → dict of codec params)
 *   SelectCodec(s codec, a{sv} params) → nothing
 */
/* BLUEALSA / IF_BLUEALSA_PCM are defined earlier in the file (top of the
 * device branch), ahead of on_props/ui_bt_init which also need them. */

/* Write the active A2DP source PCM volume. bluealsa packs both channels into one
 * uint16: (left<<8)|right, each a 7-bit 0..127 level (bit7 would be per-channel
 * mute; we leave it clear). SoftVolume=true (bluealsa's default) means it
 * attenuates the PCM in software before LDAC encoding -- HW-verified audible on
 * the WF-1000XM5. No-op until a PCM is cached (see on_bluealsa_objects). */
static void apply_bt_volume(int pct) {
    if (!s_bt_pcm[0] || !ui_dbus_conn()) return;
    guint16 packed = bt_vol_pack(bt_vol_pct_to_ch(pct));
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEALSA, s_bt_pcm, IF_PROPS, "Set",
        g_variant_new("(ssv)", IF_BLUEALSA_PCM, "Volume",
                      g_variant_new_uint16(packed)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Is BT the active audio sink (a device is connected and MPD is routed there)? */
int ui_bt_audio_active(void) { return s_routed_bt == 1; }

/* Set the BT headphone volume (0..100). Remembered so it can be re-applied the
 * moment the PCM appears, and applied immediately if one is already live. */
void ui_bt_set_volume_pct(int pct) {
    pct = ui_vol_cap(pct, ui_power_vollimit_get());   /* 0..100 and Volume Limit */
    apply_bt_volume(pct);
    s_bt_vol_pct = pct;           /* remember in RAM; flushed to disk on disconnect/shutdown */
}

/* Flush the connected device's level to disk. Called from the graceful-shutdown
 * path (ui_power_off) so the normal poweroff persists the current volume without
 * a write on every wheel step. No-op if nothing is connected. */
void ui_bt_persist_volume(void) {
    if (s_bt_dev[0] && s_bt_vol_pct >= 0)
        bt_vol_save(s_bt_dev, s_bt_vol_pct);
}

void ui_bt_set_volume_cb(ui_bt_volume_cb cb) { s_volume_cb = cb; }

/* Context passed through the two-step async chain. */
typedef struct {
    char *dev_path;   /* owned: the org.bluez device object path */
    char *pcm_path;   /* owned: the org.bluealsa PCM object path */
    char codec[16];   /* the PCM's current Codec at discovery ("" = unknown) */
} CodecCtx;

/* Codec-switch wait over (new PCM added, SelectCodec failed, or timed out). */
static void codec_wait_end(void) {
    s_codec_wait[0] = 0;
    if (s_codec_wait_timer) { g_source_remove(s_codec_wait_timer); s_codec_wait_timer = 0; }
}
static gboolean codec_wait_timeout(gpointer u) {
    (void)u;
    s_codec_wait_timer = 0;   /* this source is being removed: don't remove it twice */
    codec_wait_end();
    notify(NULL);             /* never strand MPD on the DAC: route on what exists */
    return G_SOURCE_REMOVE;
}

static void codec_ctx_free(CodecCtx *ctx) {
    if (!ctx) return;
    free(ctx->dev_path);
    free(ctx->pcm_path);
    free(ctx);
}

/* Step 3: SelectCodec reply — just surface errors. */
/* End of a codec upgrade (any outcome): the stream is settled, so route MPD. */
static void codec_done(CodecCtx *ctx) {
    codec_ctx_free(ctx);
    if (s_bt_codec_busy > 0) s_bt_codec_busy--;
    notify(NULL);
}

static void on_select_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    CodecCtx *ctx = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (reply) {
        g_variant_unref(reply);
    } else {
        char msg[256];
        snprintf(msg, sizeof msg, "SelectCodec failed: %s",
                 err ? err->message : "unknown");
        codec_wait_end();   /* no transport rebuild is coming */
        notify(msg);
        g_clear_error(&err);
    }
    codec_done(ctx);
}

/* Step 2: GetCodecs reply — pick best codec, call SelectCodec. */
static void on_codecs(GObject *src, GAsyncResult *res, gpointer user_data) {
    CodecCtx *ctx = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!reply) {
        char msg[256];
        snprintf(msg, sizeof msg, "GetCodecs failed: %s",
                 err ? err->message : "unknown");
        notify(msg);
        g_clear_error(&err);
        codec_done(ctx);
        return;
    }

    /* reply is (a{sa{sv}}) — unwrap the tuple to get the dict. */
    GVariant *codecs;
    g_variant_get(reply, "(@a{sa{sv}})", &codecs);

    /* Build array of codec name pointers for bt_pick_codec. */
    gsize n = g_variant_n_children(codecs);
    const char **names = g_new0(const char *, n);
    GVariantIter it;
    g_variant_iter_init(&it, codecs);
    const char *cname; GVariant *cparams;
    gsize i = 0;
    while (g_variant_iter_next(&it, "{&s@a{sv}}", &cname, &cparams)) {
        names[i++] = cname;   /* points into codecs; valid while codecs is alive */
        g_variant_unref(cparams);
    }

    int chosen = bt_pick_codec(names, (int)n);

    /* COPY the name before unreffing codecs (the &s pointers are into codecs). */
    char *chosen_name = (chosen >= 0) ? g_strdup(names[chosen]) : NULL;
    g_free(names);
    g_variant_unref(codecs);
    g_variant_unref(reply);

    if (!bt_codec_switch_needed(ctx->codec[0] ? ctx->codec : NULL, chosen_name)) {
        /* No supported codec found, or already on it: leave the stream alone. */
        g_free(chosen_name);
        codec_done(ctx);
        return;
    }

    /* Routing waits for the recreated PCM (see s_codec_wait). Set BEFORE the
     * call: the new PCM may be added before the reply is read. */
    snprintf(s_codec_wait, sizeof s_codec_wait, "%s", chosen_name);
    if (s_codec_wait_timer) g_source_remove(s_codec_wait_timer);
    s_codec_wait_timer = g_timeout_add_seconds(BT_CODEC_WAIT_SECS, codec_wait_timeout, NULL);

    /* Call SelectCodec(s, a{sv}) with an empty param dict. */
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEALSA, ctx->pcm_path, IF_BLUEALSA_PCM,
        "SelectCodec",
        g_variant_new("(s@a{sv})", chosen_name,
                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_select_done, ctx);

    g_free(chosen_name);
    /* ctx ownership transferred to on_select_done. */
}

/* SoftVolume=false Set completed -> restore THIS device's saved volume. We push
 * our own remembered level (per-device, from disk) rather than adopt whatever
 * bluealsa reports: at connect the AVRCP absolute-volume handshake hasn't synced
 * yet, so bluealsa still shows its full-scale default -- adopting that is exactly
 * what blew out ears after a reboot. A device we've never seen gets a safe
 * default (not max). SoftVolume=false means this Volume write is sent to the
 * headphones as an AVRCP SetAbsoluteVolume, so it actually sets the device level.
 * Subsequent device-side (button) changes still flow back via on_ba_props. */
static void on_softvol_set(GObject *src, GAsyncResult *res, gpointer u) {
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) g_variant_unref(r);
    g_free(u);                                  /* the g_strdup'd pcm path, unused now */

    int pct = bt_vol_load(s_bt_dev);
    if (pct < 0) pct = BT_VOL_DEFAULT_PCT;      /* first time we've seen this device */
    pct = ui_vol_cap(pct, ui_power_vollimit_get());   /* a saved level above Volume Limit */
    apply_bt_volume(pct);                       /* AVRCP absolute-volume write */
    s_bt_vol_pct = pct;                         /* seed the live level for this connection */
    bt_vol_save(s_bt_dev, pct);                 /* establish a first-time device's entry (idempotent: a returning device is a no-op) */
    if (s_volume_cb) s_volume_cb(pct);          /* reflect in the UI bar */
}

/* Step 1: GetManagedObjects reply — find the a2dp-source PCM for dev_path. */
static void on_bluealsa_objects(GObject *src, GAsyncResult *res, gpointer user_data) {
    CodecCtx *ctx = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!reply) {
        if (err) notify(err->message);
        g_clear_error(&err);
        codec_ctx_free(ctx);
        return;
    }

    /* reply: (a{oa{sa{sv}}}) — map of object-path → iface-map */
    GVariant *objs;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objs);

    char *pcm_path = NULL;
    GVariantIter obj_it;
    const char *opath; GVariant *ifaces;
    g_variant_iter_init(&obj_it, objs);
    while (!pcm_path && g_variant_iter_next(&obj_it, "{&o@a{sa{sv}}}", &opath, &ifaces)) {
        GVariant *pcm_iface = g_variant_lookup_value(ifaces, IF_BLUEALSA_PCM, NULL);
        if (pcm_iface) {
            /* Check Device property matches our bluez path. */
            GVariant *dev_v = g_variant_lookup_value(pcm_iface, "Device",
                                                      G_VARIANT_TYPE_OBJECT_PATH);
            GVariant *mode_v = g_variant_lookup_value(pcm_iface, "Mode",
                                                       G_VARIANT_TYPE_STRING);
            if (dev_v && mode_v) {
                const char *dev  = g_variant_get_string(dev_v, NULL);
                const char *mode = g_variant_get_string(mode_v, NULL);
                /* A2DP-source transport, but the PCM Mode is "sink": bluealsa
                 * names Mode by the application's I/O direction and MPD *writes*
                 * playback audio. (Matching "source" here silently found nothing,
                 * so codec-upgrade + volume caching were both no-ops.) */
                if (strcmp(dev, ctx->dev_path) == 0 &&
                    strcmp(mode, "sink") == 0) {
                    pcm_path = g_strdup(opath);
                    GVariant *cv = g_variant_lookup_value(pcm_iface, "Codec",
                                                          G_VARIANT_TYPE_STRING);
                    if (cv) {
                        snprintf(ctx->codec, sizeof ctx->codec, "%s",
                                 g_variant_get_string(cv, NULL));
                        g_variant_unref(cv);
                    }
                }
            }
            if (dev_v)  g_variant_unref(dev_v);
            if (mode_v) g_variant_unref(mode_v);
            g_variant_unref(pcm_iface);
        }
        g_variant_unref(ifaces);
    }

    g_variant_unref(objs);
    g_variant_unref(reply);

    if (!pcm_path) {
        /* BlueALSA doesn't have a source PCM for this device yet — not an error. */
        codec_ctx_free(ctx);
        return;
    }

    ctx->pcm_path = pcm_path;   /* hand ownership to ctx */

    /* Cache the PCM path for the volume wheel, and the device path as the
     * per-device volume key (see bt_vol_save/on_softvol_set). */
    snprintf(s_bt_pcm, sizeof s_bt_pcm, "%s", pcm_path);
    snprintf(s_bt_dev, sizeof s_bt_dev, "%s", ctx->dev_path);
    bt_rc_audio(&s_rc, ctx->dev_path);   /* last audio device, for Off -> On */
    s_bt_codec_busy++;   /* routing waits for codec_done(): see s_bt_codec_busy */
    /* AVRCP absolute volume, not bluealsa software attenuation: SoftVolume=false
     * makes our Volume writes drive the headphones' OWN volume (synced both ways,
     * no over-the-air quality loss). bluealsa defaults this to true per PCM, so
     * set it on every (re)discovery. The Set is async: on_softvol_set then
     * restores this device's saved level once the switch has settled, which
     * also avoids a brief full-scale moment. */
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEALSA, s_bt_pcm, IF_PROPS, "Set",
        g_variant_new("(ssv)", IF_BLUEALSA_PCM, "SoftVolume",
                      g_variant_new_boolean(FALSE)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_softvol_set, g_strdup(s_bt_pcm));    /* restore saved level after the switch settles */

    /* Step 2: get available codecs for this PCM. */
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEALSA, ctx->pcm_path, IF_BLUEALSA_PCM,
        "GetCodecs", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_codecs, ctx);
    /* ctx ownership transferred to on_codecs. */
}

/* Entry point called by on_connect_done with the org.bluez device path. */
void ui_bt_codec_upgrade(const char *dev_path) {
    CodecCtx *ctx = malloc(sizeof *ctx);
    if (!ctx) return;
    ctx->dev_path = strdup(dev_path);
    ctx->pcm_path = NULL;
    ctx->codec[0] = 0;
    if (!ctx->dev_path) { free(ctx); return; }

    /* Step 1: enumerate all BlueALSA objects to find the PCM for this device.
     * BlueALSA's ObjectManager lives at /org/bluealsa (NOT "/", which returns
     * UnknownMethod) -- getting this wrong made discovery error out silently. */
    g_dbus_connection_call(
        ui_dbus_conn(), BLUEALSA, "/org/bluealsa", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_bluealsa_objects, ctx);
    /* ctx ownership transferred to on_bluealsa_objects. */
}
#endif

/* Remember the user's Bluetooth on/off choice across reboots. One char, 0/1;
 * same one-value-per-file convention as ui_power.c. Build-agnostic (outside the
 * EROSQ_HOST guard): save is pure file I/O, restore calls ui_bt_set_power which
 * no-ops on the host preview. */
#define BT_PREF_PATH "/data/erosq-ui.bt"

void bt_pref_save(int on)
{
    int fd = open(BT_PREF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;                       /* no /data -> just don't persist */
    if (write(fd, on ? "1" : "0", 1) < 0) { /* ignore */ }
    close(fd);
}

/* Restore at boot, from on_bus_ready (BT power needs the system bus up). Only
 * powers on for a saved "on": BT is off at boot by default, so "off" is already
 * satisfied and we skip a needless bt_down.sh teardown of a stack never started. */
void bt_pref_restore(void)
{
    int fd = open(BT_PREF_PATH, O_RDONLY);
    if (fd < 0) return;
    char b; int r = read(fd, &b, 1); close(fd);
    if (r == 1 && b == '1') ui_bt_power_on_deferred();   /* backgrounded: don't freeze the UI at boot */
}
