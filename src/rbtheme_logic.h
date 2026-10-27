#ifndef EROSQ_RBTHEME_LOGIC_H
#define EROSQ_RBTHEME_LOGIC_H
/* Rockbox theme importer (.cfg -> struct theme_model, theme_logic.h). Our own
 * implementation of the documented .cfg grammar (Rockbox is GPLv2+; nothing
 * is copied). Dependency-free, host-tested in tests/test_rbtheme.c.
 *
 * A theme is unzipped onto the SD root exactly as for Rockbox, so its .cfg is
 * <root>/.rockbox/themes/<name>.cfg and its paths ("/.rockbox/...") resolve
 * against <root>. Keys used: foreground color, background color, line
 * selector start/end/text color, selector type, backdrop. Everything else
 * (font, wps, sbs, iconset, ...) is ignored until a later phase. */
#include "theme_logic.h"

/* Rockbox "selector type" choices, in its own order. */
enum { RBT_SEL_POINTER = 0, RBT_SEL_INVERSE = 1, RBT_SEL_COLOR = 2, RBT_SEL_GRADIENT = 3 };

struct rbt_cfg {             /* 0xRRGGBB colours */
    uint32_t fg, bg, lss, lse, lst;
    int sel;                 /* RBT_SEL_* */
    char backdrop[160];      /* Rockbox path as written; "" = none */
};

/* Parse a whole .cfg (NUL-terminated, modified in place). Missing keys keep
 * Rockbox's own defaults, as a theme applied over a reset Rockbox would.
 * Returns the number of recognised keys (0 = not a colour theme). */
int rbt_parse_cfg(char *buf, struct rbt_cfg *out);

void rbt_model_from_cfg(const struct rbt_cfg *c, const char *root, struct theme_model *m);

/* "/.rockbox/backdrops/x.bmp" -> "<root>/.rockbox/backdrops/x.bmp". Rejects
 * "", "-", relative paths and any ".." (theme files come off the SD card).
 * 0 = ok, -1 = no/invalid path. */
int rbt_resolve_path(const char *rb_path, const char *root, char *out, size_t n);

/* "<name>.cfg" -> name in out; 0 = it is a .cfg, -1 otherwise. */
int rbt_cfg_name(const char *file, char *out, size_t n);

/* Theme names under <root>/.rockbox/themes (unsorted); count. */
int rbt_list(const char *root, char (*names)[THEME_NAME_MAX], int max);

/* <root>/.rockbox/themes/<name>.cfg -> model. 0 = ok, -1 = unreadable/not a theme. */
int rbt_import(const char *root, const char *name, struct theme_model *m);

#endif
