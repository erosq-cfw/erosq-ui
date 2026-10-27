#ifndef UI_MEMTRIM_H
#define UI_MEMTRIM_H
#include <stddef.h>
#include <stdint.h>
/* Give the LVGL heap's free pages back to the kernel (malloc_trim for the TLSF
 * pool). The pool is a 384 KB static array: pages a big screen touched (Songs
 * = ~160 KB of rows) stay dirty -- resident or in zram -- for the life of the
 * process even after the screen is gone. Called after a screen is disposed. */
void ui_mem_trim(void);

/* Pure maths, header-only (host-tested in test_memtrim.c). A free TLSF block
 * at `ptr` with payload `size` keeps live allocator data in its first two
 * pointers (next_free/prev_free) and its last pointer (the next block's
 * prev_phys_block). Everything strictly between is dead and may be zeroed by
 * MADV_DONTNEED. Returns the page-aligned [*lo, *hi) inside that window, or 0
 * when it holds no whole page. `pg` must be a power of two. */
static inline int ui_memtrim_range(uintptr_t ptr, size_t size, size_t pg,
                                   uintptr_t *lo, uintptr_t *hi)
{
    const uintptr_t keep_head = 2 * sizeof(void *), keep_tail = sizeof(void *);
    if (size <= keep_head + keep_tail) return 0;
    uintptr_t l = (ptr + keep_head + pg - 1) & ~(uintptr_t)(pg - 1);
    uintptr_t h = (ptr + size - keep_tail) & ~(uintptr_t)(pg - 1);
    if (h <= l) return 0;
    *lo = l; *hi = h;
    return 1;
}
#endif
