#!/usr/bin/env python3
"""make_compare_video.py - one console-vs-port video a person can watch.

    make_compare_video.py OUT.mp4 --dump DIR [--lo N] [--hi N]
                                  [--offset 0] [--row-shift 1] [--fps 60]
                                  [--scale 3] [--crf 20] [--label "..."]
                                  [--note "..."]

Reference on the left, port on the right, the reference frame number in the middle
and the number of differing pixels on the right.  Output is H.264/MP4.

WHERE THE PICTURES COME FROM
---------------------------
Reference: `$REFDATA/shots/fNNNNNNNN.raw` (see docs/VERIFICATION.md).
  256 x 232 words, little-endian **RGB555** (5 bits each channel, bit 10 always
  clear).  Only the top 224 rows are picture; the last 8 are the capture's border.

Port: `fNNNNN.ppm` written by `XLATE_FRAME_DIR` + `XLATE_FRAME_EVERY`.
  By the frame-dump convention the port writes at the frame boundary *before*
  rendering that frame's rows, so file f(N) holds the picture of port frame N-1.
  **The picture of port frame k is therefore file f(k+1).**

FRAME OFFSET AND ROW SHIFT
--------------------------
`--offset` is reference_frame = port_frame + offset.  It is a parameter, not an
assumption: measure it per route, e.g. with `tools/lockstep/clock_offset.py`, which asks
which reference frame's 8 KB of work RAM the port's frame looks like.  A
nonzero offset is how the *game-age* comparison is made, when the two machines reach
the same game instant at different frame numbers: port frame k is then shown beside
reference frame k+offset.  The counter printed on the canvas is always the REFERENCE
frame, because that is the number both panels share once an offset is in force;
`--note` prints a second caption line under it (its own `%d`, if present, is the port
frame) so the clip can say which port frame is on screen.

`--note` is drawn on a fourth header row; `--label` stays on the third, centred,
between the capture name and the differing-pixel readout.

`--row-shift` is how many of the port's own rows to drop before display, i.e. which
port row holds the reference's row 0.  It is a parameter because it is a property of
the run, not of the machine: measure it once per route (mean differing pixels over a
window, with and without the shift) rather than assume it.  Dropping the port's first
row aligns the panels, so the number on the right describes what the eye sees; the
framing difference is stated here rather than left to make every frame look wrong.
The bottom row is repeated to keep the picture the same height.

THE NUMBER ON THE RIGHT
-----------------------
Differing pixels at per-channel tolerance 8.  Green <= 9 (the noise floor:
essentially identical), amber <= 2,000, red above the 2,000 gate.  A still screen
shows a few pixels of difference sometimes; red means "this frame is not the
reference's frame".
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 256, 224                 # the picture both machines show
REF_W, REF_H = 256, 232        # the capture's raw frame (224 picture + 8 border)
REF_DIR = os.path.join(
    os.environ.get("REFDATA") or os.path.join(
        os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..")), "refdata"), "shots")
GATE = 2000
NOISE = 9
TOLERANCE = 8

#: Where a TrueType font is looked for; a missing one is not an error (PIL's own
#: bitmap font is used instead, which is smaller but legible).
FONT_DIRS = ["/usr/share/fonts/dejavu-sans-fonts", "/usr/share/fonts/TTF",
             "/usr/share/fonts/truetype/dejavu"]


def font(size, mono=False):
    names = (["DejaVuSansMono-Bold.ttf", "DejaVuSansMono.ttf"] if mono else []) + \
            ["DejaVuSans-Bold.ttf", "DejaVuSans.ttf"]
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def load_console(f, ref_dir=REF_DIR):
    """232x256 RGB555 -> the console's 224 picture rows (rows 0..223)."""
    p = os.path.join(ref_dir, "f%08d.raw" % f)
    if not os.path.exists(p):
        return None
    with open(p, "rb") as fh:
        data = fh.read(REF_W * REF_H * 2)
    if len(data) < REF_W * REF_H * 2:
        return None
    w = np.frombuffer(data, dtype="<u2").reshape(REF_H, REF_W).astype(np.uint16)
    r = (w >> 11) & 0x1F
    g = (w >> 5) & 0x1F
    b = w & 0x1F
    out = np.stack([(r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)],
                   axis=-1).astype(np.uint8)
    return out[:H]


def load_ppm(path):
    with open(path, "rb") as fh:
        if fh.readline().strip() != b"P6":
            return None
        w, h = map(int, fh.readline().split())
        if int(fh.readline()) != 255:
            return None
        a = np.frombuffer(fh.read(w * h * 3), dtype=np.uint8)
    return a.reshape(h, w, 3)[:H, :W]


def align_port(img, row_shift):
    """Drop `row_shift` port rows so port row `row_shift` holds the console's row 0."""
    if row_shift <= 0:
        return img
    out = np.empty_like(img)
    lo = min(row_shift, len(img) - 1)
    out[:len(img) - row_shift] = img[lo:lo + len(img) - row_shift]
    out[len(img) - row_shift:] = img[-1]     # repeat the last row
    return out



def diff(a, b):
    return int((np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2) > TOLERANCE).sum())


def colour_for(v):
    if v <= NOISE:
        return (110, 220, 120)
    if v <= GATE:
        return (245, 200, 90)
    return (245, 90, 90)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--dump", required=True, help="directory of fNNNNN.ppm from the port")
    ap.add_argument("--lo", type=int, default=None, help="first port frame (default: first file)")
    ap.add_argument("--hi", type=int, default=None, help="one past the last port frame")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--row-shift", type=int, default=1)
    ap.add_argument("--ref", default=REF_DIR)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--crf", type=int, default=26)
    ap.add_argument("--label", default="")
    ap.add_argument("--note", default="", help="second caption line; its own %%d is the port frame")
    args = ap.parse_args(argv[1:])

    files = {}
    for fn in os.listdir(args.dump):
        if fn.startswith("f") and fn.endswith(".ppm"):
            try:
                files[int(fn[1:6])] = os.path.join(args.dump, fn)
            except ValueError:
                pass
    if not files:
        raise SystemExit("no f*.ppm in %s" % args.dump)
    # file f(N) holds port frame N-1
    ports = sorted(k for k in files if k >= 1)
    lo = args.lo if args.lo is not None else ports[0] - 1
    hi = args.hi if args.hi is not None else ports[-1]

    s = args.scale
    pw, ph = W * s, H * s
    hdr = 152
    gap = 12                       # a visible gutter, so the two panels never read as one picture
    cw, ch = pw * 2 + gap, hdr + ph
    f_lab = font(34)
    f_num = font(44, mono=True)
    f_sm = font(24)

    ff = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", "%dx%d" % (cw, ch), "-r", str(args.fps), "-i", "-",
         "-c:v", "libx264", "-preset", "slow", "-crf", str(args.crf),
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", args.out],
        stdin=subprocess.PIPE)

    n = exact = over = 0
    worst = 0
    worst_frame = -1
    for k in range(lo, hi):
        p = files.get(k + 1)          # picture of frame k lives in f(k+1)
        if p is None:
            continue
        con_no = k + args.offset      # the console frame this port frame is shown beside
        con = load_console(con_no, args.ref)
        port = load_ppm(p)
        if con is None or port is None:
            continue
        port = align_port(port, args.row_shift)
        v = diff(con, port)
        exact += (v == 0)
        over += (v > GATE)
        worst = max(worst, v)
        if v == worst:
            worst_frame = con_no

        canvas = Image.new("RGB", (cw, ch), (8, 8, 10))
        canvas.paste(Image.fromarray(con).resize((pw, ph), Image.NEAREST), (0, hdr))
        canvas.paste(Image.fromarray(port).resize((pw, ph), Image.NEAREST), (pw + gap, hdr))
        dr = ImageDraw.Draw(canvas)
        dr.text((12, 2), "CONSOLE", font=f_lab, fill=(110, 220, 120))
        dr.text((cw - 12, 2), "PORT", font=f_lab, fill=(245, 200, 90), anchor="rt")
        # the console's frame number: the clock both panels share once --offset is applied
        dr.text((cw // 2, 36), "FRAME %d" % con_no, font=f_num, fill=(235, 235, 235), anchor="mt")
        dr.text((12, 86), "BizHawk / TurboNyma", font=f_sm, fill=(120, 120, 130))
        dr.text((cw - 12, 86), "%d px differ" % v, font=f_sm, fill=colour_for(v), anchor="rt")
        if args.label:
            dr.text((cw // 2, 88), args.label, font=f_sm, fill=(120, 120, 130), anchor="mt")
        if args.note:
            note = (args.note % k) if "%d" in args.note else args.note
            dr.text((cw // 2, 118), note, font=f_sm, fill=(160, 160, 175), anchor="mt")
        dr.rectangle([(pw, hdr), (pw + gap, ch)], fill=(30, 30, 36))
        dr.line([(pw, hdr), (pw, ch)], fill=(90, 90, 100), width=1)
        dr.line([(pw + gap, hdr), (pw + gap, ch)], fill=(90, 90, 100), width=1)
        dr.line([(0, hdr - 3), (cw, hdr - 3)], fill=(60, 60, 70), width=2)
        ff.stdin.write(canvas.tobytes())
        n += 1

    ff.stdin.close()
    ff.wait()
    if n == 0:
        print("no frames written")
        return 2
    print("wrote %s  %.1f MB, %d frames, %.1f s at %g fps"
          % (args.out, os.path.getsize(args.out) / 1e6, n, n / args.fps, args.fps))
    print("  identical %d/%d   over the 2,000 px gate %d   worst %d px at frame %d"
          % (exact, n, over, worst, worst_frame))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
