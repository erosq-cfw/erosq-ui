/* test_memtrim -- the page window ui_mem_trim may discard inside a free TLSF block. */
#include <assert.h>
#include <stdio.h>
#include "ui_memtrim.h"

int main(void)
{
    const size_t P = 4096, PTR = sizeof(void *);
    uintptr_t lo, hi;

    /* tiny / sub-page blocks: nothing to give back */
    assert(!ui_memtrim_range(0x1000, 0, P, &lo, &hi));
    assert(!ui_memtrim_range(0x1000, 3 * PTR, P, &lo, &hi));
    assert(!ui_memtrim_range(0x1008, P, P, &lo, &hi));            /* straddles, no whole page */

    /* page-aligned block: the head page holds next/prev_free -> skipped;
     * the tail page holds the next block's prev_phys -> skipped */
    assert(ui_memtrim_range(0x10000, 4 * P, P, &lo, &hi));
    assert(lo == 0x11000 && hi == 0x13000);

    /* dead window exactly page-aligned at both ends: all of it goes */
    assert(ui_memtrim_range(0x10000 - 2 * PTR, 3 * P + 3 * PTR, P, &lo, &hi));
    assert(lo == 0x10000 && hi == 0x13000);
    /* one byte short at the tail: the last page now holds prev_phys -> kept */
    assert(ui_memtrim_range(0x10000 - 2 * PTR, 3 * P + 3 * PTR - 1, P, &lo, &hi));
    assert(lo == 0x10000 && hi == 0x12000);

    /* every result stays strictly inside the dead window */
    for (uintptr_t ptr = 0x20000; ptr < 0x20000 + 2 * P; ptr += PTR)
        for (size_t sz = 3 * PTR + PTR; sz < 5 * P; sz += 4 * PTR + 4)
            if (ui_memtrim_range(ptr, sz, P, &lo, &hi)) {
                assert(lo >= ptr + 2 * PTR && hi <= ptr + sz - PTR);
                assert(lo % P == 0 && hi % P == 0 && hi > lo);
            }
    puts("memtrim ok");
    return 0;
}
