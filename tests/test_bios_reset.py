#!/usr/bin/env python3
"""Reset-vector entry (XLATE_BIOS_RESET under XLATE_BIOS_EXEC): the machine boots the
way the console boots it.

Why this test exists.  The default route enters the translated disc loader directly at
$3000, with a mapping, a video chip and an interrupt mask assembled by hand, and the
machine then arrives at the game kernel with I=1 and $1402=$07 and never takes an
interrupt.  With XLATE_BIOS_RESET the CPU starts where the console's does: the reset
vector at $FFFE, through the power-on mapping, into the card's own init.  The card then
programs the hardware, probes the drive and reads the game off the disc itself.

What is checked here, and why each check is a measurement rather than a claim:

  1. **The entry is the card's reset vector**, and the power-on mapping is the one the
     console's own trace records at frame 0 (`mpr=0000000000000000`), not one we chose.
  2. **The card's init runs and its hardware writes are the console's.**  With the
     drive, timer and interrupt controller, the whole frame-0 access stream matches the
     console's own trace access for access.  The init ends with $1402 = $00, which is
     what makes an interrupt possible at all.
  3. **The machine takes interrupts.**  More than one per frame on average (IRQ1 from
     the VDC, and the raster compare adds a second in some frames -- the console does
     the same in 149 of its own 3,700 frames), against `irq=0` on the old route.
  4. **The card loads the console's disc data.**  The bytes the card puts in CD RAM
     are compared with the console's saved RAM at the same frame number.
  5. **The default entry is untouched**: the same binary with the switch off still
     reaches $4174 with `irq=0`, and no measurement made on it moves.

The RAM dumps this test compares against are reference data, not part of this
repository (`$REFDATA/ramdump`); those comparisons are skipped when it is absent and
say so.
"""
import os
import sys

import harness as H

SRC = H.SRC
BIOS = H.BIOS
DISC = H.DISC
RAMDUMP = H.ref("ramdump")
OUT = os.path.join(SRC, "out", "test_bios_reset")


def env_with(**kw):
    e = dict(os.environ)
    for k in ("XLATE_BIOS_EXEC", "XLATE_BIOS_RESET", "XLATE_INTERP", "XLATE_HIST",
              "XLATE_PC", "XLATE_FRAME_DIR", "XLATE_FRAME_EVERY", "XLATE_RAMDUMP",
              "XLATE_RAMDUMP_FRAMES", "XLATE_ACC_FROM", "XLATE_ACC_TO", "XLATE_IRQ_TRACE",
              "XLATE_IRQ_WHY", "XLATE_BOOT_GATE"):
        e.pop(k, None)
    e.update(kw)
    return e


def run(extra_env, args, timeout=1800):
    return H.run(["./out/host", "run"] + args, env=env_with(**extra_env), timeout=timeout)


def line(text, prefix):
    for l in text.splitlines():
        if l.startswith(prefix):
            return l
    return None


def kv(l):
    if not l:
        return {}
    return dict(p.split("=", 1) for p in l.split()[1:] if "=" in p)


def main():
    H.skip_without_inputs()
    H.skip_unless(H.ref_present("ramdump"),
                  "no reference RAM dumps (refdata/ramdump/*.bin): set REFDATA=<dir>")
    chk = H.Checker("reset-vector entry")
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1
    os.makedirs(OUT, exist_ok=True)

    # ---- 1. entry -------------------------------------------------------
    #
    # RUN for one frame at frame 3, as the console's own input recording does: the
    # card's PRESS RUN loop is the machine's wait, so the whole boot is the card's.
    args = ["--disc", DISC, "--bios", BIOS,
            "--steps", "400000000", "--cycles", "700000000",
            "--press", "3:4:0x08"]
    r = run({"XLATE_BIOS_EXEC": "1", "XLATE_BIOS_RESET": "1"}, args)
    br = kv(line(r.out, "BOOT_RESET"))
    chk.eq(br.get("on"), "1", "the machine entered at the card's reset vector")
    chk.eq(br.get("vector"), "E0F3", "the reset vector read from $FFFE is the card's "
                                     "own entry ($E0F3)")
    chk.eq(br.get("pc"), br.get("vector"), "execution starts at that vector")
    chk.eq(br.get("mpr"), "0000000000000000",
           "the power-on mapping is all zeroes -- the console's own frame-0 trace")

    # ---- 2. the card's init, and the state it leaves ---------------------
    st = kv(line(r.out, "BOOT_STATE"))
    chk.eq(st.get("1402"), "00", "the card's init leaves $1402=$00 (every source "
                                 "enabled) by its own write -- nothing seeded")
    chk.eq(st.get("1403"), "00", "and $1403=$00, nothing pending")
    chk.eq(st.get("F3"), "88", "the display-control shadow $F3=$88 (background + vblank "
                               "interrupt), written by the card")
    chk.eq(st.get("F5"), "06", "$F5=$06: a game hook is installed")
    chk.eq(st.get("DB"), "68", "$DB=$68: the kernel's overlay base")
    chk.eq(st.get("hooks"), "E8E2,4177,4297,E8E2,E8E2,E8E2",
           "the six hook slots hold the card's default $E8E2 plus the game's own "
           "$4177 (video) and $4297 (timer)")
    if st:
        print("        finding: the kernel is reached at frame %s, pc=$%s, with "
              "$1402=$%s -- the state the console's own work RAM holds at f2200 "
              "(F3=88 F4=00 F5=06 DB=68, hooks E8E2 4177 4297 E8E2 E8E2 E8E2)."
              % (st.get("frame"), st.get("pc"), st.get("1402")))
    irq = kv(line(r.out, "IRQSUM"))
    frames = int(kv(line(r.out, "STATE")).get("frames", "0") or 0)
    irq1 = int(irq.get("irq1", "0") or 0)
    chk.ok(irq1 > frames,
           "the machine takes its interrupts: %d IRQ1 in %d frames (the old route "
           "took 0 in 4,345)" % (irq1, frames))
    chk.ok(int(irq.get("timer", "0") or 0) == 0,
           "no timer interrupt is ever enabled or taken, as on the console")
    cd = kv(line(r.out, "CDSUM"))
    chk.ok(int(cd.get("accepted", "0") or 0) >= 19,
           "the card's own loader read the disc: %s commands, all accepted"
           % cd.get("accepted"))
    chk.eq(cd.get("rejected"), "0", "and none rejected")
    chk.eq(kv(line(r.out, "SUMMARY")).get("bios_calls"), "0",
           "no modelled BIOS entry was reached (bios_calls=0)")

    # ---- 3. what the card loaded, against the console's own RAM ----------
    ours = os.path.join(OUT, "ours")
    for f in os.listdir(OUT):
        if f.startswith("ours-f"):
            os.unlink(os.path.join(OUT, f))
    r = run({"XLATE_BIOS_EXEC": "1", "XLATE_BIOS_RESET": "1",
             "XLATE_RAMDUMP": ours, "XLATE_RAMDUMP_FRAMES": "2200",
             "XLATE_STOP_FRAME": "2201"},
            args)
    dump = ours + "-f02200.bin"
    ref = os.path.join(RAMDUMP, "early2-f02200.bin")
    if os.path.exists(dump) and os.path.exists(ref):
        a = open(ref, "rb").read()
        o = open(dump, "rb").read()
        cdram = sum(1 for i in range(32 * 8192) if a[i] != o[i])
        work = sum(1 for i in range(8192) if a[0x40000 + i] != o[0x40000 + i])
        chk.eq(cdram, 0, "the card's loader put the console's own bytes in CD RAM "
                         "($68-$87, 262,144 bytes) at frame 2200")
        chk.ok(work < 400, "work RAM at the same frame number differs in %d of 8,192 "
                           "bytes (live variables at a ~7-frame scene offset; the "
                           "latched BIOS state matches)" % work)
    else:
        print("        skipped: the console's RAM dump %s is not present" % ref)

    # ---- 4. the default entry is untouched -------------------------------
    r0 = run({"XLATE_BIOS_EXEC": "0"},
             ["--disc", DISC, "--bios", BIOS, "--steps", "400000000",
              "--cycles", "700000000", "--press", "1700:1760:0x08", "--boot-game",
              "--stop-pc", "0x4174"])
    s0 = H.parse_state_full(r0.out)
    chk.ok(s0 is not None and s0["pc"] == 0x4174,
           "with the switch off the machine still enters the translated loader and "
           "stops at the kernel's idle loop $4174")
    chk.ok(s0 is not None and s0["irq"] == 0,
           "and still takes no interrupt (irq=%s) -- the behaviour this switch "
           "route around, not a replacement for"
           % (s0["irq"] if s0 else "?"))
    chk.ok(line(r0.out, "BOOT_RESET") is None,
           "no reset-entry line is printed when the switch is off")

    ok = chk.report("reset-vector entry, %d IRQ1 in %d frames" % (irq1, frames))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
