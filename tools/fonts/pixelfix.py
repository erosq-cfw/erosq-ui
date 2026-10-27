"""pixelfix.py <in.bin> <out.bin> <fixes.txt> <weight> | pixelfix.py --dump <in.bin> <chars>

Hand-corrected glyphs for a 1 bpp LVGL binary font (lv_font_conv --bpp 1
--no-compress --no-kerning), applied before bpp1to4.py. The conversion of
Pixel Operator at 16 px is already 1:1 with the TTF; these fix the face's own
drawing of a few letters. fixes.txt blocks: a line "<weight> <char>" then one
row of '#'/'.' per pixel row; the box (width x height) must match the
original glyph, so metrics and spacing don't change."""
import struct, sys

def sections(b):
    out, off = {}, 0
    while off + 8 <= len(b):
        n, lab = struct.unpack_from("<I4s", b, off)
        out[lab.decode()] = (off, n)
        off += n
    return out

def font(b):
    s = sections(b)
    ho = s["head"][0]
    loc_fmt, bpp, xy, wh, adv, compr = b[ho + 34], b[ho + 37], b[ho + 38], b[ho + 39], b[ho + 40], b[ho + 41]
    assert bpp == 1 and compr == 0, "need an uncompressed 1 bpp font"
    co = s["cmap"][0]
    cmaps = []
    for i in range(struct.unpack_from("<I", b, co + 8)[0]):
        _, start, length, gid, entries, fmt = struct.unpack_from("<IIHHHB", b, co + 12 + 16 * i)
        cmaps.append((start, length, gid, entries, fmt))
    lo = s["loca"][0]
    fmt = "<I" if loc_fmt else "<H"
    esz = 4 if loc_fmt else 2
    go = s["glyf"][0]
    def glyph(ch):
        cp = ord(ch)
        for start, length, gid, entries, f in cmaps:
            if start <= cp < start + length:
                assert f == 2, "only contiguous (format 0-tiny) ranges"
                g = gid + cp - start
                break
        else:
            raise SystemExit("no glyph for %r" % ch)
        bit = (go + struct.unpack_from(fmt, b, lo + 12 + g * esz)[0]) * 8
        def rd(n):
            nonlocal bit
            v = 0
            for _ in range(n):
                v = v << 1 | (b[bit >> 3] >> (7 - (bit & 7)) & 1); bit += 1
            return v
        rd(adv); rd(xy); rd(xy)
        w, h = rd(wh), rd(wh)
        return bit, w, h
    return glyph

def rows_of(b, bit, w, h):
    return ["".join("#" if b[(bit + y * w + x) >> 3] >> (7 - ((bit + y * w + x) & 7)) & 1 else "."
                    for x in range(w)) for y in range(h)]

def parse(path):
    fixes, cur = {}, None
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith(";"):
            cur = None
            continue
        if set(line) <= set("#."):
            cur.append(line)
        else:
            wgt, ch = line.split(" ", 1)
            cur = fixes.setdefault((wgt, ch), [])
    return fixes

def main(argv):
    if argv[0] == "--dump":
        b = open(argv[1], "rb").read(); g = font(b)
        for ch in argv[2]:
            bit, w, h = g(ch)
            print(ch, "%dx%d" % (w, h)); print("\n".join(rows_of(b, bit, w, h)))
        return
    src, dst, fx, weight = argv
    b = bytearray(open(src, "rb").read()); g = font(b)
    for (wgt, ch), rows in parse(fx).items():
        if wgt != weight: continue
        bit, w, h = g(ch)
        assert len(rows) == h and all(len(r) == w for r in rows), "%s %r: box must stay %dx%d" % (wgt, ch, w, h)
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                p = bit + y * w + x
                if c == "#": b[p >> 3] |= 0x80 >> (p & 7)
                else:        b[p >> 3] &= ~(0x80 >> (p & 7)) & 0xff
    open(dst, "wb").write(b)

if __name__ == "__main__":
    main(sys.argv[1:])
