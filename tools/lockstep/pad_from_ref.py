#!/usr/bin/env python3
"""pad_from_ref.py -- the console's own held controller state, frame by frame,
taken from the reference capture's RAM rather than from a press file.

WHY THIS EXISTS. A press file says "RUN was held during frame 7,325". That is a
claim about the *recording*, and it is only true of a machine whose drive behaves
like the recording machine's. The question worth asking is "given identical
buttons, do the two machines behave identically", so the buttons are taken from
the reference machine itself:

    the System Card's pad routine ($E49F) writes, per poll,
        $2228+y  HELD  -- this poll's buttons, active high, bit3 = RUN
        $2232+y  the value HELD had at the start of this poll
        $222D+y  NEW   = HELD AND NOT $2232  (an edge, computed per poll)

$2228 is a *level*, so it is what the player physically held at that poll.  $222D
is not used as input: it is post-poll and already an edge, and feeding edges is
exactly what a poll sliding over a frame boundary destroys.

WHERE IN THE FRAME. ram.bin block N is RAM at the start of frame N. The poll
happens ~95.6 % of the way through a frame, so the state observed during frame N
is $2228 at block N+1. The stream therefore stores

    held(N) = ram.bin[N+1][$2228 .. $222C]

and the port installs entry N when its frame counter turns over -- the start of
vertical sync, the same instant vblank fires and ~1.2 % of a frame before the
game reaches its pad routine. So the state is in place before the poll of frame
N runs, and it is still in place for every poll that lands in frame N.

Consequence worth knowing: a press the console's own poll missed never appears in
$2228, so this feed cannot invent one. That is deliberate -- it is the closest
readable record of what the pad physically held.

OUTPUT. (TO-FROM+1) x 5 bytes: entry k = pads 0..4 for frame FROM+k, in the
port's bit order (bit0 I, 1 II, 2 SELECT, 3 RUN, 4 UP, 5 RIGHT, 6 DOWN, 7 LEFT).
`host run --padstream FILE --padstream-from FROM`.

ALIGNMENT (`--lag K --lag-from AT`). Entry N is normally the console's state for
frame N. With an alignment, entry N is the console's state for frame N-K once
N >= AT, and for frame N before it. It exists because a port whose boot takes
longer than the console's reaches the same screen at a different frame number,
and the honest question then is "the same button at the same point in the
machine's life", not "at the same number of frames since power-on".

  K   is MEASURED (tools/lockstep/clock_offset.py: the whole 8 KB, every frame,
      reported as a constant lag). It is not a knob to be tuned until a screen
      matches; a wrong K here is visible as the machine still missing presses.
  AT  is the frame at which the port's game got control -- printed by the port
      itself (`BANK first=<bank> frame=<n>`), which is the card handing over, not
      a number chosen for this experiment. Before AT the input is the boot's, and
      it must NOT be shifted: the card's "PRESS RUN" prompt appears at the same
      frame on both machines, and the press that releases it is what starts the
      boot, so shifting it shifts the whole boot and manufactures the offset the
      alignment was meant to remove (measured: lag 18 applied to every frame
      moves the port's boot 18 frames and leaves it 36 behind, not 18).

CHECK. `--check PRESSFILE` compares the reconstructed held windows against an
input schedule's own windows and reports every window that does not match. This is
the tool's fault injection: if the reconstruction were shifted or edge-derived, the
check would disagree with an independent source on most windows instead of almost
none.

Usage:
  pad_from_ref.py RAM.BIN FROM TO -o STREAM.bin [--check PRESSFILE]
  pad_from_ref.py RAM.BIN 0 8000 -o /tmp/pad.bin --check schedule.txt
"""
import argparse
import sys

import numpy as np

BLK = 8192
HELD = 0x228          # $2228
NPAD = 5


def held_frame(a, f, frames):
    """Pads 0..4 held during frame f, from the start of frame f+1. Zero past the end."""
    if f + 1 >= frames:
        return bytes(NPAD)
    return bytes(a[f + 1, HELD:HELD + NPAD])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram")
    ap.add_argument("lo", type=int)
    ap.add_argument("hi", type=int, help="inclusive")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--check", help="press file to compare the reconstruction against")
    ap.add_argument("--lag", type=int, default=0,
                    help="entries from --lag-from onward are this many frames earlier")
    ap.add_argument("--lag-from", type=int, default=0,
                    help="first frame the lag applies to (the port's hand-off frame)")
    a = ap.parse_args()

    raw = np.memmap(a.ram, dtype=np.uint8, mode="r")
    frames = raw.shape[0] // BLK
    a_ = raw.reshape(frames, BLK)

    n = a.hi - a.lo + 1
    out = bytearray(n * NPAD)
    for k in range(n):
        f = a.lo + k
        src = f - a.lag if f >= a.lag_from else f
        out[k * NPAD:(k + 1) * NPAD] = held_frame(a_, src, frames)
    with open(a.out, "wb") as f:
        f.write(out)
    print("wrote %s: frames %d..%d, %d bytes (5 per frame, held state)%s"
          % (a.out, a.lo, a.hi, len(out),
             ", aligned lag %d from frame %d" % (a.lag, a.lag_from) if a.lag else ""))

    if a.check:
        check(a_, a.lo, a.hi, a.check, a.lag, a.lag_from)
    return 0


def check(a_, lo, hi, path, lag=0, lag_from=0):
    """Every press file window against the reconstruction, one line each."""
    windows = []
    with open(path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            try:
                fr, to, mask = line.split(":")
                windows.append((int(fr, 0), int(to, 0), int(mask, 0)))
            except ValueError:
                continue
    frames = a_.shape[0]
    agree = miss = extra = 0
    print("\n%-14s %-6s %-9s %s" % ("window", "mask", "recon", "verdict"))
    for fr, to, mask in windows:
        if fr < lo or to - 1 > hi:
            continue
        # the window's own frames, as the reconstruction has them
        got = []
        for f in range(fr, to):
            # the entry the port will see at frame f, which is the console's
            # frame f itself unless an alignment is in force
            src = f - lag if f >= lag_from else f
            held = held_frame(a_, src, frames)[0]
            got.append(held & mask)
        anybit = any(got)
        if anybit:
            agree += 1
            verdict = "ok"
        else:
            miss += 1
            verdict = "console's latch never shows this bit"
        print("%-14s %-6s %-9s %s"
              % ("%d:%d" % (fr, to), "0x%02X" % mask,
                 " ".join("%02X" % v for v in got), verdict))
        # bits the reconstruction holds that the window does not claim
        for f in range(fr, to):
            src = f - lag if f >= lag_from else f
            held = held_frame(a_, src, frames)[0]
            if held & ~mask:
                print("    frame %d: reconstruction holds 0x%02X, press file %02X"
                      % (f, held, mask))
                extra += 1
    print("\nwindows in range: %d; reconstruction shows the bit in %d, never in %d"
          % (agree + miss, agree, miss))
    if extra:
        print("extra bits seen in %d frame(s) -- see above" % extra)


if __name__ == "__main__":
    sys.exit(main())
