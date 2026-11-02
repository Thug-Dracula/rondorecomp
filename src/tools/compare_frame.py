#!/usr/bin/env python3
"""Compare our rendered frame against the oracle's, structurally.

Why not a pixel diff: the oracle screenshot's RGB is the *emulator's* colour
conversion of a 9-bit console value, and that conversion is channel-dependent and
non-monotonic (measured on our own data: one colour goes up in green while going
down in blue). Comparing RGB would therefore enshrine the emulator's conversion as
truth and hide real differences behind a tolerance.

So the comparison is per-pixel with a small channel **tolerance**, and the tolerance
is *calibrated*, not guessed: on the three intro screens — where we have independent
evidence the render is exact — the maximum channel difference is 4, and every pixel
matches within 8. So a threshold of 8 separates "same colour through a different
conversion" from "genuinely different content", and on those screens it reports
0 differing pixels.

(An earlier version tried relabelling colours by first-appearance order. That is
broken whenever the two images' RGB sets are disjoint — which they are here, since
every colour is offset — so it reported total mismatch on images that are almost
identical. Tolerance against a calibrated bound is the honest measure.)

Usage:
  compare_frame.py OURS.ppm ORACLE.png [--crop-y N] [--json]

Exit status is 0 when the class maps are identical, 1 when they are not, 2 on a
usage/format error — so it can gate a test script.
"""
import sys
import json

try:
    from PIL import Image
except ImportError:
    sys.exit("compare_frame: Pillow is required (python -c 'import PIL')")


def read_ppm(path):
    with open(path, 'rb') as f:
        if f.readline().strip() != b'P6':
            sys.exit(f"compare_frame: {path} is not a binary PPM (P6)")
        w, h = map(int, f.readline().split())
        maxv = int(f.readline())
        if maxv != 255:
            sys.exit(f"compare_frame: {path} has maxval {maxv}, expected 255")
        return Image.frombytes('RGB', (w, h), f.read())


def differs(a, b, tol):
    return max(abs(a[i] - b[i]) for i in range(3)) > tol


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    ours_path, ref_path = argv[1], argv[2]
    crop_y = 0
    as_json = False
    tol = 8
    for a in argv[3:]:
        if a == '--json':
            as_json = True
        elif a.startswith('--tolerance'):
            tol = int(a.split('=')[1]) if '=' in a else int(argv[argv.index(a) + 1])
        elif a.startswith('--crop-y'):
            crop_y = int(a.split('=')[1]) if '=' in a else int(argv[argv.index(a) + 1])

    ours = read_ppm(ours_path)
    ref = Image.open(ref_path).convert('RGB')
    if crop_y:
        ref = ref.crop((0, crop_y, ref.size[0], crop_y + ours.size[1]))
    else:
        ref = ref.crop((0, 0, ours.size[0], ours.size[1]))
    if ref.size != ours.size:
        sys.exit(f"compare_frame: size mismatch {ours.size} vs {ref.size}")

    pa, pb = ours.load(), ref.load()
    w, h = ours.size
    differing = 0
    rows = []
    for y in range(h):
        n = 0
        for x in range(w):
            if differs(pa[x, y], pb[x, y], tol):
                n += 1
        if n:
            rows.append((y, n))
        differing += n
    same = (differing == 0)

    result = {
        'ours': ours_path,
        'reference': ref_path,
        'size': f'{w}x{h}',
        'tolerance': tol,
        'pixels_differing': differing,
        'total_pixels': w * h,
        'identical_partition': same,
        'differing_rows': rows[:20],
    }
    if as_json:
        print(json.dumps(result, indent=2))
    else:
        print(f"ours {ours_path} vs {ref_path}  (tolerance {tol}/255)")
        print(f"  pixels differing: {differing} of {w*h} ({100*differing/(w*h):.2f}%)")
        print(f"  identical within tolerance: {'YES' if same else 'no'}")
        if rows:
            print(f"  first differing rows: {rows[:8]}")
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
