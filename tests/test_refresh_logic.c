#include <assert.h>
#include <stdio.h>
#include "ui_refresh_logic.h"

static void test_period(void) {
    assert(ui_refresh_period_ms(UI_REFRESH_SMOOTH) == 14);
    assert(ui_refresh_period_ms(UI_REFRESH_STANDARD) == 16);
    /* unknown -> default (Smooth) */
    assert(ui_refresh_period_ms(-1) == 14);
    assert(ui_refresh_period_ms(2) == 14);
    assert(ui_refresh_period_ms(UI_REFRESH_DEFAULT) == 14);
}

static void test_parse(void) {
    assert(ui_refresh_parse("0") == UI_REFRESH_SMOOTH);
    assert(ui_refresh_parse("1") == UI_REFRESH_STANDARD);
    assert(ui_refresh_parse("1\n") == UI_REFRESH_STANDARD);
    assert(ui_refresh_parse("0 \r\n") == UI_REFRESH_SMOOTH);
    /* garbage / out of range / empty -> default */
    assert(ui_refresh_parse("") == UI_REFRESH_DEFAULT);
    assert(ui_refresh_parse(NULL) == UI_REFRESH_DEFAULT);
    assert(ui_refresh_parse("2") == UI_REFRESH_DEFAULT);
    assert(ui_refresh_parse("-1") == UI_REFRESH_DEFAULT);
    assert(ui_refresh_parse("16") == UI_REFRESH_DEFAULT);   /* a period, not a mode */
    assert(ui_refresh_parse("1x") == UI_REFRESH_DEFAULT);
    assert(ui_refresh_parse("x1") == UI_REFRESH_DEFAULT);
}

static void test_roundtrip(void) {
    /* what set() writes ("%d") must parse back to the same mode */
    char buf[4];
    for (int m = 0; m < UI_REFRESH_NCHOICES; m++) {
        snprintf(buf, sizeof buf, "%d", m);
        assert(ui_refresh_parse(buf) == m);
        assert(ui_refresh_valid(m));
    }
    assert(!ui_refresh_valid(UI_REFRESH_NCHOICES));
}

int main(void) {
    test_period();
    test_parse();
    test_roundtrip();
    printf("refresh_logic: OK\n");
    return 0;
}
