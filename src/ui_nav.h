/* ui_nav.h */
#ifndef EROSQ_UI_NAV_H
#define EROSQ_UI_NAV_H
typedef struct { void *scr[8]; int top; } ui_nav_t;
void  ui_nav_reset(ui_nav_t *n, void *root);
void *ui_nav_push(ui_nav_t *n, void *scr);
void *ui_nav_pop(ui_nav_t *n);
void *ui_nav_top(const ui_nav_t *n);
int   ui_nav_depth(const ui_nav_t *n);
#endif
