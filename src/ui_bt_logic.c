#include "ui_bt_logic.h"
#include <string.h>
#include <stdio.h>

int bt_dev_find(const bt_devtable_t *t, const char *path) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->dev[i].path, path) == 0) return i;
    return -1;
}

int bt_dev_upsert(bt_devtable_t *t, const char *path) {
    int i = bt_dev_find(t, path);
    if (i >= 0) return i;
    if (t->n >= BT_MAX_DEVICES) return -1;
    bt_device_t *d = &t->dev[t->n];
    memset(d, 0, sizeof *d);
    d->battery = -1;                 /* unknown until Battery1 reports */
    snprintf(d->path, sizeof d->path, "%s", path);
    return t->n++;
}

void bt_dev_remove(bt_devtable_t *t, const char *path) {
    int i = bt_dev_find(t, path);
    if (i < 0) return;
    t->dev[i] = t->dev[--t->n];   /* compact: move last into the hole */
}

/* bluez-alsa codec names are case-sensitive canonical strings: "SBC","aptX","LDAC". */
int bt_pick_codec(const char *const *avail, int n) {
    const char *ladder[] = { "LDAC", "aptX", "SBC" };
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < n; i++)
            if (strcmp(avail[i], ladder[r]) == 0) return i;
    return -1;
}

bool bt_codec_switch_needed(const char *cur, const char *chosen) {
    return chosen && (!cur || strcmp(cur, chosen) != 0);
}

bool bt_dev_listable(bool paired, bool connected, const char *icon,
                     const char *const *uuids, int n_uuids) {
    if (paired || connected) return true;
    if (icon && (!strncmp(icon, "audio-", 6) || !strcmp(icon, "input-gaming"))) return true;
    /* 16-bit SIG UUIDs as bluez spells them: "0000XXXX-0000-1000-8000-00805f9b34fb" */
    static const char *const want[] = {
        "00001812",   /* HID over GATT (LE pads) */
        "00001124",   /* HID (classic pads) */
        "0000110b",   /* Audio Sink */
        "0000110d",   /* A2DP */
        "00001108",   /* Headset */
        "0000111e",   /* Handsfree */
    };
    for (int i = 0; i < n_uuids; i++) {
        if (!uuids[i] || strlen(uuids[i]) < 8) continue;
        for (unsigned j = 0; j < sizeof want / sizeof *want; j++)
            if (!strncmp(uuids[i], want[j], 8)) return true;
    }
    return false;
}

bool bt_drop_carry_on(bool jack_in, bool discpause_on, bool dev_connected)
{
    return jack_in || !discpause_on || dev_connected;
}

bool bt_pcm_is_a2dp_sink(const char *path)
{
    static const char suf[] = "/a2dpsrc/sink";
    size_t n = path ? strlen(path) : 0;
    return n >= sizeof suf - 1 && strcmp(path + n - (sizeof suf - 1), suf) == 0;
}

const char *bt_route_cmds(bool to_bt)
{
    return to_bt ? "command_list_begin\nenableoutput 1\ndisableoutput 0\ncommand_list_end\n"
                 : "command_list_begin\nenableoutput 0\ndisableoutput 1\ncommand_list_end\n";
}

bool bt_route_want_bt(bool dev_connected, bool sink_pcm_known)
{
    return dev_connected && sink_pcm_known;
}

const char *bt_reconnect_pick(const bt_devtable_t *t, const char *audio_dev)
{
    if (audio_dev && audio_dev[0]) {
        int i = bt_dev_find(t, audio_dev);
        if (i >= 0 && t->dev[i].paired) return t->dev[i].path;
    }
    for (int i = 0; i < t->n; i++)
        if (t->dev[i].connected && t->dev[i].paired) return t->dev[i].path;
    return NULL;
}

int bt_reconnect_delay_s(int attempt)
{
    /* bt_audio_up.sh: adapter on, sleep 2, bluealsa + up to 4 s to register.
     * Connect before that finds no A2DP endpoint ("profile unavailable"). */
    static const int d[] = { 6, 5, 8, 12 };
    return attempt >= 0 && attempt < (int)(sizeof d / sizeof d[0]) ? d[attempt] : -1;
}

void bt_rc_audio(bt_rc_t *rc, const char *dev)
{
    if (!dev || !dev[0]) return;
    snprintf(rc->last_audio, sizeof rc->last_audio, "%s", dev);
    if (!strcmp(rc->dropped, dev)) rc->dropped[0] = 0;
}

void bt_rc_drop(bt_rc_t *rc, const char *dev)
{
    if (!dev || !dev[0]) return;
    if (!strcmp(rc->target, dev)) { rc->target[0] = 0; rc->tries = 0; }
    if (!strcmp(rc->last_audio, dev)) rc->last_audio[0] = 0;
    snprintf(rc->dropped, sizeof rc->dropped, "%s", dev);
}

void bt_rc_off(bt_rc_t *rc, const bt_devtable_t *t, const char *audio_dev)
{
    const char *p = bt_reconnect_pick(t, audio_dev);
    if (p && !strcmp(p, rc->dropped)) p = NULL;   /* the user disconnected it */
    if (!p && rc->last_audio[0] && strcmp(rc->last_audio, rc->dropped)) p = rc->last_audio;
    if (p && p != rc->target) snprintf(rc->target, sizeof rc->target, "%s", p);
    rc->tries = 0;
}

int bt_rc_powered(bt_rc_t *rc, int powered)
{
    int was = rc->adapter_on;
    rc->adapter_on = powered != 0;
    if (!powered || was || !rc->target[0]) return BT_RC_NONE;
    rc->tries = 0;
    return bt_reconnect_delay_s(0);
}

int bt_rc_result(bt_rc_t *rc, const char *path, int ok)
{
    if (!rc->target[0] || !path || strcmp(path, rc->target)) return BT_RC_NONE;   /* superseded */
    if (ok) { rc->target[0] = 0; return BT_RC_NONE; }
    if (!rc->adapter_on) return BT_RC_NONE;        /* Off again: the next On re-arms */
    int d = bt_reconnect_delay_s(++rc->tries);
    if (d >= 0) return d;
    rc->target[0] = 0;
    return BT_RC_GIVE_UP;
}
