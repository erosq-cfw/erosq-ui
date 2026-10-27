#include "nativetheme_logic.h"
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

/* "#rrggbb" or "rrggbb", nothing else. */
static int hex(const char *s, uint32_t *out)
{
    if (*s == '#') s++;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return -1;
        v = v << 4 | (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10));
    }
    if (s[6]) return -1;
    *out = v;
    return 0;
}

int nt_parse(char *buf, const char *dir, struct theme_model *m)
{
    memset(m, 0, sizeof *m);
    /* the iPod look's flat colours */
    m->bg = 0xffffff; m->fg = 0x000000;
    m->sel0 = m->sel1 = 0x2f6fce; m->sel_text = 0xffffff; m->sel_grad = 0;
    m->backdrop[0] = 0;

    int n = 0, has_end = 0, has_title = 0, panel_w = 0;
    uint32_t end = 0;
    for (char *line = buf, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);

        uint32_t *col = !strcasecmp(key, "background")     ? &m->bg
                      : !strcasecmp(key, "text")           ? &m->fg
                      : !strcasecmp(key, "selection")      ? &m->sel0
                      : !strcasecmp(key, "selection_end")  ? &end
                      : !strcasecmp(key, "selection_text") ? &m->sel_text
                      : !strcasecmp(key, "titlebar")       ? &m->titlebar
                      : !strcasecmp(key, "titlebar_text")  ? &m->titlebar_text
                      : !strcasecmp(key, "titlebar_line")  ? &m->titlebar_line
                      : NULL;
        char *file = !strcasecmp(key, "backdrop")     ? m->backdrop
                   : !strcasecmp(key, "sprite")       ? m->sprite[THEME_SPR_DEFAULT]
                   : !strcasecmp(key, "sprite_home")  ? m->sprite[THEME_SPR_HOME]
                   : !strcasecmp(key, "sprite_music") ? m->sprite[THEME_SPR_MUSIC]
                   : !strcasecmp(key, "sprite_bt")    ? m->sprite[THEME_SPR_BT]
                   : !strcasecmp(key, "font")         ? m->font[THEME_FONT_BODY]
                   : !strcasecmp(key, "font_bold")    ? m->font[THEME_FONT_BOLD]
                   : !strcasecmp(key, "font_large")   ? m->font[THEME_FONT_LARGE]
                   : NULL;
        if (col) {
            if (hex(val, col)) return -1;
            if (col == &end) has_end = 1;
            if (col == &m->titlebar) has_title |= 1;
            if (col == &m->titlebar_text) has_title |= 2;
            if (col == &m->titlebar_line) has_title |= 4;
            n++;
        } else if (file) {
            /* a file in the theme folder (or below it); never outside it */
            if (!*val || val[0] == '/' || strstr(val, "..")) return -1;
            int len = snprintf(file, 256, "%s/%s", dir, val);
            if (len < 0 || len >= 256) return -1;
            n++;
        } else if (!strcasecmp(key, "layout")) {
            if (!strcasecmp(val, "panel")) { if (!m->panel_w) m->panel_w = THEME_PANEL_DEFAULT; }
            else if (!strcasecmp(val, "full")) m->panel_w = 0;
            else return -1;
            n++;
        } else if (!strcasecmp(key, "row_height")) {
            char *e;
            long v = strtol(val, &e, 10);
            if (e == val || *e || v < THEME_ROW_MIN || v > THEME_ROW_MAX) return -1;
            m->row_h = (int)v;
            n++;
        } else if (!strcasecmp(key, "row_dividers")) {
            if (!strcasecmp(val, "off")) m->no_dividers = 1;
            else if (!strcasecmp(val, "on")) m->no_dividers = 0;
            else return -1;
            n++;
        } else if (!strcasecmp(key, "panel_width")) {
            char *e;
            long v = strtol(val, &e, 10);
            if (e == val || *e || v < THEME_PANEL_MIN || v > THEME_PANEL_MAX) return -1;
            panel_w = (int)v;
            n++;
        }
    }
    m->sel1 = has_end ? end : m->sel0;
    m->sel_grad = has_end;
    /* title bar: any titlebar* key switches it to its own flat colours; the
     * unset ones default to the selection's (text, line = selection_end) */
    if (has_title) {
        m->has_titlebar = 1;
        if (!(has_title & 1)) m->titlebar = m->sel0;
        if (!(has_title & 2)) m->titlebar_text = m->sel_text;
        if (!(has_title & 4)) m->titlebar_line = m->sel1;
    }
    if (m->panel_w && panel_w) m->panel_w = panel_w;   /* panel_width only with layout = panel */
    for (int i = THEME_FONT_BOLD; i < THEME_FONT_N; i++)
        if (!m->font[i][0]) memcpy(m->font[i], m->font[i - 1], sizeof m->font[i]);
    return n;
}

int nt_list(const char *root, char (*names)[THEME_NAME_MAX], int max)
{
    char dir[256], p[600];
    snprintf(dir, sizeof dir, "%s/themes", root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; n < max && (e = readdir(d)); ) {
        if (!theme_name_ok(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s/theme.txt", dir, e->d_name);
        if (access(p, R_OK)) continue;
        strcpy(names[n++], e->d_name);   /* theme_name_ok: < THEME_NAME_MAX */
    }
    closedir(d);
    return n;
}

int nt_import(const char *root, const char *name, struct theme_model *m)
{
    if (!theme_name_ok(name)) return -1;
    char dir[256], path[300], buf[4096];
    snprintf(dir, sizeof dir, "%s/themes/%s", root, name);
    snprintf(path, sizeof path, "%s/theme.txt", dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, sizeof buf - 1);   /* ponytail: a theme.txt is a few lines */
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return nt_parse(buf, dir, m) > 0 ? 0 : -1;
}
