#ifndef EROSQ_UI_VLIST_H
#define EROSQ_UI_VLIST_H
/* Virtual list: n items through at most UI_VLIST_ROWS recycled list buttons.
 * Item text and actions come from callbacks, so the data (a strtab, MPD pages)
 * stays on the libc heap and the LVGL pool holds a fixed number of rows however
 * big the library is. Up to UI_VLIST_ROWS items it is a plain lv_list, exactly
 * as before; past that, the rows are re-labelled as focus nears either end
 * (vlist_logic.c) and a thumb on the right shows the position in the whole list. */
#include "lvgl.h"

#define UI_VLIST_ROWS 48

typedef void (*ui_vlist_text_fn)(void *ctx, int i, char *buf, int n);   /* label of item i */
typedef void (*ui_vlist_click_fn)(void *ctx, int i);                    /* item i clicked */

/* The list fills the screen's body. ctx must outlive the screen. */
lv_obj_t *ui_vlist_create(lv_obj_t *scr, int n, ui_vlist_text_fn text,
                          ui_vlist_click_fn click, void *ctx);

#endif
