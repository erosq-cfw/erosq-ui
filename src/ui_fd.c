#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui_fd.h"

int ui_cloexec_by_path(const char *prefix)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    size_t plen = strlen(prefix);
    int dfd = dirfd(d), n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long fd = strtol(e->d_name, &end, 10);
        if (*end || end == e->d_name || fd < 3 || fd == dfd) continue;
        char tgt[256];
        ssize_t l = readlinkat(dfd, e->d_name, tgt, sizeof tgt - 1);
        if (l < 0) continue;
        tgt[l] = '\0';
        if (strncmp(tgt, prefix, plen) != 0) continue;
        int fl = fcntl((int)fd, F_GETFD);
        if (fl < 0 || (fl & FD_CLOEXEC)) continue;
        if (fcntl((int)fd, F_SETFD, fl | FD_CLOEXEC) == 0) n++;
    }
    closedir(d);
    return n;
}
