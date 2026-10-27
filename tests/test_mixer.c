#define EROSQ_HOST 1   /* compile ui_mixer.c without the tinyalsa hardware path */
#include <assert.h>
#include "ui_mixer.h"

int main(void)
{
    /* linear map across a 0..255 range */
    assert(mixer_pct_to_val(0,   0, 255) == 0);
    assert(mixer_pct_to_val(100, 0, 255) == 255);
    assert(mixer_pct_to_val(50,  0, 255) == 128);   /* rounded */
    /* clamp out-of-range percent */
    assert(mixer_pct_to_val(-10, 0, 255) == 0);
    assert(mixer_pct_to_val(150, 0, 255) == 255);
    /* non-zero lo */
    assert(mixer_pct_to_val(0,   40, 240) == 40);
    assert(mixer_pct_to_val(100, 40, 240) == 240);
    /* inverse round-trips at the ends, clamps in the middle */
    assert(mixer_val_to_pct(0,   0, 255) == 0);
    assert(mixer_val_to_pct(255, 0, 255) == 100);
    assert(mixer_val_to_pct(128, 0, 255) == 50);
    assert(mixer_val_to_pct(-5,  0, 255) == 0);
    assert(mixer_val_to_pct(999, 0, 255) == 100);
    /* degenerate range never divides by zero */
    assert(mixer_pct_to_val(50, 7, 7) == 7);
    assert(mixer_val_to_pct(7, 7, 7) == 0);
    return 0;
}
