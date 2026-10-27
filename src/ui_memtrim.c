/* ui_memtrim.c -- see ui_memtrim.h. */
#include <sys/mman.h>
#include <unistd.h>
#include "lvgl.h"
#include "ui_memtrim.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN && !defined(EROSQ_HOST)
#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/stdlib/builtin/lv_tlsf_private.h"

static void trim_walker(void *ptr, size_t size, int used, void *user)
{
    uintptr_t lo, hi;
    if (used || !ui_memtrim_range((uintptr_t)ptr, size, *(size_t *)user, &lo, &hi)) return;
    madvise((void *)lo, hi - lo, MADV_DONTNEED);   /* anon private: refaults as zero pages */
}

void ui_mem_trim(void)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    lv_pool_t *p;
    LV_LL_READ(&LV_GLOBAL_DEFAULT()->tlsf_state.pool_ll, p) lv_tlsf_walk_pool(*p, trim_walker, &pg);
}
#else
void ui_mem_trim(void) {}
#endif
