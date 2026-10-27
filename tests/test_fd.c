/* ui_cloexec_by_path: marks only fds whose target matches the prefix, and an
 * exec'd child really doesn't see a marked fd. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ui_fd.h"

static int cloexec(int fd) { return (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0; }

int main(void)
{
    char tmp[] = "/tmp/test_fd_XXXXXX";
    int other = mkstemp(tmp);
    int a = open("/dev/null", O_RDONLY), b = open("/dev/zero", O_RDONLY);
    assert(other >= 3 && a >= 3 && b >= 3);
    assert(!cloexec(a) && !cloexec(b) && !cloexec(other));

    assert(ui_cloexec_by_path("/dev/null") == 1);
    assert(cloexec(a) && !cloexec(b) && !cloexec(other));
    assert(ui_cloexec_by_path("/dev/null") == 0);          /* idempotent */
    assert(ui_cloexec_by_path("/dev/") == 1);              /* now /dev/zero */
    assert(cloexec(b) && !cloexec(other));
    assert(ui_cloexec_by_path("/nonexistent/") == 0);

    /* exec'd child: marked fds are closed, the unmarked one is inherited */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "test -e /proc/self/fd/%d || test -e /proc/self/fd/%d && exit 1; "
             "test -e /proc/self/fd/%d", a, b, other);
    int st = system(cmd);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    unlink(tmp);
    puts("ok");
    return 0;
}
