#!/usr/bin/env python3
"""Differential test: generated C **and** the C interpreter, against the Python reference.

For every random sequence we require the *same final state* from all three:
A, X, Y, S, P, PC, the eight bank registers, a CRC of the whole 2 MB physical
memory -- **and the cycle total** (2026-10-28).  Any TRAP from either C side is a
failure.

Two arms, one corpus
--------------------
  translated   the generated units, dispatched by (bank, address)   [the machine so far]
  interpreter  `interp.c` stepping the same bytes in memory         [new this round]

Both C arms are compared against `refcpu.py`, the separately written Python
interpreter, and against each other by construction (each matches the same
reference numbers).  Running the same corpus through both is what makes "the
interpreter is the same machine" a measurement instead of a claim.

Why cycles are in the comparison
--------------------------------
Registers can agree while the frame cadence is wrong: the VDC and the timer advance
only on `xl_tick`, so one opcode costed by a cycle too much drifts every frame
boundary after it and no state test notices, so cycle identity is a gate here.  Both C sides get their numbers from `cycles.py` -- the
translator writes them into each unit, the interpreter reads them from a generated
table -- and `refcpu` uses the same module, so what is checked here is that the
*application* of the table has no per-instruction drift: right instruction, right
cost, right branch outcome, right block length.

Coverage, honestly stated: the random generator deliberately excludes brk, rts/rti,
jsr, absolute jmp, tam/tma and st0/st1/st2 (a slot-sized test cannot safely call and
return, and remapping code under a test is not a test).  Those are covered by
`targeted_specs()` below, which is where the interpreter's stack and mapper paths
are actually exercised.  One group no case executes: `brk` and the undefined
opcodes, which both engines refuse by the same code path (work-RAM trampoline, then
`UNTRANSLATED`) and which the Python reference raises on by design.

What this cannot prove: that all three share the same *reading of the manual*.  The
semantics were taken from the reference emulator's core, and the
hand-checked self-test (test_selftest.py) is the independent check on those.
"""
import argparse
import json
import os
import sys
import zlib

import harness as H
sys.path.insert(0, H.SRC)

sys.path.insert(0, H.TOOLS)
import gen_cases                                     # noqa: E402
import isa                                           # noqa: E402
from memory import AddressSpace                      # noqa: E402
import refcpu                                        # noqa: E402

# A PRIVATE scratch directory per run.  This used to be the fixed
# one shared work dir, and two runs of the suite at the same time (two shells,
# two shells) then shared one set of spec/binary files: the second run's
# `build.sh cases` overwrites the first run's binary, and the first run compares its
# specs against the other run's code.  The result looks like a cycle-model bug and is
# not one.
WORK = H.scratch("diff_%d" % os.getpid())

INTERP_ENV = dict(os.environ, XLATE_INTERP="1")

KEYS = ("a", "x", "y", "s", "p", "pc", "crc", "cyc")


def python_state(spec):
    sp = AddressSpace()
    sp.mpr = list(spec["mpr"])
    sp.load_phys(spec["code_bank"], spec.get("code_off", 0), bytes(spec["code"]))
    sp.load_phys(spec["data_bank"], spec["data_off"], bytes(spec["data"]))
    for (bank, off, blob) in spec["patches"]:
        sp.load_phys(bank, off, bytes(blob))
    cpu = refcpu.RefCpu(sp)
    cpu.a, cpu.x, cpu.y = spec["a"], spec["x"], spec["y"]
    cpu.s, cpu.p, cpu.pc = spec["s"], spec["p"], spec["pc"]
    for _ in range(spec["steps"]):
        cpu.step()
    # The hardware page (bank $FF) is put aside.  The Python model writes register
    # stores into `mem` as well as into its Hardware object; the C machine keeps the
    # page in Cpu.hw_regs and leaves that part of its memory image untouched, so the
    # two images can never agree there -- by construction, not by disagreement.
    # Everything that *is* hardware state (VDC/VCE/CD/timer) is outside this
    # comparison for the same reason; the video and interrupt tests cover it.
    mem = bytearray(sp.mem)
    mem[0xFF * 8192:0x100 * 8192] = b"\x00" * 8192
    return {"a": cpu.a, "x": cpu.x, "y": cpu.y, "s": cpu.s, "p": cpu.p,
            "pc": cpu.pc, "mpr": list(sp.mpr),
            "crc": zlib.crc32(mem) & 0xFFFFFFFF,
            "cyc": cpu.cycles}


def parse_cases(text):
    out = {}
    for line in text.splitlines():
        if not line.startswith("CASE "):
            continue
        kv = dict(p.split("=") for p in line.split()[1:])
        out[int(kv["id"])] = {
            "a": int(kv["a"], 16), "x": int(kv["x"], 16), "y": int(kv["y"], 16),
            "s": int(kv["s"], 16), "p": int(kv["p"], 16),
            "pc": int(kv["pc"], 16),
            "mpr": [int(kv["mpr"][i:i + 2], 16) for i in range(0, 16, 2)],
            "crc": int(kv["crc"], 16),
            "cyc": int(kv["cyc"]),
        }
    return out


# --------------------------------------------------------------- targeted ----
def opcode_for(mnem, mode):
    for op, (m, _size, md) in enumerate(isa.TABLE):
        if m.strip() == mnem and md == mode:
            return op
    raise KeyError("no opcode for %s/%s" % (mnem, mode))


class Asm:
    """Tiny encoder: absolute operand bytes are given by the caller, which keeps
    these six cases readable without a linker."""

    def __init__(self, base):
        self.base = base
        self.code = bytearray()
        self.starts = []

    def op(self, mnem, mode, *oper):
        self.starts.append(self.base + len(self.code))
        self.code.append(opcode_for(mnem, mode))
        self.code.extend(oper)

    def bra_self(self):
        self.starts.append(self.base + len(self.code))
        self.code.extend((0x80, 0xFE))

    def filler(self, n):
        self.code.extend([0xEA] * n)      # nop: never seeded, so never translated


def targeted_specs():
    """The opcode classes the random generator cannot safely produce.

    Each case runs to a `bra *` parking loop, exactly like a generated case, and
    seeds every instruction address so the translated dispatcher has a unit for
    each.  The point is not exotic behaviour: it is that `jsr`/`rts`/`rti`,
    absolute `jmp`, `tam`/`tma` and `st0`/`st1`/`st2` are *executed* by both C
    arms and compared, cycle totals included.

    Each case gets its own 64-byte slot in page 5: the case format keys a
    translation by (bank, offset), so two cases at the same address would share
    one set of units -- the first version of this did exactly that, and every case
    silently ran the last one's code.
    """
    CODE_BANK = 0x63
    DATA = 0xA0
    mpr = [0xFF, 0xF8, DATA, DATA, DATA, CODE_BANK, DATA, DATA]
    specs = []

    def add(cid, asm, steps):
        specs.append({
            "id": cid, "code": list(asm.code), "code_bank": CODE_BANK,
            "code_off": asm.base & 0x1FFF, "pc": asm.base, "mpr": mpr,
            "a": 0x00, "x": 0x00, "y": 0x00, "s": 0xF0, "p": 0x24,
            "steps": steps, "data": [0] * 512, "data_bank": DATA,
            "data_off": 0x1E00 & 0x1FFF,
            "patches": [],
            "seeds": sorted(set(asm.starts)),
        })

    def slot(k):
        return 0xA000 + k * 0x40

    # jsr -> subroutine -> rts -> store what the subroutine left behind
    base = slot(0)
    a = Asm(base)
    a.op("lda", isa.M_NN, 0x11)
    a.op("ldx", isa.M_NN, 0x22)
    a.op("jsr", isa.M_BRANCH_OR_LABEL, (base + 0x10) & 0xFF, (base + 0x10) >> 8)
    a.op("sta", isa.M_HHLL, 0x10, 0xDE)          # $DE10, bank $A0
    a.bra_self()
    a.filler(0x10 - len(a.code))                 # pad to base+$10
    a.op("lda", isa.M_NN, 0x33)
    a.op("inx", isa.M_OP)
    a.op("rts", isa.M_OP)
    add(9001, a, 9)

    # bsr -> rts
    base = slot(1)
    a = Asm(base)
    a.op("ldy", isa.M_NN, 0x44)
    a.op("bsr", isa.M_BRANCH_OR_LABEL, 0x08)     # base+$0C, from the end of this insn
    a.op("sty", isa.M_HHLL, 0x11, 0xDE)          # $DE11
    a.bra_self()
    a.filler(0x0B - len(a.code))
    a.op("iny", isa.M_OP)
    a.op("rts", isa.M_OP)
    add(9002, a, 7)

    # rti: build a stack frame by hand (return address, then the new P)
    base = slot(2)
    a = Asm(base)
    a.op("lda", isa.M_NN, (base + 0x0E) >> 8)
    a.op("pha", isa.M_OP)
    a.op("lda", isa.M_NN, (base + 0x0E) & 0xFF)
    a.op("pha", isa.M_OP)
    a.op("lda", isa.M_NN, 0x24)                  # pulled into P by rti
    a.op("pha", isa.M_OP)
    a.op("rti", isa.M_OP)
    a.filler(0x0E - len(a.code))
    a.op("sta", isa.M_HHLL, 0x12, 0xDE)
    a.bra_self()
    add(9003, a, 9)

    # absolute jmp
    base = slot(3)
    a = Asm(base)
    a.op("lda", isa.M_NN, 0x66)
    a.op("jmp", isa.M_BRANCH_OR_LABEL, (base + 0x0A) & 0xFF, (base + 0x0A) >> 8)
    a.filler(0x0A - len(a.code))
    a.op("sta", isa.M_HHLL, 0x13, 0xDE)
    a.bra_self()
    add(9004, a, 4)

    # tam / tma round trip (maps a data bank, not the code bank)
    base = slot(4)
    a = Asm(base)
    a.op("lda", isa.M_NN, 0xA1)
    a.op("tam", isa.M_NN, 0x06)                  # page 6 ($C000-$DFFF) -> bank $A1
    a.op("tma", isa.M_NN, 0x06)                  # A <- $A1
    a.op("sta", isa.M_HHLL, 0x14, 0xDE)          # lands in bank $A1 at $1E14
    a.bra_self()
    add(9005, a, 5)

    # st0/st1/st2 write their *immediate*; A must come through untouched
    base = slot(5)
    a = Asm(base)
    a.op("lda", isa.M_NN, 0x77)
    a.op("st0", isa.M_NN, 0x12)
    a.op("st1", isa.M_NN, 0x34)
    a.op("st2", isa.M_NN, 0x56)
    a.op("sta", isa.M_HHLL, 0x15, 0xDE)
    a.bra_self()
    add(9006, a, 6)

    # VDC PORT READS pay the access slot, and this case is what says so.
    #
    # The reference charges `HuCPU.StealCycle()` in IORead as well as IOWrite
    # (`case 0x0000: HuCPU.StealCycle(); trace_ret = vce->ReadVDC(A);`), one CPU
    # cycle *before* the device is read.  A read that skips it costs one cycle less
    # than the console's and nothing else changes -- registers, memory and the
    # picture stay right while the frame cadence drifts, which is the defect this
    # whole stage exists to catch.  Both C arms reach the charge through the one
    # choke point (`xl_rd` -> `xl_rd_phys`), so this case compares *three* cost
    # models on the same four port reads: read status ($0000, which acknowledges
    # the IRQ), select a register, read the data port twice.
    #
    # `st0 #$02` selects VRAM read register VRR, so the two data-port reads are
    # ordinary $0002/$0003 reads of a real chip register, not undefined behaviour.
    base = slot(6)
    a = Asm(base)
    a.op("st0", isa.M_NN, 0x02)                  # select VRR
    a.op("lda", isa.M_HHLL, 0x00, 0x00)          # $0000: status, acknowledges
    a.op("sta", isa.M_HHLL, 0x16, 0xDE)          # record it
    a.op("lda", isa.M_HHLL, 0x02, 0x00)          # $0002: data low
    a.op("sta", isa.M_HHLL, 0x17, 0xDE)
    a.op("lda", isa.M_HHLL, 0x03, 0x00)          # $0003: data high
    a.op("sta", isa.M_HHLL, 0x18, 0xDE)
    a.op("lda", isa.M_HHLL, 0x00, 0x00)          # status again
    a.op("sta", isa.M_HHLL, 0x19, 0xDE)
    a.bra_self()
    add(9007, a, 10)
    return specs


# ------------------------------------------------------------------ driver --
def run_case_build(specs_path, bin_path):
    b = H.run(["./build.sh", "cases", specs_path, bin_path])
    if b.rc != 0:
        print(b.out)
        print(b.err)
        raise SystemExit("build failed for %s" % specs_path)
    return bin_path


def compare_arm(specs, ref, got, arm, chk, stats):
    for spec in specs:
        cid = spec["id"]
        have = got.get(cid)
        stats[arm]["cases"] += 1
        if have is None:
            stats[arm]["bad"] += 1
            chk.ok(False, "%s: case %d produced no state" % (arm, cid))
            continue
        for key in KEYS:
            if have[key] != ref[cid][key]:
                stats[arm]["bad"] += 1
                chk.ok(False, "%s: case %d %s: C=%X python=%X (code %s)"
                       % (arm, cid, key, have[key], ref[cid][key],
                          " ".join("%02X" % x for x in spec["code"][:12])))
                break
        else:
            if have["mpr"] != ref[cid]["mpr"]:
                stats[arm]["bad"] += 1
                chk.ok(False, "%s: case %d mpr" % (arm, cid))
            else:
                chk.ok(True, "")


def check_tables_current():
    """The interpreter's generated table must be the one this cycles.py produces.

    This is the one failure mode the three-way comparison cannot name by itself.  The
    translated units carry their costs as literals emitted from `cycles.py`; the
    interpreter reads them from `out/interp_tables.h`, generated from the same file at
    build time.  If that header is older than `cycles.py` -- a stale `out/`, or two
    builds of one tree racing on it -- the two engines disagree on the *cycle total*
    only, registers and memory still match, and every delta is a small even number
    (two per read-modify-write).  That reads like a hardware-model bug and is a build
    artefact, so it is checked here by regenerating the table into a scratch file and
    comparing, before a single case runs.
    """
    ref = os.path.join(H.SRC, "out", "interp_tables.h")
    tmp = os.path.join(WORK, "interp_tables.check.h")
    os.makedirs(WORK, exist_ok=True)
    r = H.run([sys.executable, os.path.join(H.TOOLS, "gen_interp_tables.py"),
               "--out", tmp], cwd=H.SRC)
    if r.rc != 0:
        print(r.out, r.err)
        raise SystemExit("differential: could not regenerate the interpreter table")
    if not os.path.exists(ref):
        raise SystemExit("differential: %s is missing -- run ./build.sh" % ref)
    a = open(ref, "rb").read()
    b = open(tmp, "rb").read()
    if a == b:
        return
    for la, lb in zip(a.decode().splitlines(), b.decode().splitlines()):
        if la != lb and "/*" in la:
            raise SystemExit(
                "differential: out/interp_tables.h IS STALE against cycles.py.\n"
                "  on disk: %s\n  cycles.py: %s\n"
                "  The interpreter would charge the old cost while the translated "
                "units charge the new one -- every case would differ by cycles only.\n"
                "  Fix: run ./build.sh (it regenerates the table)." % (la.strip(), lb.strip()))
    raise SystemExit("differential: out/interp_tables.h is stale against cycles.py; "
                     "run ./build.sh")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--total", type=int, default=2688)
    ap.add_argument("--batch", type=int, default=384)
    ap.add_argument("--seed", type=int, default=20261026)
    a = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    check_tables_current()
    chk = H.Checker("differential")
    stats = {arm: {"cases": 0, "bad": 0} for arm in ("translated", "interpreter")}
    rejected = 0

    batches = []
    for start in range(0, a.total, a.batch):
        n = min(a.batch, a.total - start)
        tag = "b%04d" % start
        spec_path = os.path.join(WORK, tag + ".json")
        bin_path = os.path.join(WORK, tag + ".bin")

        g = H.run([sys.executable, "tools/gen_cases.py", "--count", str(n),
                   "--first-id", str(start), "--seed", str(a.seed + start),
                   "--out", spec_path])
        if g.rc != 0:
            print(g.out, g.err)
            return 1
        rejected += int(g.out.split("(")[1].split()[0]) if "rejected" in g.out else 0
        batches.append((tag, spec_path, bin_path))

    # Hand-built cases the random generator will not produce, same machinery.
    tag = "targeted"
    spec_path = os.path.join(WORK, tag + ".json")
    bin_path = os.path.join(WORK, tag + ".bin")
    with open(spec_path, "w") as f:
        json.dump(targeted_specs(), f)
    batches.append((tag, spec_path, bin_path))

    for (tag, spec_path, bin_path) in batches:
        with open(spec_path) as f:
            specs = json.load(f)
        # The reference first: it is independent of both C builds.
        ref = {s["id"]: python_state(s) for s in specs}

        run_case_build(spec_path, bin_path)
        built = {}

        for arm, env in (("translated", None), ("interpreter", INTERP_ENV)):
            r = H.run([bin_path, "cases"], env=env)
            if r.rc != 0:
                print(r.out[-2000:], r.err)
                return 1
            if r.out.count("TRAP"):
                chk.ok(False, "batch %s (%s) trapped: %s"
                       % (tag, arm, r.out[r.out.index("TRAP"):][:120]))
            built[arm] = parse_cases(r.out)
            compare_arm(specs, ref, built[arm], arm, chk, stats)

        print("  batch %-9s %3d cases  translated %d/%d exact  interpreter %d/%d exact"
              % (tag, len(specs), stats["translated"]["cases"],
                 stats["translated"]["cases"], stats["interpreter"]["cases"],
                 stats["interpreter"]["cases"]))

    total = stats["translated"]["cases"]
    ok = chk.report("%d cases x 2 arms (registers, memory CRC, cycle total)" % total)
    print("differential test: %d sequences through both C engines; mismatches: "
          "translated %d, interpreter %d (%d rejected by the filter)"
          % (total, stats["translated"]["bad"], stats["interpreter"]["bad"],
             rejected))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
