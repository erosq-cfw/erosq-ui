#ifndef EROSQ_UI_H
#define EROSQ_UI_H
#include "lvgl.h"

struct ui_ctx {
    lv_display_t *disp;
    lv_group_t   *group;
    lv_indev_t   *indev;   /* keypad: physical buttons */
    lv_indev_t   *enc;     /* encoder: scroll wheel (focus nav) */
    lv_indev_t   *pad;     /* keypad: BT gamepad (d-pad + A/B/START) */
    int           running;
};

/* Build the initial screen tree onto the default screen. Defined in ui_screens.c.
 * Task 1 ships a temporary stub; replaced in Task 4. */
void ui_build(struct ui_ctx *ctx);
#endif
