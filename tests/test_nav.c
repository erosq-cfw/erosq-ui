#include <assert.h>
#include <stddef.h>
#include "ui_nav.h"
int main(void)
{
    ui_nav_t n; int a,b,c;
    ui_nav_reset(&n, &a);
    assert(ui_nav_top(&n) == &a);
    assert(ui_nav_depth(&n) == 1);
    ui_nav_push(&n, &b);
    ui_nav_push(&n, &c);
    assert(ui_nav_top(&n) == &c && ui_nav_depth(&n) == 3);
    assert(ui_nav_pop(&n) == &b);
    assert(ui_nav_pop(&n) == &a);
    assert(ui_nav_pop(&n) == &a);   /* never pops root */
    assert(ui_nav_depth(&n) == 1);

    /* overflow: stack holds 8 (root + 7 pushes); the 9th push is refused
     * (returns NULL) and must not corrupt the stack or advance the top. */
    ui_nav_reset(&n, &a);
    for (int i = 0; i < 7; i++) assert(ui_nav_push(&n, &b) == &b);
    assert(ui_nav_depth(&n) == 8);
    assert(ui_nav_push(&n, &c) == NULL);                 /* refused, no room */
    assert(ui_nav_depth(&n) == 8 && ui_nav_top(&n) == &b); /* unchanged */
    return 0;
}
