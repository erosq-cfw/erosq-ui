# Themes

Settings ▸ Display ▸ Theme recolours the whole UI. The list holds
**iPod (default)**, every erosq theme on the SD card, and every Rockbox theme
on the SD card. Rockbox themes are shown as "Name (Rockbox)". A choice applies
at once and is remembered across reboots. If a theme can't be read, the current
theme stays and the UI shows "Couldn't read theme".

Themes set colours, an optional backdrop, an optional side panel and
optional fonts. Icons and Now Playing layouts are not supported yet.

## erosq themes (`theme.txt`)

Each theme is a folder on the SD card, and the folder name is the theme name:

```
<SD card>/themes/Dusk/theme.txt
<SD card>/themes/Dusk/backdrop.bmp      (optional)
```

`theme.txt` holds one `key = value` per line. Lines starting with `#` are
comments, keys are case-insensitive, and every key is optional:

| key              | value     | meaning                                                  |
|------------------|-----------|----------------------------------------------------------|
| `background`     | `#rrggbb` | screen background                                        |
| `text`           | `#rrggbb` | text                                                     |
| `selection`      | `#rrggbb` | title bar and highlighted row                            |
| `selection_end`  | `#rrggbb` | if set, the selection fades top→bottom to this colour    |
| `selection_text` | `#rrggbb` | text on the title bar and highlighted row                |
| `backdrop`       | file name | a BMP in the theme folder, drawn behind every screen     |
| `titlebar`       | `#rrggbb` | title bar colour (flat). Any `titlebar*` key gives the bar its own colours |
| `titlebar_text`  | `#rrggbb` | title and status-bar glyphs (default: `selection_text`)  |
| `titlebar_line`  | `#rrggbb` | the rule under the title bar (default: `selection_end`)  |
| `layout`         | `panel` / `full` | `panel` adds a side panel to the right of the lists (default `full`) |
| `panel_width`    | 80–140    | panel width in px, with `layout = panel` (default 100)   |
| `sprite`         | file name | panel picture on every screen without its own            |
| `sprite_home`, `sprite_music`, `sprite_bt` | file name | panel picture on Home / the Music library / Bluetooth |
| `font`           | file name | text font, an LVGL `.bin` font (see Fonts below)          |
| `font_bold`      | file name | title bar and headings (default: `font`)                 |
| `font_large`     | file name | clocks and big numbers (default: `font_bold`)            |
| `row_height`     | 16–64     | list row height in px (default: the iPod look's 33 px with its 14 px font) |
| `row_dividers`   | `on` / `off` | the hairline under each list row and section header (default `on`) |

- A missing key keeps the iPod look's colour: white background, black text,
  iPod-blue selection, white selection text.
- Colours are exactly six hex digits, and the `#` is optional.
- A malformed value, such as `text = red`, makes the theme unreadable rather
  than half-applied.
- Header tints, dividers and the scrollbar are derived from `text` over
  `background`.

Examples, ready to copy to the SD card:

- [`docs/themes/Dusk/`](docs/themes/Dusk/theme.txt): colours only
- [`docs/themes/Midnight/`](docs/themes/Midnight/theme.txt), `Amber/`: colours only
- [`docs/themes/Paper/`](docs/themes/Paper/theme.txt): colours and a backdrop
- [`docs/themes/Pixel/`](docs/themes/Pixel/theme.txt): side panel with clock
  and date, the Pixel Operator font (CC0, made by `tools/fonts/`) and 27 px
  rows without dividers

```
background     = #1d2021
text           = #ebdbb2
selection      = #d79921
selection_end  = #b57614
selection_text = #1d2021
```

### Side panel

With `layout = panel` every list screen gets narrower (320 − `panel_width`),
and the panel to its right shows:

- the clock and the date (the status-bar clock then hides to avoid showing it
  twice), and
- the screen's sprite, bottom-centred.

Now Playing and the WiFi keyboard stay full width, without the panel.

Sprites:

- Format: BMP, the same formats as the backdrop.
- Transparency: the colour **#ff00ff** (magenta) is transparent, as in Rockbox.
- Size: draw them at 1:1 pixel-art size. They are enlarged by the largest whole
  number that fits the panel (pixel-exact, never blurred). At `panel_width`
  134, a sprite up to 42 px wide and 52 px tall is shown at 3×.
- RAM: the sprite on screen is held enlarged (about 33 KB at 3×), and every
  sprite is kept at 1:1 (about 3.5 KB each). Measured on the EROS Q: a panel
  theme with three 3× sprites uses about 50 KB more RAM than the iPod look.

The panel sits outside the scrolling list, so it costs nothing while you
scroll. The narrower list even scrolls with about 9% less CPU than the iPod
look (EROS Q measurement, a 134 px panel theme).

Beside the panel, a connected Bluetooth device shows only its battery level
(no "Connected"), so its name keeps room.

Changing between a panel theme and a full-width one rebuilds the menus and
returns you to Settings ▸ Display.

### Fonts

`font`, `font_bold` and `font_large` replace the iPod look's 14 px text,
14 px bold and 18 px bold fonts. One `font` key is enough: `font_bold` falls
back to it, and `font_large` to `font_bold`.

- Format: an LVGL binary font made with `lv_font_conv --format bin`
  (`tools/fonts/README.md` has the command). Use `--bpp 4` and no compression;
  compressed fonts are rejected.
- Size: without `row_height`, rows grow with the font's line height (20 px
  Pixel Operator gives 38 px rows). At most 256 KB per file.
- Pixel fonts: use the face's own pixel size or a whole multiple of it
  (Pixel Operator: 16 or 32 px). At an in-between size such as 20 px
  (1.25x) some strokes come out doubled and others vanish.

### Rows

`row_height` sets the height of a one-line list row. The text keeps its own
line height, and the space left over is split above and below it. A row can't
be shorter than its text, so a too-small value gives the tightest rows the
font allows. In a panel theme with `row_height`, the panel's clock and date
line up with the first two rows' text. The Pixel example uses 27 px rows without dividers: 6 rows fit beside the panel where the iPod look shows 6 at 33 px
with its smaller font.
- Characters the font lacks, such as the battery and arrow symbols, come from
  the iPod font.
- A font that is missing, damaged or the wrong format is checked before
  loading and replaced by the iPod font; the rest of the theme still applies.
- The status bar (battery, clock, play icon) uses the theme font only if its
  widest clock still fits beside the title (fixed at 128 px); otherwise it
  keeps the iPod font.
- RAM: the whole font stays loaded, about the size of the `.bin` file plus
  1 KB. The two 16 px Pixel Operator fonts take about 12 KB.
- Speed: use 4 bpp. LVGL unpacks 1 and 2 bpp bit by bit: at 20 px, Pixel
  Operator takes 219 k instructions to rasterise a menu string at 1 bpp,
  111 k at 4 bpp, and the iPod font 120 k (host callgrind).

Changing to a theme with different fonts rebuilds the menus and returns you
to Settings ▸ Display, like a panel change.

### Backdrop

- Format: an uncompressed BMP, 1-, 4- or 8-bit paletted, or 24- or 32-bit.
  RLE-compressed BMPs are not supported.
- Size: at most 320×240, drawn from the top-left corner. Use exactly 320×240
  for a full-screen image.
- The path is relative to the theme folder. A path that leaves the folder
  (`..` or one starting with `/`) is rejected.
- Cost depends on the image:
  - **Flat colour plus a small picture** (a logo, ruled lines): drawn as plain
    colour fills plus the small picture, so it costs about the same as no
    backdrop. Rows that are mostly one colour count as flat. (For a character
    picture, a panel `sprite` is better: it never sits under list text.)
  - **Detailed image** (photo, gradient, pattern across the screen): drawn
    whole. That costs about **+7% CPU while scrolling** on the EROS Q
    (measured with a full-screen 320×240 backdrop), which shows up as battery
    use, plus about 155 KB of RAM while the theme is active.
- Text sits directly on the image, so keep the image dark under light text,
  or light under dark text.

## Rockbox themes (`.cfg`)

Unzip a Rockbox theme for a 320×240 colour target (EROS Q, iPod Video, …) onto
the SD card root, the same way you would for Rockbox:

```
<SD card>/.rockbox/themes/<name>.cfg
<SD card>/.rockbox/backdrops/<name>.bmp
```

These `.cfg` keys are read, using Rockbox's own defaults when a key is missing:

- `foreground color`
- `background color`
- `selector type`: `bar (gradient)`, `bar (color)`, `bar (inverse)` or `pointer`.
  `pointer` is shown as an inverse bar for now.
- `line selector start color`, `line selector end color`, `line selector text color`
- `backdrop`

Everything else is ignored for now, including `font` (Rockbox `.fnt`), `wps`,
`sbs`, `iconset` and `ui viewport`. A `.cfg` must set at least one key above,
or `iconset` / `viewers iconset`: Rockbox's own `rockbox_default_icons.cfg`
sets only those, and loads as Rockbox's default colours (it used to fail with
"Couldn't read theme"). A `.cfg` with none of them still fails.

Third-party themes keep their own licences (mostly CC-BY-SA or GPL), so they
are not shipped with the firmware; each user copies their own onto the SD card.
The `.cfg` reader is our own implementation of the documented format; no
Rockbox code is used.

## For developers

- `src/theme_logic.*`: the format-neutral `struct theme_model`, the BMP loader
  and the `tag:name` ids. The saved id lives in `/data/erosq-ui.theme`.
- `src/nativetheme_logic.*` (`erosq:`) and `src/rbtheme_logic.*` (`rockbox:`)
  are the two importers. A new format is one more importer plus one branch in
  `theme_load()` (`src/ui_theme.c`).
- Host preview:
  ```
  python3 tests/rbthemes/mkcorpus.py /tmp/sd
  EROSQ_SD_ROOT=/tmp/sd EROSQ_THEME=erosq:Dusk build-host/erosq-ui
  ```
- `src/binfont_logic.*`: the structural check every theme font passes before
  `lv_binfont_create` (LVGL's loader trusts the file; a damaged one could
  crash the UI at every boot).
- Tests: `tests/test_nativetheme.c`, `tests/test_rbtheme.c`,
  `tests/test_binfont.c`, run by `tests/run.sh` and CTest.
