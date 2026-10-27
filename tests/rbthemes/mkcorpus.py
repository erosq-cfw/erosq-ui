#!/usr/bin/env python3
"""Write a small theme test corpus (our own files and generated backdrops, no
third-party theme assets) laid out as on the SD card: Rockbox themes under
<root>/.rockbox/, erosq themes under <root>/themes/ (incl. the THEMES.md
example). Host screenshots point EROSQ_SD_ROOT at <root>.
usage: mkcorpus.py <root>"""
import os, shutil, struct, sys

root = sys.argv[1]
themes = os.path.join(root, ".rockbox", "themes")
backdrops = os.path.join(root, ".rockbox", "backdrops")
os.makedirs(themes, exist_ok=True)
os.makedirs(backdrops, exist_ok=True)


def bmp24(path, w, h, px):
    """px(x, y) -> (r, g, b); y=0 is the top row."""
    stride = (w * 3 + 3) & ~3
    out = bytearray()
    for y in range(h - 1, -1, -1):           # bottom-up, as most BMPs are
        row = bytearray()
        for x in range(w):
            r, g, b = px(x, y)
            row += bytes((b, g, r))
        out += row + bytes(stride - len(row))
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(out), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(out), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + dib + out)


def bmp8(path, w, h, pal, idx):
    """8-bit paletted: pal = [(r,g,b)...], idx(x, y) -> palette index."""
    stride = (w + 3) & ~3
    out = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytes(idx(x, y) for x in range(w))
        out += row + bytes(stride - len(row))
    p = b"".join(bytes((b, g, r, 0)) for r, g, b in pal)
    off = 54 + len(p)
    hdr = struct.pack("<2sIHHI", b"BM", off + len(out), 0, 0, off)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 8, 0, len(out), 2835, 2835, len(pal), 0)
    with open(path, "wb") as f:
        f.write(hdr + dib + p + out)


# Midnight: dark, gradient selector, 24-bit backdrop (navy -> black + faint
# diagonal pinstripes so transparency is obvious in screenshots).
def midnight(x, y):
    t = y / 239
    r, g, b = int(16 * (1 - t)), int(28 * (1 - t)), int(64 * (1 - t) + 8)
    if (x + y) % 16 == 0:
        r, g, b = r + 14, g + 14, b + 22
    return r, g, b
bmp24(os.path.join(backdrops, "midnight.bmp"), 320, 240, midnight)

# Paper: light, flat colour selector, 8-bit backdrop (warm paper with soft
# horizontal ruling).
paper_pal = [(245, 240, 228), (232, 225, 208)]
bmp8(os.path.join(backdrops, "paper.bmp"), 320, 240, paper_pal,
     lambda x, y: 1 if y % 24 == 23 else 0)

cfgs = {
    "Midnight": """# erosq test corpus: dark theme with a backdrop
wps: /.rockbox/wps/midnight.wps
font: /.rockbox/fonts/15-Adobe-Helvetica.fnt
foreground color: E0E6F0
background color: 000000
selector type: bar (gradient)
line selector start color: 3FA9F5
line selector end color: 1B5E9E
line selector text color: FFFFFF
backdrop: /.rockbox/backdrops/midnight.bmp
""",
    "Paper": """# erosq test corpus: light theme, flat selector, 8-bit backdrop
foreground color: 3A3226
background color: F5F0E4
selector type: bar (color)
line selector start color: C0392B
line selector text color: FFFFFF
backdrop: /.rockbox/backdrops/paper.bmp
""",
    "Amber Inverse": """# erosq test corpus: inverse selector, no backdrop
foreground color: F2AA4C
background color: 101820
selector type: bar (inverse)
backdrop: -
""",
    "Broken": """# erosq test corpus: nothing we understand -> "Couldn't read theme"
wps: /.rockbox/wps/broken.wps
""",
}
for name, text in cfgs.items():
    with open(os.path.join(themes, name + ".cfg"), "w") as f:
        f.write(text)
# erosq (native) themes: the docs/themes examples, plus one with a backdrop.
native = os.path.join(root, "themes")
example = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "themes", "Dusk")
for t in os.listdir(os.path.dirname(example)):    # every docs/themes example
    shutil.copytree(os.path.join(os.path.dirname(example), t), os.path.join(native, t), dirs_exist_ok=True)
mint = os.path.join(native, "Mint Night")
os.makedirs(mint, exist_ok=True)
bmp24(os.path.join(mint, "backdrop.bmp"), 320, 240,
      lambda x, y: (8, 24 + y // 12, 20 + y // 16) if (x // 20 + y // 20) % 2 else (6, 20 + y // 12, 16 + y // 16))
with open(os.path.join(mint, "theme.txt"), "w") as f:
    f.write("""# erosq test corpus: native theme with a backdrop
background     = #06140f
text           = #c8f5e0
selection      = 2ec4a0
selection_text = #04120d
backdrop       = backdrop.bmp
""")
print("corpus in", root)
