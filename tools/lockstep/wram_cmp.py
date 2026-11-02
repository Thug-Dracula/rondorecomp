#!/usr/bin/env python3
"""wram_cmp.py -- the whole 8 KB work RAM, port against reference, frame by frame.

WHY THIS EXISTS. Two bytes (`$31` and `$222D`) cannot tell "in lockstep" from "in
trouble": `$222D` is written by the vblank handler every frame, so it keeps ticking on
a machine whose program has stopped progressing.  A comparison that cannot see the
other 8,190 bytes cannot tell those two cases apart.

Both sides are already in the reference's layout, so this needs no conversion:
  port       XLATE_RAMSTREAM=PATH   8,192 B per frame, block N = start of frame N,
                                    block 0 = power-on (xl_ramstream_begin)
  reference  $REFDATA/ram.bin       the same, 8,192 B per frame

Usage:
  wram_cmp.py OURS.bin FROM TO [--ref RAM.BIN] [--every N] [--tsv OUT] [--shift K]

`--shift K` compares the port's frame N against the reference's frame N+K. It
exists because the two machines can be in step while their frame *counters* are
not: the port's boot can cost frames the console's did not, and then the same
event happens at a different frame number on each machine. The offset is
measured (tools/lockstep/clock_offset.py, whole 8 KB, every frame), never chosen,
and a shifted comparison is not evidence that the unshifted one is clean -- both
are reported.

Prints one line per frame: differing bytes, the first differing address (as a work-RAM
address, `$2000`-based), and `$2031`/`$222D` on both machines. Exit status 0 always --
this is a measuring instrument; the gate is the test suite (`tests/`).
"""
import argparse
import sys

BLK = 8192
_REF = os.environ.get("REFDATA") or os.path.join(
    os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "..")), "refdata")
REF = os.path.join(_REF, "ram.bin")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("lo", type=int)
    ap.add_argument("hi", type=int, help="inclusive")
    ap.add_argument("--ref", default=REF)
    ap.add_argument("--every", type=int, default=1, help="print every Nth frame")
    ap.add_argument("--tsv", help="also write every frame to this file, tab separated")
    ap.add_argument("--shift", type=int, default=0,
                    help="compare port frame N with reference frame N+SHIFT")
    a = ap.parse_args()

    f = open(a.ref, "rb")
    g = open(a.ours, "rb")
    if a.lo + a.shift < 0:
        sys.exit("shift %d would read before the reference starts" % a.shift)
    f.seek((a.lo + a.shift) * BLK)
    g.seek(a.lo * BLK)

    tsv = open(a.tsv, "w") if a.tsv else None
    if tsv:
        tsv.write("frame\tdiffering_bytes\tfirst_differing_addr"
                  "\tref_2031\tport_2031\tref_222D\tport_222D\n")

    print("%5s %7s %8s  %s" % ("frame", "differ", "first", "$2031 r/p  $222D r/p")
          + ("   (ref frame = port frame %+d)" % a.shift if a.shift else ""))
    nbad = 0
    first_bad = None
    worst = (0, None)
    total = 0
    for n in range(a.lo, a.hi + 1):
        ours = g.read(BLK)
        ref = f.read(BLK)
        if len(ours) < BLK or len(ref) < BLK:
            print("short read at frame %d (stream ends)" % n)
            break
        total += 1
        diff = [i for i in range(BLK) if ours[i] != ref[i]]
        d = len(diff)
        first = "$%04X" % (0x2000 + diff[0]) if d else "-"
        if d:
            nbad += 1
            if first_bad is None:
                first_bad = n
            if d > worst[0]:
                worst = (d, n)
        if tsv:
            tsv.write("%d\t%d\t%s\t%02X\t%02X\t%02X\t%02X\n"
                      % (n, d, first, ref[0x31], ours[0x31], ref[0x22D], ours[0x22D]))
        if d or n % a.every == 0:
            print("%5d %7d %8s    %02X/%-2.2s     %02X/%-2.2s"
                  % (n, d, first, ref[0x31], "%02X" % ours[0x31],
                     ref[0x22D], "%02X" % ours[0x22D]))
    if tsv:
        tsv.close()
    print("\nframes compared %d (%d..%d); identical %d; differing %d"
          % (total, a.lo, a.hi, total - nbad, nbad))
    print("first differing frame %s; worst %s with %d bytes"
          % (first_bad if first_bad is not None else "none",
             worst[1] if worst[1] is not None else "none", worst[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
