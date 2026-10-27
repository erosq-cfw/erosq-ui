/* Deterministic scroll-wheel event injector for HW responsiveness A/B tests.
 * Writes N encoder detents to the scrollwheel evdev so erosq-ui's CPU cost per
 * focus-move can be measured under different render modes. Hand-rolled 16-byte
 * event record = the 32-bit-MIPS kernel's input_event_size() exactly (avoids
 * musl's 64-bit-time_t timeval, which would make write() -EINVAL). Time field is
 * ignored on write (the input core timestamps). Usage: scrollinject [n] [dir] [us] */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

struct ie { unsigned int sec, usec; unsigned short type, code; int value; };
#define EV_SYN 0
#define EV_REL 2
#define REL_X  0
#define SYN_REPORT 0

static int emit(int fd, unsigned short t, unsigned short c, int v) {
    struct ie e; memset(&e, 0, sizeof e);
    e.type = t; e.code = c; e.value = v;
    return write(fd, &e, sizeof e) == (long)sizeof e ? 0 : -1;
}

int main(int argc, char **argv) {
    int n   = argc > 1 ? atoi(argv[1]) : 100;
    int dir = argc > 2 ? atoi(argv[2]) : 1;
    int us  = argc > 3 ? atoi(argv[3]) : 30000;
    int fd = open("/dev/input/event0", O_WRONLY);
    if (fd < 0) return 1;
    for (int i = 0; i < n; i++) {
        if (emit(fd, EV_REL, REL_X, dir)) return 2;
        if (emit(fd, EV_SYN, SYN_REPORT, 0)) return 3;
        usleep(us);
    }
    return 0;
}
