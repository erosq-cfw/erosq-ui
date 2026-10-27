#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui_np.h"

static char b[32];
#define REM(el, du) (ui_np_fmt_remaining(b, sizeof b, (el), (du)), b)
#define TM(s)       (ui_np_fmt_time(b, sizeof b, (s)), b)

int main(void) {
    /* elapsed */
    assert(strcmp(TM(0),    "0:00")  == 0);
    assert(strcmp(TM(65),   "1:05")  == 0);
    assert(strcmp(TM(3725), "62:05") == 0);   /* m:ss, as before */
    assert(strcmp(TM(-3),   "0:00")  == 0);   /* never "-0:03" */

    /* remaining: never "-0:00" */
    assert(strcmp(REM(0, 0),     "")      == 0);   /* nothing playing: hidden */
    assert(strcmp(REM(42, 0),    "")      == 0);   /* stream: unknown duration */
    assert(strcmp(REM(0, -1),    "")      == 0);
    assert(strcmp(REM(0, 210),   "-3:30") == 0);
    assert(strcmp(REM(209, 210), "-0:01") == 0);   /* last second is still time left */
    assert(strcmp(REM(210, 210), "0:00")  == 0);   /* at the end: plain zero */
    assert(strcmp(REM(215, 210), "0:00")  == 0);   /* elapsed overshoot */
    assert(strcmp(REM(-5, 210),  "-3:30") == 0);   /* bogus negative elapsed */
    /* tiny buffers truncate, never overflow */
    char s[3]; ui_np_fmt_remaining(s, sizeof s, 0, 210); assert(strcmp(s, "-3") == 0);
    ui_np_fmt_remaining(s, 0, 0, 210);             /* n=0: no write */

    /* title fallback */
    assert(strcmp(ui_np_title("Song", "a/b.flac"), "Song") == 0);
    assert(strcmp(ui_np_title("", "Artist/Album/03 x.flac"), "03 x.flac") == 0);
    assert(strcmp(ui_np_title(NULL, "x.mp3"), "x.mp3") == 0);
    assert(strcmp(ui_np_title(NULL, "dir/"), "dir/") == 0);  /* no empty basename */
    assert(ui_np_title(NULL, NULL) == NULL);                 /* nothing playing */
    assert(ui_np_title("", "") == NULL);

    printf("np: ok\n");
    return 0;
}
