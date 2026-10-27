#include "ui_vlist.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "vlist_logic.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    int n, rows, base, prev, busy;
    ui_vlist_text_fn text;
    ui_vlist_click_fn click;
    void *ctx;
    lv_obj_t *list, *thumb;
    lv_obj_t *btn[UI_VLIST_ROWS];
} vlist_t;

static void label(vlist_t *v, int j)
{
    char buf[300];
    v->text(v->ctx, v->base + j, buf, sizeof buf);
    lv_obj_t *l = lv_obj_get_child(v->btn[j], -1);   /* lv_list_add_button: the label (no icon) */
    lv_label_set_text(l, buf);
}

/* Position in the whole list: thumb on the scrollbar track (the list's own
 * scrollbar would only show the position within the window). */
static void thumb(vlist_t *v, int item)
{
    if (!v->thumb) return;
    int track = lv_obj_get_height(v->list) - 4, rowh = lv_obj_get_height(v->btn[0]);
    int h = rowh > 0 ? track * (lv_obj_get_height(v->list) / rowh) / v->n : 0;
    if (h < 10) h = 10;
    lv_obj_set_size(v->thumb, 3, h);
    lv_obj_set_pos(v->thumb, lv_obj_get_x(v->list) + lv_obj_get_width(v->list) - 4,
                   lv_obj_get_y(v->list) + 2 + (track - h) * item / (v->n - 1));
}

static void focused_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    int j = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e)), prev = v->prev;
    v->prev = j;
    if (v->busy) return;
    int nb, row, sh = vlist_step(v->n, v->rows, v->base, prev, j, &nb, &row);
    if (nb != v->base) {
        v->base = nb;
        for (int k = 0; k < v->rows; k++) label(v, k);
        if (sh) {   /* same items, other rows: move the view with them, instantly */
            lv_obj_update_layout(v->list);
            int rowh = lv_obj_get_y(v->btn[1]) - lv_obj_get_y(v->btn[0]);
            lv_obj_scroll_by(v->list, 0, sh * rowh, LV_ANIM_OFF);
        }
        if (row != j) {
            v->busy = 1;
            lv_group_focus_obj(v->btn[row]);
            v->busy = 0;
            v->prev = row;
        }
    }
    lv_obj_scroll_to_view(v->btn[row], LV_ANIM_ON);   /* what SCROLL_ON_FOCUS did */
    thumb(v, v->base + row);
}

static void click_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    int j = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    v->click(v->ctx, v->base + j);
}

/* LVGL sends DELETE to the list before deleting its rows, and deleting a
 * focused row refocuses the group, which would run focused_cb on freed state.
 * Take the rows out of the group first (busy: their refocusing is ignored). */
static void delete_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    v->busy = 1;
    for (int j = 0; j < v->rows; j++) lv_group_remove_obj(v->btn[j]);
    free(v);
}

lv_obj_t *ui_vlist_create(lv_obj_t *scr, int n, ui_vlist_text_fn text,
                          ui_vlist_click_fn click, void *ctx)
{
    vlist_t *v = calloc(1, sizeof *v);
    lv_obj_t *list = ui_screen_list(scr);   /* body-sized: narrower beside a theme's side panel */
    if (!v) return list;
    v->n = n; v->rows = n < UI_VLIST_ROWS ? n : UI_VLIST_ROWS; v->prev = -1;
    v->text = text; v->click = click; v->ctx = ctx; v->list = list;
    lv_obj_add_event_cb(list, delete_cb, LV_EVENT_DELETE, v);
    int windowed = n > UI_VLIST_ROWS;
    for (int j = 0; j < v->rows; j++) {
        lv_obj_t *b = v->btn[j] = lv_list_add_button(list, NULL, "");
        lv_obj_set_user_data(b, (void *)(intptr_t)j);
        label(v, j);
        ui_focus_add(b); ui_apply_focus_style(b);
        lv_obj_add_event_cb(b, click_cb,     LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY,     NULL);
        if (windowed) {
            lv_obj_set_scroll_on_focus(b, false);   /* focused_cb scrolls, after re-labelling */
            lv_obj_add_event_cb(b, focused_cb, LV_EVENT_FOCUSED, v);
        }
    }
    /* Adding the first row to an empty group focused it before focused_cb was
     * attached, so record that row: with prev left at -1 the first wheel-up
     * from item 0 was not seen as a wrap and landed on item w-1, not n-1. */
    lv_obj_t *f = lv_group_get_focused(lv_obj_get_group(v->btn[0]));
    for (int j = 0; j < v->rows; j++) if (v->btn[j] == f) v->prev = j;
    if (windowed) {
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        v->thumb = lv_obj_create(ui_screen_body(scr));
        lv_obj_remove_style_all(v->thumb);
        ui_style_scroll_thumb(v->thumb);
        lv_obj_remove_flag(v->thumb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_update_layout(list);
        thumb(v, 0);
    }
    return list;
}
