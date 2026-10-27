#ifndef UI_SEEK_H
#define UI_SEEK_H
/* Now Playing wheel scrub: pure maths, header-only (host-tested in test_seek.c).
 * One wheel detent moves duration/60 seconds, clamped to [5, 60]: ~5s on a
 * song, 30s on a 30-min podcast, 60s on anything an hour or longer. */

static inline int ui_seek_step(int duration)
{
    int s = duration / 60;
    return s < 5 ? 5 : s > 60 ? 60 : s;
}

/* Target after `detents` wheel clicks from `pos`. Clamped to [0, duration-1]:
 * `seekcur <duration>` would end the track and skip. duration<=0 (streams,
 * unknown) can't seek, so the position stays put. 64-bit so a runaway detent
 * count can't overflow. */
static inline int ui_seek_target(int pos, int detents, int duration)
{
    if (duration <= 0) return pos;
    long long t = (long long)pos + (long long)detents * ui_seek_step(duration);
    return t < 0 ? 0 : t > duration - 1 ? duration - 1 : (int)t;
}
#endif
