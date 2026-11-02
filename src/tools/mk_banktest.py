#!/usr/bin/env python3
"""Build the bank-switch test programs.

Why this file exists: the whole point of keying translation by (bank, address) is
that *the same address* holds different code depending on which bank is paged in.
These programs make that happen and make the difference visible:

  unit     -- two tiny modules at $4000, one in bank $DB and one in bank $DC.  Each
              records a different byte, so if the dispatcher runs the wrong module
              the recorded bytes say so.  Then it pages in bank $DD, which has no
              translated code, and must stop with a recorded "untranslated" event.
  kern_lo  -- pages in bank $DB and calls $4000 with the *real* game kernel image
              there; the trace's first instruction must be the kernel's first byte.
  kern_hi  -- the same with bank $DC, i.e. the kernel's second 8 KB.  The two must
              execute different bytes at the same address.

The kernel is 12 KB (record 67), so at the overlay base the loader's own convention
uses ($DB) it occupies banks $DB and $DC; that is a placement choice, stated here
rather than hidden -- the boot program does not yet reach the kernel.
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

DRIVER_BASE = 0x8000
BANK_DB, BANK_DC, BANK_DD = 0xDB, 0xDC, 0xDD
COUNTER_A = 0xDF00
COUNTER_B = 0xDF01

MODULE_A = [("lda", "#$A5"), ("sta", "$%04X" % COUNTER_A), ("rts", None)]
MODULE_B = [("lda", "#$5A"), ("sta", "$%04X" % COUNTER_B), ("rts", None)]

DRIVER_UNIT = [
    ("lda", "#$%02X" % BANK_DB), ("tam", "#$04"),
    ("jsr", "$4000"),
    ("lda", "#$%02X" % BANK_DC), ("tam", "#$04"),
    ("jsr", "$4000"),
    ("lda", "#$%02X" % BANK_DD), ("tam", "#$04"),
    ("jsr", "$4000"),          # no code in this bank: must be reported, not run
    "spin:", ("bra", "spin"),
]

DRIVER_LO = [
    ("lda", "#$%02X" % BANK_DB), ("tam", "#$04"),
    ("jsr", "$4000"),
    "spin:", ("bra", "spin"),
]

DRIVER_HI = [
    ("lda", "#$%02X" % BANK_DC), ("tam", "#$04"),
    ("jsr", "$4000"),
    "spin:", ("bra", "spin"),
]


def build(name, driver_src, extra_images, out_c, out_json, lg):
    driver, labels = asm6280.assemble(driver_src, DRIVER_BASE)
    mod_a, _ = asm6280.assemble(MODULE_A, 0x4000)
    mod_b, _ = asm6280.assemble(MODULE_B, 0x4000)

    images = [{"bank": recompiler.DEFAULT_MPR[(DRIVER_BASE >> 13) & 7],
               "base": DRIVER_BASE, "data": driver, "load": True,
               "name": name + "_driver"}]
    for (bank, base, data, nm) in extra_images:
        images.append({"bank": bank, "base": base, "data": data, "load": True,
                       "name": nm})

    text, tr, _banks = recompiler.build_program(images, DRIVER_BASE, lg)
    with open(out_c, "w") as f:
        f.write(text)

    banks = {}
    for (b, _o) in tr.units:
        banks[b] = banks.get(b, 0) + 1
    info = {"entry": DRIVER_BASE, "driver": list(driver), "banks": banks,
            "units": {("%02X_%04X" % (b, o)): bytes(i.b).hex()
                      for (b, o), i in sorted(tr.units.items()) if o < 0x40}}
    with open(out_json, "w") as f:
        json.dump(info, f, indent=1)
    lg.log("banktest %s: %d units in banks %s"
           % (name, len(tr.units), " ".join("$%02X" % b for b in sorted(banks))))
    print("%-8s %d instruction units, banks %s"
          % (name, len(tr.units), " ".join("$%02X" % b for b in sorted(banks))))
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=os.path.join(XL, "out"))
    a = ap.parse_args()
    lg = xlog.announce()

    mod_a, _ = asm6280.assemble(MODULE_A, 0x4000)
    mod_b, _ = asm6280.assemble(MODULE_B, 0x4000)
    # The kernel's first 16 KB, sliced from the user's data track (slice_disc.py).
    sys.path.insert(0, os.path.normpath(os.path.join(XL, "..", "tests")))
    import harness as H
    kernel = open(H.block_paths()["kernel"], "rb").read()

    os.makedirs(a.outdir, exist_ok=True)
    build("unit", DRIVER_UNIT,
          [(BANK_DB, 0x4000, mod_a, "moduleA"),
           (BANK_DC, 0x4000, mod_b, "moduleB")],
          os.path.join(a.outdir, "bank_unit.c"),
          os.path.join(a.outdir, "bank_unit_expect.json"), lg)
    build("kern_lo", DRIVER_LO,
          [(BANK_DB, 0x4000, kernel, "kernel")],
          os.path.join(a.outdir, "bank_klo.c"),
          os.path.join(a.outdir, "bank_klo_expect.json"), lg)
    # The second 8 KB of the kernel, placed at bank $DC: that is what the same
    # address would hold if the overlay loader had paged in the kernel's second
    # bank instead of its first.
    build("kern_hi", DRIVER_HI,
          [(BANK_DC, 0x4000, kernel[8192:], "kernel_hi")],
          os.path.join(a.outdir, "bank_khi.c"),
          os.path.join(a.outdir, "bank_khi_expect.json"), lg)

    print("kernel bytes at $4000: bank $DB starts %02X %02X, bank $DC starts %02X %02X"
          % (kernel[0], kernel[1], kernel[8192], kernel[8193]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
