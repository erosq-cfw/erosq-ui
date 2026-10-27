#include <assert.h>
#include <stdio.h>
#include "../src/ui_seek.h"

int main(void) {
    /* step = duration/60, clamped to [5, 60] seconds per wheel detent */
    assert(ui_seek_step(0)    == 5);    /* unknown duration (stream) */
    assert(ui_seek_step(-1)   == 5);
    assert(ui_seek_step(210)  == 5);    /* 3:30 song */
    assert(ui_seek_step(300)  == 5);    /* 5:00 -> exactly 5 */
    assert(ui_seek_step(600)  == 10);   /* 10 min */
    assert(ui_seek_step(1800) == 30);   /* 30 min podcast */
    assert(ui_seek_step(3600) == 60);   /* 1 h */
    assert(ui_seek_step(4*3600) == 60); /* 4 h audiobook: capped */

    /* target = pos + detents*step, clamped to [0, duration-1] */
    assert(ui_seek_target(100, 3, 1800)  == 190);
    assert(ui_seek_target(100, -2, 1800) == 40);
    assert(ui_seek_target(20, -5, 1800)  == 0);      /* clamp at start */
    assert(ui_seek_target(1790, 4, 1800) == 1799);   /* never past the end: seekcur
                                                      * at duration = skip track */
    assert(ui_seek_target(10, 1, 210)    == 15);
    assert(ui_seek_target(1799, 0, 1800) == 1799);
    /* unknown duration: can't seek, stay put */
    assert(ui_seek_target(42, 3, 0)      == 42);
    /* huge spin must not overflow */
    assert(ui_seek_target(100, 2000000000, 1800) == 1799);
    assert(ui_seek_target(100, -2000000000, 1800) == 0);

    printf("seek: ok\n");
    return 0;
}
