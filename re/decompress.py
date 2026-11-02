#!/usr/bin/env python3
"""Rondo of Blood (PC Engine Super CD-ROM2) graphics decompressor.

Reversed from the game's own code: the routine at **$644D** in the disk's
game kernel (disc sector 67, loaded at $4000-$6FFF).  The interpreter loop
itself is at $6476.  See format.md for the annotated disassembly.

WHAT IT IS
----------
The stream is not an LZ77 stream.  It is a tiny *painting program* for the
VDC: a stream of one-byte commands that emit 16-bit VRAM words and
occasionally move the destination.  Long stretches of a picture are
produced by "emit this byte N times" / "emit this byte, then +1, then -1..."
commands, and the high byte of every emitted word comes from a separate
"current high byte" register set by dedicated commands.  That is why the
compressed data is full of byte pairs that look like complements of each
other (7F/80, 79/86, 07/F9): the low byte of a run and the high-byte
register are two halves of the same pixel word.

COMMAND SET (verified against the disassembly, all opcodes inclusive)
--------------------------------------------------------------------
    $FF            end of stream
    $FE lo hi      set VRAM destination address to (lo, hi) and select the
                   data port (jsr $651E)
    $F0            advance destination by $40 words: "start the next block
                   of 4 tiles" (jsr $6538)
    $E0 lo hi      set the stream read pointer to (lo, hi) - a relocation
                   into another bank of the same data blob
    $C0-$CF        high byte of emitted word: high nibble = cmd & $0F
    $D0-$DF        high byte of emitted word: low  nibble = cmd & $0F
    $00 b0 b1      emit one literal word  b0 | b1<<8
    $01-$1F b      emit b | hi<<8  (cmd & $1F) times
    $20-$3F b      emit b, b+1, b+2 ... (cmd & $1F) times
    $40-$5F b      emit b, b-1, b+1, b-2 ... alternating, (cmd & $1F) times
    $60-$7F b      as $40-$5F but starting downward
    $80-$BF b      emit b | hi<<8  (cmd & $3F) times

HOW A TILE COMES OUT
--------------------
A PC Engine 8x8 4bpp tile is 32 bytes = 16 VRAM words.  Two words per row:
word 2r = (plane0, plane1) of row r and word 2r+1 = (plane2, plane3).
So the byte stream for a tile, row by row, is p0,p1,p2,p3 - four bytes per
row - which is what `words_to_tile_bytes` produces.
"""
import argparse
import os
import sys

CMD_END = 0xFF
CMD_SETADDR = 0xFE
CMD_NEXTBLOCK = 0xF0
CMD_SETPTR = 0xE0


class Result:
    __slots__ = ("words", "positions", "end", "reason", "events")

    def __init__(self):
        self.words = []          # emitted 16-bit words, in order
        self.positions = []      # VRAM word address of each word
        self.end = 0             # stream pointer after the stream
        self.reason = ""         # why decoding stopped
        self.events = []         # (cmd, arg, nwords) for analysis

    @property
    def nbytes(self):
        return len(self.words) * 2


def decode(data, ptr=0, vram=0, hi=0, max_words=None, max_steps=1 << 20,
           addr_from_stream=True):
    """Run one stream.  `data` is a bytearray of the whole blob.

    `ptr` is the offset in `data` where the stream starts; $E0 relocations are
    interpreted as offsets into the same blob (the kernel's bank window makes
    those look like addresses, but the payload is contiguous in practice).
    """
    r = Result()
    steps = 0
    n = len(data)
    # $644D does jsr $651E before entering the loop, and $651E reads two
    # bytes from the stream as the VRAM destination address.
    if addr_from_stream:
        if ptr + 1 >= n:
            r.reason = "truncated header"
            r.end = ptr
            return r
        vram = data[ptr] | (data[ptr + 1] << 8)
        ptr += 2
    while True:
        steps += 1
        if steps > max_steps:
            r.reason = "step limit"
            break
        if max_words is not None and len(r.words) > max_words:
            r.reason = "word limit"
            break
        if ptr < 0 or ptr >= n:
            r.reason = "pointer out of range"
            break
        cmd = data[ptr]
        ptr += 1

        if cmd == CMD_END:
            r.reason = "end"
            break
        elif cmd == CMD_SETADDR:
            if ptr + 1 >= n:
                r.reason = "truncated $FE"
                break
            vram = data[ptr] | (data[ptr + 1] << 8)
            ptr += 2
            r.events.append((cmd, vram, 0))
        elif cmd == CMD_NEXTBLOCK:
            vram += 0x40
            r.events.append((cmd, None, 0))
        elif cmd == CMD_SETPTR:
            if ptr + 1 >= n:
                r.reason = "truncated $E0"
                break
            ptr = data[ptr] | (data[ptr + 1] << 8)
            r.events.append((cmd, ptr, 0))
            continue
        elif 0xD0 <= cmd <= 0xFF:
            # the code sends every $D0-$FF value it has not already matched
            # to $6417, so $E1-$EF and $F1-$FD set the low nibble too
            hi = (hi & 0xF0) | (cmd & 0x0F)
            r.events.append((cmd, hi, 0))
        elif 0xC0 <= cmd <= 0xCF:
            hi = (hi & 0x0F) | ((cmd & 0x0F) << 4)
            r.events.append((cmd, hi, 0))
        elif cmd == 0x00:
            if ptr + 1 >= n:
                r.reason = "truncated literal"
                break
            w = data[ptr] | (data[ptr + 1] << 8)
            ptr += 2
            r.words.append(w)
            r.positions.append(vram)
            vram += 1
            r.events.append((cmd, w, 1))
        elif 0x01 <= cmd <= 0x1F:
            if ptr >= n:
                r.reason = "truncated run"
                break
            b = data[ptr]
            ptr += 1
            cnt = cmd & 0x1F
            w = b | (hi << 8)
            for _ in range(cnt):
                r.words.append(w)
                r.positions.append(vram)
                vram += 1
            r.events.append((cmd, b, cnt))
        elif 0x20 <= cmd <= 0x3F:
            if ptr >= n:
                r.reason = "truncated run"
                break
            b = data[ptr]
            ptr += 1
            cnt = cmd & 0x1F
            for i in range(cnt):
                r.words.append(((b + i) & 0xFF) | (hi << 8))
                r.positions.append(vram)
                vram += 1
            r.events.append((cmd, b, cnt))
        elif 0x40 <= cmd <= 0x5F:
            if ptr >= n:
                r.reason = "truncated run"
                break
            b = data[ptr]
            ptr += 1
            cnt = cmd & 0x1F
            up = True
            v = b
            for _ in range(cnt):
                r.words.append((v & 0xFF) | (hi << 8))
                r.positions.append(vram)
                vram += 1
                v = (b + (v - b + 1 if up else v - b - 1))
                up = not up
            r.events.append((cmd, b, cnt))
        elif 0x60 <= cmd <= 0x7F:
            if ptr >= n:
                r.reason = "truncated run"
                break
            b = data[ptr]
            ptr += 1
            cnt = cmd & 0x1F
            v = b
            up = False
            for _ in range(cnt):
                r.words.append((v & 0xFF) | (hi << 8))
                r.positions.append(vram)
                vram += 1
                v = b + 1 if not up else b - 1
                up = not up
            r.events.append((cmd, b, cnt))
        elif 0x80 <= cmd <= 0xBF:
            if ptr >= n:
                r.reason = "truncated run"
                break
            b = data[ptr]
            ptr += 1
            cnt = cmd & 0x3F
            w = b | (hi << 8)
            for _ in range(cnt):
                r.words.append(w)
                r.positions.append(vram)
                vram += 1
            r.events.append((cmd, b, cnt))
        else:
            r.reason = "impossible command $%02X" % cmd
            break
    r.end = ptr
    return r


def words_to_tile_bytes(words, layout="pce"):
    """16 VRAM words -> 32 bytes of an 8x8 4bpp tile.

    layout 'pce' (default): row r occupies 4 bytes  p0,p1,p2,p3  taken from
    word 2r (low, high) and word 2r+1 (low, high) - the order the VDC reads.
    layout 'planar16'      : bytes 0-15 = planes 0/1, 16-31 = planes 2/3
                             (the SNES-style arrangement, kept for comparison).
    """
    out = bytearray()
    if layout == "pce":
        for r in range(8):
            w0 = words[2 * r] & 0xFFFF
            w1 = words[2 * r + 1] & 0xFFFF
            out += bytes((w0 & 0xFF, (w0 >> 8) & 0xFF,
                          w1 & 0xFF, (w1 >> 8) & 0xFF))
    else:
        p01 = bytearray()
        p23 = bytearray()
        for r in range(8):
            w0 = words[2 * r] & 0xFFFF
            w1 = words[2 * r + 1] & 0xFFFF
            p01 += bytes((w0 & 0xFF, (w0 >> 8) & 0xFF))
            p23 += bytes((w1 & 0xFF, (w1 >> 8) & 0xFF))
        out = p01 + p23
    return bytes(out)


def decode_to_words(data, ptr, **kw):
    return decode(data, ptr, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("payload", help="data payload bin (2048-byte sectors)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), required=True,
                    help="payload byte offset of the stream")
    ap.add_argument("--words", type=int, default=None,
                    help="stop after this many output words")
    ap.add_argument("--length", type=lambda s: int(s, 0), default=None,
                    help="stop after this many output BYTES (rounded up to words)")
    ap.add_argument("--out", default=None, help="write output words as bytes (LE)")
    ap.add_argument("--tiles", default=None, help="write output as 32-byte tiles")
    ap.add_argument("--layout", default="pce", choices=["pce", "planar16"])
    ap.add_argument("--trace", type=int, default=0, help="print first N events")
    a = ap.parse_args()

    data = open(a.payload, "rb").read()
    maxw = a.words
    if a.length is not None:
        need = (a.length + 1) // 2
        maxw = need if maxw is None else min(maxw, need)
    r = decode(data, a.offset, max_words=maxw)
    print("stream at payload 0x%X: %d words (%d bytes), end 0x%X (consumed %d), reason=%s"
          % (a.offset, len(r.words), r.nbytes, r.end, r.end - a.offset, r.reason))
    for i, e in enumerate(r.events[:a.trace]):
        print("   %3d: cmd $%02X arg=%s words=%d" %
              (i, e[0], ("$%X" % e[1]) if e[1] is not None else "-", e[2]))
    if a.out:
        body = b"".join(bytes((w & 0xFF, w >> 8)) for w in r.words)
        if a.length is not None:
            body = body[:a.length]
        with open(a.out, "wb") as f:
            f.write(body)
        print("wrote", a.out)
    if a.tiles:
        with open(a.tiles, "wb") as f:
            ntile = len(r.words) // 16
            for t in range(ntile):
                f.write(words_to_tile_bytes(r.words[16 * t:16 * t + 16], a.layout))
        print("wrote %s (%d tiles)" % (a.tiles, len(r.words) // 16))


if __name__ == "__main__":
    main()
