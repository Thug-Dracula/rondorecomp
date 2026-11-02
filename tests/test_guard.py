#!/usr/bin/env python3
"""Overlay guard stage: a bank whose bytes were replaced is interpreted, and released.

The guard is the rule that a bank's translated units may run only while the bytes in
memory are the bytes the translation was built from.  The other stages never exercise it:
both frame arms run inside the window where the guard is silent.  A rule nothing tests is
a rule that can be deleted without anyone noticing, so this stage is the guard's own.

The route (measured, not chosen): the boot program reloads three program banks from the
CD under itself.  With `XLATE_READTIME=1` the reload lands at emulated frame 3,506, and
that is early enough to see the whole cycle -- fire, interpret, release, finish -- inside
one ~1.2 G-cycle run that ends on its step budget, not on a stall.

THE FRAMES IN THIS FILE MOVED ONCE, AND WHY IS RECORDED HERE SO THE NEXT MOVE IS A
DECISION AND NOT A MYSTERY.  The cycle table was corrected on 2026-10-30 (read-modify-
write on a memory operand is 6/7 cycles, not 4/5, and a VDC port read pays the access
slot).  Every cycle cost in the machine is now the console's, which is what moves the
frames below by one or two -- the CD reload happens a frame later because the code that
waits for it costs what it really costs:

    first firing    3,505 -> 3,506   (bank $6C, pc $8006, 5,673 of 8,192 bytes)
    bank $72 / $6B  3,663 -> 3,664
    unguarded stop  3,662 -> 3,664   (still $607B, still reason=untranslated)
    guarded run end 10,027 -> 10,028

Nothing here is asserted against a literal frame: the checks derive the frames from the
run and assert the *relations* (log == stdout, release after firing, unguarded dies
earlier).  These numbers are the recorded values, not expectations the test compares to,
so a further cycle correction moves them without weakening anything -- update this block
when it does.

What is asserted on the guarded run:

  * one `GUARD first bank=.. frame=.. pc=.. differ=..` line on stdout, with a real bank,
    a real frame and a non-zero byte count.  This is a firing, not a claim: the count is
    5,673 of 8,192 bytes against the compiled image;
  * the same firing in the log (`--log`), for the same bank and frame -- a log is the
    record when nobody is watching, so it must carry the event too;
  * at least one `-> translation` release: the guard must let go as well as hold, or the
    machine would quietly become an interpreter with a translation bolted on;
  * `SPLIT` with both halves non-zero, the halves adding up to the step total on the
    STATE line, and the printed percentages agreeing with the counts they describe.

And on the unguarded arm (`XLATE_GUARD=0`), same route, same seed:

  * the run stops `reason=untranslated` at $607B, frame 3,664, long before the guarded
    run's frame 10,028.  With the guard off the stale bank's *old* translation keeps
    running, that old code jumps to an address nothing was translated for, and the
    machine has nowhere to go.  So the guard is not decoration: it is the difference
    between a run that finishes its budget and one that dies at 36% of the frames;
  * `interpreted=0` and no `GUARD first` line -- nothing was interpreted, which is what
    "off" means, and it is the failure this stage must produce if the guard is ever
    removed rather than switched.
  * the log still carries the report-only `stale: bank ...` measurements, so the arm is
    demonstrably the same route -- the routing is the only thing missing.

`--positive-guard 0` forces the switch *off* for the positive arm.  That is the
deliberate break used to prove these assertions fail when the guard does not work; it is
not part of the gate.
"""
import argparse
import os
import re
import sys
import time

import harness as H

# A PRIVATE scratch directory per run: the host *appends* to its log, so two runs
# sharing one directory interleave their lines, the frame in the log stops matching the
# frame on stdout, and release lines can be truncated away.  That reads as a guard
# failure and is not one.
WORK = H.scratch("guard_%d" % os.getpid())

ROUTE = ["--disc", H.DISC, "--bios", H.BIOS,
         "--press", "1700:1760:0x08", "--press", "2600:2660:0x08",
         "--steps", "4000000000", "--cycles", "1200000000", "--boot-game"]

FIRST = re.compile(r"^GUARD first bank=([0-9A-F]{2}) frame=(\d+) pc=([0-9A-F]{4}) "
                   r"gen=(\d+) differ=(\d+) writer_pc=([0-9A-F]{4}) "
                   r"writer_bank=([0-9A-F]{2}) writer_frame=(\d+) writer=(.+)$",
                   re.M)
# xl_logf prefixes every line with f=NNNNN; match the message, not the prefix.
ROUTED = re.compile(r"guard: bank \$([0-9A-F]{2}) -> interpreter at frame (\d+)")
RELEASED = re.compile(r"guard: bank \$([0-9A-F]{2}) -> translation at frame (\d+)")
STALE = re.compile(r"stale: bank \$([0-9A-F]{2}) gen \d+ frame (\d+)")
SPLIT = re.compile(r"^SPLIT translated=(\d+) interpreted=(\d+) total=(\d+) "
                   r"translated_pct=([\d.]+) interpreted_pct=([\d.]+)$", re.M)
FIRINGS = re.compile(r"^GUARD firings=(\d+) routed_banks=(\d+) scans=(\d+)$", re.M)
STOP = re.compile(r"^STOP reason=(\S+) pc=([0-9A-F]{4})$", re.M)


def run_arm(tag, extra_env, guard=None):
    """One host run of the shared route.  `guard` sets XLATE_GUARD for this arm only."""
    d = os.path.join(WORK, tag)
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "guard.log")
    # The host appends to its log (a trail must survive a second run at the same path).
    # This stage reads a run's log as that run's evidence, so it starts from nothing:
    # a leftover line from an earlier run would be counted as this run's firing.
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ)
    env.pop("XLATE_GUARD", None)      # the arm decides; a stray shell switch must not
    env.update(extra_env)
    if guard is not None:
        env["XLATE_GUARD"] = guard
    t0 = time.time()
    r = H.run(["./out/host", "run"] + ROUTE + ["--log", log], env=env, timeout=1800)
    elapsed = time.time() - t0
    text = ""
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            text = f.read()
    r.log_path, r.log, r.elapsed = log, text, elapsed
    return r


def split_line(text):
    m = SPLIT.search(text)
    if not m:
        return None
    return {"translated": int(m.group(1)), "interpreted": int(m.group(2)),
            "total": int(m.group(3)), "translated_pct": float(m.group(4)),
            "interpreted_pct": float(m.group(5))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--positive-guard", default=None, metavar="VALUE",
                    help="force XLATE_GUARD to VALUE on the positive arm (use 0 to "
                         "prove the stage fails when the guard is off); not for the gate")
    a = ap.parse_args()
    chk = H.Checker("overlay guard")

    # ---- the guarded run --------------------------------------------------
    on = run_arm("guard_on", {"XLATE_READTIME": "1"}, a.positive_guard)
    st = H.parse_state_full(on.out)
    sp = split_line(on.out)
    fired = FIRST.search(on.out)
    routed_log = ROUTED.findall(on.log)
    released_log = RELEASED.findall(on.log)
    stale_log = STALE.findall(on.log)
    fire_counts = FIRINGS.search(on.out)
    stop = STOP.search(on.out)

    chk.ok(st is not None, "the guarded run produced a STATE line")
    chk.ok(len(on.log) > 0, "the guarded run wrote a log (%s)" % on.log_path)
    chk.ok(stop is not None and stop.group(1) == "steps",
           "the guarded run ends on its step budget, not on a stall (%s)"
           % (("reason=" + stop.group(1)) if stop else "no STOP line"))

    # 1. a firing, named and measured
    chk.ok(fired is not None, "the guard fired and printed its first-firing line")
    if fired:
        bank = int(fired.group(1), 16)
        gframe = int(fired.group(2))
        differ = int(fired.group(5))
        writer = fired.group(9).strip()
        chk.ok(0 < differ <= 8192,
               "the firing is a measurement: %d of 8,192 bytes differ from the "
               "compiled image" % differ)
        chk.eq(len(FIRST.findall(on.out)), 1, "'GUARD first' is printed once per run")
        # the same event in the log, same bank and frame
        chk.ok((("%02X" % bank), str(gframe)) in routed_log,
               "the log carries the same firing: bank $%02X -> interpreter at frame %d"
               % (bank, gframe))
        if (("%02X" % bank), str(gframe)) not in routed_log:
            # Say what the log *does* hold.  A log whose frame for the same bank is
            # one or two frames off, or which carries a bank this run never routed,
            # is not a guard bug: the host appends to its log, so a second run of this
            # stage at the same time writes into the same file.  WORK is per-process
            # now; this line is what makes a recurrence obvious instead of mysterious.
            print("        log says, for bank $%02X: %s"
                  % (bank, [f for b, f in routed_log if b == ("%02X" % bank)] or "nothing"))
            print("        log file: %s (%d bytes)" % (on.log_path, len(on.log)))
        chk.ok(gframe > 0, "the firing names an emulated frame ($%02X at frame %d, "
                           "pc $%04X)" % (bank, gframe, int(fired.group(3), 16)))
        chk.ok(writer != "", "the firing names what wrote the bank (%s)" % writer)
    else:
        bank = gframe = differ = 0
        writer = "?"

    # 2. it lets go again
    released_banks = sorted(set(b for b, _f in released_log))
    routed_banks = sorted(set(b for b, _f in routed_log))
    chk.ok(len(released_banks) >= 1,
           "the guard releases: %d bank(s) returned to translation (%s)"
           % (len(released_banks), ", ".join("$" + b for b in released_banks) or "none"))
    for b, f in released_log:
        chk.ok(int(f) > gframe,
               "bank $%s was released at frame %s, after the first firing at frame %d"
               % (b, f, gframe))
    if fire_counts:
        chk.eq(int(fire_counts.group(1)), len(routed_log),
               "the summary's firing count matches the log's routing lines")
        chk.eq(int(fire_counts.group(2)), len(released_banks),
               "the summary's routed-bank count matches the banks the log released")

    # 3. the split adds up
    chk.ok(sp is not None, "the run printed a SPLIT line")
    if sp and st:
        chk.ok(sp["interpreted"] > 0,
               "translation was not the whole run: %d steps interpreted (%.4f%%)"
               % (sp["interpreted"], sp["interpreted_pct"]))
        chk.ok(sp["translated"] > 0,
               "the interpreter was not the whole run: %d steps translated (%.4f%%)"
               % (sp["translated"], sp["translated_pct"]))
        chk.eq(sp["translated"] + sp["interpreted"], sp["total"],
               "SPLIT's two halves are the step total")
        chk.eq(sp["total"], st["steps"], "SPLIT's total is the STATE line's step count")
        chk.ok(abs(sp["translated_pct"] + sp["interpreted_pct"] - 100.0) <= 0.001,
               "the two percentages add to 100 (%.4f + %.4f)"
               % (sp["translated_pct"], sp["interpreted_pct"]))
        for key in ("translated", "interpreted"):
            want = 100.0 * sp[key] / sp["total"] if sp["total"] else 0.0
            chk.ok(abs(sp[key + "_pct"] - want) <= 0.001,
                   "the %s percentage matches its count (%.4f vs %.4f)"
                   % (key, sp[key + "_pct"], want))

    # ---- the unguarded arm: the same route with the routing switched off --
    off = run_arm("guard_off", {"XLATE_READTIME": "1", "XLATE_GUARD": "0"})
    off_st = H.parse_state_full(off.out)
    off_sp = split_line(off.out)
    off_stop = STOP.search(off.out)
    off_fired = FIRST.findall(off.out)
    off_released = RELEASED.findall(off.log)
    off_stale = STALE.findall(off.log)

    chk.ok(off_stop is not None, "the unguarded run produced a STOP line")
    chk.ok(len(off.log) > 0, "the unguarded run wrote a log too")
    if off_stop:
        chk.eq(off_stop.group(1), "untranslated",
               "with the guard off the run stops reason=untranslated (at $%s, frame %s)"
               % (off_stop.group(2),
                  off_st["frames"] if off_st else "?"))
        chk.eq(off_stop.group(2), "607B",
               "and it stops at the same address it always did: $607B")
    chk.eq(off.rc, 3, "the unguarded stop is reported to the caller (exit 3)")
    chk.eq(len(off_fired), 0, "the unguarded run never fires (nothing is interpreted)")
    chk.eq(len(off_released), 0, "the unguarded log has no release line either")
    chk.ok(len(off_stale) >= 1,
           "the unguarded arm measured the same staleness (%d banks) -- same route, "
           "only the routing is missing" % len(set(b for b, _f in off_stale)))
    if off_st and st:
        chk.ok(off_st["frames"] < st["frames"],
               "the unguarded run dies earlier: frame %d vs the guarded run's %d"
               % (off_st["frames"], st["frames"]))
    if off_sp:
        chk.eq(off_sp["interpreted"], 0,
               "with the guard off nothing was interpreted (interpreted=%d)"
               % off_sp["interpreted"])
        chk.eq(off_sp["translated"], off_sp["total"],
               "and the whole run counted as translated")
        chk.ok(abs(off_sp["interpreted_pct"]) <= 0.001,
               "its SPLIT says so too (interpreted_pct=%.4f)" % off_sp["interpreted_pct"])

    # ---- evidence ---------------------------------------------------------
    print("  guard on : frame %s, %s, %s steps, interpreted %s (%.4f%%)"
          % (st["frames"] if st else "?", ("reason=" + stop.group(1)) if stop else "?",
             "{:,}".format(st["steps"]) if st else "?",
             "{:,}".format(sp["interpreted"]) if sp else "?",
             sp["interpreted_pct"] if sp else 0.0))
    if fired:
        print("  first firing: bank $%02X at frame %d, pc $%04X, %s of 8,192 bytes "
              "differ (written by %s, pc $%s in bank $%s at frame %s)"
              % (bank, gframe, int(fired.group(3), 16),
                 "{:,}".format(differ), writer, fired.group(6), fired.group(7),
                 fired.group(8)))
    print("  guard log: %d -> interpreter, %d -> translation (%s)"
          % (len(routed_log), len(released_log),
             ", ".join("$" + b for b in released_banks) or "none"))
    print("  guard off: frame %s, %s at $%s, interpreted %s (%.4f%%), %d stale banks "
          "measured but not routed"
          % (off_st["frames"] if off_st else "?",
             ("reason=" + off_stop.group(1)) if off_stop else "?",
             off_stop.group(2) if off_stop else "?",
             "{:,}".format(off_sp["interpreted"]) if off_sp else "?",
             off_sp["interpreted_pct"] if off_sp else 0.0,
             len(set(b for b, _f in off_stale))))
    print("  wall: %.1f s (guarded %.1f s, unguarded %.1f s); logs %s and %s"
          % (on.elapsed + off.elapsed, on.elapsed, off.elapsed, on.log_path,
             off.log_path))

    ok = chk.report("%d frames guarded, %d unguarded"
                    % (st["frames"] if st else 0, off_st["frames"] if off_st else 0))
    if ok and off_stop and st:
        print("        finding: the guard is what makes the reload survivable -- with it "
              "the run interprets the reloaded banks and finishes at frame %d, without "
              "it the stale translation runs on and stops untranslated at $%s, frame %d."
              % (st["frames"], off_stop.group(2), off_st["frames"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
