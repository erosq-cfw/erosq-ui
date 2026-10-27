#include <assert.h>
#include <stdio.h>
#include "../src/bt_vol.h"

int main(void) {
    /* boundaries */
    assert(bt_vol_pct_to_ch(0)   == 0);
    assert(bt_vol_pct_to_ch(100) == 127);
    assert(bt_vol_ch_to_pct(0)   == 0);
    assert(bt_vol_ch_to_pct(127) == 100);

    /* clamping */
    assert(bt_vol_pct_to_ch(-5)  == 0);
    assert(bt_vol_pct_to_ch(200) == 127);

    /* HITL-observed live values (regression anchors, MEGABOOM 2026-07-04) */
    assert(bt_vol_unpack_ch(10023) == 39);       /* 0x2727 */
    assert(bt_vol_unpack_ch(14135) == 55);       /* 0x3737 */
    assert(bt_vol_packed_to_pct(10023) == 31);   /* 39/127 -> 31% */
    assert(bt_vol_packed_to_pct(14135) == 43);   /* 55/127 -> 43% */

    /* unpack strips bluealsa's per-channel mute bit (bit 7): 0xA7 -> 0x27 */
    assert(bt_vol_unpack_ch(0xA727) == 39);

    /* pack clamps out-of-range channel levels before packing */
    assert(bt_vol_pack(-5)  == 0x0000);
    assert(bt_vol_pack(200) == 0x7f7f);   /* 127 in both channels */

    /* pack/unpack is exact for every channel level */
    for (int ch = 0; ch <= 127; ch++)
        assert(bt_vol_unpack_ch(bt_vol_pack(ch)) == ch);

    /* UI percent round-trips exactly through the channel scale (no drift) */
    for (int p = 0; p <= 100; p++)
        assert(bt_vol_ch_to_pct(bt_vol_pct_to_ch(p)) == p);

    /* documents the known one-way artifact: distinct speaker levels can collapse
     * to the same displayed percent (harmless — we never push the percent back) */
    assert(bt_vol_ch_to_pct(54) == bt_vol_ch_to_pct(55));  /* both 43 */

    /* per-device volume lookup (/data/erosq-ui.btvol) */
    const char *db =
        "/org/bluez/hci0/dev_AA_BB 30\n"
        "/org/bluez/hci0/dev_CC_DD 75\n";
    assert(bt_vol_lookup(db, "/org/bluez/hci0/dev_AA_BB") == 30);   /* headphones */
    assert(bt_vol_lookup(db, "/org/bluez/hci0/dev_CC_DD") == 75);   /* speaker */
    assert(bt_vol_lookup(db, "/org/bluez/hci0/dev_EE_FF") == -1);   /* unseen -> caller defaults */
    assert(bt_vol_lookup("", "/org/bluez/hci0/dev_AA_BB") == -1);   /* empty db */
    assert(bt_vol_lookup(db, "") == -1);                            /* no key */
    /* last line wins (append = update); malformed/out-of-range ignored */
    assert(bt_vol_lookup("k 20\nk 45\n", "k") == 45);
    assert(bt_vol_lookup("k 200\n", "k") == -1);
    /* non-numeric value must be rejected, not atoi'd to a valid 0 (mute) */
    assert(bt_vol_lookup("k garbage\n", "k") == -1);
    assert(bt_vol_lookup("k 40\nk oops\n", "k") == 40);  /* junk update ignored, prior kept */
    /* prefix must not partial-match a longer key */
    assert(bt_vol_lookup("dev_AA_BBB 90\n", "dev_AA_BB") == -1);

    printf("bt_vol: all assertions passed\n");
    return 0;
}
