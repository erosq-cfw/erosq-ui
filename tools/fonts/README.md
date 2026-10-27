# Theme fonts

Theme fonts are LVGL binary fonts (`.bin`) named by `font`, `font_bold` and
`font_large` in `theme.txt` (THEMES.md). Convert a TTF with
[lv_font_conv](https://github.com/lvgl/lv_font_conv) (MIT, npm):

```
npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 16 \
    --font Face.ttf -r 0x20-0x7E,0xA0-0xFF,0x2022,0x2026 --format bin -o Face16.bin
```

- `--bpp 4`, also for pixel fonts (their pixels are then 0 or 15, so they
  draw exactly as at 1 bpp). LVGL unpacks 1 and 2 bpp bit by bit: measured
  with callgrind, rasterising a menu string in Pixel Operator took 157 k
  instructions at 1 bpp, 83 k at 4 bpp and 77 k at 8 bpp (iPod font: 120 k).
  4 bpp costs about 2.7 KB more RAM per font than 1 bpp.
- `--no-compress`: the loader keeps the whole font in RAM either way, and
  uncompressed glyphs draw without decoding.
- Battery, chevron and other `LV_SYMBOL_*` glyphs come from the iPod font, so
  the range needs only text.
- A pixel font at a size that isn't a multiple of its design grid comes out
  anti-aliased (blurry) at 4 bpp. Convert it crisp at `--bpp 1 --no-kerning`,
  then `bpp1to4.py in.bin out.bin` repacks it as 4 bpp with the same pixels.

## Pixel Operator (docs/themes/Pixel)

`pixeloperator.sh <out dir>` downloads, verifies and converts it at 16 px
(`PO_URL=file:///path/to.zip` reuses a downloaded copy; the sha256 is still checked).

- Size: the face's design grid is 16 px, so the Pixel theme uses 16 px: one
  font pixel = one screen pixel (2e). The earlier 20 px (1.25x) was not a whole
  multiple: at 1 bpp some strokes doubled and others vanished ("s" read as
  "3", gaps in "M"); 32 px (2x) does not fit the 220 px list beside the
  panel. Rendered at 1 bpp and repacked to 4 bpp (same pixels).
- Hand fixes (2e4): the 16 px conversion matches the TTF pixel for pixel,
  but the face's own regular x/X (doubled end rows) and s/S (spurs) looked
  rough on the device. `pixelfix.py` redraws them from
  `pixeloperator-fixes.txt` (bold s/S too) in the same boxes, so spacing is
  unchanged; CC0 allows the edits.
- `docs/themes/Pixel/PixelOperator20*.bin` stay only as test_binfont's
  fixtures; the theme no longer references them, so packs leave them out.

- Author: Jayvee Enaguas (HarvettFox96), version 2018.10.04-1.
- Licence: **CC0 1.0** (public domain dedication; commercial use allowed). Checked
  2026-10-23 in the package itself: `LICENSE.txt` is the CC0 1.0 Universal legal
  code, and the TTF name table's licence field says "Creative Commons Zero (CC0) 1.0".
- Source: `https://dl.dafont.com/dl/?f=pixel_operator`, zip sha256
  `95b33667e4884f5714db33d8324472df28f9d5ffed49108824c5819c1a217dad`
  (PixelOperator.ttf `8d805274…764a3e`, PixelOperator-Bold.ttf `c1963fa2…4614b0`).
  The upstream NotABug repository (HarvettFox96/ttf-pixeloperator) no longer
  resolves.
