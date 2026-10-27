#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_settings_logic.h"

static void test_parse(void) {
    assert(ui_setting_parse("2", 0, 2, 9) == 2);
    assert(ui_setting_parse("0\n", 0, 2, 9) == 0);
    assert(ui_setting_parse("1 \r\n", 0, 2, 9) == 1);
    assert(ui_setting_parse("3", 0, 2, 9) == 9);     /* above hi */
    assert(ui_setting_parse("-1", 0, 2, 9) == 9);    /* below lo */
    assert(ui_setting_parse("", 0, 2, 9) == 9);
    assert(ui_setting_parse(NULL, 0, 2, 9) == 9);
    assert(ui_setting_parse("1x", 0, 2, 9) == 9);
    assert(ui_setting_parse("x1", 0, 2, 9) == 9);
}

static void test_blkeys(void) {
    /* All: everything acts (today's behaviour, the default) */
    assert(ui_blkeys_volume(UI_BLKEYS_ALL) && ui_blkeys_media(UI_BLKEYS_ALL));
    assert(UI_BLKEYS_DEFAULT == UI_BLKEYS_ALL);
    /* Volume only */
    assert(ui_blkeys_volume(UI_BLKEYS_VOLUME) && !ui_blkeys_media(UI_BLKEYS_VOLUME));
    /* None */
    assert(!ui_blkeys_volume(UI_BLKEYS_NONE) && !ui_blkeys_media(UI_BLKEYS_NONE));
    /* unknown -> default (All) */
    assert(ui_blkeys_volume(7) && ui_blkeys_media(7));
    assert(ui_blkeys_volume(-1) && ui_blkeys_media(-1));
}

static void test_vollimit(void) {
    assert(ui_vollimit_parse("0") == 0);
    assert(ui_vollimit_parse("50") == 50);
    assert(ui_vollimit_parse("90\n") == 90);
    assert(ui_vollimit_parse("55") == UI_VOLLIMIT_OFF);   /* not a choice */
    assert(ui_vollimit_parse("100") == UI_VOLLIMIT_OFF);
    assert(ui_vollimit_parse("") == UI_VOLLIMIT_OFF);
    assert(ui_vollimit_parse("abc") == UI_VOLLIMIT_OFF);
    /* every choice round-trips */
    char b[8];
    for (int i = 0; i < UI_VOLLIMIT_NCHOICES; i++) {
        snprintf(b, sizeof b, "%d", ui_vollimit_choices[i]);
        assert(ui_vollimit_parse(b) == ui_vollimit_choices[i]);
    }
    /* cap */
    assert(ui_vol_cap(76, 0) == 76);      /* off */
    assert(ui_vol_cap(100, 0) == 100);
    assert(ui_vol_cap(101, 0) == 100);
    assert(ui_vol_cap(-3, 0) == 0);
    assert(ui_vol_cap(76, 50) == 50);     /* above the limit -> limit */
    assert(ui_vol_cap(50, 50) == 50);     /* at the limit -> unchanged */
    assert(ui_vol_cap(49, 50) == 49);     /* below -> unchanged */
    assert(ui_vol_cap(-3, 50) == 0);
    assert(ui_vol_cap(95, 90) == 90);
    assert(ui_vol_cap(95, 100) == 95);    /* 100 = no limit */
    assert(ui_vol_cap(95, -5) == 95);     /* garbage limit = no limit */
}

static void test_repeat(void) {
    assert(ui_repeat_from_status(0, 0) == UI_REPEAT_OFF);
    assert(ui_repeat_from_status(1, 0) == UI_REPEAT_ALL);
    assert(ui_repeat_from_status(1, 1) == UI_REPEAT_ONE);
    assert(ui_repeat_from_status(0, 1) == UI_REPEAT_OFF);   /* "stop after song": not ours */
    assert(strstr(ui_repeat_cmd(UI_REPEAT_OFF), "repeat 0\nsingle 0\n"));
    assert(strstr(ui_repeat_cmd(UI_REPEAT_ALL), "repeat 1\nsingle 0\n"));
    assert(strstr(ui_repeat_cmd(UI_REPEAT_ONE), "repeat 1\nsingle 1\n"));
    assert(strstr(ui_repeat_cmd(9), "repeat 0\nsingle 0\n"));  /* unknown -> Off */
    for (int m = 0; m < UI_REPEAT_NCHOICES; m++) {
        const char *c = ui_repeat_cmd(m);
        /* one command list -> one OK, which mpd_cmd waits for */
        assert(strncmp(c, "command_list_begin\n", 19) == 0);
        assert(strcmp(c + strlen(c) - 16, "command_list_end") == 0);
        /* what the command sets reads back as the same mode */
        int rep = strstr(c, "repeat 1") != NULL, sing = strstr(c, "single 1") != NULL;
        assert(ui_repeat_from_status(rep, sing) == m);
    }
}

static void test_btbuttons(void) {
    assert(UI_BTBUTTONS_DEFAULT == 1);                       /* On: today's behaviour */
    assert(strcmp(ui_btbuttons_filter(1, "pause"), "pause") == 0);
    assert(strcmp(ui_btbuttons_filter(1, "next"), "next") == 0);
    assert(ui_btbuttons_filter(0, "pause") == NULL);         /* Off drops the command */
    assert(ui_btbuttons_filter(0, "stop") == NULL);
    assert(ui_btbuttons_filter(1, NULL) == NULL);            /* unmapped key stays ignored */
    assert(ui_btbuttons_filter(0, NULL) == NULL);
    /* persisted file -> mode, same parser as the other On/Off settings */
    assert(ui_setting_parse("0", 0, 1, UI_BTBUTTONS_DEFAULT) == 0);
    assert(ui_setting_parse("1\n", 0, 1, UI_BTBUTTONS_DEFAULT) == 1);
    assert(ui_setting_parse("junk", 0, 1, UI_BTBUTTONS_DEFAULT) == 1);
}

static void test_files(void) {
    assert(ui_volfile_parse("0") == 0);
    assert(ui_volfile_parse("76\n") == 76);
    assert(ui_volfile_parse("100") == 100);
    assert(ui_volfile_parse("101") == -1);
    assert(ui_volfile_parse("-1") == -1);
    assert(ui_volfile_parse("") == -1);
    assert(ui_blfile_parse("39") == 39);
    assert(ui_blfile_parse("255\n") == 255);
    assert(ui_blfile_parse("1") == 1);
    assert(ui_blfile_parse("0") == -1);     /* never boot to a black panel */
    assert(ui_blfile_parse("256") == -1);
    assert(ui_blfile_parse("junk") == -1);
}

int main(void) {
    test_parse();
    test_blkeys();
    test_vollimit();
    test_repeat();
    test_btbuttons();
    test_files();
    printf("settings_logic: OK\n");
    return 0;
}
