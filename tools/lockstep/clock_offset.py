#!/usr/bin/env python3
"""clock_offset.py -- how far ahead or behind the port's *machine time* is, from
the whole 8 KB of work RAM rather than from one variable.

WHY THIS EXISTS. `$31` and `$222D` cannot answer "are the two machines in step":
both are written every frame whether or not the program is progressing.  This asks
the question
directly -- for the port's frame N, which reference frame's 8 KB does it look
like? -- and refuses to answer when the answer would be a coincidence.

The winner is reported only when it beats the runner-up by `--margin` bytes and
the frame's state actually changes: on a still screen (or in a run of identical
RAM blocks) several offsets fit equally well and none of them means anything.

A *constant* offset with a stable winning frame is a clock offset: no rate error,
one machine is simply further along. An offset that wanders is a rate difference,
which is a different bug and is reported as a range.

Usage:
  clock_offset.py OURS.bin FROM TO [--ref RAM.BIN] [--span 40] [--every N] [--stride N]
  clock_offset.py rs.bin 7000 7400

The number printed is a LAG: ref frame = port frame - lag, so a positive lag means
the port's machine is that many frames BEHIND the console at the same frame number.
It is reported, never applied: tools/lockstep/wram_cmp.py --shift does the shifted
comparison, and only with a lag this tool measured.
"""
import argparse
import sys

import numpy as np

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
    ap.add_argument("--span", type=int, default=40, help="search +/- this many frames")
    ap.add_argument("--margin", type=int, default=8,
                    help="winner must beat the runner-up by this many bytes")
    ap.add_argument("--every", type=int, default=25)
    ap.add_argument("--stride", type=int, default=1, help="score every Nth frame")
    a = ap.parse_args()

    ref = np.memmap(a.ref, dtype=np.uint8, mode="r").reshape(-1, BLK)
    ours = np.fromfile(a.ours, dtype=np.uint8).reshape(-1, BLK)
    if a.hi >= ours.shape[0]:
        sys.exit("stream holds %d frames, cannot compare to %d" % (ours.shape[0], a.hi))

    print("%5s %6s %7s %7s  %s" % ("frame", "lag", "differs", "runner", "verdict"))
    offsets = []
    for n in range(a.lo, a.hi + 1, a.stride):
        row = ours[n]
        scored = []
        for k in range(-a.span, a.span + 1):
            m = n + k
            if m < 0 or m >= ref.shape[0]:
                continue
            scored.append((int((row != ref[m]).sum()), k))
        scored.sort()
        best, k = scored[0]
        runner = scored[1][0] if len(scored) > 1 else best
        decisive = (runner - best) >= a.margin
        # Frames whose RAM is identical to their neighbours carry no time
        # information at all; say so instead of reporting a meaningless k.
        same_neighbour = bool(np.array_equal(row, ours[n - 1])) if n > 0 else False
        verdict = "lag %+d" % -k if decisive else "-"
        if not decisive:
            verdict = "ambiguous"
        elif same_neighbour and abs(k) > 0:
            verdict = "lag %+d (static RAM: weak)" % -k
        else:
            offsets.append(-k)
        if n == a.lo or n % a.every == 0 or n == a.hi or (not decisive and n % 5 == 0):
            print("%5d %6s %7d %7d  %s"
                  % (n, "%+d" % -k if decisive else "?", best, runner, verdict))

    if offsets:
        vals = np.array(offsets)
        print("\ndecisive frames: %d of %d" % (len(vals), (a.hi - a.lo) // a.stride + 1))
        print("lag (port frames behind the console) min %+d  median %+d  max %+d"
              % (vals.min(), int(np.median(vals)), vals.max()))
        uniq, cnt = np.unique(vals, return_counts=True)
        print("counts: " + "  ".join("%+d:%d" % (u, c) for u, c in zip(uniq, cnt)))
        if vals.min() == vals.max():
            print("constant offset -- no rate error in this window")
        else:
            print("offset varies -- a rate difference, not a fixed delay")
    else:
        print("\nno decisive frame: every offset fits equally well (RAM is static)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
