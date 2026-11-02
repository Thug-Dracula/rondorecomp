#!/usr/bin/env python3
"""Generate random HuC6280 instruction sequences for the differential test.

Each case is a short, *self-contained* program in its own 64-byte slot:

  code      at a unique visible address (so the address-keyed dispatcher has one
            entry per case); 128 slots per 8 KB bank, three banks per batch
  data      a 512-byte window at visible $DE00-$DFFF of a data bank
  zero page a random page at $2000-$20FF (which is where this CPU's ZP lives)

The generator then *rehearses* each case in the Python reference interpreter and
throws it away if
  * execution leaves the slot (the translation unit only contains the slot), or
  * anything writes outside {zero page/stack bank, data bank, hardware page}.
That filter is what makes "same final state" a meaningful comparison instead of a
comparison of two crashes.

Deliberately excluded from random generation (covered by targeted tests instead):
  brk (no interrupt model), rts/rti (need a matching stack frame), jsr and
  absolute jmp (would leave the slot), tam/tma (would remap code under the test),
  st0/st1/st2 (covered by the boot test).  jmp ($xxxx) *is* included, with its
  pointer patched to a valid instruction start: that is the dispatcher test.
"""
import argparse
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.dirname(HERE)
sys.path.insert(0, XL)

import isa                                    # noqa: E402
import refcpu                                 # noqa: E402
from memory import AddressSpace               # noqa: E402

SLOT = 64
SLOTS_PER_BANK = 8192 // SLOT                 # 128
CODE_BANK0 = 0x60                             # physical banks $60,$61,$62
VISIBLE = [0x4000, 0x6000, 0x8000]
ZP_BANK = 0x70
DATA_BANK = 0xA0
DATA_VIS = 0xDE00                             # 512-byte window at $DE00-$DFFF
HW_BANK = 0xFF

SKIP_MNEM = {"brk", ".db", "jsr", "rts", "rti", "jmp", "tam", "tma",
             "st0", "st1", "st2"}

ALLOWED_MODES = {
    isa.M_OP, isa.M_OP_A, isa.M_NN, isa.M_ZZ, isa.M_ZZX, isa.M_ZZY,
    isa.M_HHLL, isa.M_HHLLX, isa.M_HHLLY, isa.M_IZZ, isa.M_IZZX, isa.M_IZZY,
    isa.M_NN_ZZ, isa.M_NN_ZZX, isa.M_NN_HHLL, isa.M_NN_HHLLX,
    isa.M_ZZ_LABEL, isa.M_BLOCK,
}


def candidates():
    out = []
    for op, (mnem, size, mode) in enumerate(isa.TABLE):
        mnem = mnem.strip()
        if mnem in SKIP_MNEM:
            continue
        if mode == isa.M_BRANCH_OR_LABEL and size == 2:
            out.append((op, mnem, size, mode))
        elif mode in ALLOWED_MODES:
            out.append((op, mnem, size, mode))
    out.append((0x6C, "jmp", 3, isa.M_IHHLL))     # indirect jump: dispatcher test
    return out


CANDS = candidates()


class GuardSpace(AddressSpace):
    """AddressSpace that remembers where writes landed."""

    def __init__(self):
        super().__init__()
        self.touched = []

    def write(self, addr, val):
        bank = self.mpr[(addr & 0xFFFF) >> 13]
        self.touched.append((bank, addr & 0x1FFF))
        super().write(addr, val)


def gen_case(rng, cid, batch):
    n = rng.randint(4, 12)
    slot = cid % (SLOTS_PER_BANK * 3)
    bank_idx = slot // SLOTS_PER_BANK
    within = slot % SLOTS_PER_BANK
    base = VISIBLE[bank_idx] + within * SLOT
    mpr = [HW_BANK, ZP_BANK, CODE_BANK0 + 0, CODE_BANK0 + 1, CODE_BANK0 + 2,
           0x71, DATA_BANK, 0x72]

    # ---- pick instructions and lay them out ------------------------------
    picks = [CANDS[rng.randrange(len(CANDS))] for _ in range(n)]
    offs = []
    total = 0
    for (op, mnem, size, mode) in picks:
        offs.append(total)
        total += size
        if total + 2 > SLOT:
            total -= size
            offs.pop()
            picks.pop()
            break
    if not picks:
        return None
    n = len(picks)
    term_off = total
    layout_addr = [base + o for o in offs]
    term_addr = base + term_off

    zp_page = bytearray(rng.randrange(256) for _ in range(256))
    data = bytearray(512)                     # visible $DE00-$DFFF
    ptr_targets = []                          # (zp_index, visible target)
    pend = []                                 # (offset_in_code, target_visible)

    def data_byte(vis_off):
        return vis_off - DATA_VIS

    code = bytearray()
    for i, (op, mnem, size, mode) in enumerate(picks):
        addr = layout_addr[i]
        body = [op]
        def db(): return rng.randrange(256)
        if mode == isa.M_OP or mode == isa.M_OP_A:
            pass
        elif mode == isa.M_NN:
            body.append(db())
        elif mode in (isa.M_ZZ,):
            body.append(db())
        elif mode in (isa.M_ZZX, isa.M_ZZY):
            body.append(db())
        elif mode in (isa.M_HHLL, isa.M_HHLLX, isa.M_HHLLY):
            a = DATA_VIS + rng.randrange(0x00, 0x80)
            body += [a & 0xFF, a >> 8]
        elif mode == isa.M_IZZ:
            z = rng.randrange(256)
            ptr_targets.append((z, DATA_VIS + rng.randrange(0x00, 0x80)))
            body.append(z)
        elif mode == isa.M_IZZX:
            z = rng.randrange(256)
            body.append(z)
            pend.append(("zx", z, DATA_VIS + rng.randrange(0x00, 0x80)))
        elif mode == isa.M_IZZY:
            z = rng.randrange(256)
            ptr_targets.append((z, DATA_VIS + rng.randrange(0x00, 0x60)))
            body.append(z)
        elif mode == isa.M_IHHLL:
            a = DATA_VIS + rng.randrange(0x00, 0x80)
            body += [a & 0xFF, a >> 8]
            t = rng.randrange(n + 1)
            pend.append(("word", a, (base + (offs[t] if t < n else term_off))))
        elif mode == isa.M_NN_ZZ:
            body += [db(), db()]
        elif mode == isa.M_NN_ZZX:
            body += [db(), db()]
        elif mode == isa.M_NN_HHLL or mode == isa.M_NN_HHLLX:
            a = DATA_VIS + rng.randrange(0x00, 0x80)
            body += [db(), a & 0xFF, a >> 8]
        elif mode == isa.M_ZZ_LABEL:
            t = rng.randrange(n + 1)
            target = base + (offs[t] if t < n else term_off)
            rel = target - (addr + 3)
            body += [db(), rel & 0xFF]
        elif mode == isa.M_BLOCK:
            s = DATA_VIS + rng.randrange(0x00, 0x60)
            d = DATA_VIS + rng.randrange(0x00, 0x60)
            ln = rng.randrange(0, 8)
            body += [s & 0xFF, s >> 8, d & 0xFF, d >> 8, ln & 0xFF, 0]
        elif mode == isa.M_BRANCH_OR_LABEL and size == 2:
            t = rng.randrange(0, n + 1)
            target = base + (offs[t] if t < n else term_off)
            rel = target - (addr + 2)
            body.append(rel & 0xFF)
        else:
            return None
        if len(body) != size:
            return None
        code += bytes(body)

    # terminator: bra to itself, so a fall-through path cannot escape the slot
    code += bytes([0x80, 0xFE])

    # ---- resolve pointer/word patches ------------------------------------
    # Invariant, and it matters: any location the code reads as a pointer must be
    # written *after* the random data sprinkle below, or a stray random byte
    # turns a jump into an escape from the slot.
    for _ in range(24):
        data[rng.randrange(512)] = rng.randrange(256)

    zp_patches = []
    for (z, target) in ptr_targets:
        zp_patches.append((z, target))
    for p in pend:
        if p[0] == "word":
            a = p[1]
            data[data_byte(a)] = p[2] & 0xFF
            data[data_byte(a) + 1] = (p[2] >> 8) & 0xFF
        # "zx" needs X, unknown until the registers are chosen; handled below

    regs = dict(a=rng.randrange(256), x=rng.randrange(256), y=rng.randrange(256),
                s=rng.randrange(0x00, 0xF0), p=rng.randrange(256))
    for p in pend:
        if p[0] == "zx":
            z, target = p[1], p[2]
            cell = (z + regs["x"]) & 0xFF
            zp_patches.append((cell, target))
    for (cell, target) in zp_patches:
        zp_page[cell] = target & 0xFF
        zp_page[(cell + 1) & 0xFF] = (target >> 8) & 0xFF

    spec = {
        "id": cid,
        "code": list(code),
        "code_bank": CODE_BANK0 + bank_idx,
        "code_off": base & 0x1FFF,
        "pc": base,
        "mpr": mpr,
        "a": regs["a"], "x": regs["x"], "y": regs["y"],
        "s": regs["s"], "p": regs["p"],
        "steps": n + 1,
        "data": list(data),
        "data_bank": DATA_BANK,
        "data_off": DATA_VIS & 0x1FFF,
        "patches": [[ZP_BANK, 0x0000, list(zp_page)]],
        "seeds": sorted(set([base + o for o in offs] + [term_addr])),
    }
    return spec


def rehearse(spec):
    """Run the case in the Python reference; return (state, reason_or_None)."""
    sp = GuardSpace()
    sp.mpr = list(spec["mpr"])
    sp.load_phys(spec["code_bank"], spec["code_off"], bytes(spec["code"]))
    sp.load_phys(spec["data_bank"], spec["data_off"], bytes(spec["data"]))
    for (bank, off, blob) in spec["patches"]:
        sp.load_phys(bank, off, bytes(blob))
    cpu = refcpu.RefCpu(sp)
    cpu.a, cpu.x, cpu.y = spec["a"], spec["x"], spec["y"]
    cpu.s, cpu.p, cpu.pc = spec["s"], spec["p"], spec["pc"]
    lo, hi = spec["pc"], spec["pc"] + len(spec["code"])
    allowed = {ZP_BANK, DATA_BANK, HW_BANK}
    try:
        for _ in range(spec["steps"]):
            if not (lo <= cpu.pc < hi):
                return None, "pc left the slot at $%04X" % cpu.pc
            cpu.step()
        bad = [(b, o) for (b, o) in sp.touched if b not in allowed]
        if bad:
            return None, "wrote outside the test regions: %r" % (bad[:3],)
        return cpu.state(), None
    except refcpu.Unsupported as e:
        return None, "unsupported: %s" % e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=384, help="cases in this batch")
    ap.add_argument("--first-id", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    rng = random.Random(a.seed)
    specs, dropped = [], 0
    attempts = 0
    while len(specs) < a.count and attempts < a.count * 8:
        attempts += 1
        cid = a.first_id + len(specs)
        spec = gen_case(rng, cid, 0)
        if spec is None:
            dropped += 1
            continue
        state, why = rehearse(spec)
        if why:
            dropped += 1
            continue
        specs.append(spec)

    with open(a.out, "w") as f:
        json.dump(specs, f)
    print("wrote %s: %d cases (%d rejected by the rehearsal filter, %d attempts)"
          % (a.out, len(specs), dropped, attempts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
