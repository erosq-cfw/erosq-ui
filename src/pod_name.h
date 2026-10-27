#ifndef EROSQ_POD_NAME_H
#define EROSQ_POD_NAME_H
/* The single filename rule, shared by podindex (writes names) and erosq-ui
 * (reads them back off a removable card). Header-only so both binaries get the
 * same rule without linking each other's objects -- same approach as bt_vol.h.
 *
 * The load-bearing invariant: '/' is not in the whitelist, so a name that
 * passes can never escape its directory. '"' and '\' are also excluded, which
 * is what makes an absolute path safe to interpolate into MPD's `add "..."`. */

static inline int pod_name_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static inline int pod_name_ok(const char *s)
{
    if (!s || !*s || *s == '.') return 0;   /* empty, ".", "..", ".hidden" */
    for (; *s; s++) if (!pod_name_char_ok(*s)) return 0;
    return 1;
}

/* The single feed-DIRECTORY-name rule. Distinct from pod_name_ok() above (which
 * validates an already-derived FILE name) because a feed directory is DERIVED
 * from a human-written podcasts.txt line and may contain spaces.
 *
 * ============================ MUST STAY IN SYNC =============================
 * podfetch.sh implements the identical rule in shell (it creates the directory;
 * this creates the path the UI reads back). If they disagree by even one byte
 * the UI reads a directory the script never writes: the feed shows
 * "(not indexed)" forever and every wanted.txt lands where nothing looks.
 *
 *   1. keep only bytes in [A-Za-z0-9._-] plus space   (tr -cd 'A-Za-z0-9 ._-')
 *   2. truncate to outlen-1 bytes (63 for UP_NAME_MAX) (cut -b1-63)
 *   3. strip TRAILING spaces -- after truncation, not before (sed -e 's/ *$//')
 *   4. reject empty, and reject an all-dots name: ".." is whitelisted by rule 1
 *      but resolves to the PARENT directory, which would put index.tsv straight
 *      into /media/sd. Neither side invents a fallback name; both skip the feed,
 *      so an unusable name can never make the two disagree about where it lives.
 *
 * tests/test_pod_dirname_agree.sh runs this function and the real podfetch.sh
 * over the same podcasts.txt and diffs the resulting names, including the
 * 63/64-byte boundary. Change one side and that test fails.
 * ============================================================================
 *
 * Returns the length written, or 0 if the name is unusable (out is set empty). */
static inline int pod_dir_name(const char *in, size_t inlen, char *out, size_t outlen)
{
    size_t o = 0, d = 0;
    if (!out || outlen == 0) return 0;
    for (size_t i = 0; i < inlen && o + 1 < outlen; i++) {
        char c = in[i];
        if (pod_name_char_ok(c) || c == ' ') out[o++] = c;
    }
    while (o && out[o - 1] == ' ') o--;      /* trailing spaces, post-truncation */
    out[o] = 0;
    while (d < o && out[d] == '.') d++;
    if (o == 0 || d == o) { out[0] = 0; return 0; }
    return (int)o;
}
#endif
