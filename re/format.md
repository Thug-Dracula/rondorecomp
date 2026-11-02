# The VRAM/BAT "painting program" stream format

A byte-stream format found in the game's kernel, verified against the disassembly and
implemented in `decompress.py`; it decodes real disc data at 7–12:1. It is **not** the
tile-art codec: it paints map data, not tile bitplanes (see the last section).

Routine: **`$644D`** in the game kernel module (disc sector 67, loaded at
`$4000-$6FFF`). Entry point #65 of 81 in that module's vector table.
The interpreter loop is at **`$6476`**. Full listing: `dis/03_kernel_4000.asm`.

## How it is called

```
A  = index of a "graphic block"     (into a pointer table whose address is in
                                     RAM $8014/$8015)
Y  = index of a sub-entry in the block's own table

$644D: take table[B] -> $00/$01          ; 16-bit pointer table
       take table2[A] -> $02/$03         ; the stream itself
       jsr $402D  (= $5B6C)              ; VDC reg 5 = CR, write 0
       jsr $651E                         ; read 2 bytes from stream, set VRAM
                                         ;   address (reg 0 = MAWR), then select
                                         ;   reg 2 (VWR) for data
       -> loop at $6476
```

`$4021`/`$4027`/`$402D`/`$402D` are stubs that jump through the module's own
vector table:
* `$4021` (`$5B61`) = select VDC register 0 (MAWR low)
* `$4027` (`$5B59`) = select VDC register 2 (VWR low)
* `$402D` (`$5B6C`) = select VDC register 5 (CR) and write a value

CR is set to 0, which means "advance the VRAM address by 1 word per data
write". So the stream is a sequence of 16-bit VRAM words with occasional
address moves.

## Command set

Every command is one byte, read straight from the stream, in this order of
tests (this ordering matters: it is why `$E1-$EF` and `$F1-$FD` behave
differently from what you would guess):

| bytes | meaning |
|---|---|
| `FF` | end of stream (`rts` at `$6420`) |
| `FE lo hi` | set VRAM address to `lo | hi<<8`, and select the VWR data port |
| `F0` | advance VRAM address by `$40` words (i.e. start the next 4-tile block) |
| `E0 lo hi` | set the stream read pointer to `lo | hi<<8` (relocate into another bank) |
| `D0-$FF` (anything not matched above) | set the low nibble of the "high byte" register `$04` to `cmd & $0F` |
| `C0-$CF` | set the high nibble of `$04` to `cmd & $0F` |
| `00 b0 b1` | emit one literal word `b0 | b1<<8` |
| `01-$1F b` | emit `b | $04<<8` — `cmd & $1F` times |
| `20-$3F b` | emit `b, b+1, b+2, ...` — `cmd & $1F` times |
| `40-$5F b` | emit `b, b+1, b, b+1, ...` (alternating) — `cmd & $1F` times |
| `60-$7F b` | as `40-$5F` but starting downward |
| `80-$BF b` | emit `b | $04<<8` — `cmd & $3F` times |

The alternating commands (`$40-$7F`) are a dither: two adjacent values written
pixel by pixel. That single feature explains the "complementary byte pairs"
the earlier investigation noticed in the raw data (`7F 80`, `79 86`): a run of
`b` and `b+1` pairs, viewed as bytes, looks like near-complements.

`$C0-$DF` exist only to set the high byte of the emitted word once, then emit
hundreds of words with cheap one-byte commands. When that high byte holds a
palette/index field for every word in a run, the stream compresses enormously.

## Sizes actually measured

`decompress.py --offset <offset>` on real disc blocks:

| disc sector | input bytes | output words | output bytes | ratio | terminator |
|---|---|---|---|---|---|
| 9580 | 22,595 | 78,973 | 157,946 | 7.0:1 | clean `FF` |
| 8274 | 20,518 | 69,410 | 138,820 | 6.8:1 | clean `FF` |
| 2727 | 10,562 | 54,395 | 108,790 | 10.3:1 | clean `FF` |
| 1639 | 6,767 | 24,002 | 48,004 | 7.1:1 | clean `FF` |
| 46 | 2,218 | 13,882 | 27,764 | 12.5:1 | clean `FF` |

3,045 of the 10,149 data sectors parse to a clean `FF` terminator. Because a
random byte survives the `FF` test only about 256 commands, a 20 KB stream
cannot be a coincidence — these are genuine streams.

**A count not to quote.** A looser test — a clean `FF` after 64 output words — is
passed by most random data, so it counts nothing. The per-stream sizes in the table
above stand; a strict re-test over 32 records (121,427 bytes of input, expanding
2.3:1 to 16.8:1) is what the numbers rest on.

## Why this is not the tile-art codec

The output words, placed at their VRAM addresses and decoded as PC Engine 4bpp
tiles, come out as regular vertical stripe fields, not as pictures. Two
independent checks confirm the decoder is right and the *interpretation as
tile art* is wrong:

1. Placing the words at their own VRAM addresses (honouring `$FE` and `$F0`)
   changes nothing; the stripes are in the data, not an artefact of the
   sequencing.
2. For sector 100 the emitted words are long runs of one value with the
   address stepping `$0000, $0040, $0080, ...` — a run-filled rectangular
   grid, i.e. the shape of **map data** (one word per map cell) or of a
   flat-shaded area, not tile bitplanes.

Most likely role: this is the routine that paints a **background/name table
or a flat-shaded panel** — one 16-bit value per destination cell, with the
high byte (palette / attribute field) set once per band and then run-length
coded. That is a real decompressor and a real piece of Rondo's pipeline, but
the stage *tile art* is produced by something else.
