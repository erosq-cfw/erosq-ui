#ifndef EROSQ_UI_LAUNCH_H
#define EROSQ_UI_LAUNCH_H
#include "ui.h"
typedef struct { char name[32]; char exec[96]; char cat[16]; } ui_app_t;
int  ui_manifest_parse(const char *text, ui_app_t *out, int max);
void ui_launch_run(struct ui_ctx *ctx, const char *exec_path);
#endif
