#ifndef UI_FD_H
#define UI_FD_H

/* Mark every open fd whose /proc/self/fd target starts with `prefix`
 * close-on-exec. For fds a library opened without O_CLOEXEC (tinyalsa's
 * /dev/snd/controlC0), so system()/fork+exec children (udhcpc, wifi_up.sh, ...)
 * don't inherit them. Our own opens use O_CLOEXEC directly. Returns the number
 * of fds newly marked, or -1 if /proc/self/fd can't be read. */
int ui_cloexec_by_path(const char *prefix);

#endif
