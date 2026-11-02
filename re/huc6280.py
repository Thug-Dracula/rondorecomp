#!/usr/bin/env python3
"""HuC6280 (PC Engine) disassembler with recursive-descent code discovery.

The opcode table is generated from Etripator's opcodes.c (see mk_table.py), so
the decode of every one of the 256 opcodes matches a maintained, in-use
disassembler rather than a hand-typed table.

Why this exists: a bank image has no entry point, and following one flow from one
address stops at the first jump table.  A worklist-based descent lets the caller
seed many entry points and see exactly which bytes ended up as code.

Usage:
  huc6280.py --image IMG --base 0x4000 --seed 0x4000[,0x5000...] \
             [--out listing.asm] [--sweep-out sweeps.asm] [--stats]

With --sweep-out the tool also does a linear sweep of every byte range that
the descent did not claim, so data regions are visible as data instead of
silently disappearing.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
from huc_table import TABLE  # noqa: E402

# ---------------------------------------------------------------- tables ----

M_OP, M_OP_A, M_NN, M_NN_ZZ, M_NN_ZZX, M_NN_HHLL, M_NN_HHLLX = 0, 1, 2, 3, 4, 5, 6
M_ZZ, M_ZZX, M_ZZY, M_IZZ, M_IZZX, M_IZZY = 7, 8, 9, 10, 11, 12
M_ZZ_HHLL = 13
M_HHLL, M_IHHLL, M_HHLLX, M_HHLLY = 14, 15, 16, 17
M_BLOCK, M_BRANCH_OR_LABEL, M_ZZ_LABEL, M_IHHLLX, M_DB = 18, 19, 20, 21, 22

TERMINAL = {"rts", "rti", "brk", "csl", "csh"}
BRANCHES = {"bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq",
            "bra", "bsr", "bbs0", "bbs1", "bbs2", "bbs3",
            "bbs4", "bbs5", "bbs6", "bbs7", "bbr0", "bbr1", "bbr2",
            "bbr3", "bbr4", "bbr5", "bbr6", "bbr7"}
SUBCALL = {"bsr"}


class Insn:
    __slots__ = ("addr", "size", "text", "mnem", "raw")

    def __init__(self, addr, size, text, mnem, raw):
        self.addr = addr
        self.size = size
        self.text = text
        self.mnem = mnem
        self.raw = raw

    def __repr__(self):
        return "%04X %s" % (self.addr, self.text)


def _branch_target(addr, size, rel):
    if rel >= 0x80:
        rel -= 0x100
    return (addr + size + rel) & 0xFFFF


def decode(mem, addr):
    """Decode one instruction at addr. Returns Insn or None if out of image."""
    if addr not in mem:
        return None
    op = mem[addr]
    mnem, size, mode = TABLE[op]
    mnem = mnem.strip()
    raw = [mem.get(addr + i) for i in range(size)]
    if any(b is None for b in raw):
        return None
    b = raw

    def hhll():
        return b[1] | (b[2] << 8)

    if mode == M_OP:
        txt = mnem
    elif mode == M_OP_A:
        txt = mnem + " a"
    elif mode == M_NN:
        txt = "%s #$%02X" % (mnem, b[1])
    elif mode == M_NN_ZZ:
        txt = "%s #$%02X,$%02X" % (mnem, b[1], b[2])
    elif mode == M_NN_ZZX:
        txt = "%s #$%02X,$%02X,x" % (mnem, b[1], b[2])
    elif mode == M_NN_HHLL:
        txt = "%s #$%02X,$%04X" % (mnem, b[1], hhll())
    elif mode == M_NN_HHLLX:
        txt = "%s #$%02X,$%04X,x" % (mnem, b[1], hhll())
    elif mode == M_ZZ:
        txt = "%s $%02X" % (mnem, b[1])
    elif mode == M_ZZX:
        txt = "%s $%02X,x" % (mnem, b[1])
    elif mode == M_ZZY:
        txt = "%s $%02X,y" % (mnem, b[1])
    elif mode == M_IZZ:
        txt = "%s ($%02X)" % (mnem, b[1])
    elif mode == M_IZZX:
        txt = "%s ($%02X,x)" % (mnem, b[1])
    elif mode == M_IZZY:
        txt = "%s ($%02X),y" % (mnem, b[1])
    elif mode == M_ZZ_HHLL:
        txt = "%s $%02X,$%04X" % (mnem, b[1], hhll())
    elif mode == M_HHLL:
        txt = "%s $%04X" % (mnem, hhll())
    elif mode == M_IHHLL:
        txt = "%s ($%04X)" % (mnem, hhll())
    elif mode == M_HHLLX:
        txt = "%s $%04X,x" % (mnem, hhll())
    elif mode == M_HHLLY:
        txt = "%s $%04X,y" % (mnem, hhll())
    elif mode == M_BLOCK:
        txt = "%s $%04X,$%04X,$%04X" % (mnem, b[1] | (b[2] << 8),
                                        b[3] | (b[4] << 8), b[5] | (b[6] << 8))
    elif mode == M_BRANCH_OR_LABEL:
        if size == 2:                      # relative branch
            txt = "%s $%04X" % (mnem, _branch_target(addr, size, b[1]))
        else:                              # jsr / jmp absolute
            txt = "%s $%04X" % (mnem, hhll())
    elif mode == M_ZZ_LABEL:
        hi = b[3] if len(b) > 3 else 0
        txt = "%s $%02X,$%04X" % (mnem, b[1], b[2] | (hi << 8))
    elif mode == M_IHHLLX:
        txt = "%s [$%04X,x]" % (mnem, hhll())
    elif mode == M_DB:
        txt = ".db $%02X" % b[0]
    else:
        txt = ".db $%02X" % b[0]
    return Insn(addr, size, txt, mnem, bytes(raw))


# ------------------------------------------------------------ recursion ----

def descend(mem, seeds, claimed=None, limit=400000):
    """Worklist descent. Returns dict addr -> Insn."""
    out = {} if claimed is None else claimed
    todo = list(seeds)
    n = 0
    while todo:
        addr = todo.pop()
        addr &= 0xFFFF
        if addr in out:
            continue
        cur = addr
        while True:
            if cur in out:
                break
            ins = decode(mem, cur)
            if ins is None:
                break
            out[cur] = ins
            n += 1
            if n > limit:
                return out
            m = ins.mnem
            if m in TERMINAL:
                break
            if m == "jmp":
                if ins.size == 3 and ins.raw[0] in (0x4C,):
                    todo.append((ins.raw[1] | (ins.raw[2] << 8)))
                break                       # indirect jump: stop
            if m == "jsr":
                todo.append(ins.raw[1] | (ins.raw[2] << 8))
                cur += ins.size
                continue
            if m in BRANCHES:
                t = _branch_target(ins.addr, ins.size, ins.raw[-1])
                if m == "bsr":
                    todo.append(t)
                else:
                    todo.append(t)
                if m in ("bra",):
                    break
                cur += ins.size
                continue
            cur += ins.size
    return out


def gaps(mem, claimed):
    """Return list of (start, end) byte ranges with no claimed instruction."""
    out = []
    addrs = sorted(mem)
    run = None
    for a in addrs:
        if a in claimed:
            if run is not None:
                out.append(run)
                run = None
        else:
            if run is None:
                run = [a, a]
            elif a == run[1] + 1:
                run[1] = a
            else:
                out.append(run)
                run = [a, a]
    if run is not None:
        out.append(run)
    return [(s, e) for s, e in out]


def load_image(path, base, length=None):
    data = open(path, "rb").read()
    if length is not None:
        data = data[:length]
    return {base + i: b for i, b in enumerate(data)}


def fmt_listing(insns, labels=None, header=""):
    labels = labels or {}
    lines = []
    if header:
        lines.append("; " + header)
    addr = min(insns) if insns else 0
    for a in sorted(insns):
        ins = insns[a]
        if a in labels:
            lines.append("")
            lines.append("%s:" % labels[a])
        hexs = "".join("%02X" % x for x in ins.raw)
        lines.append("%04X  %-8s %s" % (a, hexs, ins.text))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--len", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--seed", default=None, help="comma separated addresses")
    ap.add_argument("--out", default=None)
    ap.add_argument("--sweep-out", default=None)
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()

    mem = load_image(a.image, a.base, a.len)
    seeds = [int(x, 0) for x in (a.seed or hex(a.base)).replace(" ", "").split(",") if x]
    claimed = descend(mem, seeds)
    if a.out:
        with open(a.out, "w") as f:
            f.write(fmt_listing(claimed, header="%s %s" % (a.image, hex(a.base))))
        print("wrote", a.out, len(claimed), "instructions")

    g = gaps(mem, claimed)
    if a.stats:
        print("image bytes: %d" % len(mem))
        print("claimed code bytes: %d (%.1f%%)" %
              (sum(i.size for i in claimed.values()),
               100.0 * sum(i.size for i in claimed.values()) / len(mem)))
        print("unclaimed runs: %d" % len(g))
        for s, e in g:
            if e - s > 3:
                print("   $%04X-$%04X  %d bytes" % (s, e, e - s + 1))
    if a.sweep_out:
        sw = {}
        for s, e in g:
            for addr in range(s, e + 1):
                if addr in claimed or addr in sw:
                    continue
                ins = decode(mem, addr)
                if ins:
                    sw[addr] = ins
        with open(a.sweep_out, "w") as f:
            f.write(fmt_listing(sw, header="linear sweep of unclaimed ranges"))
        print("wrote", a.sweep_out, len(sw), "instructions")


if __name__ == "__main__":
    main()
