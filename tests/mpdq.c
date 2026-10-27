/* Minimal MPD AF_UNIX client for probing/verifying protocol behavior on-device.
 * Sends argv[1] (one command line) to /run/mpd.sock, prints the raw reply until
 * OK/ACK. Used to verify `window` pagination support before coding it into the UI. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mpdq <command>\n"); return 2; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/run/mpd.sock", sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); return 1; }
    char cmd[512];
    int n = snprintf(cmd, sizeof cmd, "%s\n", argv[1]);
    if (write(fd, cmd, n) != n) return 1;
    char buf[4096]; int r; int total = 0;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, r, stdout);
        total += r;
        /* stop once we see a terminating line */
        if (r >= 3 && (memmem(buf, r, "\nOK\n", 4) || memmem(buf, r, "\nACK", 4)
                       || !strncmp(buf, "OK\n", 3) || !strncmp(buf, "ACK", 3))) break;
        if (total > 200000) { printf("\n[TRUNCATED at 200KB]\n"); break; }
    }
    close(fd);
    return 0;
}
