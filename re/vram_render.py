#!/usr/bin/env python3
"""Render 16-bit VRAM words as PC Engine 4bpp tiles, contact sheets.

Model (from the VDC and the game's own upload code):

  * VRAM is 16-bit words, addressed linearly.
  * A 4bpp 8x8 tile is 16 consecutive words = 32 bytes.
  * Row r uses words 2r and 2r+1:
        word 2r   : low byte = bitplane 0, high byte = bitplane 1
        word 2r+1 : low byte = bitplane 2, high byte = bitplane 3
  * pixel colour index at column c, row r =
        bit c of plane0 | bit c of plane1 << 1 | bit c of plane2 << 2 |
        bit c of plane3 << 3

This is the layout `decompress.words_to_tile_bytes(..., layout="pce")` produces.
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow")

RAINBOW = [(0, 0, 0), (60, 0, 0), (0, 100, 0), (70, 70, 0),
           (0, 0, 150), (130, 0, 130), (0, 120, 120), (150, 150, 150),
           (95, 95, 95), (255, 70, 70), (70, 255, 70), (255, 255, 70),
           (90, 90, 255), (255, 90, 255), (90, 255, 255), (255, 255, 255)]


def tile_pixels(words, t):
    """Return 8x8 colour indices for tile t of the word list."""
    base = 16 * t
    rows = []
    for r in range(8):
        w0 = words[base + 2 * r]
        w1 = words[base + 2 * r + 1]
        p0, p1 = w0 & 0xFF, (w0 >> 8) & 0xFF
        p2, p3 = w1 & 0xFF, (w1 >> 8) & 0xFF
        row = [((p0 >> c) & 1) | (((p1 >> c) & 1) << 1) |
               (((p2 >> c) & 1) << 2) | (((p3 >> c) & 1) << 3)
               for c in range(8)]
        rows.append(row)
    return rows


def sheet(words, out, count=None, scale=2, per_row=32, pal=None):
    pal = pal or RAINBOW
    ntiles = len(words) // 16
    if count is not None:
        ntiles = min(ntiles, count)
    tx = per_row
    ty = (ntiles + tx - 1) // tx
    step = 8 * scale + scale
    img = Image.new("RGB", (tx * step, ty * step), (20, 20, 30))
    px = img.load()
    for t in range(ntiles):
        rows = tile_pixels(words, t)
        ox = (t % tx) * step
        oy = (t // tx) * step
        for r in range(8):
            for c in range(8):
                col = pal[rows[r][c]]
                for dy in range(scale):
                    for dx in range(scale):
                        px[ox + c * scale + dx, oy + r * scale + dy] = col
    img.save(out)
    return out, ntiles


if __name__ == "__main__":
    import argparse
    import decompress as D
    ap = argparse.ArgumentParser()
    ap.add_argument("payload")
    ap.add_argument("--offset", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--max-words", type=int, default=200000)
    ap.add_argument("--count", type=int, default=1024)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--per-row", type=int, default=32)
    ap.add_argument("--out", default="sheet.png")
    a = ap.parse_args()
    data = open(a.payload, "rb").read()
    r = D.decode(data, a.offset, max_words=a.max_words)
    print("stream: %d words, reason=%s" % (len(r.words), r.reason))
    o, n = sheet(r.words, a.out, a.count, a.scale, a.per_row)
    print("wrote", o, n, "tiles")
