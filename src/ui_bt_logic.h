#ifndef EROSQ_UI_BT_LOGIC_H
#define EROSQ_UI_BT_LOGIC_H
#include <stdbool.h>

#define BT_MAX_DEVICES 32
#define BT_PATH_MAX    128
#define BT_NAME_MAX    64
#define BT_ADDR_LEN    18    /* "XX:XX:XX:XX:XX:XX\0" */
#define BT_CODEC_MAX   16

typedef struct {
    char path[BT_PATH_MAX];   /* org.bluez object path: the stable table key */
    char addr[BT_ADDR_LEN];
    char name[BT_NAME_MAX];
    char codec[BT_CODEC_MAX]; /* current A2DP codec, "" if not connected */
    int  rssi;                /* 0 if unknown */
    int  battery;             /* 0..100 from org.bluez.Battery1, -1 if unknown */
    bool paired;
    bool connected;
} bt_device_t;

typedef struct {
    bt_device_t dev[BT_MAX_DEVICES];
    int n;
} bt_devtable_t;

int  bt_dev_find(const bt_devtable_t *t, const char *path);
int  bt_dev_upsert(bt_devtable_t *t, const char *path);   /* create-if-absent; -1 if full */
void bt_dev_remove(bt_devtable_t *t, const char *path);
int  bt_pick_codec(const char *const *avail, int n);      /* LDAC>aptX>SBC; -1 if none */
/* SelectCodec only when the PCM isn't already on `chosen`: every A2DP switch
 * recreates the transport (and its PCM), so a redundant one is a needless
 * stream rebuild under MPD. cur NULL = unknown -> switch. */
bool bt_codec_switch_needed(const char *cur, const char *chosen);
/* Scan-list admission: paired/connected, an audio-* or input-gaming icon, or a
 * HID / audio-sink service UUID. Everything else (LE beacons) stays off the list. */
/* BT audio sink lost, MPD now on the DAC: carry on playing (true), or let
 * ui_bt_drop_cmd pause it? A jack in or Pause on Disconnect Off carries on. So
 * does a sink lost while the device is still connected: a codec reconfigure
 * re-adds it, and a real power-off's Device1 Connected=false follows ~0.2 s
 * later and makes the final call. */
bool bt_drop_carry_on(bool jack_in, bool discpause_on, bool dev_connected);
/* BlueALSA PCM object path is an A2DP playback sink (what MPD's BT output uses). */
bool bt_pcm_is_a2dp_sink(const char *path);
/* MPD command list moving playback to Bluetooth (true) or the DAC (false).
 * Enables the new output BEFORE disabling the old one: the other order lets
 * MPD's player see zero outputs and pause with "All audio outputs are disabled". */
const char *bt_route_cmds(bool to_bt);
/* Route MPD to Bluetooth only when a device is connected AND its A2DP sink PCM
 * is known to exist; until then the DAC keeps playing. */
bool bt_route_want_bt(bool dev_connected, bool sink_pcm_known);
/* Bluetooth Off -> On reconnect. Picked at Off time, while the table still
 * shows what was connected: the audio device in use (audio_dev, the volume key)
 * if it is still in the table and paired, else the first connected + paired
 * device, else NULL. The Off path powers the adapter down via bluez, which
 * disconnects cleanly, so headphones treat it as deliberate and don't page back
 * by themselves after On: the player has to reconnect them. */
const char *bt_reconnect_pick(const bt_devtable_t *t, const char *audio_dev);
/* Seconds before reconnect attempt n (0-based) after the adapter powers on;
 * -1 = give up. The first waits for bluealsa to register its A2DP endpoints. */
int bt_reconnect_delay_s(int attempt);
/* Off -> On reconnect state (the glue in ui_bt.c only runs the timer and the
 * D-Bus calls). Every bluez-reported adapter Powered value, and org.bluez
 * vanishing (bluetoothd stopped by bt_down.sh = Powered 0), goes through
 * bt_rc_powered; a NEW bluetoothd after On reports Powered 0 then 1 (or 1 in
 * its first GetManagedObjects), and that 0->1 edge arms the first attempt. */
typedef struct {
    char target[BT_PATH_MAX];      /* device to reconnect after On; "" = none */
    char last_audio[BT_PATH_MAX];  /* last device whose A2DP sink came up (this session) */
    int  adapter_on;               /* last Powered value bluez reported */
    int  tries;                    /* attempts made for target */
    char dropped[BT_PATH_MAX];     /* last device the user disconnected/forgot in the UI */
} bt_rc_t;
enum { BT_RC_NONE = -1, BT_RC_GIVE_UP = -2 };
/* An A2DP sink came up for dev: remember it as the last audio device. */
void bt_rc_audio(bt_rc_t *rc, const char *dev);
/* Bluetooth Off: target = bt_reconnect_pick (what is connected now), else the
 * last audio device -- headphones switched off (or dropped) just before Off
 * are still the ones to call back -- else keep a still-pending target. */
void bt_rc_off(bt_rc_t *rc, const bt_devtable_t *t, const char *audio_dev);
/* Adapter Powered as bluez reports it. Seconds until the first attempt on a
 * 0->1 edge with a target, else BT_RC_NONE. */
int  bt_rc_powered(bt_rc_t *rc, int powered);
/* Outcome of an attempt on path (ok: Connect replied, or the device is already
 * connected). Seconds until the next attempt, BT_RC_NONE (done / superseded /
 * adapter off), or BT_RC_GIVE_UP (target cleared; tell the user). */
int  bt_rc_result(bt_rc_t *rc, const char *path, int ok);
/* The user disconnected or forgot dev in the UI: never auto-reconnect it on
 * a later Off -> On (target and last audio cleared; bt_rc_off skips it even
 * if bluez still reports it connected at Off). Its next A2DP sink coming up
 * (bt_rc_audio, i.e. the user connected it again) lifts this. */
void bt_rc_drop(bt_rc_t *rc, const char *dev);
bool bt_dev_listable(bool paired, bool connected, const char *icon,
                     const char *const *uuids, int n_uuids);

#endif
