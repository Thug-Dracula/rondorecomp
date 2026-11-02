#!/usr/bin/env python3
"""Slice a dumped RAM image (the XLST1 file tools/mkstate.py writes) into physical
8 KB banks, so the translator can be pointed at the code a *running* machine holds.

Why this exists: the disc-derived banks are the boot chain's modules.  A stage module
is loaded from the disc later, into the same bank numbers, and is different code
(bank $6B in the stage-1 capture differs from the disc's bank $6B in 7,642 of its
8,192 bytes).  Static translation of the disc image therefore covers the loader, not
the stage; translating the *state's* banks covers the stage.

Output (under --out):
  bNN.bin        one file per physical bank present in the image ($68-$7F, and $F8
                 work RAM if the caller asks for it)
  bytes.args     one BANK:ADDR=PATH per line, ADDR 0, i.e. the bank placed at its own
                 offset 0 -- given to recompiler.py --bytes (bytes, not an entry point)
  mapped.args    one BANK:ADDR=PATH per line for the banks the state has *mapped*, at
                 the page it has them mapped at -- given to recompiler.py --code (an
                 entry point, and the page the machine is really using)
  meta.txt       pc=.. mpr=.. banks=..

The distinction matters: an unmapped bank has no page we can observe, and a bank
seeded at a guessed page gets its units pinned there (the translator keeps the first
page it saw and reports later ones as conflicts), which would make the real placement
undiscoverable.  Bytes-only banks are translated when the code's own `tam` maps them
in, or when a discovered entry point names them.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(XL, "tools"))
sys.path.insert(0, XL)

import oracle_state as osd                                   # noqa: E402

BANK_SIZE = 8192
CDRAM_BASE = 0x68            # SysCardRAM's first physical bank (see xl_load_state_file)
CDRAM_BANKS = 24


def read_image(path):
    """Return (pc, a, x, y, s, p, mpr, banks{phys_bank: bytes}) from an XLST1 file."""
    buf = open(path, "rb").read()
    if buf[:8] != b"XLST1\0\0\0":
        raise SystemExit("%s is not an XLST1 image (magic %r)" % (path, buf[:8]))
    off = 8
    pc = int.from_bytes(buf[off:off + 2], "little"); off += 2
    a, x, y, s, p = buf[off:off + 5]; off += 5
    mpr = list(buf[off:off + 8]); off += 8
    off += 12 * 2                 # VDC registers
    off += 512 * 2                # palette
    off += 32768 * 2              # VRAM
    off += 256 * 2                # sprite table
    banks = {}
    for i in range(CDRAM_BANKS):
        b = CDRAM_BASE + i
        banks[b] = buf[off:off + BANK_SIZE]
        off += BANK_SIZE
    banks[0xF8] = buf[off:off + BANK_SIZE]      # BaseRAM / work RAM
    return {"pc": pc, "a": a, "x": x, "y": y, "s": s, "p": p,
            "mpr": mpr, "banks": banks}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-workram", action="store_true",
                    help="also emit bank $F8 (code the game built at run time; the "
                         "translator must never treat it as a fixed image by default)")
    a = ap.parse_args(argv)

    st = read_image(a.image)
    os.makedirs(a.out, exist_ok=True)
    lines_bytes, lines_mapped = [], []
    written = []
    for bank, data in sorted(st["banks"].items()):
        if bank == 0xF8 and not a.with_workram:
            continue
        rel = "b%02X.bin" % bank
        with open(os.path.join(a.out, rel), "wb") as f:
            f.write(data)
        written.append(bank)
    # A bank the state maps at a page: that page is observed, not guessed.
    placed = set()
    for page in range(8):
        bank = st["mpr"][page]
        if bank in st["banks"] and bank not in placed and bank != 0xF8:
            placed.add(bank)
            lines_mapped.append("0x%02X:0x%04X=%s"
                                % (bank, page << 13,
                                   os.path.join(a.out, "b%02X.bin" % bank)))
    # Everything else is bytes only: no page is observable for it, so it must not be
    # pinned to a guess.  (A bank in both lists would register its image twice.)
    for bank in written:
        if bank in placed or bank == 0xF8:
            continue
        lines_bytes.append("0x%02X:0x0000=%s"
                           % (bank, os.path.join(a.out, "b%02X.bin" % bank)))
    with open(os.path.join(a.out, "bytes.args"), "w") as f:
        f.write("\n".join(lines_bytes) + "\n")
    with open(os.path.join(a.out, "mapped.args"), "w") as f:
        f.write("\n".join(lines_mapped) + "\n")
    with open(os.path.join(a.out, "meta.txt"), "w") as f:
        f.write("pc=%04X\n" % st["pc"])
        f.write("mpr=%s\n" % " ".join("%02X" % v for v in st["mpr"]))
        f.write("banks=%s\n" % " ".join("%02X" % b for b in written))

    print("state %s: pc=$%04X mpr=%s"
          % (a.image, st["pc"], " ".join("%02X" % v for v in st["mpr"])))
    print("  banks sliced: %s" % " ".join("$%02X" % b for b in written))
    print("  mapped (entry points, observed pages): %s"
          % (", ".join(l.split("=")[0] for l in lines_mapped) or "none"))
    print("  pc is in bank $%02X (page %d)"
          % (st["mpr"][st["pc"] >> 13], st["pc"] >> 13))

    # How much of this differs from the disc-derived translation, when one exists.
    diff_lines = []
    for bank in written:
        d = os.path.join(XL, "out", "block", "b%02X.bin" % bank)
        if not os.path.exists(d):
            continue
        ref = open(d, "rb").read()
        n = sum(1 for i in range(min(len(ref), BANK_SIZE))
                if ref[i] != st["banks"][bank][i])
        diff_lines.append("$%02X:%d" % (bank, n))
    if diff_lines:
        print("  bytes differing from the disc-derived bank: %s" % " ".join(diff_lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
