/* Host test for ui_input_idle() (src/ui_wake_logic.h): when the main loop may
 * pause the indev read timers and block in poll(), awake or blanked.
 * Pure, dep-free (run by tests/run.sh). */
#include <assert.h>
#include <stdio.h>
#include "ui_wake_logic.h"

int main(void)
{
    /* nothing held: sleep on the fds (static Now Playing, idle menus, blanked) */
    assert(ui_input_idle(0, 0, 0) == 1);
    /* POWER held: hold-to-shutdown needs the 1.5 s check on periodic reads */
    assert(ui_input_idle(1, 0, 0) == 0);
    /* a keypad key held: LVGL long-press (LONG_PRESSED rows) and key repeat
     * (held volume keys) only advance on periodic reads */
    assert(ui_input_idle(0, 1, 0) == 0);
    /* a BT pad button / d-pad held: same, via the pad indev */
    assert(ui_input_idle(0, 0, 1) == 0);
    /* any combination held: never sleep */
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 2; k++)
            for (int g = 0; g < 2; g++)
                assert(ui_input_idle(p, k, g) == (!p && !k && !g));
    printf("input_idle ok\n");
    return 0;
}
