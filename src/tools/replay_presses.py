#!/usr/bin/env python3
"""Derive `--press FROM:TO:MASK` arguments from an emulator input replay.

The replay is RetroArch's BSV v2 movie: per emulated frame a header and a list of
input events, one of which is the joypad mask (port 0, id 256).  The core queries the
mask twice per frame, so event index // 2 is the frame number.  We do not guess the
cadence: it comes from the file the oracle itself played.

Usage: tools/replay_presses.py REPLAY [--button 0x08]
"""
import argparse
import struct
import sys

# <u8 port><u8 device><u8 idx><u8 pad><u16 id> then <u16 value>
EVENT_PREFIX = bytes([0x00, 0x01, 0x00, 0x00, 0x00, 0x01])   # port0, JOYPAD, idx0, id 0x0100
QUERIES_PER_FRAME = 2


def windows(replay, button):
    d = open(replay, "rb").read()
    vals = []
    i = d.find(EVENT_PREFIX)
    while i != -1:
        if i + 8 <= len(d):
            vals.append(struct.unpack("<H", d[i + 6:i + 8])[0])
        i = d.find(EVENT_PREFIX, i + 1)
    frames = sorted({idx // QUERIES_PER_FRAME for idx, v in enumerate(vals)
                     if v & button})
    wins = []
    for f in frames:
        if wins and f == wins[-1][1]:
            wins[-1][1] = f + 1
        else:
            wins.append([f, f + 1])
    return vals, frames, wins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--button", type=lambda s: int(s, 0), default=0x0008)
    ap.add_argument("--as-args", action="store_true", help="print host arguments only")
    a = ap.parse_args()
    vals, frames, wins = windows(a.replay, a.button)
    if a.as_args:
        print(" ".join("--press %d:%d:0x%02X" % (lo, hi, a.button) for lo, hi in wins))
        return 0
    print("replay: %s" % a.replay)
    print("  joypad-mask events: %d -> %d emulated frames"
          % (len(vals), len(vals) // QUERIES_PER_FRAME))
    print("  button 0x%04X held in %d frame(s), %d window(s):"
          % (a.button, len(frames), len(wins)))
    for lo, hi in wins:
        print("    frames %5d .. %5d  (%d frames)" % (lo, hi - 1, hi - lo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
