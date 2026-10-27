/* Off -> On trigger across a bluetoothd restart, over a real D-Bus daemon.
 *
 * Bluetooth Off runs bt_down.sh, which stops bluetoothd; On starts a NEW one,
 * so org.bluez gets a new unique owner. ui_bt.c subscribes to bluez's
 * PropertiesChanged by the WELL-KNOWN sender name once, at startup, and never
 * re-subscribes. This test makes the same subscription + name watch, then:
 *   owner 1 owns org.bluez, reports Powered=true, exits (bluetoothd stopped);
 *   owner 2 (new unique name) owns org.bluez, reports Powered=false, then true;
 * and checks the subscriber sees owner 2's signals and that feeding them to
 * the reconnect state (bt_rc_powered, as adapter_powered does) arms an attempt.
 *
 * Run inside a private bus: tests/test_bt_owner.sh (dbus-run-session). Built
 * against the device's GLib too and run under qemu-mipsel (see the .sh). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include "ui_bt_logic.h"

#define BLUEZ      "org.bluez"
#define IF_PROPS   "org.freedesktop.DBus.Properties"
#define IF_ADAPTER "org.bluez.Adapter1"
#define ADAPTER    "/org/bluez/hci0"
#define XM5        "/org/bluez/hci0/dev_AC_80_0A_46_6D_34"

static bt_rc_t s_rc;
static int s_seen, s_armed, s_appeared, s_vanished;
static char s_last_sender[64];

static void on_props(GDBusConnection *c, const char *sender, const char *opath,
                     const char *iface, const char *sig, GVariant *params, gpointer u)
{
    (void)c; (void)opath; (void)iface; (void)sig; (void)u;
    const char *ci; GVariant *changed, *inval;
    g_variant_get(params, "(&s@a{sv}@as)", &ci, &changed, &inval);
    gboolean p;
    if (!strcmp(ci, IF_ADAPTER) && g_variant_lookup(changed, "Powered", "b", &p)) {
        s_seen++;
        snprintf(s_last_sender, sizeof s_last_sender, "%s", sender);
        if (bt_rc_powered(&s_rc, p) > 0) s_armed++;
    }
    g_variant_unref(changed); g_variant_unref(inval);
}
static void on_up(GDBusConnection *c, const char *n, const char *o, gpointer u)
{ (void)c; (void)n; (void)o; (void)u; s_appeared++; }
static void on_down(GDBusConnection *c, const char *n, gpointer u)
{ (void)c; (void)n; (void)u; s_vanished++; bt_rc_powered(&s_rc, 0); }   /* = on_bluez_down */

static GDBusConnection *bus(void)
{
    GError *e = NULL;
    const char *addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    assert(addr && "run under dbus-run-session (tests/test_bt_owner.sh)");
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &e);
    if (!c) { fprintf(stderr, "bus: %s\n", e->message); assert(0); }
    return c;
}
static void own(GDBusConnection *c)
{
    GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", BLUEZ, 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    guint32 v; g_variant_get(r, "(u)", &v); g_variant_unref(r);
    assert(v == 1);   /* PRIMARY_OWNER */
}
static void powered(GDBusConnection *c, gboolean p)
{
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "Powered", g_variant_new_boolean(p));
    g_dbus_connection_emit_signal(c, NULL, ADAPTER, IF_PROPS, "PropertiesChanged",
        g_variant_new("(sa{sv}as)", IF_ADAPTER, &b, NULL), NULL);
    g_dbus_connection_flush_sync(c, NULL, NULL);
}
/* Pump the default context (ui_dbus.c's pump) until cond or ~3 s. */
#define PUMP_UNTIL(cond) do { for (int _i = 0; _i < 300 && !(cond); _i++) { \
    while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(10000); } } while (0)

int main(void)
{
    GDBusConnection *ui = bus();
    /* exactly ui_bt_init's subscription: well-known sender, any path */
    g_dbus_connection_signal_subscribe(ui, BLUEZ, IF_PROPS, "PropertiesChanged", NULL, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_props, NULL, NULL);
    g_bus_watch_name_on_connection(ui, BLUEZ, G_BUS_NAME_WATCHER_FLAGS_NONE, on_up, on_down, NULL, NULL);
    PUMP_UNTIL(s_vanished);   /* nobody owns it yet */

    /* boot bluetoothd: up, powered, headphones in use */
    GDBusConnection *b1 = bus();
    own(b1);
    PUMP_UNTIL(s_appeared == 1);
    powered(b1, TRUE);
    PUMP_UNTIL(s_seen == 1);
    assert(s_seen == 1 && s_armed == 0);   /* nothing to reconnect yet */
    char owner1[64]; snprintf(owner1, sizeof owner1, "%s", s_last_sender);
    bt_rc_audio(&s_rc, XM5);

    /* Bluetooth Off: target picked, Powered=false, bluetoothd stopped */
    bt_devtable_t t; memset(&t, 0, sizeof t);
    int a = bt_dev_upsert(&t, XM5); t.dev[a].paired = t.dev[a].connected = true;
    bt_rc_off(&s_rc, &t, XM5);
    assert(!strcmp(s_rc.target, XM5));
    powered(b1, FALSE);
    PUMP_UNTIL(s_seen == 2);
    g_dbus_connection_close_sync(b1, NULL, NULL);
    g_object_unref(b1);
    PUMP_UNTIL(s_vanished == 2);
    assert(s_vanished == 2);

    /* Bluetooth On: a NEW bluetoothd (new unique name) owns org.bluez */
    GDBusConnection *b2 = bus();
    own(b2);
    PUMP_UNTIL(s_appeared == 2);
    assert(s_appeared == 2);
    powered(b2, FALSE);   /* adapter comes up unpowered (AutoEnable=false) */
    PUMP_UNTIL(s_seen == 3);
    assert(s_seen == 3 && s_armed == 0);
    powered(b2, TRUE);    /* bt_audio_up.sh: bluetoothctl power on */
    PUMP_UNTIL(s_seen == 4);
    assert(s_seen == 4);
    assert(strcmp(s_last_sender, owner1) != 0);   /* it really came from the new owner */
    assert(s_armed == 1);                         /* ...and the reconnect trigger fired */
    printf("test_bt_owner: new owner %s (was %s): Powered edge seen, reconnect armed\n",
           s_last_sender, owner1);
    g_dbus_connection_close_sync(b2, NULL, NULL);
    g_object_unref(b2);
    g_object_unref(ui);
    return 0;
}
