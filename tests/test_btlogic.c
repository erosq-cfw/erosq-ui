#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ui_bt_logic.h"

static void test_pick_codec(void) {
    const char *a[] = { "SBC", "aptX", "LDAC" };
    assert(bt_pick_codec(a, 3) == 2);          /* LDAC wins */
    const char *b[] = { "SBC", "aptX" };
    assert(bt_pick_codec(b, 2) == 1);          /* aptX over SBC */
    const char *c[] = { "SBC" };
    assert(bt_pick_codec(c, 1) == 0);          /* SBC fallback */
    const char *d[] = { "AAC", "MP3" };
    assert(bt_pick_codec(d, 2) == -1);         /* none of the three */
    assert(bt_pick_codec(a, 0) == -1);         /* empty */
}

static void test_codec_switch_needed(void) {
    assert(!bt_codec_switch_needed("LDAC", "LDAC"));   /* already there: no rebuild */
    assert(bt_codec_switch_needed("SBC", "LDAC"));
    assert(bt_codec_switch_needed(NULL, "LDAC"));      /* unknown: switch */
    assert(!bt_codec_switch_needed("SBC", NULL));      /* nothing chosen */
    assert(bt_codec_switch_needed("ldac", "LDAC"));    /* names are case-sensitive */
}

static void test_devtable(void) {
    bt_devtable_t t = {0};
    assert(bt_dev_find(&t, "/x") == -1);
    int i = bt_dev_upsert(&t, "/org/bluez/hci0/dev_AA");
    assert(i == 0 && t.n == 1);
    int j = bt_dev_upsert(&t, "/org/bluez/hci0/dev_AA");  /* same path: no dup */
    assert(j == 0 && t.n == 1);
    bt_dev_upsert(&t, "/org/bluez/hci0/dev_BB");
    assert(t.n == 2);
    bt_dev_remove(&t, "/org/bluez/hci0/dev_AA");          /* removes + compacts */
    assert(t.n == 1);
    assert(bt_dev_find(&t, "/org/bluez/hci0/dev_BB") == 0);

    bt_dev_remove(&t, "/org/bluez/hci0/dev_ZZ");          /* absent: no-op, no underflow */
    assert(t.n == 1);
}

static void test_devtable_full(void) {
    bt_devtable_t t = {0};
    char p[32];
    for (int i = 0; i < BT_MAX_DEVICES; i++) {
        snprintf(p, sizeof p, "/dev_%02d", i);
        assert(bt_dev_upsert(&t, p) == i);
    }
    assert(t.n == BT_MAX_DEVICES);
    assert(bt_dev_upsert(&t, "/dev_overflow") == -1);     /* full: refused */
    assert(t.n == BT_MAX_DEVICES);                        /* count unchanged */
}

static void test_dev_battery_init(void) {
    bt_devtable_t t = {0};
    int i = bt_dev_upsert(&t, "/dev_new");
    assert(t.dev[i].battery == -1);   /* unknown until org.bluez.Battery1 reports */
}

/* Scan-list admission. Real objects from a live auto-transport scan (2026-10-06):
 * the 8BitDo Ultimate 2 is LE-only HOGP and was invisible under Transport=bredr. */
static void test_listable(void) {
    const char *hogp[]   = { "00001800-0000-1000-8000-00805f9b34fb", "0000180f-0000-1000-8000-00805f9b34fb",
                             "00001812-0000-1000-8000-00805f9b34fb" };       /* 8BitDo: GAP, battery, HID-over-GATT */
    const char *fastp[]  = { "0000fcf1-0000-1000-8000-00805f9b34fb" };      /* random-addr LE beacon */
    const char *serial[] = { "00001122-0000-1000-8000-00805f9b34fb" };      /* unnamed-class stranger */
    const char *a2dp[]   = { "0000110b-0000-1000-8000-00805f9b34fb" };      /* audio sink, no icon yet */
    const char *bthid[]  = { "00001124-0000-1000-8000-00805f9b34fb" };      /* classic HID pad */

    assert(bt_dev_listable(false, false, "input-gaming", NULL, 0));          /* pad in pairing mode, icon only */
    assert(bt_dev_listable(false, false, NULL, hogp, 3));                    /* pad, UUIDs only */
    assert(bt_dev_listable(false, false, "audio-headphones", NULL, 0));      /* BR/EDR headphones (Class icon) */
    assert(bt_dev_listable(false, false, "audio-card", NULL, 0));
    assert(bt_dev_listable(false, false, NULL, a2dp, 1));
    assert(bt_dev_listable(false, false, NULL, bthid, 1));
    assert(bt_dev_listable(true,  false, NULL, NULL, 0));                    /* paired: always listed */
    assert(bt_dev_listable(false, true,  NULL, NULL, 0));                    /* connected: always listed */

    assert(!bt_dev_listable(false, false, NULL, NULL, 0));                   /* bare beacon */
    assert(!bt_dev_listable(false, false, NULL, fastp, 1));
    assert(!bt_dev_listable(false, false, NULL, serial, 1));
    assert(!bt_dev_listable(false, false, "phone", NULL, 0));                /* can't stream to/from it here */
    assert(!bt_dev_listable(false, false, "computer", NULL, 0));
    assert(!bt_dev_listable(false, false, "audio", NULL, 0));                /* prefix needs the dash */
    assert(!bt_dev_listable(false, false, "input-mouse", NULL, 0));          /* gamepads only, not every input-* */
    assert(!bt_dev_listable(false, false, "", NULL, 0));
    const char *bad[] = { NULL, "1812" };                                    /* NULL / short strings: no crash */
    assert(!bt_dev_listable(false, false, NULL, bad, 2));
}

/* BT sink lost (capture 2026-10-16: PCM removed 0.21 s before Device1
 * Connected=false, and MPD fails at the PCM removal). */
static void test_drop_carry_on(void) {
    /* Still connected: PCM-only loss -> carry on, whatever the jacks/setting say */
    assert(bt_drop_carry_on(false, true, true));
    /* Device gone, nothing plugged, Pause on Disconnect On -> the only pause case */
    assert(!bt_drop_carry_on(false, true, false));
    assert(bt_drop_carry_on(true, true, false));     /* wired headphones in: carry on */
    assert(bt_drop_carry_on(false, false, false));   /* Pause on Disconnect Off: carry on */
    assert(bt_drop_carry_on(true, false, true));
}

static void test_pcm_is_a2dp_sink(void) {
    assert(bt_pcm_is_a2dp_sink("/org/bluealsa/hci0/dev_AC_80_0A_46_6D_34/a2dpsrc/sink"));
    assert(!bt_pcm_is_a2dp_sink("/org/bluealsa/hci0/dev_AC_80_0A_46_6D_34/a2dpsrc/source"));
    assert(!bt_pcm_is_a2dp_sink("/org/bluealsa/hci0/dev_AC_80_0A_46_6D_34/hfpag/sink"));  /* SCO, not A2DP */
    assert(!bt_pcm_is_a2dp_sink("/a2dpsrc/sinkX"));
    assert(bt_pcm_is_a2dp_sink("/a2dpsrc/sink"));          /* exactly the suffix */
    assert(!bt_pcm_is_a2dp_sink("sink"));
    assert(!bt_pcm_is_a2dp_sink(""));
    assert(!bt_pcm_is_a2dp_sink(NULL));
}

/* MPD output switch: enable the new output BEFORE disabling the old one, or MPD
 * can act between the two and pause with "All audio outputs are disabled"
 * (device mpd.log 2026-10-16 02:28:39). Output 0 = DAC, 1 = Bluetooth. */
static void test_route_cmds(void) {
    const char *bt = bt_route_cmds(true), *dac = bt_route_cmds(false);
    assert(bt && dac && bt != dac);
    assert(strstr(bt, "command_list_begin\n") == bt);
    assert(strstr(bt, "\ncommand_list_end\n"));
    assert(strstr(bt, "enableoutput 1") && strstr(bt, "disableoutput 0"));
    assert(strstr(bt, "enableoutput 1") < strstr(bt, "disableoutput 0"));
    assert(strstr(dac, "enableoutput 0") && strstr(dac, "disableoutput 1"));
    assert(strstr(dac, "enableoutput 0") < strstr(dac, "disableoutput 1"));
    assert(!strstr(bt, "enableoutput 0") && !strstr(dac, "enableoutput 1"));
}

/* Route MPD to Bluetooth only once the A2DP sink PCM exists: enabling the output
 * at bluez Connected=true (~1.3 s before the PCM) made MPD fail to open it. */
static void test_route_want_bt(void) {
    assert(bt_route_want_bt(true, true));
    assert(!bt_route_want_bt(true, false));    /* connected, PCM not there yet: stay on the DAC */
    assert(!bt_route_want_bt(false, true));    /* PCM seen before Connected=true: wait for it */
    assert(!bt_route_want_bt(false, false));
}

static void test_reconnect(void) {
    bt_devtable_t t; memset(&t, 0, sizeof t);
    int a = bt_dev_upsert(&t, "/org/bluez/hci0/dev_AA");   /* paired headphones, connected */
    int b = bt_dev_upsert(&t, "/org/bluez/hci0/dev_BB");   /* paired pad, connected */
    int c = bt_dev_upsert(&t, "/org/bluez/hci0/dev_CC");   /* unpaired, connected */
    t.dev[a].paired = t.dev[a].connected = true;
    t.dev[b].paired = t.dev[b].connected = true;
    t.dev[c].connected = true;
    /* the audio device in use wins over table order */
    assert(!strcmp(bt_reconnect_pick(&t, "/org/bluez/hci0/dev_BB"), "/org/bluez/hci0/dev_BB"));
    /* no audio device known: first connected + paired */
    assert(!strcmp(bt_reconnect_pick(&t, ""), "/org/bluez/hci0/dev_AA"));
    assert(!strcmp(bt_reconnect_pick(&t, NULL), "/org/bluez/hci0/dev_AA"));
    /* audio_dev not paired / not in the table: fall back */
    assert(!strcmp(bt_reconnect_pick(&t, "/org/bluez/hci0/dev_CC"), "/org/bluez/hci0/dev_AA"));
    assert(!strcmp(bt_reconnect_pick(&t, "/org/bluez/hci0/dev_ZZ"), "/org/bluez/hci0/dev_AA"));
    /* nothing connected + paired: nothing to reconnect (unpaired never) */
    t.dev[a].connected = t.dev[b].connected = false;
    assert(bt_reconnect_pick(&t, NULL) == NULL);
    /* ...but a paired audio device in use still counts (Connected may already be false) */
    assert(!strcmp(bt_reconnect_pick(&t, "/org/bluez/hci0/dev_AA"), "/org/bluez/hci0/dev_AA"));
    /* retry schedule: first waits >= 6 s (bluealsa endpoints), bounded, then gives up */
    assert(bt_reconnect_delay_s(0) >= 6);
    int n = 0, total = 0;
    while (bt_reconnect_delay_s(n) > 0) total += bt_reconnect_delay_s(n++);
    assert(n >= 3 && total <= 60 && bt_reconnect_delay_s(n) == -1 && bt_reconnect_delay_s(-1) == -1);
}

/* Off -> On across a bluetoothd restart (bt_down.sh stops it; On starts a new
 * one = new org.bluez owner), as the 2e.2 device captures show it. */
static void test_rc_restart(void) {
    const char *XM5 = "/org/bluez/hci0/dev_AC_80_0A_46_6D_34";
    bt_devtable_t t; memset(&t, 0, sizeof t);
    int a = bt_dev_upsert(&t, XM5);
    t.dev[a].paired = t.dev[a].connected = true;
    bt_rc_t rc; memset(&rc, 0, sizeof rc);
    assert(bt_rc_powered(&rc, 1) == BT_RC_NONE);   /* boot: adapter up, nothing to reconnect */
    bt_rc_audio(&rc, XM5);                         /* A2DP sink up */

    /* 1. connected at Off: Powered=false, org.bluez gone, table emptied */
    bt_rc_off(&rc, &t, XM5);
    assert(!strcmp(rc.target, XM5));
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE);   /* Powered false */
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE);   /* org.bluez down */
    t.n = 0;
    /* On: new bluetoothd; GetManagedObjects says Powered=false, then
     * PropertiesChanged Powered=true (bt_audio_up's `bluetoothctl power on`) */
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE);
    int d = bt_rc_powered(&rc, 1);
    assert(d == bt_reconnect_delay_s(0) && d > 0);   /* the trigger fires */
    assert(bt_rc_powered(&rc, 1) == BT_RC_NONE);     /* repeated true: no second arm */
    /* attempts fail until bluealsa is up, then succeed */
    assert(bt_rc_result(&rc, XM5, 0) == bt_reconnect_delay_s(1) && rc.tries == 1);
    assert(bt_rc_result(&rc, XM5, 1) == BT_RC_NONE && rc.target[0] == 0);

    /* 2. same, but the new bluetoothd's first GetManagedObjects already says
     * Powered=true (power-on raced the seed): that alone is the edge */
    bt_rc_off(&rc, &t, XM5);
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE);
    assert(bt_rc_powered(&rc, 1) > 0);
    /* Off again before any attempt: replies from the old cycle are dropped
     * while the adapter is off; the next On re-arms */
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE);
    assert(bt_rc_result(&rc, XM5, 0) == BT_RC_NONE && !strcmp(rc.target, XM5));
    assert(bt_rc_powered(&rc, 1) > 0);
    /* every attempt fails: bounded, then one give-up */
    int r, n = 0;
    while ((r = bt_rc_result(&rc, XM5, 0)) > 0) n++;
    assert(r == BT_RC_GIVE_UP && rc.target[0] == 0 && n >= 2);
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) == BT_RC_NONE);   /* nothing left */

    /* 3. second device capture: the headphones were switched off 1.6 s
     * BEFORE Off (Connected=false, audio key cleared) -> nothing connected at
     * Off; the last audio device is still the one to call back */
    t.n = 0; a = bt_dev_upsert(&t, XM5); t.dev[a].paired = true;   /* paired, not connected */
    bt_rc_off(&rc, &t, "");
    assert(!strcmp(rc.target, XM5));
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) > 0);

    /* a stale reply for another path never touches the current target */
    assert(bt_rc_result(&rc, "/org/bluez/hci0/dev_00", 1) == BT_RC_NONE && !strcmp(rc.target, XM5));
    /* no audio device ever, nothing connected: no target, no attempt */
    bt_rc_t z; memset(&z, 0, sizeof z);
    bt_devtable_t e; memset(&e, 0, sizeof e);
    bt_rc_off(&z, &e, NULL);
    assert(z.target[0] == 0 && bt_rc_powered(&z, 1) == BT_RC_NONE);
}

/* The user disconnects (or forgets) a device in the UI: the next Off -> On
 * must NOT call it back -- not as the pending target, not as the last audio
 * device, not even if bluez still lists it connected at Off. Connecting it
 * again in the UI (its A2DP sink comes up) makes it eligible again. */
static void test_rc_drop(void) {
    const char *XM5 = "/org/bluez/hci0/dev_AC_80_0A_46_6D_34";
    const char *OTH = "/org/bluez/hci0/dev_00_11_22_33_44_55";
    bt_devtable_t t; memset(&t, 0, sizeof t);
    int a = bt_dev_upsert(&t, XM5);
    t.dev[a].paired = true;                        /* paired, now disconnected */
    bt_rc_t rc; memset(&rc, 0, sizeof rc);
    assert(bt_rc_powered(&rc, 1) == BT_RC_NONE);
    bt_rc_audio(&rc, XM5);

    /* 1. UI disconnect, then Off -> On: no target, no attempt */
    bt_rc_drop(&rc, XM5);
    assert(rc.last_audio[0] == 0);
    bt_rc_off(&rc, &t, "");
    assert(rc.target[0] == 0);
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) == BT_RC_NONE);

    /* 2. Off before bluez reported the disconnect (still Connected): skipped */
    t.dev[a].connected = true;
    bt_rc_off(&rc, &t, XM5);
    assert(rc.target[0] == 0);
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) == BT_RC_NONE);

    /* 3. reconnected in the UI: eligible again */
    bt_rc_audio(&rc, XM5);
    bt_rc_off(&rc, &t, XM5);
    assert(!strcmp(rc.target, XM5));
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) > 0);

    /* 4. disconnected while retries are pending: target cleared, the in-flight
     * reply is superseded, and the next On has nothing to do */
    assert(bt_rc_result(&rc, XM5, 0) > 0);
    bt_rc_drop(&rc, XM5);
    assert(rc.target[0] == 0 && rc.tries == 0);
    assert(bt_rc_result(&rc, XM5, 0) == BT_RC_NONE);
    t.dev[a].connected = false;
    bt_rc_off(&rc, &t, "");
    assert(rc.target[0] == 0);
    assert(bt_rc_powered(&rc, 0) == BT_RC_NONE && bt_rc_powered(&rc, 1) == BT_RC_NONE);

    /* 5. dropping another device leaves this one's target alone (forget) */
    bt_rc_audio(&rc, XM5);
    t.dev[a].connected = true;
    bt_rc_off(&rc, &t, XM5);
    bt_rc_drop(&rc, OTH);
    assert(!strcmp(rc.target, XM5) && !strcmp(rc.last_audio, XM5));
    bt_rc_drop(&rc, NULL); bt_rc_drop(&rc, "");    /* no-ops */
    assert(!strcmp(rc.target, XM5) && !strcmp(rc.dropped, OTH));
}

int main(void) {
    test_rc_restart();
    test_rc_drop();
    test_reconnect();
    test_listable();
    test_pick_codec();
    test_codec_switch_needed();
    test_devtable();
    test_devtable_full();
    test_dev_battery_init();
    test_drop_carry_on();
    test_pcm_is_a2dp_sink();
    test_route_cmds();
    test_route_want_bt();
    printf("ok\n");
    return 0;
}
