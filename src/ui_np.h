#ifndef UI_NP_H
#define UI_NP_H
/* Now Playing text: pure formatting, header-only (host-tested in test_np.c). */
#include <stdio.h>
#include <string.h>

/* "m:ss". Negative (clock skew, bogus elapsed) clamps to 0:00. */
static inline void ui_np_fmt_time(char *buf, size_t n, int secs)
{
    if (secs < 0) secs = 0;
    snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
}

/* Remaining time: "-m:ss" while time is left, "0:00" at the end -- never a
 * negative zero -- and "" (hidden) when the duration is unknown (streams,
 * nothing playing), where a countdown means nothing. */
static inline void ui_np_fmt_remaining(char *buf, size_t n, int el, int du)
{
    if (n == 0) return;
    if (du <= 0) { buf[0] = 0; return; }
    if (el < 0) el = 0;                      /* same clamp as the elapsed label */
    int rem = du > el ? du - el : 0;
    if (rem == 0) { ui_np_fmt_time(buf, n, 0); return; }
    char t[16];
    ui_np_fmt_time(t, sizeof t, rem);
    snprintf(buf, n, "-%s", t);
}

/* What to show as the title: the Title tag, else the file's basename (untagged
 * files are still playing something), else NULL = nothing playing. Either
 * argument may be NULL or empty. Returns a pointer into one of them. */
static inline const char *ui_np_title(const char *title, const char *file)
{
    if (title && *title) return title;
    if (!file || !*file) return NULL;
    const char *s = strrchr(file, '/');
    return s && s[1] ? s + 1 : file;
}
#endif
