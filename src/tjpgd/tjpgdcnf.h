/* TJpgDec configuration for erosq-ui's album art (src/ui_art.c).
 * Own copy of LVGL 9.6's tjpgd with upstream R0.03's descaler restored: a
 * 4000x4000 cover decodes at 1/8 (DC only, no IDCT) instead of full size. */
#define JD_SZBUF        512
#define JD_FORMAT       0   /* RGB888, emitted as B,G,R (LVGL's byte order, kept) */
#define JD_USE_SCALE    1
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1
