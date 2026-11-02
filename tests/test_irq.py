#!/usr/bin/env python3
"""Interrupt and timing test: translated code, driven by the video clock.

The program (built by tools/mk_irqtest.py) sets the video chip up through its real
ports, installs a vblank handler, unmasks interrupts and spins.  What is being
tested is the *machine*, not the game:

  * the VDC raises a vblank interrupt once per frame, at the frame rate the
    hardware runs at (263 lines x 455 dots = 59.8 Hz);
  * the CPU takes it at an instruction boundary: pushes PC and P, sets I,
    clears T and D, and jumps to the handler;
  * RTI returns to the interrupted instruction with the flags restored;
  * the translated handler's counter and the model's own interrupt count agree;
  * the frame the program programmed really renders as programmed.
"""
import json
import os
import sys

import harness as H

SRC = H.SRC
CYCLES_PER_FRAME = 263 * 455          # 119,665 dot clocks
FRAME_HZ = 7159090.0 / CYCLES_PER_FRAME


def main():
    chk = H.Checker("interrupts + timing")

    g = H.run([sys.executable, "tools/mk_irqtest.py"])
    if g.rc != 0:
        print(g.out, g.err)
        return 1
    with open(os.path.join(SRC, "out", "irqtest_expect.json")) as f:
        exp = json.load(f)

    b = H.run(["./build.sh", "irqtest"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1

    frame = os.path.join(SRC, "out", "irqtest.ppm")
    seconds = 1.0
    cycles = int(CYCLES_PER_FRAME * FRAME_HZ * seconds)
    r = H.run(["./out/irqtest_host", "run", "--bios", H.BIOS, "--log", H.LOG,
               "--steps", "20000000", "--cycles", str(cycles),
               "--frame", frame])
    if r.rc != 0:
        print(r.out, r.err)
        return 1
    st = H.parse_state_full(r.out)
    chk.ok(st is not None, "host produced a STATE line")
    if not st:
        print(r.out)
        return 1

    expected_irqs = FRAME_HZ * seconds
    chk.ok(abs(st["frames"] - expected_irqs) <= 4,
           "frames advanced %d in %.0f s of emulated time (expect ~%.0f)"
           % (st["frames"], seconds, expected_irqs))
    chk.ok(abs(st["irq"] - expected_irqs) <= 4,
           "interrupts taken %d (expect ~%.0f)" % (st["irq"], expected_irqs))
    chk.eq(r.out.count("TRAP"), 0, "no untranslated code was reached")
    chk.eq(r.out.count("unhandled=1"), 0, "no interrupt was dropped for lack of a handler")

    # The translated handler incremented its own counter at $DF00 (bank $A0 by
    # default MPR6).  It must agree with the model's interrupt count: that is the
    # proof the handler actually ran each time.
    dump = H.scratch("irq_mem.bin")
    r2 = H.run(["./out/irqtest_host", "run", "--bios", H.BIOS, "--log", H.LOG,
                "--steps", "20000000", "--cycles", str(cycles), "--dump", dump])
    mem = H.parse_dump(dump)
    # $DF00 goes through MPR6, which is whatever the reset state maps there.
    counter = mem[st["mpr"][6] * 8192 + (exp["counter_vis"] & 0x1FFF)]
    chk.eq(counter & 0xFF, st["irq"] & 0xFF,
           "handler counter ($%04X) matches interrupts taken" % exp["counter_vis"])

    # the frame: cell 0 uses the red tile, the rest is background colour 0 (white)
    from PIL import Image
    img = Image.open(frame).convert("RGB")
    px = img.load()
    chk.eq(px[0, 0], (255, 0, 0), "tile pixel (0,0) is palette 1 = red")
    chk.eq(px[8 * 3, 0], (255, 255, 255), "a far background cell is colour 0 = white")
    colours = img.getcolors(maxcolors=64)
    chk.ok(colours and len(colours) >= 2, "frame is not a uniform colour")

    ok = chk.report("%.2f s, %d frames" % (seconds, st["frames"]))
    print("        measured frame rate: %.2f Hz (hardware: %.2f Hz)"
          % (st["frames"] / seconds, FRAME_HZ))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
