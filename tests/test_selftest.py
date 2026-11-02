#!/usr/bin/env python3
"""Hand-checked self-test: 20 short sequences with expectations worked out by hand.

Every expectation comes from mk_selftest.py, next to the instructions that produce
it -- not from any interpreter.  This test only runs the translated code and
compares.
"""
import json
import os
import sys

import harness as H

SRC = H.SRC
MPR_FOR_WINDOW = {1: 0xF8, 2: 0xA5, 6: 0x84}   # work RAM, the TAM'd bank, data bank


def main():
    chk = H.Checker("selftest (hand-checked)")

    # Regenerate first: the expectations and the binary must come from the same
    # source, never from a previous run.
    b = H.run(["./build.sh", "selftest"])
    if b.rc != 0:
        print(b.out)
        print(b.err)
        return 1
    exp_path = os.path.join(SRC, "out", "selftest_expect.json")
    with open(exp_path) as f:
        exp = json.load(f)

    dump = H.scratch("selftest_mem.bin")
    r = H.run(["./out/selftest_host", "run", "--bios", H.BIOS, "--log", H.LOG,
               "--steps", "20000", "--stop-pc", hex(exp["done_pc"]), "--dump", dump, "--hw"])
    if r.rc not in (0,):
        print(r.out)
        print(r.err)
        print("host failed")
        return 1

    state = H.parse_state(r.out)
    chk.ok(state is not None, "host printed a STATE line")
    if not state:
        print(r.out)
        return 1
    mem = H.parse_dump(dump)
    hw = H.parse_hw(r.out)
    stop = H.parse_stop(r.out)
    chk.eq(stop["reason"] if stop else None, "stop-pc", "stopped at the final self-loop")
    chk.eq(stop["pc"] if stop else None, exp["done_pc"], "stopped at 'done'")

    for blk in exp["blocks"]:
        e = blk["expect"]
        i = blk["index"]
        rec = blk["record"]
        # the record table lives at $DF00 in the bank mapped at $C000
        off = (0x84 * 8192) + (rec & 0x1FFF)
        got_a, got_p, got_x, got_y = mem[off:off + 4]
        tag = "[%02d %s]" % (i, blk["name"])
        if e.get("a") is not None:
            chk.eq(got_a, e["a"], tag + " A (%s)" % e["why"])
        if e.get("x") is not None:
            chk.eq(got_x, e["x"], tag + " X")
        if e.get("y") is not None:
            chk.eq(got_y, e["y"], tag + " Y")
        mask = (e.get("mask", 0)) & ~(H.FLAG_B | H.FLAG_T)
        chk.eq(got_p & mask, e.get("flags", 0) & mask, tag + " flags")
        for (addr, want) in e.get("mem", []):
            bank = MPR_FOR_WINDOW.get((addr >> 13) & 7)
            gotv = mem[bank * 8192 + (addr & 0x1FFF)]
            chk.eq(gotv, want, tag + " mem[$%04X]" % addr)
        if "mpr2" in e:
            chk.eq(state["mpr"][2], e["mpr2"], tag + " MPR2")
        for (reg, count) in e.get("hw", []):
            chk.eq(hw.get(reg, 0), count, tag + " hardware writes at $%04X" % reg)

    # The self-test must not have trapped anywhere.
    traps = r.out.count("TRAP")
    chk.eq(traps, 0, "no traps during the self-test")

    ok = chk.report("%d blocks" % len(exp["blocks"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
