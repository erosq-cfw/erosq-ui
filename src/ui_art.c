/* Album art: MPD readpicture/albumart bytes -> TJpgDec -> small RGB565 tile.
 * TJpgDec emits one MCU block at a time, so the full-size cover is never held
 * in RAM; each block is subsampled straight into the T x T tile. */
#include "ui_art.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "tjpgd/tjpgd.h"

typedef struct { ui_art_in_fn in; void *src; uint16_t *out; int TW, TH, W, H; } jctx_t;

static size_t jin(JDEC *jd, uint8_t *buf, size_t n)
{
    jctx_t *c = jd->device;
    return c->in(c->src, buf, n);
}

typedef struct { const unsigned char *p; int len, pos; } memsrc_t;

static size_t mem_in(void *src, uint8_t *buf, size_t n)
{
    memsrc_t *m = src;
    if (m->pos + (int)n > m->len) n = m->len - m->pos;
    if (buf) memcpy(buf, m->p + m->pos, n);
    m->pos += (int)n;
    return n;
}

static int jout(JDEC *jd, void *bitmap, JRECT *r)
{
    jctx_t *c = jd->device;
    const uint8_t *px = bitmap;                       /* JD_FORMAT 0: 24-bit, LVGL's copy emits B,G,R */
    int W = c->W, H = c->H, TW = c->TW, TH = c->TH;   /* decoded (descaled) size -> TW x TH */
    for (int y = r->top; y <= r->bottom; y++) {
        int ty0 = y * TH / H, ty1 = (y + 1) * TH / H; if (ty1 <= ty0) ty1 = ty0 + 1;
        for (int x = r->left; x <= r->right; x++, px += 3) {
            int tx0 = x * TW / W, tx1 = (x + 1) * TW / W; if (tx1 <= tx0) tx1 = tx0 + 1;
            /* ponytail: nearest-neighbour; each source pixel paints its whole target
             * span so upscaling small covers leaves no holes. Box-filter if 96px looks noisy. */
            uint16_t v = (uint16_t)(((px[2] & 0xf8) << 8) | ((px[1] & 0xfc) << 3) | (px[0] >> 3));
            for (int ty = ty0; ty < ty1 && ty < TH; ty++)
                for (int tx = tx0; tx < tx1 && tx < TW; tx++) c->out[ty * TW + tx] = v;
        }
    }
    return 1;
}

int ui_art_scale(int w, int h, int T)
{
    int m = w < h ? w : h, s = 0;
    while (s < 3 && (m >> (s + 1)) >= T) s++;   /* largest 1/2^s that still covers the tile */
    return s;
}

int ui_art_decode(const unsigned char *jpg, int len, uint16_t *out, int T)
{
    memsrc_t m = { jpg, len, 0 };
    return ui_art_decode_stream(mem_in, &m, out, T);
}

/* TW x TH output; square tiles pass TW == TH == T. The descale is chosen on
 * the larger target side, so neither axis decodes below its target size. */
static int decode(ui_art_in_fn in, void *src, uint16_t *out, int TW, int TH)
{
    uint8_t pool[4096];   /* tjpgd wants ~3.1KB + JD_SZBUF; on the stack: workers decode concurrently */
    jctx_t c = { in, src, out, TW, TH, 0, 0 };
    JDEC jd;
    if (jd_prepare(&jd, jin, pool, sizeof pool, &c) != JDR_OK) return -1;
    if (jd.width == 0 || jd.height == 0) return -1;
    int s = ui_art_scale(jd.width, jd.height, TW > TH ? TW : TH);
    c.W = jd.width >> s; c.H = jd.height >> s;
    return jd_decomp(&jd, jout, (uint8_t)s) == JDR_OK ? 0 : -1;
}

int ui_art_decode_stream(ui_art_in_fn in, void *src, uint16_t *out, int T)
{
    return decode(in, src, out, T, T);
}

int ui_art_decode_rect(const unsigned char *jpg, int len, uint16_t *out, int TW, int TH)
{
    memsrc_t m = { jpg, len, 0 };
    return decode(mem_in, &m, out, TW, TH);
}

#ifndef UI_ART_NO_LVGL
#include "ui_art_job.h"
#include "ui_art_share.h"
#include "ui_theme.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop: private since 9.6 */

/* One fetch+decode per song for ALL tiles (ui_art_share.h): the Home preview
 * and Now Playing tiles are the same size and follow the same current song,
 * so the second tile attaches to the first one's pixels (or waits for the
 * same job) instead of running a second MPD fetch + decode. A tile holds
 * pixels only while a cover is shown (most of the time it's the placeholder).
 * The fetch+decode runs on a worker (ui_art_job); `S.poll` picks the result
 * up on the LVGL thread. Everything here is LVGL-thread only. */
typedef struct {
    lv_obj_t *img; lv_image_dsc_t dsc; int T; char uri[256]; art_pb_t *pb;
} art_t;

#define ART_MAX_TILES 4
static struct {
    art_slot_t slot;            /* uri/T of the current job or result, weak pb */
    unsigned gen; art_job_t *job; lv_timer_t *poll;
    art_t *tiles[ART_MAX_TILES];
} S;

static void art_show(art_t *a, art_pb_t *pb)
{
    art_pb_ref(pb); a->pb = pb;
    a->dsc.data = (const uint8_t *)pb->px;
    lv_image_cache_drop(&a->dsc);
    lv_image_set_src(a->img, &a->dsc);
    lv_obj_clear_flag(a->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(a->img);
}

static void art_drop_px(art_t *a)
{
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);   /* never draw a freed buffer */
    lv_image_set_src(a->img, NULL);
    a->dsc.data = NULL;
    lv_image_cache_drop(&a->dsc);
    art_pb_unref(a->pb, &S.slot.pb); a->pb = NULL;
}

/* Abandon the in-flight fetch (song changed). The worker keeps its own ref,
 * notices the cancel at its next read and frees the job. */
static void art_abandon(void)
{
    if (S.poll) { lv_timer_delete(S.poll); S.poll = NULL; }
    if (S.job)  { art_job_cancel(S.job); art_job_release(S.job); S.job = NULL; }
    S.slot.busy = 0;
}

static void art_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.job || !art_job_done(S.job)) return;
    art_job_t *j = S.job;
    uint16_t *px = art_job_take(j, S.gen);
    art_pb_t *pb = px ? art_pb_wrap(px) : NULL;
    if (pb) {
        S.slot.pb = pb;
        art_pb_ref(pb);                            /* hold it while handing it out */
        int n = 0;
        for (int i = 0; i < ART_MAX_TILES; i++) {
            art_t *a = S.tiles[i];
            if (a && !a->pb && a->T == S.slot.T && !strcmp(a->uri, S.slot.uri)) { art_show(a, pb); n++; }
        }
        LV_LOG_USER("art: %d bytes -> %dpx fetched+decoded in %d ms (worker), %d tile(s)", j->bytes, S.slot.T, j->ms, n);
        art_pb_unref(pb, &S.slot.pb);              /* no tile wants it any more: freed */
    } else {
        S.slot.failed = 1;
        if (j->bytes > 0) LV_LOG_WARN("art: decode failed (%d bytes; progressive/PNG?) %s", j->bytes, j->uri);
    }
    art_abandon();                                 /* deletes this timer: allowed from its own cb */
}

static void art_del_cb(lv_event_t *e)
{
    art_t *a = lv_obj_get_user_data(lv_event_get_target(e));
    for (int i = 0; i < ART_MAX_TILES; i++) if (S.tiles[i] == a) S.tiles[i] = NULL;
    lv_image_set_src(a->img, NULL);   /* detach before the dsc goes away */
    art_pb_unref(a->pb, &S.slot.pb);
    free(a);                          /* an in-flight job stays: another tile may want it */
}

lv_obj_t *ui_art_create(lv_obj_t *parent, int size)
{
    /* iPod missing-art placeholder: gray gradient tile + note glyph. The real
     * cover is an lv_image child, shown over it when a decode succeeds. */
    lv_obj_t *art = lv_obj_create(parent);
    lv_obj_set_size(art, size, size);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(art, 4, 0);
    lv_obj_set_style_border_width(art, 1, 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(art, 0, 0);
    lv_obj_t *note = lv_label_create(art);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);   /* default font (ipod_14) carries the symbols */
    ui_style_art_placeholder(art, note);           /* colours follow the theme */
    lv_obj_center(note);

    int slot = -1;
    for (int i = 0; i < ART_MAX_TILES; i++) if (!S.tiles[i]) { slot = i; break; }
    if (slot < 0) return art;                     /* placeholder only */
    int T = size - 2;                             /* inside the 1px border */
    art_t *a = calloc(1, sizeof *a);
    if (!a) return art;
    a->T = T;
    a->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    a->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    a->dsc.header.w = T; a->dsc.header.h = T; a->dsc.header.stride = T * 2;
    a->dsc.data_size = T * T * 2;
    a->img = lv_image_create(art);    /* src is set once pixels exist (dsc.data != NULL) */
    lv_obj_center(a->img);
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(art, a);
    lv_obj_add_event_cb(art, art_del_cb, LV_EVENT_DELETE, NULL);
    S.tiles[slot] = a;
    return art;
}

void ui_art_update(lv_obj_t *tile, const char *uri)
{
    art_t *a = lv_obj_get_user_data(tile);
    if (!a) return;
    if (!uri) uri = "";
    if (strcmp(a->uri, uri) == 0) return;         /* same song/file: nothing to do */
    lv_snprintf(a->uri, sizeof a->uri, "%s", uri);
    art_drop_px(a);                               /* placeholder until the new cover is ready */
    switch (art_share_want(&S.slot, uri, a->T)) {
    case ART_ATTACH: art_show(a, S.slot.pb); break;
    case ART_START:
        art_abandon();                            /* a late result for the old song is dropped */
        lv_snprintf(S.slot.uri, sizeof S.slot.uri, "%s", uri);
        S.slot.T = a->T; S.slot.failed = 0; S.gen++;
        S.job = art_job_start(S.gen, uri, a->T, ui_art_fetch, NULL);
        if (S.job) { S.slot.busy = 1; S.poll = lv_timer_create(art_poll_cb, 50, NULL); }
        else S.slot.failed = 1;
        break;
    default: break;                               /* ART_WAIT: the poll hands it out; ART_NONE */
    }
}
#endif
