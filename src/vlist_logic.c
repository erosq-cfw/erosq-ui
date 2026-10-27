#include "vlist_logic.h"

int vlist_step(int n, int w, int base, int prev, int j, int *new_base, int *row)
{
    *new_base = base; *row = j;
    if (n <= w) return 0;                               /* every item has a row */
    if (prev == w - 1 && j == 0) {                       /* wrapped past the end */
        *new_base = 0;
        return 0;                                        /* jump: caller re-texts and scrolls to row 0 */
    }
    if (prev == 0 && j == w - 1) {                       /* wrapped past the start */
        *new_base = n - w;
        return 0;
    }
    int shift = 0, half = w / 2;
    if (j >= w - VLIST_MARGIN && base + w < n) {         /* near the bottom: later items */
        shift = base + w + half > n ? n - w - base : half;
    } else if (j < VLIST_MARGIN && base > 0) {           /* near the top: earlier items */
        shift = -(base < half ? base : half);
    }
    *new_base = base + shift;
    *row = j - shift;
    return shift;
}
