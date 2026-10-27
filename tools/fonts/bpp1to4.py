"""bpp1to4.py <in.bin> <out.bin>: repack a 1 bpp LVGL binary font as 4 bpp.

A pixel font at a size that isn't a multiple of its design grid (Pixel
Operator at 20 px) is only crisp without anti-aliasing, i.e. lv_font_conv
--bpp 1. But LVGL unpacks 1 bpp bit by bit, so it rasterises about twice as
slowly as 4 bpp. Same pixels, faster format: every set bit becomes nibble 15.
Uncompressed fonts without kerning only (lv_font_conv --no-compress
--no-kerning); the result is checked by the UI's binfont_check on load."""
import struct, sys

def sections(b):
    out, off = {}, 0
    while off + 8 <= len(b):
        n, lab = struct.unpack_from("<I4s", b, off)
        out[lab.decode()] = (off, n)
        off += n
    return out

def bits_reader(b, start_bit):
    pos = start_bit
    def read(n):
        nonlocal pos
        v = 0
        for _ in range(n):
            v = v << 1 | (b[pos >> 3] >> (7 - (pos & 7)) & 1)
            pos += 1
        return v
    return read

def main(src, dst):
    b = open(src, "rb").read()
    s = sections(b)
    ho, hl = s["head"]
    hdr = bytearray(b[ho:ho + hl])
    tables = struct.unpack_from("<H", hdr, 12)[0]
    loc_fmt, bpp, xy, wh, adv, compr = hdr[34], hdr[37], hdr[38], hdr[39], hdr[40], hdr[41]
    assert bpp == 1 and compr == 0 and tables == 3, "need an uncompressed 1 bpp font without kerning"
    lo, ll = s["loca"]
    cnt = struct.unpack_from("<I", b, lo + 8)[0]
    fmt = "<I" if loc_fmt else "<H"
    esz = 4 if loc_fmt else 2
    offs = [struct.unpack_from(fmt, b, lo + 12 + i * esz)[0] for i in range(cnt)]
    go, gl = s["glyf"]
    nbits = adv + 2 * xy + 2 * wh
    glyphs = []
    for i in range(cnt):
        rd = bits_reader(b, (go + offs[i]) * 8)   # offsets count from the section start (its 8-byte prefix included)
        fields = [rd(adv), rd(xy), rd(xy), rd(wh), rd(wh)]
        w, h = fields[3], fields[4]
        px = [rd(1) for _ in range(w * h)] if i and w * h else []
        # 4 bpp: header bits, then one nibble per pixel, packed MSB-first, byte-padded
        out, acc, n = bytearray(), 0, 0
        def put(v, k):
            nonlocal acc, n
            for j in range(k - 1, -1, -1):
                acc = acc << 1 | (v >> j & 1); n += 1
                if n == 8: out.append(acc); acc = n = 0
        for v, k in zip(fields, [adv, xy, xy, wh, wh]):
            put(v & ((1 << k) - 1), k)
        for p in px:
            put(15 if p else 0, 4)
        if n: out.append(acc << (8 - n))
        glyphs.append(bytes(out))
    body, newoffs = bytearray(), []
    for g in glyphs:
        newoffs.append(8 + len(body))
        body += g
    if not loc_fmt and newoffs[-1] > 0xFFFF:
        raise SystemExit("glyf over 64 KB: rebuild the 1 bpp font with a 32-bit loca")
    hdr[37] = 4
    loca = struct.pack("<I4sI", 12 + cnt * esz, b"loca", cnt) + b"".join(struct.pack(fmt, o) for o in newoffs)
    glyf = struct.pack("<I4s", 8 + len(body), b"glyf") + body
    co, cl = s["cmap"]
    open(dst, "wb").write(bytes(hdr) + b[co:co + cl] + loca + glyf)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
