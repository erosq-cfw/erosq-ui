#include "rbtheme_logic.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Rockbox reads the first six hex digits and ignores the rest. */
static int hex6(const char *s, uint32_t *out)
{
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i])) return -1;
    char tmp[7];
    memcpy(tmp, s, 6); tmp[6] = 0;
    *out = (uint32_t)strtoul(tmp, NULL, 16);
    return 0;
}

static const char *const sel_names[] = {
    "pointer", "bar (inverse)", "bar (color)", "bar (gradient)"
};

int rbt_parse_cfg(char *buf, struct rbt_cfg *c)
{
    /* Rockbox's DEFAULT_THEME_* values */
    c->fg = 0xcecfce; c->bg = 0x000000;
    c->lss = 0xffeb9c; c->lse = 0xb58e00; c->lst = 0x000000;
    c->sel = RBT_SEL_GRADIENT;
    c->backdrop[0] = 0;

    int n = 0;
    for (char *line = buf, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        if (*line == '#') continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = trim(line), *val = trim(colon + 1);

        uint32_t *col = !strcasecmp(key, "foreground color")         ? &c->fg
                      : !strcasecmp(key, "background color")         ? &c->bg
                      : !strcasecmp(key, "line selector start color") ? &c->lss
                      : !strcasecmp(key, "line selector end color")   ? &c->lse
                      : !strcasecmp(key, "line selector text color")  ? &c->lst
                      : NULL;
        if (col) {
            if (!hex6(val, col)) n++;
        } else if (!strcasecmp(key, "selector type")) {
            for (int i = 0; i < 4; i++)
                if (!strcasecmp(val, sel_names[i])) { c->sel = i; n++; }
        } else if (!strcasecmp(key, "backdrop")) {
            if (strlen(val) < sizeof c->backdrop) { strcpy(c->backdrop, val); n++; }
        } else if (!strcasecmp(key, "iconset") || !strcasecmp(key, "viewers iconset")) {
            /* Rockbox's stock rockbox_default_icons.cfg carries only these.
             * No icons here yet, so it applies as the Rockbox default colours
             * instead of failing with "Couldn't read theme". */
            n++;
        }
    }
    return n;
}

void rbt_model_from_cfg(const struct rbt_cfg *c, const char *root, struct theme_model *m)
{
    memset(m, 0, sizeof *m);   /* no panel, sprites or title-bar colours in .cfg */
    m->bg = c->bg;
    m->fg = c->fg;
    switch (c->sel) {
    case RBT_SEL_COLOR:
        m->sel0 = m->sel1 = c->lss; m->sel_text = c->lst; m->sel_grad = 0; break;
    case RBT_SEL_GRADIENT:
        m->sel0 = c->lss; m->sel1 = c->lse; m->sel_text = c->lst; m->sel_grad = 1; break;
    default:
        /* ponytail: "pointer" draws Rockbox's > cursor icon; shown as an
         * inverse bar until icons come in a later phase. */
        m->sel0 = m->sel1 = c->fg; m->sel_text = c->bg; m->sel_grad = 0; break;
    }
    if (rbt_resolve_path(c->backdrop, root, m->backdrop, sizeof m->backdrop))
        m->backdrop[0] = 0;
}

int rbt_resolve_path(const char *rb, const char *root, char *out, size_t n)
{
    if (!rb || rb[0] != '/' || strstr(rb, "..")) return -1;
    int len = snprintf(out, n, "%s%s", root, rb);
    return len < 0 || (size_t)len >= n ? -1 : 0;
}

int rbt_cfg_name(const char *file, char *out, size_t n)
{
    size_t len = strlen(file);
    if (len <= 4 || strcasecmp(file + len - 4, ".cfg") || len - 4 >= n) return -1;
    memcpy(out, file, len - 4);
    out[len - 4] = 0;
    return theme_name_ok(out) ? 0 : -1;
}

int rbt_list(const char *root, char (*names)[THEME_NAME_MAX], int max)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s/.rockbox/themes", root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; n < max && (e = readdir(d)); )
        if (!rbt_cfg_name(e->d_name, names[n], THEME_NAME_MAX)) n++;
    closedir(d);
    return n;
}

int rbt_import(const char *root, const char *name, struct theme_model *m)
{
    if (!theme_name_ok(name)) return -1;
    char path[256], buf[8192];
    snprintf(path, sizeof path, "%s/.rockbox/themes/%s.cfg", root, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, sizeof buf - 1);   /* ponytail: cfgs are ~1 KB; keys past 8 KB are ignored */
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    struct rbt_cfg c;
    if (rbt_parse_cfg(buf, &c) == 0) return -1;
    rbt_model_from_cfg(&c, root, m);
    return 0;
}
