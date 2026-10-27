/* Native erosq theme.txt importer (src/nativetheme_logic.c). Host cc, assert only. */
#include "nativetheme_logic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_parse(void)
{
    struct theme_model m;
    char empty[] = "# nothing\n\n";
    assert(nt_parse(empty, "/d", &m) == 0);            /* iPod defaults */
    assert(m.bg == 0xffffff && m.fg == 0 && m.sel0 == 0x2f6fce && m.sel_text == 0xffffff);
    assert(!m.sel_grad && m.backdrop[0] == 0);

    char buf[] =
        "# Gruvbox-ish\r\n"
        "  Background = #1D2021 \r\n"
        "text=ebdbb2\n"
        "selection = #d79921\n"
        "selection_text = #1d2021\n"
        "backdrop = art/bg.bmp\n"
        "unknown = whatever\n"
        "no equals sign\n";
    assert(nt_parse(buf, "/sd/themes/G", &m) == 5);
    assert(m.bg == 0x1d2021 && m.fg == 0xebdbb2 && m.sel0 == 0xd79921 && m.sel1 == 0xd79921);
    assert(m.sel_text == 0x1d2021 && !m.sel_grad);
    assert(!strcmp(m.backdrop, "/sd/themes/G/art/bg.bmp"));

    char grad[] = "selection = 112233\nselection_end = #445566\n";
    assert(nt_parse(grad, "/d", &m) == 2 && m.sel_grad && m.sel0 == 0x112233 && m.sel1 == 0x445566);

    /* malformed values in known keys are an error, not a silent default */
    char bad1[] = "background = #12345\n";       assert(nt_parse(bad1, "/d", &m) == -1);
    char bad2[] = "background = #1234567\n";     assert(nt_parse(bad2, "/d", &m) == -1);
    char bad3[] = "text = red\n";                assert(nt_parse(bad3, "/d", &m) == -1);
    char bad4[] = "backdrop = ../x.bmp\n";       assert(nt_parse(bad4, "/d", &m) == -1);
    char bad5[] = "backdrop = /etc/x.bmp\n";     assert(nt_parse(bad5, "/d", &m) == -1);
    char bad6[] = "backdrop =\n";                assert(nt_parse(bad6, "/d", &m) == -1);
}

static void test_panel_keys(void)
{
    struct theme_model m;
    char none[] = "background = #ffffff\n";
    assert(nt_parse(none, "/d", &m) == 1);
    assert(m.panel_w == 0 && !m.has_titlebar && !m.sprite[THEME_SPR_DEFAULT][0]);

    char buf[] =
        "panel_width = 120\n"                  /* before layout: order-independent */
        "layout = panel\n"
        "selection = #112233\nselection_end = #445566\nselection_text = #eeeeee\n"
        "titlebar = #acdcec\n"
        "sprite = s/def.bmp\nsprite_home = home.bmp\nsprite_music = m.bmp\nsprite_bt = bt.bmp\n";
    assert(nt_parse(buf, "/t", &m) == 10);
    assert(m.panel_w == 120);
    assert(m.has_titlebar && m.titlebar == 0xacdcec);
    assert(m.titlebar_text == 0xeeeeee && m.titlebar_line == 0x445566);   /* unset -> selection's */
    assert(!strcmp(m.sprite[THEME_SPR_DEFAULT], "/t/s/def.bmp"));
    assert(!strcmp(m.sprite[THEME_SPR_HOME], "/t/home.bmp"));
    assert(!strcmp(m.sprite[THEME_SPR_MUSIC], "/t/m.bmp") && !strcmp(m.sprite[THEME_SPR_BT], "/t/bt.bmp"));

    char dflt[] = "layout = panel\n";
    assert(nt_parse(dflt, "/t", &m) == 1 && m.panel_w == THEME_PANEL_DEFAULT);
    char nolayout[] = "panel_width = 120\n";               /* width alone: no panel */
    assert(nt_parse(nolayout, "/t", &m) == 1 && m.panel_w == 0);
    char full[] = "layout = panel\nlayout = full\n";
    assert(nt_parse(full, "/t", &m) == 2 && m.panel_w == 0);

    char b1[] = "layout = sideways\n";     assert(nt_parse(b1, "/t", &m) == -1);
    char b2[] = "panel_width = 79\n";      assert(nt_parse(b2, "/t", &m) == -1);
    char b3[] = "panel_width = 141\n";     assert(nt_parse(b3, "/t", &m) == -1);
    char b4[] = "panel_width = 100px\n";   assert(nt_parse(b4, "/t", &m) == -1);
    char b5[] = "sprite = ../x.bmp\n";     assert(nt_parse(b5, "/t", &m) == -1);
    char b6[] = "titlebar_line = #12\n";   assert(nt_parse(b6, "/t", &m) == -1);
}

static void test_font_keys(void)
{
    struct theme_model m;
    char none[] = "text = #000000\n";
    assert(nt_parse(none, "/t", &m) == 1);
    for (int i = 0; i < THEME_FONT_N; i++) assert(!m.font[i][0]);   /* iPod fonts */

    char one[] = "font = f/po16.bin\n";       /* one key restyles every role */
    assert(nt_parse(one, "/t", &m) == 1);
    for (int i = 0; i < THEME_FONT_N; i++) assert(!strcmp(m.font[i], "/t/f/po16.bin"));

    char two[] = "font_bold = b.bin\nfont = r.bin\n";   /* large follows bold */
    assert(nt_parse(two, "/t", &m) == 2);
    assert(!strcmp(m.font[THEME_FONT_BODY], "/t/r.bin"));
    assert(!strcmp(m.font[THEME_FONT_BOLD], "/t/b.bin") && !strcmp(m.font[THEME_FONT_LARGE], "/t/b.bin"));

    char three[] = "font = r.bin\nfont_large = l.bin\n";  /* bold follows body */
    assert(nt_parse(three, "/t", &m) == 2);
    assert(!strcmp(m.font[THEME_FONT_BOLD], "/t/r.bin") && !strcmp(m.font[THEME_FONT_LARGE], "/t/l.bin"));

    char rows[] = "row_height = 27\nrow_dividers = off\n";
    assert(nt_parse(rows, "/t", &m) == 2 && m.row_h == 27 && m.no_dividers);
    char plain[] = "text = #000000\n";
    assert(nt_parse(plain, "/t", &m) == 1 && m.row_h == 0 && !m.no_dividers);   /* iPod rows */
    char r1[] = "row_height = 15\n";   assert(nt_parse(r1, "/t", &m) == -1);
    char r2[] = "row_height = 65\n";   assert(nt_parse(r2, "/t", &m) == -1);
    char r3[] = "row_height = 27px\n"; assert(nt_parse(r3, "/t", &m) == -1);
    char r4[] = "row_dividers = no\n"; assert(nt_parse(r4, "/t", &m) == -1);

    char b1[] = "font = ../x.bin\n";   assert(nt_parse(b1, "/t", &m) == -1);
    char b2[] = "font_bold = /x.bin\n"; assert(nt_parse(b2, "/t", &m) == -1);
    char b3[] = "font =\n";            assert(nt_parse(b3, "/t", &m) == -1);
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void test_import(void)
{
    char root[] = "/tmp/nativetheme-XXXXXX", p[256];
    assert(mkdtemp(root));
    const char *dirs[] = { "themes", "themes/Dusk", "themes/NoFile", "themes/Bad", "themes/.hidden" };
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof p, "%s/%s", root, dirs[i]); assert(!mkdir(p, 0755)); }
    snprintf(p, sizeof p, "%s/themes/Dusk/theme.txt", root);    put(p, "background = #202030\nbackdrop = bg.bmp\n");
    snprintf(p, sizeof p, "%s/themes/Bad/theme.txt", root);     put(p, "background = nope\n");
    snprintf(p, sizeof p, "%s/themes/.hidden/theme.txt", root); put(p, "text = #ffffff\n");
    snprintf(p, sizeof p, "%s/themes/stray.txt", root);         put(p, "text = #ffffff\n");

    char names[8][THEME_NAME_MAX];
    int n = nt_list(root, names, 8);
    assert(n == 2);                                      /* Dusk + Bad: a theme.txt; not NoFile/.hidden/files */
    assert((!strcmp(names[0], "Dusk") && !strcmp(names[1], "Bad")) ||
           (!strcmp(names[1], "Dusk") && !strcmp(names[0], "Bad")));
    assert(nt_list(root, names, 1) == 1);
    assert(nt_list("/nonexistent-root", names, 8) == 0);

    struct theme_model m;
    assert(nt_import(root, "Dusk", &m) == 0 && m.bg == 0x202030 && m.fg == 0);
    snprintf(p, sizeof p, "%s/themes/Dusk/bg.bmp", root);
    assert(!strcmp(m.backdrop, p));
    assert(nt_import(root, "Bad", &m) == -1);
    assert(nt_import(root, "NoFile", &m) == -1);
    assert(nt_import(root, "../themes/Dusk", &m) == -1);

    const char *files[] = { "themes/Dusk/theme.txt", "themes/Bad/theme.txt", "themes/.hidden/theme.txt", "themes/stray.txt" };
    for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", root, files[i]); unlink(p); }
    for (int i = 4; i >= 0; i--) { snprintf(p, sizeof p, "%s/%s", root, dirs[i]); rmdir(p); }
    rmdir(root);
}

int main(void)
{
    test_parse();
    test_panel_keys();
    test_font_keys();
    test_import();
    puts("ok");
    return 0;
}
