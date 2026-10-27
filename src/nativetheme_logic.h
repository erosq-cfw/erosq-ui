#ifndef EROSQ_NATIVETHEME_LOGIC_H
#define EROSQ_NATIVETHEME_LOGIC_H
/* Native erosq theme importer (theme.txt -> struct theme_model, theme_logic.h).
 * Format: THEMES.md. One folder per theme, <root>/themes/<name>/theme.txt:
 *
 *   # comment
 *   background     = #1d2021
 *   text           = #ebdbb2
 *   selection      = #d79921     (title bar + highlighted row)
 *   selection_end  = #b57614     (optional: vertical gradient to this)
 *   selection_text = #1d2021
 *   backdrop       = backdrop.bmp (optional, in the theme folder)
 *
 * Missing keys keep the iPod look's colours. Dependency-free, host-tested in
 * tests/test_rbtheme.c. */
#include "theme_logic.h"

/* Parse a whole theme.txt (NUL-terminated, modified in place); dir = the theme
 * folder, for the backdrop. Returns the number of recognised keys (0 = not a
 * theme) or -1 on a malformed recognised value. */
int nt_parse(char *buf, const char *dir, struct theme_model *m);

/* Folders under <root>/themes that hold a theme.txt (unsorted); count. */
int nt_list(const char *root, char (*names)[THEME_NAME_MAX], int max);

/* <root>/themes/<name>/theme.txt -> model. 0 = ok, -1 = unreadable/not a theme. */
int nt_import(const char *root, const char *name, struct theme_model *m);

#endif
