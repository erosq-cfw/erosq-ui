#ifndef UI_MIXER_H
#define UI_MIXER_H

/* Pure mapping helpers (no hardware) — testable on host. */
int mixer_pct_to_val(int pct, int lo, int hi);   /* 0..100 -> [lo,hi], clamped+rounded */
int mixer_val_to_pct(int val, int lo, int hi);    /* [lo,hi] -> 0..100, clamped */

/* Hardware (device only; compiled out under EROSQ_HOST). */
int  ui_mixer_open(void);          /* 0 on success, -1 if card/control absent */
int  ui_mixer_get_pct(void);       /* current volume 0..100, or -1 if unavailable */
void ui_mixer_set_pct(int pct);    /* clamp 0..100, apply headroom+polarity, write both channels */

#endif
