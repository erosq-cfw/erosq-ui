#!/bin/sh
# Generate the iPod-Video font set as LVGL C fonts. Helvetica-metric faces:
#   regular = Liberation Sans (OFL), bold = Arimo (Apache) — same metric family.
# The 14px regular is the default/body font and carries the merged FontAwesome
# symbol range (so LV_SYMBOL_* — battery, chevron, +, refresh — keep working).
# Mirrors lvgl/scripts/built_in_font/built_in_font_gen.py for the symbol set.
set -e
cd "$(dirname "$0")"
REG="$1"   # Liberation Sans Regular ttf
BOLD="$2"  # Arimo Bold ttf
FA="lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
SYMS="61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"
ASCII="0x20-0x7F,0xB0,0x2022"
gen() { # face size out  [extra-font-args...]
  FACE="$1"; SZ="$2"; OUT="$3"; shift 3
  npx -y lv_font_conv --no-compress --bpp 4 --size "$SZ" \
    --font "$FACE" -r "$ASCII" "$@" \
    --format lvgl -o "fonts/$OUT" --force-fast-kern-format
  perl -0pi -e 's{#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif}{#include "lvgl.h"}' "fonts/$OUT"
  echo "generated fonts/$OUT"
}
gen "$REG"  14 lv_font_ipod_14.c  --font "$FA" -r "$SYMS"   # body + symbols
gen "$BOLD" 14 lv_font_ipod_14b.c                           # title bar (bold)
gen "$BOLD" 18 lv_font_ipod_18b.c                           # now-playing title (bold)
# Empty-state icons only (Bluetooth U+F293, WiFi U+F1EB, music U+F001): no ASCII, so ~2 KB.
npx -y lv_font_conv --no-compress --bpp 4 --size 40 --font "$FA" -r 62099,61931,61441 \
  --format lvgl -o fonts/lv_font_ipod_icon40.c --force-fast-kern-format
perl -0pi -e 's{#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif}{#include "lvgl.h"}' fonts/lv_font_ipod_icon40.c
echo "generated fonts/lv_font_ipod_icon40.c"
