/* test_art_share -- shared album-art slot (src/ui_art_share.c): decision table
 * and the weak-pointer refcount. Pure C; ASan via run.sh. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_art_share.h"

int main(void)
{
    art_slot_t s; memset(&s, 0, sizeof s);
    /* empty slot */
    assert(art_share_want(&s, "", 118) == ART_NONE);
    assert(art_share_want(&s, NULL, 118) == ART_NONE);
    assert(art_share_want(&s, "a.flac", 118) == ART_START);

    /* job for a.flac in flight: the second tile waits instead of a 2nd fetch */
    strcpy(s.uri, "a.flac"); s.T = 118; s.busy = 1;
    assert(art_share_want(&s, "a.flac", 118) == ART_WAIT);
    assert(art_share_want(&s, "b.flac", 118) == ART_START);   /* song changed */
    assert(art_share_want(&s, "a.flac", 94)  == ART_START);   /* other size: own decode */
    assert(art_share_want(&s, "", 118) == ART_NONE);

    /* decoded: later tiles (Now Playing opened) attach, no fetch */
    s.busy = 0;
    uint16_t *px = malloc(118 * 118 * 2);
    art_pb_t *pb = art_pb_wrap(px);
    assert(pb && pb->refs == 0 && pb->px == px);
    s.pb = pb;
    art_pb_ref(pb); art_pb_ref(pb);                            /* Home + NP */
    assert(art_share_want(&s, "a.flac", 118) == ART_ATTACH);
    art_pb_unref(pb, &s.pb);                                   /* NP closed */
    assert(s.pb == pb && pb->refs == 1);
    art_pb_unref(pb, &s.pb);                                   /* Home moved on: freed, weak cleared */
    assert(s.pb == NULL);
    assert(art_share_want(&s, "a.flac", 118) == ART_START);   /* nobody holds it: refetch */

    /* a weak pointer to a different buffer is left alone */
    art_pb_t *p1 = art_pb_wrap(malloc(8)), *p2 = art_pb_wrap(malloc(8));
    art_pb_ref(p1); art_pb_ref(p2);
    art_pb_t *weak = p2;
    art_pb_unref(p1, &weak); assert(weak == p2);
    art_pb_unref(p2, &weak); assert(weak == NULL);
    art_pb_unref(NULL, &weak);                                 /* no-op */

    /* known failure (no art / progressive) for this song: don't refetch per tile */
    s.failed = 1;
    assert(art_share_want(&s, "a.flac", 118) == ART_NONE);
    assert(art_share_want(&s, "b.flac", 118) == ART_START);
    puts("art_share OK");
    return 0;
}
