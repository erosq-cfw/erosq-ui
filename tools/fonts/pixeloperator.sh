#!/bin/sh
# pixeloperator.sh <out dir>: Pixel Operator (Jayvee Enaguas, CC0 1.0) as LVGL
# binary fonts for theme.txt `font` / `font_bold`, at 16 px (the Pixel
# example theme). 16 px is the face's own pixel grid (1:1), so every font pixel
# is one screen pixel; at 20 px (1.25x) strokes doubled or vanished. Converted
# at --bpp 1, then bpp1to4.py repacks it as 4 bpp (same pixels), which LVGL
# rasterises about twice as fast as 1 bpp (README.md). pixelfix.py first
# redraws x/X/s/S by hand (pixeloperator-fixes.txt).
# PO_URL=file:///path/to/pixel_operator.zip reuses a downloaded copy (the
# sha256 below is still checked).
set -e
URL="${PO_URL:-https://dl.dafont.com/dl/?f=pixel_operator}"
SHA="95b33667e4884f5714db33d8324472df28f9d5ffed49108824c5819c1a217dad"
OUT="$1"; [ -n "$OUT" ] || { echo "usage: $0 <out dir>" >&2; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
curl -sSL -o "$T/po.zip" "$URL"
echo "$SHA  $T/po.zip" | sha256sum -c - >/dev/null || { echo "pixel_operator zip changed upstream: check its licence again" >&2; exit 1; }
unzip -q "$T/po.zip" -d "$T/po"
grep -q "CC0 1.0 Universal" "$T/po/LICENSE.txt"
# ASCII + Latin-1 (accented names) + bullet + ellipsis; LV_SYMBOL_* come from
# the iPod font (the theme font's fallback).
R="0x20-0x7E,0xA0-0xFF,0x2022,0x2026"
conv() { npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --no-kerning --bpp 1 --size 16 \
           --font "$T/po/$1.ttf" -r "$R" --format bin -o "$T/$2"
         python3 "$(dirname "$0")/pixelfix.py" "$T/$2" "$T/fix-$2" "$(dirname "$0")/pixeloperator-fixes.txt" "$3"
         python3 "$(dirname "$0")/bpp1to4.py" "$T/fix-$2" "$OUT/$2"; }
mkdir -p "$OUT"
conv PixelOperator      PixelOperator16.bin      regular
conv PixelOperator-Bold PixelOperator16-Bold.bin bold
cp "$T/po/LICENSE.txt" "$OUT/PixelOperator-LICENSE.txt"
ls -l "$OUT"/PixelOperator*
