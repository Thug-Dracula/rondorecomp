#!/usr/bin/env python3
"""Bank-keyed translation: same address, different modules.

Four things are checked, because four things could go wrong:

1. **Two modules at the same address.** A driver pages bank $DB into $4000, calls
   $4000, then pages bank $DC into the same address and calls it again. Each module
   writes a different byte, so "the right module ran" is observable, not assumed.

2. **The untranslated path.** Paging in bank $DD, which has no translated code,
   must stop with a recorded event naming the bank -- not run whatever bytes are
   there. The summary must list it.

3. **Real code, two banks.** The game kernel image (record 67) placed at $DB and
   $DC: the traced opcode at $4000 must be the first byte of the *right* half of
   that image, which differs between the two.

4. **The boot chain really changes banks.** The loader's final `jmp $4000` leaves
   bank $F8 and lands in bank $80; both resolutions are asserted.
"""
import json
import os
import re
import sys

import harness as H

SRC = H.SRC
KERNEL = None


def parse_trace(text):
    rows = []
    for m in re.finditer(r"^TRACE pc=([0-9A-F]{4}) bank=([0-9A-F]{2}) op=([0-9A-F]{2})",
                         text, re.M):
        rows.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)))
    return rows


def main():
    H.skip_without_inputs()
    global KERNEL
    KERNEL = H.block_paths()["kernel"]
    chk = H.Checker("bank-keyed translation")
    b = H.run(["./build.sh", "banktest"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1
    # the boot chain is checked here too, so its host must be current
    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out, b.err)
        return 1

    # ---------------------------------------------------------------- 1 & 2 --
    dump = H.scratch("bank_mem.bin")
    r = H.run(["./out/bank_unit_host", "run", "--steps", "500", "--dump", dump,
               "--log", H.LOG])
    st = H.parse_state_full(r.out)
    mem = H.parse_dump(dump)
    data_bank = st["mpr"][6] if st else 0x84
    a = mem[data_bank * 8192 + 0x1F00]
    bl = mem[data_bank * 8192 + 0x1F01]
    chk.eq(a, 0xA5, "bank $DB's module at $4000 ran (wrote $A5)")
    chk.eq(bl, 0x5A, "bank $DC's module at the same $4000 ran (wrote $5A)")

    stop = H.parse_stop(r.out)
    chk.eq(stop["reason"] if stop else None, "untranslated",
           "the run ended on the untranslated-bank path, not a crash")
    chk.ok("UNTRANSLATED bank=DD pc=4000" in r.out,
           "the event names the bank and the address")
    chk.ok("UNTRANSLATED_BANKS DD" in r.out, "the summary lists bank $DD")
    chk.eq(r.rc, 3, "exit code distinguishes 'untranslated' from a crash")
    # the event must also reach the log file, not just the console
    with open(H.LOG, encoding="utf-8", errors="replace") as f:
        logged = f.read()
    chk.ok("untranslated: bank $DD has no code for $4000" in logged,
           "the event is recorded in the log file")

    # the two units really are different code at the same logical address
    with open(os.path.join(SRC, "out", "bank_unit_expect.json")) as f:
        exp = json.load(f)
    ua = exp["units"].get("DB_0000")
    ub = exp["units"].get("DC_0000")
    chk.ok(ua and ub, "both banks have a unit at offset $0000")
    chk.ok(ua != ub, "the two units at offset $0000 hold different instructions "
                     "(%s vs %s)" % (ua, ub))
    chk.ok("DD_0000" not in exp["units"], "bank $DD has no unit, as intended")

    # ------------------------------------------------------------------- 3 --
    kernel = open(KERNEL, "rb").read()
    traces = {}
    for tag, bank, first in (("klo", 0xDB, kernel[0]), ("khi", 0xDC, kernel[8192])):
        rr = H.run(["./out/bank_%s_host" % tag, "run", "--steps", "4", "--trace"])
        rows = parse_trace(rr.out)
        at4000 = [x for x in rows if x[0] == 0x4000]
        chk.ok(bool(at4000), "%s: execution reached $4000" % tag)
        if at4000:
            pc, bnk, op = at4000[0]
            chk.eq(bnk, bank, "%s: $4000 resolves to bank $%02X" % (tag, bank))
            chk.eq(op, first, "%s: the byte executed at $4000 is that bank's first "
                              "byte ($%02X)" % (tag, first))
            traces[tag] = (bnk, op)
    chk.ok(traces.get("klo") != traces.get("khi"),
           "the same address executed different bytes in the two banks (%r vs %r)"
           % (traces.get("klo"), traces.get("khi")))

    # ------------------------------------------------------------------- 4 --
    rr = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
                "--steps", "200000", "--stop-pc", "0x4004", "--trace"])
    rows = parse_trace(rr.out)
    before = [x for x in rows if x[0] == 0x3099]
    after = [x for x in rows if x[0] == 0x4000]
    chk.ok(before and before[-1][1] == 0xF8,
           "boot chain: the loader's jmp at $3099 runs in bank $F8")
    chk.ok(after and after[0][1] == 0x80,
           "boot chain: the jump lands at $4000 in bank $80 (a real bank change)")
    if before and after:
        chk.ok(before[-1][1] != after[0][1],
               "before/after: $3099 resolves to bank $%02X, $4000 to bank $%02X"
               % (before[-1][1], after[0][1]))

    ok = chk.report("4 groups")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
