#!/usr/bin/env python3
"""Input test: replay a schedule of pad windows against the translated chain.

The schedule is read from `$REFDATA/replays/*.replay` (an input log), which holds RUN
in four windows: frames 1700, 4000, 7000 and 10000, sixty frames each.

What this test reports, and asserts, is what happens at each press:

  * which bank/module is executing when the press lands, and which one runs after;
  * whether a third or fourth disc read appears;
  * which banks ever execute;
  * whether any untranslated bank is hit.

The result is the finding: the first press takes the boot program's hand-off, and the
later presses land in the hand-off's own animation loop, which reads the pad but never
leaves. That is asserted rather than described, so a future change that *does* advance
will fail this test and be noticed.
"""
import os
import re
import sys

import harness as H

SRC = H.SRC
REPLAY = H.ref("replays", "press_into_gameplay.replay")
FRAMES = 12100
CYCLES = FRAMES * 119665


def press_args():
    r = H.run([sys.executable, "tools/replay_presses.py", REPLAY, "--as-args"])
    return r.out.split()


def main():
    H.skip_without_inputs()
    H.skip_unless(os.path.exists(REPLAY),
                  "no input schedule (refdata/replays/*.replay): set REFDATA=<dir>")
    chk = H.Checker("input replay")
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1

    args = press_args()
    chk.eq(len([a for a in args if a == "--press"]), 4,
           "the replay holds RUN in four windows")

    r = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
               "--log", H.LOG, "--steps", "400000000", "--cycles", str(CYCLES)]
              + args)
    pads = [(int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
            for m in re.finditer(r"^PAD frame=(\d+) mask=([0-9A-F]{2}) pc=([0-9A-F]{4}) "
                                 r"bank=([0-9A-F]{2})", r.out, re.M)]
    calls = H.parse_calls(r.out)
    reads = [c for c in calls if c["pc"] == 0xE009]
    st = H.parse_state_full(r.out)
    banks = re.search(r"^BANKS_EXECUTED (.*)$", r.out, re.M)

    print("  %-6s %-5s %-6s %-5s %s" % ("frame", "mask", "pc", "bank", "what"))
    for f, mask, pc, bank in pads:
        what = "press" if mask else "release"
        print("  %-6d %-5s %-6s %-5s %s" % (f, "RUN" if mask & 8 else "-", "$%04X" % pc,
                                            "$%02X" % bank, what))

    chk.eq(len(pads), 8, "four presses and four releases were applied")
    chk.ok(st is not None, "the run produced state")
    if st:
        chk.ok(st["frames"] >= 12000, "ran %d emulated frames" % st["frames"])

    # the first press moves the program out of its per-frame loop
    if len(pads) >= 2:
        first_press = pads[0]
        after_release = pads[1]
        chk.ok(first_press[2] < 0x4200, "before the first press the program is in its "
               "per-frame loop (pc $%04X)" % first_press[2])
        chk.eq(after_release[2], 0x41E9,
               "after the first press it is in the hand-off loop (pc $41E9)")

    # the later presses land in that loop and change nothing
    later = [p for p in pads if p[0] >= 4000 and p[1] & 8]
    chk.eq(len(later), 3, "three later presses, as in the replay")
    for (f, _mask, pc, bank) in later:
        chk.eq((pc, bank), (0x41E9, 0x80),
               "press at frame %d lands in the same loop, same bank" % f)

    chk.eq(len(reads), 2, "still exactly two disc reads (no third, no fourth)")
    if len(reads) == 2:
        chk.eq(reads[1]["sector"], 3, "the second read is the 24-sector block")
    chk.eq(r.out.count("UNTRANSLATED bank"), 0, "no untranslated bank was hit")
    if banks:
        used = [b.split("@")[0] for b in banks.group(1).split()]
        chk.eq(used, ["00", "80", "F8"],
               "only the BIOS, the boot program and the loader ever execute")

    ok = chk.report("%d frames" % (st["frames"] if st else 0))
    print("        finding: the presses are read (the loop polls the pad through "
          "$E063) but nothing advances the program past its animation")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
