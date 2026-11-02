#!/usr/bin/env python3
"""Scan a PC Engine disc image for HuC6280 code, without knowing load addresses.

Two independent properties of real code are used, both of which fail on
compressed/random data:

 1. BOUNDARY TEST -- decode linearly, then check that relative branches land on
    instruction boundaries inside the decoded stretch.
 2. CLUSTER TEST -- the absolute targets of JSR/JMP/branch instructions from one
    program land in a narrow window (a program is loaded into a few 8 KB banks,
    typically $4000-$9FFF), whereas random 16-bit values spread over all 64 KB.

A sector is called code only if both tests pass.
"""
import sys
import os
import argparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import huc6280 as H


def analyse(mem, addr, limit=400):
    seen = {}
    order = []
    cur = addr
    for _ in range(limit):
        ins = H.decode(mem, cur)
        if ins is None:
            break
        seen[cur] = ins
        order.append(cur)
        cur += ins.size
    if not order:
        return dict(bscore=0.0, nb=0, n=0, tscore=0.0, nt=0, span=0, rts=0)
    last = order[-1]
    good = total = 0
    abs_targets = []
    for a in order:
        ins = seen[a]
        if ins.mnem in H.BRANCHES:
            total += 1
            t = H._branch_target(ins.addr, ins.size, ins.raw[-1])
            if addr <= t <= last + 3 and t in seen:
                good += 1
            abs_targets.append(t)
        elif ins.mnem in ("jsr", "jmp") and ins.size == 3 and ins.raw[0] in (0x20, 0x4C):
            abs_targets.append(ins.raw[1] | (ins.raw[2] << 8))
    rts = sum(1 for a in order if seen[a].mnem in ("rts", "rti"))
    if total == 0:
        bscore = 0.0
    else:
        bscore = good / float(total)
    span = (max(abs_targets) - min(abs_targets)) if abs_targets else 0
    # tightness: share of targets inside a 24 KB window
    tight = 0.0
    if abs_targets:
        lo = min(abs_targets)
        tight = sum(1 for t in abs_targets if t - lo < 0x6000) / float(len(abs_targets))
    return dict(bscore=bscore, nb=total, n=len(order), tscore=tight,
                nt=len(abs_targets), span=span, rts=rts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--sector-size", type=int, default=2048)
    ap.add_argument("--pregap", type=int, default=225)
    ap.add_argument("--out", default=None)
    ap.add_argument("--limit", type=int, default=400)
    ap.add_argument("--min-branches", type=int, default=15)
    ap.add_argument("--min-tight", type=float, default=0.85)
    ap.add_argument("--min-bscore", type=float, default=0.80)
    a = ap.parse_args()

    data = open(a.image, "rb").read()
    ss = a.sector_size
    nsec = len(data) // ss
    rows = []
    for s in range(a.pregap, nsec):
        off = s * ss
        mem = {i: data[off + i] for i in range(ss)}
        st = analyse(mem, 0, a.limit)
        ok = (st["bscore"] >= a.min_bscore and st["nb"] >= a.min_branches
              and st["tscore"] >= a.min_tight and st["rts"] >= 1)
        rows.append((s, ok, st))

    runs = []
    cur = None
    for s, ok, st in rows:
        if ok:
            if cur and cur[1] == s - 1:
                cur[1] = s
            else:
                cur = [s, s]
                runs.append(cur)
        else:
            cur = None

    print("scanned %d sectors; %d code-like runs" % (len(rows), len(runs)))
    tot = 0
    for s0, s1 in sorted(runs, key=lambda r: -(r[1] - r[0])):
        n = s1 - s0 + 1
        tot += n
        print("  sectors %5d..%-5d  %3d sec  payload 0x%06X..0x%06X"
              % (s0, s1, n, s0 * ss, (s1 + 1) * ss))
    print("total code-like sectors: %d (%.1f KB)" % (tot, tot * ss / 1024.0))
    if a.out:
        with open(a.out, "w") as f:
            f.write("sector\tcode\tbscore\tnb\tn\ttight\tnt\tspan\trts\n")
            for s, ok, st in rows:
                f.write("%d\t%d\t%.3f\t%d\t%d\t%.3f\t%d\t%d\t%d\n" %
                        (s, 1 if ok else 0, st["bscore"], st["nb"], st["n"],
                         st["tscore"], st["nt"], st["span"], st["rts"]))
        print("wrote", a.out)


if __name__ == "__main__":
    main()
