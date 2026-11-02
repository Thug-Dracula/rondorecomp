#!/usr/bin/env python3
"""frame_vs_ref.py -- port pictures against the reference corpus, with the pairing
conventions made explicit and *testable* rather than assumed.

CONVENTIONS (project's, not this tool's):
  * port file f(N) holds the picture of frame N-1 (`XLATE_FRAME_DIR` writes at the
    frame boundary, before that frame's rows are rendered), so port frame k is f(k+1);
  * reference shots/fNNNNNNNN.raw is the picture *produced by* frame N;
  * the capture is 232 rows, the port draws 224, and on this route row 0 is row 0.

A convention is only believable if the tool can show it working: on frames where both
machines are on the same still, `--check` reports **0 differing pixels**. If it cannot,
the pairing is wrong and every other number from this tool is noise.

Colour: both sides are reduced to the VCE's own 5-bit levels (expand v<<3 | v>>5 for the
reference, take the port's 8-bit output as rendered) and compared per channel at a
tolerance (default 8, the project's calibrated figure). No 8-bit round trip.

Also prints the luminance correlation, which is the robust instrument when a fade or a
palette offset makes the absolute pixel count useless: correlation ~1 with a big pixel
count means "same scene, different brightness"; correlation ~0 means "different scene".

Usage:
  frame_vs_ref.py --ppm DIR --frames 7199,7699 [--ref DIR] [--tol 8] [--search 80]
  frame_vs_ref.py --ppm DIR --check 199,299,399      # expect 0 differing pixels
"""
import argparse
import os
import sys

import numpy as np

W, H = 256, 224
_REF = os.environ.get("REFDATA") or os.path.join(
    os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "..")), "refdata")
REF = os.path.join(_REF, "shots")


def _expand5(v):
    o = v << 3
    return o | (o >> 5)


def ref_frame(refdir, n):
    p = os.path.join(refdir, "f%08d.raw" % n)
    if not os.path.exists(p):
        return None
    a = np.fromfile(p, dtype="<u2").reshape(232, 256)
    r = _expand5((a >> 10) & 31)
    g = _expand5((a >> 5) & 31)
    b = _expand5(a & 31)
    return np.stack([r, g, b], axis=-1).astype(np.float64)[:H]


def port_frame(ppmdir, k):
    p = os.path.join(ppmdir, "f%05d.ppm" % (k + 1))
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        f.readline()
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(), dtype=np.uint8).reshape(h, w, 3).astype(np.float64)


def diff_pixels(a, b, tol):
    return int((np.abs(a - b).max(axis=2) > tol).sum())


def corr(a, b):
    x = a.mean(axis=2).ravel()
    y = b.mean(axis=2).ravel()
    x = x - x.mean()
    y = y - y.mean()
    den = np.linalg.norm(x) * np.linalg.norm(y)
    if den:
        return float(x.dot(y) / den)
    # Two flat pictures have no variance to correlate.  Equal flat pictures are the
    # same picture (a black screen is a screen); different flat ones are not comparable
    # this way, and 0.0 says "no information" rather than "no match".
    return 1.0 if np.array_equal(a, b) else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ppm", required=True, help="XLATE_FRAME_DIR")
    ap.add_argument("--ref", default=REF)
    ap.add_argument("--frames", help="comma separated port frame numbers")
    ap.add_argument("--check", help="frames that must report 0 differing pixels")
    ap.add_argument("--tol", type=int, default=8)
    ap.add_argument("--search", type=int, default=0,
                    help="also find the best reference frame within +-N frames")
    a = ap.parse_args()

    todo = a.check if a.check else a.frames
    if not todo:
        sys.exit("give --frames or --check")
    frames = [int(x) for x in todo.split(",")]

    if a.check:
        bad = 0
        for k in frames:
            p = port_frame(a.ppm, k)
            r = ref_frame(a.ref, k)
            if p is None or r is None:
                print("frame %d: missing picture (port %s, ref %s)"
                      % (k, p is None, r is None))
                bad += 1
                continue
            d = diff_pixels(p, r, a.tol)
            cor = corr(p, r)
            print("frame %d: %d differing pixels (tolerance %d), correlation %.4f"
                  % (k, d, a.tol, cor))
            if d:
                bad += 1
        print("\n%d of %d frames differ" % (bad, len(frames)))
        return 1 if bad else 0

    print("%6s %10s %10s %8s" % ("frame", "diffpx", "corr", "best"))
    for k in frames:
        p = port_frame(a.ppm, k)
        r = ref_frame(a.ref, k)
        if p is None:
            print("%6d  no port picture" % k)
            continue
        if r is None:
            print("%6d  no reference frame" % k)
            continue
        line = "%6d %10d %10.4f %8s" % (k, diff_pixels(p, r, a.tol), corr(p, r), "-")
        if a.search:
            best = (corr(p, r), k)
            for m in range(k - a.search, k + a.search + 1):
                q = ref_frame(a.ref, m)
                if q is None:
                    continue
                c = corr(p, q)
                if c > best[0]:
                    best = (c, m)
            line += " %+5d (corr %.4f)" % (best[1] - k, best[0])
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
