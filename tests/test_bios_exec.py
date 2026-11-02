#!/usr/bin/env python3
"""System Card execution (XLATE_BIOS_EXEC): the switch, and the machine's
own contract around it.

What this test is for.  Until now every pc at or above $E000 was handed to a
hand-written model (`xl_bios_call`) and never run.  XLATE_BIOS_EXEC=1 makes a pc whose
page holds the user-supplied System Card execute on the interpreter instead.  Four
things then have to be true, and each of them is checked here rather than assumed:

  1. **The card is in memory and read-only.**  A store aimed at a ROM page changes
     nothing: the whole 256 KB image is compared byte for byte against the user's file
     after a run that deliberately writes to it.
  2. **The vectors are the card's own.**  $FFF6-$FFFF is read through the MPRs and
     compared against the same ten bytes of the file.  The CPU then takes interrupts
     through that table instead of through the model's hook dispatch.
  3. **A call into the card enters the routine and comes back.**  The game's per-frame
     `jsr $E045` is counted at the call site and at its return address: one return per
     entry, and the machine keeps running.
  4. **The switch is real.**  Under it, no modelled BIOS entry is reached at all
     (`bios_calls=0`) while the number of System Card instructions actually interpreted
     is large; with the switch off the same run reaches the models and interprets none
     of the card.

The last section records, as a measurement, how far the executed card gets now that the
drive has a command decoder: it counts the commands the card's own driver issued, checks
that the drive accepted every one, and names the pc and frame where the boot route stops
instead.  That assertion is deliberate -- if a later change moves the stopping point, this
test goes red and says so rather than quietly absorbing it.
"""
import os
import sys

import harness as H

SRC = H.SRC
STATE = H.ref("states", "f2400A.img")
OUT = H.scratch("biosexec")


def env_with(**kw):
    e = dict(os.environ)
    for k in ("XLATE_BIOS_EXEC", "XLATE_INTERP", "XLATE_HIST", "XLATE_PC",
              "XLATE_FRAME_DIR", "XLATE_FRAME_EVERY", "XLATE_RAMDUMP"):
        e.pop(k, None)                      # a stray shell switch must not leak in
    e.update(kw)
    return e


def run(extra_env, args, timeout=1800):
    return H.run(["./out/host", "run"] + args, env=env_with(**extra_env), timeout=timeout)


def rom_vectors():
    """The five 16-bit vectors as the CPU reads them: bank $00 at $E000, so they are the
    last ten bytes of the first 8 KB page of the user's file."""
    rom = open(H.BIOS, "rb").read()
    return [rom[0x1FF6 + 2 * i] | (rom[0x1FF6 + 2 * i + 1] << 8) for i in range(5)]


def main():
    H.skip_without_inputs()
    H.skip_unless(H.ref_present("states"),
                  "no state image (refdata/states/f2400A.img): set REFDATA=<dir>")
    chk = H.Checker("System Card execution")
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1
    os.makedirs(OUT, exist_ok=True)

    common = ["--disc", H.DISC, "--bios", H.BIOS, "--from-state", STATE]

    # ---- 1. the card is in memory, and read-only -------------------------
    mem_path = OUT + "/mem.bin"
    r = run({"XLATE_BIOS_EXEC": "1"},
            common + ["--steps", "3000000", "--cycles", "20000000",
                      "--poke", "0xE000=0xA5", "--dump", mem_path])
    rom = open(H.BIOS, "rb").read()
    mem = H.parse_dump(mem_path)
    chk.ok(len(mem) == 2 * 1024 * 1024, "the dump is the whole 2 MB address space")
    chk.ok(mem[0:len(rom)] == rom,
           "physical banks $00-$1F are the supplied System Card, byte for byte "
           "(%d bytes)" % len(rom))
    chk.ok(mem[0] == rom[0],
           "a store aimed at $E000 did not change the card (the page still starts "
           "with $%02X, the reset vector's jmp)" % mem[0])
    s = r.out
    chk.ok("rom_writes_ignored=1" in s or "rom_writes_ignored=2" in s,
           "the ignored ROM store was counted (%s)"
           % (s.split("rom_writes_ignored=")[1].split()[0]
              if "rom_writes_ignored=" in s else "no SUMMARY line"))

    # ---- 2. the vectors are the card's own ------------------------------
    m = None
    for line in r.out.splitlines():
        if line.startswith("VECTORS "):
            m = dict(kv.split("=") for kv in line.split()[1:])
    chk.ok(m is not None, "the run reported the vector table it reads")
    if m:
        want = rom_vectors()
        got = [int(m[k], 16) for k in ("irq2", "irq1", "timer", "nmi", "reset")]
        chk.eq(got, want, "vectors at $FFF6-$FFFF are the card's own table "
                          "(IRQ2, IRQ1, timer, NMI, reset)")
        chk.eq(m["from_rom"], "1", "the vectors were read through a ROM page")
        chk.ok(0xE000 <= got[1] <= 0xFFFF,
               "IRQ1's vector points into the card ($%04X)" % got[1])

    # ---- 3. a call into the card enters and returns ---------------------
    markers = "E045,B54D,E944,E95A"
    r = run({"XLATE_BIOS_EXEC": "1", "XLATE_HIST": "1", "XLATE_PC": markers},
            common + ["--steps", "20000000", "--cycles", "160000000"])
    counts = {}
    for line in r.out.splitlines():
        if line.startswith("PC_COUNTS"):
            for kv in line.split()[1:]:
                k, v = kv.split("=")
                counts[int(k, 16)] = int(v)
    chk.ok(counts.get(0xE045, 0) > 100,
           "the card's $E045 (drive status, ROM $F6DB) was actually executed "
           "(%d times)" % counts.get(0xE045, 0))
    chk.eq(counts.get(0xB54D, 0), counts.get(0xE045, -1),
           "every jsr $E045 came back: %d entries, %d returns at $B54D"
           % (counts.get(0xE045, 0), counts.get(0xB54D, 0)))

    # ---- 4. the switch is real ------------------------------------------
    ex = [999]
    for line in r.out.splitlines():
        if line.startswith("SUMMARY"):
            ex = [int(kv.split("=")[1]) for kv in line.split()[1:] if kv.startswith(
                ("bios_calls=", "bios_interp="))]
    chk.ok(len(ex) == 2, "SUMMARY carries bios_calls and bios_interp")
    if len(ex) == 2:
        chk.eq(ex[0], 0, "no modelled BIOS entry was reached (bios_calls=0)")
        chk.ok(ex[1] > 1000,
               "%d System Card instructions were interpreted instead "
               "(its interrupt handlers and the game's own BIOS calls -- no longer a "
               "poll loop: the drive answers it)" % ex[1])

    # the default path is untouched: same run, same binary, switch off
    r0 = run({"XLATE_BIOS_EXEC": "0"},
             common + ["--steps", "20000000", "--cycles", "160000000"])
    d = [0, 0]
    for line in r0.out.splitlines():
        if line.startswith("SUMMARY"):
            d = [int(kv.split("=")[1]) for kv in line.split()[1:] if kv.startswith(
                ("bios_calls=", "bios_interp="))]
    chk.ok(d[0] > 100, "with the switch off the models are still reached "
                       "(bios_calls=%d)" % d[0])
    chk.eq(d[1], 0, "and no System Card instruction is interpreted (bios_interp=0)")

    # ---- 5. the picture, and where the executed card stops --------------
    base_dir, exec_dir = OUT + "/base", OUT + "/exec"
    for d in (base_dir, exec_dir):
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.unlink(os.path.join(d, f))
    run({"XLATE_BIOS_EXEC": "0", "XLATE_FRAME_EVERY": "4", "XLATE_FRAME_DIR": base_dir},
        common + ["--steps", "3000000", "--cycles", "20000000"])
    run({"XLATE_BIOS_EXEC": "1", "XLATE_FRAME_EVERY": "4", "XLATE_FRAME_DIR": exec_dir},
        common + ["--steps", "3000000", "--cycles", "20000000"])
    fb, fe = base_dir + "/f00004.ppm", exec_dir + "/f00004.ppm"
    chk.ok(os.path.exists(fb) and os.path.exists(fe),
           "both runs produced frame 4")
    if os.path.exists(fb) and os.path.exists(fe):
        chk.eq(open(fe, "rb").read(), open(fb, "rb").read(),
               "frame 4 from the executed card is byte-identical to the modelled one")

    # ---- 5b. the drive answers the card's own driver ---------------------
    #
    # The card is a real driver: it selects the drive, pushes a command byte by byte
    # against REQ/ACK and reads the answer out of $1800-$180F.  A cold boot is the run
    # where that happens (the state above is already past the disc load), so this is
    # where the command census means something.
    r = run({"XLATE_BIOS_EXEC": "1", "XLATE_READTIME": "1", "XLATE_HIST": "1",
             "XLATE_PC": "E944,E95A,4174"},
            ["--disc", H.DISC, "--bios", H.BIOS,
             "--steps", "400000000", "--cycles", "700000000",
             "--press", "1700:1760:0x08", "--boot-game", "--stop-pc", "0x4174"])
    pc = {}
    cd = {}
    for line in r.out.splitlines():
        if line.startswith("PC_COUNTS"):
            for kv in line.split()[1:]:
                k, v = kv.split("=")
                pc[int(k, 16)] = int(v)
        if line.startswith("CDSUM"):
            for kv in line.split()[1:]:
                k, v = kv.split("=")
                cd[k] = int(v)
    chk.ok(pc.get(0xE95A, 0) > 0,
           "the drive accepted the card's command ($E95A, the transfer-ready path, was "
           "reached %d times)" % pc.get(0xE95A, 0))
    chk.eq(pc.get(0xE944, 0), pc.get(0xE95A, 0),
           "every command the card started (%d) was selected and accepted"
           % pc.get(0xE944, 0))
    chk.eq(cd.get("rejected", -1), 0,
           "no command was rejected out of %d" % cd.get("cmds", -1))
    chk.ok(cd.get("accepted", 0) >= 19,
           "the boot route issued %d commands and the drive accepted them all"
           % cd.get("accepted", 0))
    st = H.parse_state_full(r.out)
    chk.ok(st is not None and st["pc"] == 0x4174,
           "the boot route gets past the drive and runs the loaded kernel to its idle "
           "loop at $4174")
    if st:
        print("        finding: on THIS route the card's commands are accepted and the "
              "whole disc load completes; the machine then stops at pc $%04X on frame %d "
              "with P.I=%d and $1402=$07, so no IRQ1 is ever taken -- the console takes "
              "one at that same pc on frame 1,957.  That is expected here: this run enters "
              "the translated loader at $3000, so the card's reset init never runs.  "
              "test_bios_reset.py measures the other route (XLATE_BIOS_RESET=1), where the "
              "card initialises the hardware itself and the machine does take interrupts."
              % (st["pc"], st["frames"], 1 if (st["p"] & 4) else 0))

    # ---- 6. the drive's register interface, as its own unit test --------
    #
    # No CPU code and no disc: the probe drives $1801/$1800/$1802/$1808 the way the
    # card's driver does and prints what comes back.  This is where "an unknown
    # command is not silently accepted" is checked, because the card never sends one
    # on this route.
    r = H.run(["./out/host", "cdprobe"])
    lines = [l for l in r.out.splitlines() if l.startswith("CDPROBE")]
    fails = [l for l in lines if l.endswith("FAIL")]
    chk.eq(r.rc, 0, "cdprobe: every expectation held (%d failed)" % len(fails))
    chk.ok(any("unknown 0x42 -> status" in l and "got=02" in l for l in lines),
           "cdprobe: an unknown opcode is answered CHECK CONDITION, not accepted")
    chk.ok(any("unknown 0xFF counted" in l for l in lines),
           "cdprobe: refusals are counted per opcode")
    chk.ok(any("$DE track 2 second" in l and "got=53" in l for l in lines) and
           any("$DE track 22 frame" in l and "got=62" in l for l in lines),
           "cdprobe: Get Dir Info answers the console's own TOC (00:53:65 / 46:58:62)")

    ok = chk.report("card executed, %d BIOS instructions" % (ex[1] if len(ex) == 2 else 0))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
