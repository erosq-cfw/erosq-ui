#include "ui_mixer.h"

static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

int mixer_pct_to_val(int pct, int lo, int hi)
{
    pct = clampi(pct, 0, 100);
    if (hi <= lo) return lo;
    /* rounded linear interpolation */
    return lo + (pct * (hi - lo) + 50) / 100;
}

int mixer_val_to_pct(int val, int lo, int hi)
{
    if (hi <= lo) return 0;
    val = clampi(val, lo, hi);
    return ((val - lo) * 100 + (hi - lo) / 2) / (hi - lo);
}

#ifndef EROSQ_HOST
#include <tinyalsa/asoundlib.h>
#include "ui_fd.h"
#include <stddef.h>

/* HITL-confirmed on device (2026-06-24): card 0 "Playback Volume", 2 values,
 * range 0..255, current 195. Driver is SOC_DOUBLE_R_TLV(...,0,0xff,1,...) — the
 * invert flag is set, so the ALSA value already means "higher = louder"
 * (control 255 = register 0 = 0dB). Hence VOL_INVERT=0.
 * VOL_HEADROOM=0: 100% = 0dB = the hardware max (correct for a DAP). The M2
 * "full-scale hazard" was about *booting* loud; ui_volume_sync() seeds the
 * overlay from the safe -30dB probe default (control 195 -> ~76%), so we never
 * boot at max. */
#define MIXER_CARD     0
#define MIXER_CTL_NAME "Playback Volume"
#define VOL_HEADROOM   0
#define VOL_INVERT     0

static struct mixer *s_mixer;
static struct mixer_ctl *s_ctl;
static int s_lo, s_hi;            /* effective range (headroom applied) */

int ui_mixer_open(void)
{
    s_mixer = mixer_open(MIXER_CARD);
    if (!s_mixer) return -1;
    /* tinyalsa opens controlC0 without O_CLOEXEC; keep it out of children */
    ui_cloexec_by_path("/dev/snd/");
    s_ctl = mixer_get_ctl_by_name(s_mixer, MIXER_CTL_NAME);
    if (!s_ctl) { mixer_close(s_mixer); s_mixer = NULL; return -1; }
    s_lo = mixer_ctl_get_range_min(s_ctl);
    s_hi = mixer_ctl_get_range_max(s_ctl) - VOL_HEADROOM;
    if (s_hi < s_lo) s_hi = s_lo;
    return 0;
}

int ui_mixer_get_pct(void)
{
    if (!s_ctl) return -1;
    int v = mixer_ctl_get_value(s_ctl, 0);
    if (VOL_INVERT) v = s_lo + s_hi - v;
    return mixer_val_to_pct(v, s_lo, s_hi);
}

void ui_mixer_set_pct(int pct)
{
    if (!s_ctl) return;
    int v = mixer_pct_to_val(pct, s_lo, s_hi);
    if (VOL_INVERT) v = s_lo + s_hi - v;
    int n = (int)mixer_ctl_get_num_values(s_ctl);   /* 2 for L/R */
    for (int i = 0; i < n; i++) mixer_ctl_set_value(s_ctl, i, v);
}
#else
/* host build: hardware unavailable */
int  ui_mixer_open(void)        { return -1; }
int  ui_mixer_get_pct(void)     { return -1; }
void ui_mixer_set_pct(int pct)  { (void)pct; }
#endif

