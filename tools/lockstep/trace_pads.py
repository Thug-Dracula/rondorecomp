#!/usr/bin/env python3
"""trace_pads.py -- where in the frame the game polls the pad, from an rce-trace v1 file.

Works on either machine, because both emit the same format (`src/trace.c` writes it to
`XLATE_TRACE_OUT`; a reference emulator's tracer writes the same records, see
docs/TRACE-FORMAT.md).

WHY THE POLL MATTERS. The System Card's pad routine ($E49F) computes

    NEW = HELD  AND NOT held-at-the-previous-poll        ($E4D2-$E4D8)

so a press held for one frame survives only if some poll lands inside that frame. The
poll is called from the vblank handler, which starts at ~94 % of a frame; whether the
call lands before or after the frame boundary decides whether the press exists at all.
A few hundred cycles decide whether a whole frame of input exists.

Per frame, prints:
  len      the frame's length in master clocks (from the FRAME markers)
  irq@%    when IRQ1 was taken, as a percentage of the frame
  rti@%    when the handler's RTI happened (may be in the *next* frame -- these
           handlers run through the boundary)
  calls    how many times the pad routine was entered
  poll     each call's position in the frame, as a percentage

A frame with `calls=0` is a frame in which a one-frame press is lost. A call at 1-2 %
is the second half of a 0/2 pair: the previous iteration's call arriving after the
boundary it belonged to.

Usage: trace_pads.py TRACE FROM TO
"""
import collections
import sys


def load(path):
    cur = None
    fstart = {}
    rows = collections.OrderedDict()
    pending_irq = None
    with open(path) as f:
        for line in f:
            if not line.startswith("t="):
                continue
            tok = line.split()
            if tok[0] == "t=FRAME":
                kv = dict(t.split("=", 1) for t in tok[1:])
                cur = int(kv["f"])
                fstart[cur] = int(kv["c"], 16)
                rows.setdefault(cur, {"calls": [], "irq": None, "dur": None})
                continue
            if cur is None:
                continue
            kv = dict(t.split("=", 1) for t in tok[1:] if "=" in t)
            c = int(kv.get("c", "0"), 16)
            r = rows.setdefault(cur, {"calls": [], "irq": None, "dur": None})
            if tok[0] == "t=IRQ" and kv.get("k") == "irq1" and r["irq"] is None:
                r["irq"] = c - fstart[cur]
                pending_irq = (cur, c)
            elif tok[0] == "t=IRQE" and pending_irq is not None:
                f0, c0 = pending_irq
                if c > c0:
                    rows[f0]["dur"] = c - c0
                pending_irq = None
            if kv.get("la") == "1000" and kv.get("pc") == "E49F":
                r["calls"].append(c - fstart[cur])
    return fstart, rows


def main(path, lo, hi):
    fstart, rows = load(path)
    order = sorted(fstart)
    length = {a: fstart[b] - fstart[a] for a, b in zip(order, order[1:])}
    print("%5s %7s %7s %7s %5s  %s"
          % ("f", "len", "irq@%", "rti@%", "calls", "poll positions"))
    per_frame = collections.Counter()
    positions = []
    durations = []
    for f in order:
        if f < lo or f > hi:
            continue
        r = rows[f]
        L = length.get(f, 358995)
        irq = "%.1f" % (100.0 * r["irq"] / L) if r["irq"] is not None else "-"
        end = r["irq"] + r["dur"] if (r["irq"] is not None and r["dur"]) else None
        rti = "%.1f" % (100.0 * (end % L) / L) if end is not None else "-"
        calls = " ".join("%.1f%%" % (100.0 * o / L) for o in r["calls"])
        print("%5d %7d %7s %7s %5d  %s" % (f, L, irq, rti, len(r["calls"]), calls))
        per_frame[len(r["calls"])] += 1
        positions += [100.0 * o / L for o in r["calls"]]
        if r["dur"]:
            durations.append(r["dur"])
    print("\nframes %d..%d: calls per frame %s"
          % (lo, hi, dict(sorted(per_frame.items()))))
    if positions:
        positions.sort()
        print("poll position: min %.1f%%  median %.1f%%  max %.1f%%  (n=%d)"
              % (positions[0], positions[len(positions) // 2],
                 positions[-1], len(positions)))
    if durations:
        durations.sort()
        med = durations[len(durations) // 2]
        print("IRQ1 taken -> RTI: median %d mc = %d cycles (min %d, max %d, n=%d)"
              % (med, med // 3, durations[0], durations[-1], len(durations)))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: trace_pads.py TRACE FROM TO")
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
