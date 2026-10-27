#ifndef UI_ART_SHARE_H
#define UI_ART_SHARE_H
#include <stdint.h>
/* One fetch+decode per song, shared by every art tile (Home preview + Now
 * Playing are both 118 px and both follow MPD's current song). Pure C, no
 * LVGL; UI-thread only, so no locking. Host-tested in tests/test_art_share.c.
 *
 * art_pb_t: a decoded tile, refcounted by the tiles showing it. The module
 * keeps only a WEAK pointer to the latest one (cleared when the last tile lets
 * go), so sharing pins no memory beyond what the tiles already hold. */
typedef struct art_pb { int refs; uint16_t *px; } art_pb_t;

art_pb_t *art_pb_wrap(uint16_t *px);                /* takes px; refs = 0 */
void      art_pb_ref(art_pb_t *pb);
/* Drop one ref; on the last, free it and clear *weak if it points here. */
void      art_pb_unref(art_pb_t *pb, art_pb_t **weak);

/* What a tile asking for `uri` at size T should do, given the shared slot
 * (last requested uri/T, a job in flight, a live decoded result, or a known
 * failure for that uri). */
typedef enum { ART_NONE, ART_ATTACH, ART_WAIT, ART_START } art_act_t;
typedef struct { char uri[256]; int T, busy, failed; art_pb_t *pb; } art_slot_t;
art_act_t art_share_want(const art_slot_t *s, const char *uri, int T);
#endif
