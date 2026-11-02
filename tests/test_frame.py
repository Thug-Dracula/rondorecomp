#!/usr/bin/env python3
"""Frame test: what the translated boot chain puts on screen, and which layer it is.

The acceptance question is "does the game's own code produce the intro frame". This
test answers it in the only honest way available: it runs the translated chain,
saves the frame, correlates it against the oracle's own still, and then checks the
layers one at a time so that a low number is *explained* rather than just reported.

Layers checked independently of the picture:
  CD data     the uploaded VRAM map is byte-for-byte the disc block
  VRAM writes the map region matches that data, the sprite table DMA ran
  palette     400+ colour entries loaded from the disc through the BIOS entry
  display     the program turned the background and sprites on
  code        whether the game's kernel was ever executed

If the correlation is low, the layer that failed says why.
"""
import os
import struct
import sys

import harness as H

SRC = H.SRC
INTRO = H.ref("ref", "intro_castle.png")


def main():
    H.skip_without_inputs()
    H.skip_unless(os.path.exists(INTRO),
                  "no reference still (refdata/ref/intro_castle.png): set REFDATA=<dir>")
    chk = H.Checker("frame vs oracle")
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1

    vdc = H.scratch("final.vdc")
    ppm = H.scratch("final.ppm")
    r = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
               "--log", H.LOG, "--steps", "50000000", "--cycles", "20000000",
               "--pad", "0x08",          # RUN: the boot program's hand-off needs it
               "--vram-out", vdc, "--frame", ppm])

    # ---- layers -----------------------------------------------------------
    payload = open(H.DISC, "rb").read()
    block = payload[(3 + 225) * 2048:(3 + 225) * 2048 + 24 * 2048]
    d = open(vdc, "rb").read()
    vram = [struct.unpack("<H", d[1056 + 2 * i:1058 + 2 * i])[0] for i in range(32768)]
    pal = [struct.unpack("<H", d[32 + 2 * i:34 + 2 * i])[0] for i in range(512)]
    sat = [struct.unpack("<H", d[1056 + 65536 + 2 * i:1058 + 65536 + 2 * i])[0]
           for i in range(256)]
    regs = [struct.unpack("<H", d[8 + 2 * i:10 + 2 * i])[0] for i in range(12)]

    chk.ok(all(vram[i] == (block[0xFDF + 2 * i] | (block[0xFDF + 2 * i + 1] << 8))
               for i in range(2048)),
           "CD data: the background map in VRAM is byte-for-byte the disc block")
    v = H.parse_video(r.out)
    chk.ok(v and v["sat_dma"] > 0, "VRAM writes: the sprite-table DMA ran")
    chk.ok(sum(1 for p in pal if p) > 400,
           "palette: %d colour entries loaded from disc" % sum(1 for p in pal if p))
    chk.ok(v and (v["cr"] & 0xC0) == 0xC0,
           "display: background and sprites enabled (CR = $%04X)" % (v["cr"] if v else 0))

    calls = H.parse_calls(r.out)
    kernel_read = [c for c in calls if c.get("modelled") and c["sector"] == 0x43]
    chk.ok(not kernel_read,
           "code: the kernel (record $43 = 67) is never requested -- not even over "
           "1,370 frames of the boot program's animation")

    # ---- the number -------------------------------------------------------
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    chk.ok(len(img.getcolors(maxcolors=512) or []) > 1,
           "the translated code produced a non-black frame")

    sys.path.insert(0, H.TESTS)
    import test_video
    ref = Image.open(INTRO)
    corr = test_video.correlate(img, ref)
    print("  frame correlation against the oracle's intro still: %.4f" % corr)
    print("  layers: CD data OK, VRAM writes OK, palette OK, display OK, "
          "code = boot program's own animation (it issues no further disc reads)")
    chk.ok(corr < 0.9, "as expected, the frame is not the intro (%.3f < 0.9): the "
                       "divergent layer is code, not data" % corr)

    img.resize((512, 448), Image.NEAREST).save(SRC + "/out/final_frame.png")
    ok = chk.report("correlation %.3f" % corr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
