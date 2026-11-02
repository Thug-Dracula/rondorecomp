#!/usr/bin/env python3
"""state_cmp.py - compare our work RAM against the console corpus, byte for byte.

    state_cmp.py OURS.bin [--console $REFDATA/ram.bin] [--base 0]
                 [--lag 0] [--window 500] [--show 8] [--json out.json]

Comparing one byte ($2047, the game's frame counter) answers "is the clock in step"; it
cannot see a *state* divergence, because a machine can keep counting frames while running
entirely different code.

This tool compares all 8,192 bytes and reports, per window, how many frames are
byte-identical and which frame is the **first** that is not — with the differing byte
offsets of that frame, so the next question ("which variable moved?") starts from data.

    OURS.bin      written by XLATE_RAMSTREAM (8,192 bytes per frame)
    --base B      console frame that our frame 0 corresponds to (route-dependent)
    --lag L       console frame = base + our_frame + L; scan with --scan

Exit status 0 only if every compared frame is byte-identical.
"""
import argparse
import json
import os
import sys

import numpy as np

CONSOLE = os.path.join(
    os.environ.get("REFDATA") or os.path.join(
        os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..")), "refdata"),
    "ram.bin")


def load(path):
    return np.memmap(path, dtype=np.uint8, mode="r").reshape(-1, 8192)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("--console", default=CONSOLE)
    ap.add_argument("--base", type=int, default=0)
    ap.add_argument("--lag", type=int, default=None)
    ap.add_argument("--scan", action="store_true", help="search lag -12..+12 first")
    ap.add_argument("--window", type=int, default=500)
    ap.add_argument("--bar", type=int, default=0,
                    help="report the first frame with more than this many differing bytes")
    ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--json")
    args = ap.parse_args(argv[1:])

    ours = load(args.ours)
    cons = load(args.console)
    print(f"ours {ours.shape[0]} frames, console {cons.shape[0]} frames")

    def score(lag, lo, hi):
        """Return (frames, identical, max differing bytes, first frame over the bar)."""
        same = 0
        n = 0
        worst = 0
        first = None
        for k in range(lo, hi):
            cf = args.base + k + lag
            if cf < 0 or cf >= cons.shape[0]:
                continue
            n += 1
            nd = int((ours[k] != cons[cf]).sum())
            worst = max(worst, nd)
            if nd == 0:
                same += 1
            elif first is None and nd > args.bar:
                first = k
        return n, same, worst, first

    lag = args.lag
    if lag is None or args.scan:
        best = None
        for L in range(-12, 13):
            n, same, _, _ = score(L, 0, min(ours.shape[0], 4000))
            if n and (best is None or same > best[1]):
                best = (L, same, n)
        if best is None:
            print("no comparable frames")
            return 2
        lag = best[0]
        print(f"best lag over the first 4,000 frames: {lag:+d} "
              f"({best[1]}/{best[2]} byte-identical)\n")
    else:
        print(f"lag {lag:+d}\n")

    print(f"  {'window':>13}  {'identical':>9}  {'max bytes':>9}  {'first over bar':>14}")
    total = same = 0
    first_any = None
    for lo in range(0, ours.shape[0], args.window):
        hi = min(lo + args.window, ours.shape[0])
        n, s, worst, first = score(lag, lo, hi)
        if not n:
            continue
        total += n
        same += s
        if first is not None and first_any is None:
            first_any = first
        print(f"  {lo:5d}-{hi - 1:5d}  {s:5d}/{n:<3d}  {worst:9d}  {('-' if first is None else first):>14}")

    print(f"\n  total: {same}/{total} frames byte-identical")

    if first_any is not None:
        cf = args.base + first_any + lag
        d = np.nonzero(ours[first_any] != cons[cf])[0]
        print(f"  first divergence: our {first_any} = console {cf}, "
              f"{len(d)} bytes differ")
        print(f"    offsets {[hex(int(x)) for x in d[:args.show]]}"
              f"{' ...' if len(d) > args.show else ''}")
        print(f"    ours {[f'{ours[first_any][x]:02X}' for x in d[:args.show]]}")
        print(f"    cons {[f'{cons[cf][x]:02X}' for x in d[:args.show]]}")
        if args.json:
            json.dump({"lag": lag, "first": int(first_any),
                       "offsets": [int(x) for x in d]}, open(args.json, "w"))
            print(f"\nwrote {args.json}")
        return 1

    print("\nPASS: every compared frame is byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
