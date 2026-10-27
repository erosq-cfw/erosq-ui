#ifndef EROSQ_BINFONT_LOGIC_H
#define EROSQ_BINFONT_LOGIC_H
/* Structural check of an LVGL binary font (lv_font_conv --format bin) before
 * lv_binfont_create sees it. LVGL's loader trusts the file: a corrupt theme
 * font on the SD card can make it allocate a huge table (LV_ASSERT_MALLOC
 * halts), or index past its glyph tables when drawing (SEGV). Theme fonts come
 * from the SD card and load at every boot, so one bad file would be a crash
 * loop. This mirrors every count, offset and index the loader and the fmt_txt
 * renderer use (lvgl/src/font/binfont_loader, fmt_txt), all bounded by the
 * file. Dependency-free; tests/test_binfont.c. */
#include <stddef.h>
#include <stdint.h>

#define BINFONT_MAX_SIZE   (256 * 1024)   /* whole file; also keeps bitmap_index in its 20 bits */
#define BINFONT_MAX_GLYPHS 4096
#define BINFONT_MAX_CMAPS  64
#define BINFONT_MAX_LINE_H 64

/* 0 = safe to load: uncompressed, 1/2/4/8 bpp, every glyph id, bitmap and
 * kerning index in range. -1 otherwise. */
int binfont_check(const uint8_t *b, size_t n);

#endif
