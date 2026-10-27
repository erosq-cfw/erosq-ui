#ifndef EROSQ_UI_PODCAST_H
#define EROSQ_UI_PODCAST_H
#include "ui.h"

/* Paged episode picker for the feed selected on scr_podcasts. Page state lives
 * in file-scope statics because ui_goto() takes a no-argument builder -- the
 * same approach s_libdir uses for Folders navigation. */
lv_obj_t *scr_pod_feed(void);

#endif
