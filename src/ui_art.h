#ifndef UI_ART_H
#define UI_ART_H
#include <stddef.h>
#include <stdint.h>

/* Baseline-JPEG -> T x T RGB565 (nearest-neighbour, whole-image fit, no aspect
 * padding: covers are square). Pure C, no LVGL; host-testable. Returns 0 on OK. */
int ui_art_decode(const unsigned char *jpg, int len, uint16_t *out, int T);

/* TJpgDec descale exponent for a w x h JPEG on a T px tile: the largest
 * s <= 3 with min(w,h)/2^s >= T, so the decode never drops below tile size. */
int ui_art_scale(int w, int h, int T);

/* Baseline-JPEG -> TW x TH RGB565, same nearest-neighbour whole-image fit (the
 * caller supplies an image of that aspect; e.g. a 320x240 full-screen image). */
int ui_art_decode_rect(const unsigned char *jpg, int len, uint16_t *out, int TW, int TH);

/* Same, pulling the JPEG through `in` (buf NULL = skip n; returns bytes
 * supplied, short = end/error) so the whole file never has to be in RAM. */
typedef size_t (*ui_art_in_fn)(void *src, uint8_t *buf, size_t n);
int ui_art_decode_stream(ui_art_in_fn in, void *src, uint16_t *out, int T);

/* Worker body for ui_art_job (runs off the LVGL thread, no LVGL calls):
 * MPD readpicture/albumart streamed into TJpgDec into a fresh job->px. */
struct art_job;
int ui_art_fetch(struct art_job *j);

#ifndef UI_ART_NO_LVGL
#include "lvgl.h"
lv_obj_t *ui_art_create(lv_obj_t *parent, int size);      /* placeholder tile */
void      ui_art_update(lv_obj_t *tile, const char *uri); /* refetch only on uri change */
#endif
#endif
