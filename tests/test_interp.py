#!/usr/bin/env python3
"""Forced-interpretation frame test: the same picture out of a different engine.

The interpreter (`interp.c`) is proved instruction-for-instruction by
`tests/test_diff.py`.  This is the other half of gate O-3: that the *pictures* come
out the same when every instruction is interpreted (`XLATE_INTERP=1`) instead of
dispatched to a translated unit.

What is compared, and why each one is here:

  * frames 4 and 1200 of the injected acceptance state against the console's own
    captures -- 0 of 57,344 pixels, from **both** engines.  This is the same gate
    the port has always been held to, so a difference would be a real regression,
    not a new measurement;
  * the whole STATE line from both runs: registers, the eight bank registers, the
    2 MB states CRC, cycles, frames, interrupts.  Equal numbers mean the two
    engines did the same work, not merely the same amount of it;
  * `INTERP steps` from each run.  The translated run must report 0 and the forced
    run must report every step -- otherwise "forced" would be a claim with nothing
    behind it, and a plant that quietly fell back would look identical to one that
    did not.

`--boot` adds the cold-boot logo frame (our frame 2400 vs the console capture),
which is the picture the port draws 48 of 57,344 pixels wrong; the requirement is
that the interpreter draws exactly the same 48, on the same rows.

Not part of `tests/run_all.sh`: the gate's ten stages are fixed this round, and
this test is the O-3 frame arm measured on the command line.  Folding it in as an
eleventh stage is a gate change, not a measurement.
"""
import argparse
import os
import re
import sys

import harness as H

WORK = H.scratch("interp")
REF = H.ref("captures")
STATE = H.ref("states", "f2400A.img")
INTERP_ENV = dict(os.environ, XLATE_INTERP="1")


def pixels_differing(ours, oracle):
    r = H.run([sys.executable, "tools/compare_frame.py", ours, oracle])
    m = re.search(r"pixels differing: (\d+) of 57344", r.out)
    if not m:
        raise SystemExit("compare_frame produced no count:\n%s\n%s" % (r.out, r.err))
    return int(m.group(1))


def run(frames_dir, extra_env, args, every):
    os.makedirs(frames_dir, exist_ok=True)
    env = dict(os.environ)
    env.update(extra_env)
    env["XLATE_FRAME_EVERY"] = str(every)
    env["XLATE_FRAME_DIR"] = frames_dir
    r = H.run(["./out/host", "run"] + args, env=env, timeout=1800)
    if r.rc != 0:
        print(r.out[-2000:], r.err)
        raise SystemExit("host run failed")
    return r.out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", action="store_true",
                    help="also run the cold boot and compare the logo frame")
    a = ap.parse_args()
    H.skip_without_inputs()
    H.skip_unless(H.ref_present("captures") and os.path.exists(STATE),
                  "no reference captures or state image (refdata/captures, refdata/states): "
                  "set REFDATA=<dir>")
    chk = H.Checker("forced interpretation")

    # ---- injected state: frames 4 and 1200 --------------------------------
    injected = ["--from-state", STATE,
                "--steps", "60000000", "--cycles", "160000000"]
    tr_dir = os.path.join(WORK, "translated")
    it_dir = os.path.join(WORK, "interpreter")
    tr_out = run(tr_dir, {}, injected, 4)
    it_out = run(it_dir, {"XLATE_INTERP": "1"}, injected, 4)

    tr_state = H.parse_state_full(tr_out)
    it_state = H.parse_state_full(it_out)
    chk.ok(tr_state and it_state, "both runs produced a STATE line")
    if tr_state and it_state:
        for key in ("a", "x", "y", "s", "p", "pc", "mpr", "crc", "cycles",
                    "frames", "irq", "vblank", "steps"):
            chk.eq(it_state[key], tr_state[key], "injected run: %s identical" % key)
    chk.eq(len(re.findall(r"^INTERP steps=0$", tr_out, re.M)), 1,
           "translated run executed nothing through the interpreter")
    chk.eq(len(re.findall(r"^INTERP steps=(\d+)$", it_out, re.M)), 1,
           "forced run reported its interpreted instruction count")
    if it_state:
        m = re.search(r"^INTERP steps=(\d+)$", it_out, re.M)
        chk.eq(int(m.group(1)), it_state["steps"],
               "forced run: every step was interpreted (%d)" % it_state["steps"])

    for frame, ref in ((4, "f2400_A.png"), (1200, "f3600_A.png")):
        name = "f%05d.ppm" % frame
        oracle = os.path.join(REF, ref)
        for engine, d in (("translated", tr_dir), ("interpreter", it_dir)):
            path = os.path.join(d, name)
            chk.ok(os.path.exists(path), "%s produced %s" % (engine, name))
            if os.path.exists(path):
                n = pixels_differing(path, oracle)
                chk.eq(n, 0, "injected frame %d, %s: %d of 57,344 differ"
                       % (frame, engine, n))

    # ---- optional: the cold boot's logo frame -----------------------------
    if a.boot:
        boot = ["--disc", H.DISC, "--bios", H.BIOS,
                "--steps", "600000000", "--cycles", "520000000",
                "--press", "1700:1760:0x08", "--boot-game"]
        btr = os.path.join(WORK, "boot_translated")
        bit = os.path.join(WORK, "boot_interpreter")
        trb = run(btr, {"XLATE_READTIME": "1"}, boot, 50)
        itb = run(bit, {"XLATE_READTIME": "1", "XLATE_INTERP": "1"}, boot, 50)
        s_tr = H.parse_state_full(trb)
        s_it = H.parse_state_full(itb)
        for key in ("pc", "mpr", "crc", "frames", "cycles", "irq", "vblank"):
            chk.eq(s_it[key], s_tr[key], "booted run: %s identical" % key)
        name = "f02400.ppm"
        n_tr = pixels_differing(os.path.join(btr, name),
                                os.path.join(REF, "f2400_A.png"))
        n_it = pixels_differing(os.path.join(bit, name),
                                os.path.join(REF, "f2400_A.png"))
        chk.eq(n_it, n_tr, "booted logo: interpreter draws the same pixels (%d)" % n_it)
        print("  booted logo at frame 2400: translated %d of 57,344 differ, "
              "interpreter %d of 57,344 differ" % (n_tr, n_it))
    else:
        print("  booted logo not run (--boot to include it)")

    return 0 if chk.report("registers, crc, cycles, pixels") else 1


if __name__ == "__main__":
    sys.exit(main())
