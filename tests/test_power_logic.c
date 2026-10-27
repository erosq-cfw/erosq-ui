#include <assert.h>
#include <stdio.h>
#include "ui_power_logic.h"

static void test_should_power_off(void) {
    /* disabled: never fires, regardless of idle/playing */
    assert(autooff_should_power_off(0, 99u*60000u, 0) == 0);
    /* negative minutes also disabled (defensive: set() clamps, but guard it) */
    assert(autooff_should_power_off(-1, 99u*60000u, 0) == 0);
    /* below threshold */
    assert(autooff_should_power_off(5, 4u*60000u, 0) == 0);
    /* exactly at threshold, not playing -> fire */
    assert(autooff_should_power_off(5, 5u*60000u, 0) == 1);
    /* one ms under threshold */
    assert(autooff_should_power_off(5, 5u*60000u - 1u, 0) == 0);
    /* well past threshold but music playing -> blocked */
    assert(autooff_should_power_off(5, 60u*60000u, 1) == 0);
    /* 60-min choice does not overflow uint32 */
    assert(autooff_should_power_off(60, 60u*60000u, 0) == 1);
}

static void test_should_blank(void) {
    /* disabled (Off) never blanks, regardless of idle */
    assert(backlight_should_blank(0, 99u*1000u) == 0);
    /* negative timeout disabled (defensive) */
    assert(backlight_should_blank(-1, 99u*1000u) == 0);
    /* below threshold */
    assert(backlight_should_blank(30, 29u*1000u) == 0);
    /* exactly at threshold -> blank */
    assert(backlight_should_blank(30, 30u*1000u) == 1);
    /* one ms under */
    assert(backlight_should_blank(15, 15u*1000u - 1u) == 0);
    /* longest choice (5 min) does not overflow uint32 */
    assert(backlight_should_blank(300, 300u*1000u) == 1);
}

static void test_should_wake(void) {
    /* mode 2 (Any button): always wakes, any key, either blank origin */
    assert(backlight_should_wake(2, 0, 0) == 1);
    assert(backlight_should_wake(2, 1, 0) == 1);
    assert(backlight_should_wake(2, 0, 1) == 1);
    /* mode 0 (Power only): wakes only on POWER, regardless of origin */
    assert(backlight_should_wake(0, 0, 0) == 0);
    assert(backlight_should_wake(0, 1, 0) == 0);
    assert(backlight_should_wake(0, 0, 1) == 1);
    assert(backlight_should_wake(0, 1, 1) == 1);
    /* mode 1 (Auto): auto-blank wakes on anything; manual wakes only on POWER */
    assert(backlight_should_wake(1, 1, 0) == 1);   /* auto blank, non-power -> wake */
    assert(backlight_should_wake(1, 1, 1) == 1);   /* auto blank, power     -> wake */
    assert(backlight_should_wake(1, 0, 0) == 0);   /* manual blank, non-power -> no */
    assert(backlight_should_wake(1, 0, 1) == 1);   /* manual blank, power     -> wake */
    /* unknown mode treated as Auto (default) */
    assert(backlight_should_wake(7, 1, 0) == 1);
    assert(backlight_should_wake(7, 0, 0) == 0);
}

static void test_critbatt(void) {
    int n = 0;
    /* debounce: first low read never fires, second does */
    assert(critbatt_should_power_off(5, 5, 1, &n) == 0 && n == 1);
    assert(critbatt_should_power_off(5, 4, 1, &n) == 1);
    /* stays latched-high while still low (counter saturates, no overflow) */
    for (int i = 0; i < 1000; i++) assert(critbatt_should_power_off(5, 4, 1, &n) == 1);
    assert(n == CRITBATT_READS);
    /* a single dip between good reads never fires (OC-game load sag) */
    n = 0;
    assert(critbatt_should_power_off(10, 9, 1, &n) == 0);
    assert(critbatt_should_power_off(10, 11, 1, &n) == 0 && n == 0);
    assert(critbatt_should_power_off(10, 9, 1, &n) == 0);
    /* charger plugged in between low reads resets the count */
    n = 0;
    assert(critbatt_should_power_off(5, 3, 1, &n) == 0);
    assert(critbatt_should_power_off(5, 3, 0, &n) == 0 && n == 0);
    assert(critbatt_should_power_off(5, 3, 1, &n) == 0);
    /* never on external power, even at 0% */
    n = 0;
    for (int i = 0; i < 5; i++) assert(critbatt_should_power_off(5, 0, 0, &n) == 0);
    /* Off never fires; negative threshold treated as Off */
    n = 0;
    for (int i = 0; i < 5; i++) assert(critbatt_should_power_off(0, 0, 1, &n) == 0);
    for (int i = 0; i < 5; i++) assert(critbatt_should_power_off(-1, 0, 1, &n) == 0);
    /* unreadable gauge (-1) is not "low" and breaks the streak */
    n = 0;
    assert(critbatt_should_power_off(5, 2, 1, &n) == 0);
    assert(critbatt_should_power_off(5, -1, 1, &n) == 0 && n == 0);
    for (int i = 0; i < 5; i++) assert(critbatt_should_power_off(5, -1, 1, &n) == 0);
    /* boundary: at threshold fires, one above never does; each choice */
    for (int i = 1; i < CRITBATT_NCHOICES; i++) {
        int th = critbatt_choices[i];
        n = 0; critbatt_should_power_off(th, th, 1, &n);
        assert(critbatt_should_power_off(th, th, 1, &n) == 1);
        n = 0; critbatt_should_power_off(th, th + 1, 1, &n);
        assert(critbatt_should_power_off(th, th + 1, 1, &n) == 0);
    }
}

static void test_critbatt_parse(void) {
    /* the choice set the Settings picker offers */
    assert(CRITBATT_DEFAULT == 20);
    int want[] = { 0, 5, 10, 15, 20 };
    assert(CRITBATT_NCHOICES == 5);
    for (int i = 0; i < 5; i++) assert(critbatt_choices[i] == want[i]);
    /* persisted values round-trip, with or without newline */
    assert(critbatt_parse("0") == 0);
    assert(critbatt_parse("5\n") == 5);
    assert(critbatt_parse("10") == 10);
    assert(critbatt_parse("15\n") == 15);
    assert(critbatt_parse("20") == 20);
    /* old/other values fall back to the default, never to Off */
    assert(critbatt_parse("3") == 20);
    assert(critbatt_parse("25") == 20);
    assert(critbatt_parse("-5") == 20);
    assert(critbatt_parse("") == 20);
    assert(critbatt_parse("junk") == 20);
}

int main(void) {
    test_critbatt();
    test_critbatt_parse();
    test_should_power_off();
    test_should_blank();
    test_should_wake();
    printf("ok\n");
    return 0;
}
