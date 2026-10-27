#ifndef EROSQ_BT_VOL_H
#define EROSQ_BT_VOL_H
/* Pure AVRCP/bluealsa volume conversions. bluealsa packs both channels into a
 * uint16 (left<<8)|right, each a 7-bit 0..127 level (bit7 = per-channel mute,
 * left clear here). The EROS Q UI uses 0..100. Dependency-free (no LVGL/GLib)
 * so it is unit-testable on the host. Single source of truth for this math. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Look up the saved percent for `key` in a "<key> <pct>\n"-per-line text buffer
 * (see /data/erosq-ui.btvol). Returns 0..100, or -1 if absent/malformed. Last
 * matching line wins (append-to-update is fine). Pure: no I/O, host-testable. */
static inline int bt_vol_lookup(const char *buf, const char *key) {
    if (!buf || !key || !*key) return -1;
    size_t klen = strlen(key);
    int found = -1;
    for (const char *p = buf; *p; ) {
        const char *eol = strchr(p, '\n');
        /* line is "<key> <digits>": key, one space, then a value */
        if (strncmp(p, key, klen) == 0 && p[klen] == ' ' &&
            p[klen + 1] >= '0' && p[klen + 1] <= '9') {   /* require a real digit, not atoi's 0-on-junk */
            int v = atoi(p + klen + 1);
            if (v >= 0 && v <= 100) found = v;   /* last match wins */
        }
        if (!eol) break;
        p = eol + 1;
    }
    return found;
}

static inline int bt_vol_clamp_pct(int pct) {
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
/* 0..100 -> 0..127 channel level (rounded). */
static inline int bt_vol_pct_to_ch(int pct) {
    pct = bt_vol_clamp_pct(pct);
    return (pct * 127 + 50) / 100;
}
/* 0..127 channel level -> 0..100 (rounded; inverse of bt_vol_pct_to_ch). */
static inline int bt_vol_ch_to_pct(int ch) {
    if (ch < 0) ch = 0; else if (ch > 127) ch = 127;
    return (ch * 100 + 63) / 127;
}
/* pack a channel level into bluealsa's dual-channel uint16 (mute bits clear). */
static inline uint16_t bt_vol_pack(int ch) {
    if (ch < 0) ch = 0; else if (ch > 127) ch = 127;
    return (uint16_t)((ch << 8) | ch);
}
/* extract the (left) channel level from bluealsa's packed uint16. */
static inline int bt_vol_unpack_ch(uint16_t v) {
    return (v >> 8) & 0x7f;
}
/* bluealsa packed uint16 -> 0..100 percent. */
static inline int bt_vol_packed_to_pct(uint16_t v) {
    return bt_vol_ch_to_pct(bt_vol_unpack_ch(v));
}
#endif
