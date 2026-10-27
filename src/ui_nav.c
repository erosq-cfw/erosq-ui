/* ui_nav.c */
#include <stddef.h>
#include "ui_nav.h"
void  ui_nav_reset(ui_nav_t *n, void *root){ n->scr[0]=root; n->top=0; }
void *ui_nav_push(ui_nav_t *n, void *scr){ if(n->top>=7) return NULL; n->scr[++n->top]=scr; return scr; }
void *ui_nav_pop(ui_nav_t *n){ if(n->top>0) n->top--; return n->scr[n->top]; }
void *ui_nav_top(const ui_nav_t *n){ return n->scr[n->top]; }
int   ui_nav_depth(const ui_nav_t *n){ return n->top+1; }
