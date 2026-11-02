#!/usr/bin/env python3
"""Build the interrupt/timing test program: translated code that must see vblank IRQs.

The program is deliberately tiny and self-contained, so that a failure points at
the interrupt path and nothing else:

  * it sets up the video chip through the same ports the game uses
    (MWR / HDR / VDR / VSR / CR), which proves the register interface works;
  * it writes one tile and one palette entry, so the frame has a known pixel in it
    (the renderer test proves the *renderer*; this proves the *plumbing*);
  * it installs a handler at the port's IRQ hook location ($2200) and enables
    vblank interrupts;
  * the handler acknowledges the interrupt and increments a counter, then returns
    with RTI;
  * the main loop spins, so every instruction boundary is available for an
    interrupt to be taken.

Expectation: one vblank interrupt per frame, i.e. about 60 per emulated second.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.dirname(HERE)
sys.path.insert(0, XL)
sys.path.insert(0, HERE)

import asm6280                                      # noqa: E402
import isa                                          # noqa: E402
import log as xlog                                  # noqa: E402
import recompiler                                   # noqa: E402

BASE = 0x8000
COUNTER = 0xDF00

SRC = [
    # ---- MWR = $50: background map 64 cells wide (this game's setting)
    ("st0", "#$09"), ("lda", "#$50"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),
    # ---- HDR = $041F: 256 pixels wide
    ("st0", "#$0B"), ("lda", "#$1F"), ("sta", "$0002"), ("lda", "#$04"), ("sta", "$0003"),
    # ---- VDR = 224 lines
    ("st0", "#$0D"), ("lda", "#$E0"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),
    # ---- VSR = $0F02
    ("st0", "#$0C"), ("lda", "#$02"), ("sta", "$0002"), ("lda", "#$0F"), ("sta", "$0003"),
    # ---- BXR = 0, BYR = 0
    ("st0", "#$07"), ("lda", "#$00"), ("sta", "$0002"), ("sta", "$0003"),
    ("st0", "#$08"), ("lda", "#$00"), ("sta", "$0002"), ("sta", "$0003"),

    # ---- palette: colour 0 white, colour 1 red
    ("lda", "#$00"), ("sta", "$0402"), ("sta", "$0403"),
    ("lda", "#$FF"), ("sta", "$0404"), ("lda", "#$01"), ("sta", "$0405"),
    ("lda", "#$01"), ("sta", "$0402"), ("lda", "#$00"), ("sta", "$0403"),
    ("lda", "#$38"), ("sta", "$0404"), ("lda", "#$00"), ("sta", "$0405"),

    # ---- one solid-colour-1 tile at charname 1 (VRAM words 16..31)
    ("st0", "#$00"), ("lda", "#$10"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),
    ("st0", "#$02"),
    ("ldx", "#$08"),
    "l1:",
    ("lda", "#$FF"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),
    ("dex", None), ("bne", "l1"),
    ("ldx", "#$08"),
    "l2:",
    ("lda", "#$00"), ("sta", "$0002"), ("sta", "$0003"),
    ("dex", None), ("bne", "l2"),
    # background cell 0 -> charname 1, palette 0
    ("st0", "#$00"), ("lda", "#$00"), ("sta", "$0002"), ("sta", "$0003"),
    ("st0", "#$02"), ("lda", "#$01"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),

    # ---- CR = $C8: background on, sprites on, vblank interrupt on
    ("st0", "#$05"), ("lda", "#$C8"), ("sta", "$0002"), ("lda", "#$00"), ("sta", "$0003"),

    # ---- install the handler where the BIOS's IRQ path actually dispatches:
    # $E86D is `jmp ($2202)`, so the pointer lives at $2202/$2203 (verified against
    # the oracle's work RAM, which holds $4177 there at the logo).
    ("lda", "#<irq"), ("sta", "$2202"),
    ("lda", "#>irq"), ("sta", "$2203"),
    ("lda", "#$00"), ("sta", "$2204"),
    ("stz", "$%04X" % COUNTER),
    # unmask the video interrupt in the CPU's interrupt controller.  The controller
    # is $1402 (mask, INVERTED: 1 = disabled) and $1403 (status/acknowledge), not the
    # $0C02/$0C03 -- those belong to the timer.  The VDC is IRQ1, so its mask bit is
    # bit 1; writing $00 clears every mask bit, which is what the console's own BIOS
    # does at frame 0.
    ("lda", "#$00"), ("sta", "$1402"),
    ("cli", None),
    "spin:",
    ("bra", "spin"),

    "irq:",
    ("pha", None),
    ("lda", "$0000"),          # read VDC status: acknowledges the interrupt
    ("inc", "$%04X" % COUNTER),
    ("pla", None),
    ("rti", None),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(XL, "out", "irqtest.c"))
    ap.add_argument("--expect", default=os.path.join(XL, "out", "irqtest_expect.json"))
    a = ap.parse_args()
    lg = xlog.announce()

    code, labels = asm6280.assemble(SRC, BASE)

    # disassemble back: a wrong encoding must not slip through
    listing = []
    pos = 0
    while pos < len(code):
        ins = isa.decode(lambda x: code[x - BASE] if 0 <= x - BASE < len(code) else None,
                         BASE + pos)
        if ins is None:
            raise SystemExit("decode failed at $%04X" % (BASE + pos))
        listing.append("%04X  %s" % (ins.addr, ins.text))
        pos += ins.size
    if pos != len(code):
        raise SystemExit("listing stopped early")

    images = [{"bank": recompiler.DEFAULT_MPR[(BASE >> 13) & 7], "base": BASE,
               "data": code, "load": True, "name": "irqtest"}]
    # The handler is reached only through the interrupt hook, which is an indirect
    # jump as far as static discovery is concerned, so name it as an entry point.
    text, tr, _space = recompiler.build_program(images, BASE, lg,
                                                extra_seeds=[labels["irq"]])
    with open(a.out, "w") as f:
        f.write(text)

    with open(a.expect, "w") as f:
        json.dump({"entry": BASE, "counter_vis": COUNTER, "irq_pc": labels["irq"],
                   "spin_pc": labels["spin"], "listing": listing}, f, indent=1)
    lg.log("irqtest: %d bytes, %d instruction functions, handler at $%04X"
           % (len(code), len(tr.units), labels["irq"]))
    print("wrote %s and %s (%d bytes, handler $%04X)"
          % (a.out, a.expect, len(code), labels["irq"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
