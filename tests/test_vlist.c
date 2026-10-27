/* Virtual-list window maths (src/vlist_logic.c). Host cc, assert only. */
#include "vlist_logic.h"
#include <assert.h>
#include <stdio.h>

/* Walk the wheel through every item and back, the way LVGL's wrapping focus
 * group moves: the shown item must always be the one the user expects. */
static void walk(int n, int w)
{
    int rows = n < w ? n : w, base = 0, row = 0, nb, nr;
    for (int step = 0; step < 3 * n + 5; step++) {       /* down, past the end (wrap) */
        int item = base + row, j = row + 1 == rows ? 0 : row + 1;
        int sh = vlist_step(n, rows, base, row, j, &nb, &nr);
        base = nb; row = nr;
        assert(base >= 0 && base + rows <= n && row >= 0 && row < rows);
        assert(base + row == (item + 1) % n);
        /* after a shift focus sits clear of the edges, unless the window hit an end */
        if (n > rows && sh) assert((row >= VLIST_MARGIN || base == 0) && (row < rows - VLIST_MARGIN || base + rows == n));
    }
    for (int step = 0; step < 3 * n + 5; step++) {       /* and up, past the start */
        int item = base + row, j = row == 0 ? rows - 1 : row - 1;
        vlist_step(n, rows, base, row, j, &nb, &nr);
        base = nb; row = nr;
        assert(base >= 0 && base + rows <= n && row >= 0 && row < rows);
        assert(base + row == (item + n - 1) % n);
    }
}

int main(void)
{
    int nb, nr;
    assert(vlist_step(10, 48, 0, 3, 4, &nb, &nr) == 0 && nb == 0 && nr == 4);   /* fits: no window */
    assert(vlist_step(2000, 48, 0, 44, 45, &nb, &nr) == 24 && nb == 24 && nr == 21);
    assert(vlist_step(2000, 48, 1952, 46, 47, &nb, &nr) == 0 && nb == 1952);    /* last rows: no more items */
    assert(vlist_step(2000, 48, 1952, 47, 0, &nb, &nr) == 0 && nb == 0 && nr == 0);       /* wrap to item 0 */
    assert(vlist_step(2000, 48, 0, 0, 47, &nb, &nr) == 0 && nb == 1952 && nr == 47);      /* wrap to the last */
    assert(vlist_step(2000, 48, 10, 3, 2, &nb, &nr) == -10 && nb == 0 && nr == 12);       /* partial shift up */
    int sizes[] = { 1, 2, 47, 48, 49, 50, 71, 72, 73, 256, 257, 356, 2000, 20000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) walk(sizes[i], 48);
    walk(500, 12);                                       /* smallest sane window: 4 * VLIST_MARGIN */
    puts("ok");
    return 0;
}
