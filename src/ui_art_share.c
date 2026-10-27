/* ui_art_share.c -- see ui_art_share.h. */
#include "ui_art_share.h"
#include <stdlib.h>
#include <string.h>

art_pb_t *art_pb_wrap(uint16_t *px)
{
    art_pb_t *pb = calloc(1, sizeof *pb);
    if (pb) pb->px = px; else free(px);
    return pb;
}

void art_pb_ref(art_pb_t *pb) { pb->refs++; }

void art_pb_unref(art_pb_t *pb, art_pb_t **weak)
{
    if (!pb || --pb->refs > 0) return;
    if (weak && *weak == pb) *weak = NULL;
    free(pb->px);
    free(pb);
}

art_act_t art_share_want(const art_slot_t *s, const char *uri, int T)
{
    if (!uri || !*uri) return ART_NONE;                   /* nothing playing: placeholder */
    if (s->T != T || strcmp(s->uri, uri)) return ART_START;
    if (s->pb) return ART_ATTACH;                         /* another tile already has it */
    if (s->busy) return ART_WAIT;                         /* in flight: the poll hands it out */
    if (s->failed) return ART_NONE;                       /* no art / progressive: don't refetch */
    return ART_START;                                     /* decoded once, since let go: refetch */
}
