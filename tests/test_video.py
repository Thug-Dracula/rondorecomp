#!/usr/bin/env python3
"""Video test: our renderer vs the emulator's own screenshots.

Method (this is the honest version of "does the video shim work"): take a captured
console state -- the video chip's VRAM, palette, sprite table and registers, exactly
as the emulator had them on a given frame -- load it into *our* device model, render
one frame with *our* renderer, and correlate that frame against the screenshot the
emulator produced at the same moment.

That isolates the video path: if the numbers are high, the format constants, the
palette decoding, tile addressing, sprite composition and geometry in video.c are
right.  It says nothing about the CPU side, which has its own tests.
"""
import argparse
import glob
import json
import os
import sys

import harness as H

SRC = H.SRC
REF = H.ref("ref")


def correlate_fit(a_img, b_img):
    """Correlation after a per-channel linear fit.

    Diagnostic, not a pass criterion: it removes any pure palette/brightness
    difference, so a high value with a low raw correlation means "same picture,
    different colours" -- which is what a capture pair taken either side of a
    palette fade looks like.
    """
    from PIL import Image
    import math
    b = b_img.convert("RGB")
    px = b.load()
    rows = [y for y in range(b.height)
            if any(px[x, y] != (0, 0, 0) for x in range(0, b.width, 4))]
    if rows:
        b = b.crop((0, min(rows), b.width, max(rows) + 1))
    b = b.resize(a_img.size, Image.BOX)

    def chan(img, i):
        return [p[i] for p in img.convert("RGB").getdata()]

    out = 1.0
    for i in range(3):
        la, lb = chan(a_img, i), chan(b, i)
        ma, mb = sum(la) / len(la), sum(lb) / len(lb)
        sab = sum((x - ma) * (y - mb) for x, y in zip(la, lb))
        saa = sum((x - ma) ** 2 for x in la) or 1
        # residual after fitting b onto a
        resid = sum(((mb + sab / saa * (x - ma)) - y) ** 2 for x, y in zip(la, lb))
        total = sum((y - mb) ** 2 for y in lb) or 1
        out = min(out, 1.0 - resid / total)
    return out


def mean_abs_error(a_img, b_img):
    from PIL import Image
    b = b_img.convert("RGB")
    px = b.load()
    rows = [y for y in range(b.height)
            if any(px[x, y] != (0, 0, 0) for x in range(0, b.width, 4))]
    if rows:
        b = b.crop((0, min(rows), b.width, max(rows) + 1))
    b = b.resize(a_img.size, Image.BOX)
    la = list(a_img.convert("L").getdata())
    lb = list(b.convert("L").getdata())
    return sum(abs(x - y) for x, y in zip(la, lb)) / len(la)


def correlate(a_img, b_img):
    """Grayscale correlation, with the emulator shot trimmed of black borders and
    rescaled to our frame size (the emulator's window is not 1:1 with our raster)."""
    from PIL import Image
    import math
    b = b_img.convert("RGB")
    px = b.load()
    rows = [y for y in range(b.height)
            if any(px[x, y] != (0, 0, 0) for x in range(0, b.width, 4))]
    if rows:
        b = b.crop((0, min(rows), b.width, max(rows) + 1))
    b = b.resize(a_img.size, Image.BOX)
    la = list(a_img.convert("L").getdata())
    lb = list(b.convert("L").getdata())
    ma = sum(la) / len(la)
    mb = sum(lb) / len(lb)
    num = sum((x - ma) * (y - mb) for x, y in zip(la, lb))
    den = math.sqrt(sum((x - ma) ** 2 for x in la) * sum((y - mb) ** 2 for y in lb)) or 1
    return num / den


def check_vdc_bus(chk):
    """The data port's own rules, compiled straight against video.c.

    The capture comparison below can only say "the picture is right for these
    frames"; it cannot say why, and it cannot see a defect that this game happens
    not to exercise.  These are the rules the defect lived in, checked directly:
    one increment per word, upper-half writes discarded, undriven reads retained.
    """
    import tempfile
    work = tempfile.mkdtemp(prefix="xlate_vdcbus_")
    exe = os.path.join(work, "test_vdc_bus")
    cc = H.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Wpedantic",
                "-Werror", "-I.", os.path.join(H.TESTS, "test_vdc_bus.c"),
                "video.c", "-o", exe], timeout=120)
    chk.ok(cc.rc == 0, "the bus test compiles clean (%s)"
           % (cc.err.strip().splitlines()[-1] if cc.rc else "ok"))
    if cc.rc != 0:
        return
    r = H.run([exe], timeout=60)
    for line in r.out.splitlines():
        if line.strip():
            print("  " + line)
    chk.ok(r.rc == 0, "the data port follows the chip's prefetch/increment rules")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=float, default=0.9, help="required correlation")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    from PIL import Image

    from PIL import Image
    chk = H.Checker("video renderer")
    check_vdc_bus(chk)
    H.skip_unless(H.ref_present("ref"),
                  "no reference frames (refdata/ref/*.state + *.png): set REFDATA=<dir>")
    pairs = []
    for st in sorted(glob.glob(os.path.join(REF, "*.state"))):
        png = st[:-len(".state")] + ".png"
        if os.path.exists(png):
            pairs.append((st, png))
    chk.ok(pairs, "reference states found in %s" % REF)
    if not pairs:
        return 1

    results = []
    for st, png in pairs:
        name = os.path.basename(st)[:-len(".state")]
        blob = os.path.join(SRC, "out", name + ".vdc")
        ppm = os.path.join(SRC, "out", name + ".ppm")
        d = H.run([sys.executable, "tools/state_dump.py", st, "--out", blob])
        if d.rc != 0:
            chk.ok(False, "%s: state_dump failed: %s" % (name, d.err.strip()))
            continue
        r = H.run(["./out/host", "render", "--state", blob, "--out", ppm])
        if r.rc != 0:
            chk.ok(False, "%s: render failed: %s" % (name, r.err.strip()))
            continue
        mine = Image.open(ppm).convert("RGB")
        theirs = Image.open(png)
        c = correlate(mine, theirs)
        mae = mean_abs_error(mine, theirs)
        fit = correlate_fit(mine, theirs)
        results.append((name, c, fit, mae))
        chk.ok(c >= a.min, "%s: correlation %.4f (need %.2f)" % (name, c, a.min))
        if a.verbose:
            print("  %-8s raw %.4f   palette-normalised %.4f   mean|err| %.2f/255"
                  % (name, c, fit, mae))
        # keep the visual for a human: side-by-side at native size
        side = Image.new("RGB", (mine.width * 2 + 8, mine.height), (0, 0, 0))
        side.paste(mine, (0, 0))
        side.paste(theirs.convert("RGB").resize(mine.size, Image.BOX), (mine.width + 8, 0))
        side.save(os.path.join(SRC, "out", name + "_cmp.png"))

    if results:
        # The strong claim: at least one capture pair must be near pixel-exact.
        # (A pair captured either side of a palette change cannot be, and saying so
        # is better than quietly averaging it away.)
        best = min(r[3] for r in results)
        chk.ok(best <= 1.0, "best pair mean|err| %.2f/255 (need <= 1.0): the renderer "
               "is pixel-accurate, not merely similar" % best)
        avg = sum(r[1] for r in results) / len(results)
        fitavg = sum(r[2] for r in results) / len(results)
        chk.ok(avg >= 0.95, "mean correlation %.4f over %d frames (need 0.95)"
               % (avg, len(results)))
        print("  mean raw correlation: %.4f   mean palette-normalised: %.4f"
              % (avg, fitavg))
    ok = chk.report("%d frames" % len(results))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
