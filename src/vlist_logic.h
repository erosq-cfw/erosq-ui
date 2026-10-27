#ifndef EROSQ_VLIST_LOGIC_H
#define EROSQ_VLIST_LOGIC_H
/* Window maths for a virtual list: n items shown through w recycled rows,
 * row j showing item base + j. LVGL's focus group wraps, so the wheel moves
 * focus row to row (and from the last row to the first). Dependency-free;
 * tests/test_vlist.c. */

#define VLIST_MARGIN 3           /* shift before focus gets this close to an edge; w >= 4 * VLIST_MARGIN */

/* Focus moved from row prev to row j (prev -1: first focus). Sets *base and
 * *row (the row that should hold focus now, showing the same item unless a
 * wrap happened); returns the number of rows the content moved (+ = the rows
 * now show later items, so the view must scroll up that many rows to stay
 * put; 0 = nothing to do). A wrap from the last row to the first is a wrap
 * of the whole list (item n-1 -> item 0), and the reverse. */
int vlist_step(int n, int w, int base, int prev, int j, int *new_base, int *row);

#endif
