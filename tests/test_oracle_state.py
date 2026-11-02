#!/usr/bin/env python3
"""State-level comparison against the oracle's savestates.

Frame-level comparison is weak here: our chain starts at the loader, the oracle's clock
starts at the BIOS screen, and the two machines are in different code for most of the
run. So this test compares *states*: PC, registers, MPR and memory hashes, read
directly out of the savestates (tools/oracle_state.py).

What it establishes:

  1. the oracle's own phase structure, from its PC and MPR alone;
  2. that our model's bank numbering agrees with the oracle's during the boot phase
     (both have the boot program at banks $80-$83), so the comparison is meaningful;
  3. where the machines diverge: during the whole pre-boot phase the oracle executes
     ROM bank 1 at $C000 (the BIOS drawing its screen and waiting), and by the time the
     Konami logo appears it is running game code from banks $69+;
  4. that our translated chain never executes any bank outside {$00,$80,$F8} - i.e.
     it never runs BIOS code and never runs the game.
"""
import os
import sys

import harness as H

SRC = H.SRC
REF = H.ref("captures")
sys.path.insert(0, H.TOOLS)
from oracle_state import load                      # noqa: E402


def main():
    H.skip_unless(H.ref_present("captures"),
                  "no reference savestates (refdata/captures/*.state): set REFDATA=<dir>")
    chk = H.Checker("oracle state comparison")

    phases = {}
    for f in ("f600_A", "f1200_A", "f2400_A", "f3600_A", "f10800_A", "f10800_C",
              "f11400_C", "f12600_C", "f18000_A", "f21600_A"):
        path = os.path.join(REF, f + ".state")
        if not os.path.exists(path):
            continue
        phases[f] = load(path)
    chk.ok(len(phases) >= 8, "read %d savestates" % len(phases))

    # 1. the phase structure, printed so the numbers are visible, not just asserted
    print("  %-9s %-6s %-3s %-22s %s" % ("capture", "PC", "pg", "MPR", "phase"))
    for f, st in sorted(phases.items(), key=lambda kv: int(kv[0][1:].split('_')[0])):
        mpr = " ".join("%02X" % b for b in st["mpr"])
        phase = ("BIOS ROM bank %02X" % st["mpr"][6]) if st["pc"] >= 0xC000 else \
                ("CD RAM banks %02X.." % st["mpr"][2])
        print("  %-9s $%04X  %d   %-22s %s" % (f, st["pc"], st["pc_page"], mpr, phase))

    # 2. bank numbering must agree during the boot phase
    boot = phases.get("f600_A") or phases.get("f1200_A")
    chk.ok(boot is not None, "a pre-boot capture exists")
    if boot:
        chk.eq(boot["mpr"][2:6], [0x80, 0x81, 0x82, 0x83],
               "the oracle's boot program is in banks $80-$83, as in our model")
        chk.eq(boot["mpr"][6], 0x01,
               "and ROM bank $01 is mapped at $C000 while the BIOS draws its screen")
        chk.ok(boot["pc"] >= 0xC000,
               "during the pre-boot phase the oracle executes ROM code at $%04X"
               % boot["pc"])

    # 3. by the Konami-logo capture the game is running from CD RAM banks below $80
    logo = phases.get("f2400_A")
    chk.ok(logo is not None, "a post-boot capture exists")
    if logo:
        chk.ok(0x4000 <= logo["pc"] < 0xC000,
               "at the logo the oracle runs game code at $%04X" % logo["pc"])
        chk.ok(min(logo["mpr"][2:7]) < 0x80,
               "from CD RAM banks below $80 (%s), i.e. a different region from the "
               "boot program's" % " ".join("%02X" % b for b in logo["mpr"][2:7]))
        chk.ok(logo["mpr"][2:7] != boot["mpr"][2:7] if boot else True,
               "and a different bank set than the boot program's")

    # 4. our chain never leaves the three banks it starts with
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1
    r = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
               "--steps", "400000000", "--cycles", "1450000000",
               "--press", "1700:1760:0x08", "--press", "4000:4060:0x08",
               "--press", "7000:7060:0x08", "--press", "10000:10060:0x08"])
    import re
    m = re.search(r"^BANKS_EXECUTED (.*)$", r.out, re.M)
    used = [x.split("@")[0] for x in m.group(1).split()] if m else []
    chk.eq(used, ["00", "80", "F8"],
           "our translated chain executes only the BIOS stub, the boot program and "
           "the loader")
    chk.ok(r.out.count("UNTRANSLATED bank") == 0, "no untranslated bank was hit")

    ok = chk.report("%d captures" % len(phases))
    print("        divergence: the oracle spends the pre-boot phase inside ROM bank 1")
    print("        (BIOS code at $C9xx) and then runs game code from banks $69+; our")
    print("        model stubs all ROM, so neither ever happens here.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
