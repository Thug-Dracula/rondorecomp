#!/usr/bin/env python3
"""HuC6280 instruction decode, shared by the recompiler and the reference CPU.

The opcode table is imported from `huc_table.py` beside this file, which is
machine-extracted from Etripator's `opcodes.c` (see the header of that file).
Nothing here re-derives opcode numbers: this module only turns the table's
(name, size, mode) triples into concrete operand bytes.

One deliberate divergence from the old disassembler: `csl`/`csh` are *not*
treated as control-flow terminators, because they are three-cycle single
instructions that fall through.  They are not no-ops -- they switch the CPU's
clock (see `xl_speed_set` in runtime.h) -- but they never change the flow.
Only `rts`, `rti` and `brk` end a linear run.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from huc_table import TABLE  # noqa: E402

# Mode ids as used by the generated table (same numbering as re/huc6280.py).
M_OP, M_OP_A, M_NN, M_NN_ZZ, M_NN_ZZX, M_NN_HHLL, M_NN_HHLLX = 0, 1, 2, 3, 4, 5, 6
M_ZZ, M_ZZX, M_ZZY, M_IZZ, M_IZZX, M_IZZY = 7, 8, 9, 10, 11, 12
M_ZZ_HHLL = 13
M_HHLL, M_IHHLL, M_HHLLX, M_HHLLY = 14, 15, 16, 17
M_BLOCK, M_BRANCH_OR_LABEL, M_ZZ_LABEL, M_IHHLLX, M_DB = 18, 19, 20, 21, 22

MODE_NAMES = {
    M_OP: "implied", M_OP_A: "a", M_NN: "imm8", M_NN_ZZ: "imm8,zp",
    M_NN_ZZX: "imm8,zp,x", M_NN_HHLL: "imm8,abs", M_NN_HHLLX: "imm8,abs,x",
    M_ZZ: "zp", M_ZZX: "zp,x", M_ZZY: "zp,y", M_IZZ: "(zp)", M_IZZX: "(zp,x)",
    M_IZZY: "(zp),y", M_ZZ_HHLL: "zp,abs", M_HHLL: "abs", M_IHHLL: "(abs)",
    M_HHLLX: "abs,x", M_HHLLY: "abs,y", M_BLOCK: "block",
    M_BRANCH_OR_LABEL: "rel", M_ZZ_LABEL: "zp,rel", M_IHHLLX: "(abs,x)",
    M_DB: "db",
}

TERMINATORS = {"rts", "rti", "brk"}
BRANCHES = {"bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq", "bra"}
BIT_BRANCHES = {"bbr0", "bbr1", "bbr2", "bbr3", "bbr4", "bbr5", "bbr6", "bbr7",
                "bbs0", "bbs1", "bbs2", "bbs3", "bbs4", "bbs5", "bbs6", "bbs7"}


class Ins:
    __slots__ = ("addr", "op", "mnem", "size", "mode", "b")

    def __init__(self, addr, op, mnem, size, mode, b):
        self.addr = addr
        self.op = op
        self.mnem = mnem
        self.size = size
        self.mode = mode
        self.b = list(b)

    # -- operand helpers -----------------------------------------------------
    @property
    def imm(self):
        return self.b[1]

    @property
    def zp(self):
        return self.b[1]

    @property
    def abs16(self):
        return self.b[1] | (self.b[2] << 8)

    @property
    def zp2(self):
        """zp for the imm8,zp forms."""
        return self.b[2]

    @property
    def abs_imm(self):
        """abs for the imm8,abs forms."""
        return self.b[2] | (self.b[3] << 8)

    @property
    def rel(self):
        v = self.b[-1]
        return v - 256 if v >= 0x80 else v

    @property
    def rel_target(self):
        return (self.addr + self.size + self.rel) & 0xFFFF

    @property
    def block(self):
        return (self.b[1] | (self.b[2] << 8),
                self.b[3] | (self.b[4] << 8),
                self.b[5] | (self.b[6] << 8))

    def __repr__(self):
        return "%04X %s" % (self.addr, self.text)

    @property
    def text(self):
        return fmt(self)


def decode(fetch, addr):
    """Decode one instruction at addr.  `fetch` is a callable addr->byte."""
    addr &= 0xFFFF
    op = fetch(addr)
    if op is None:
        return None
    mnem, size, mode = TABLE[op]
    mnem = mnem.strip()
    b = [fetch(addr + i) for i in range(size)]
    if any(x is None for x in b):
        return None
    return Ins(addr, op, mnem, size, mode, b)


def fmt(ins):
    m = ins.mnem
    md = ins.mode
    if md == M_OP:
        return m
    if md == M_OP_A:
        return m + " a"
    if md == M_NN:
        return "%s #$%02X" % (m, ins.b[1])
    if md == M_NN_ZZ:
        return "%s #$%02X,$%02X" % (m, ins.b[1], ins.b[2])
    if md == M_NN_ZZX:
        return "%s #$%02X,$%02X,x" % (m, ins.b[1], ins.b[2])
    if md == M_NN_HHLL:
        return "%s #$%02X,$%04X" % (m, ins.b[1], ins.abs_imm)
    if md == M_NN_HHLLX:
        return "%s #$%02X,$%04X,x" % (m, ins.b[1], ins.abs_imm)
    if md == M_ZZ:
        return "%s $%02X" % (m, ins.b[1])
    if md == M_ZZX:
        return "%s $%02X,x" % (m, ins.b[1])
    if md == M_ZZY:
        return "%s $%02X,y" % (m, ins.b[1])
    if md == M_IZZ:
        return "%s ($%02X)" % (m, ins.b[1])
    if md == M_IZZX:
        return "%s ($%02X,x)" % (m, ins.b[1])
    if md == M_IZZY:
        return "%s ($%02X),y" % (m, ins.b[1])
    if md == M_ZZ_HHLL:
        return "%s $%02X,$%04X" % (m, ins.b[1], ins.abs_imm)
    if md == M_HHLL:
        return "%s $%04X" % (m, ins.abs16)
    if md == M_IHHLL:
        return "%s ($%04X)" % (m, ins.abs16)
    if md == M_HHLLX:
        return "%s $%04X,x" % (m, ins.abs16)
    if md == M_HHLLY:
        return "%s $%04X,y" % (m, ins.abs16)
    if md == M_BLOCK:
        s, d, n = ins.block
        return "%s $%04X,$%04X,$%04X" % (m, s, d, n)
    if md == M_BRANCH_OR_LABEL:
        if ins.size == 2:
            return "%s $%04X" % (m, ins.rel_target)
        return "%s $%04X" % (m, ins.abs16)
    if md == M_ZZ_LABEL:
        return "%s $%02X,$%04X" % (m, ins.b[1], ins.rel_target)
    if md == M_IHHLLX:
        return "%s [$%04X,x]" % (m, ins.abs16)
    return ".db $%02X" % ins.b[0]


# -- control-flow metadata used by the descent ------------------------------
def flow(ins):
    """Return (follow_targets, falls_through) for one instruction."""
    m = ins.mnem
    if m in TERMINATORS:
        return [], False
    if m == "jmp":
        if ins.mode in (M_HHLL, M_BRANCH_OR_LABEL):
            return [ins.abs16], False
        return [], False                     # indirect: dispatcher handles it
    if m == "jsr":
        return [ins.abs16], True
    if m == "bsr":
        return [ins.rel_target], True
    if m == "bra":
        return [ins.rel_target], False
    if m in BRANCHES:
        return [ins.rel_target], True
    if m in BIT_BRANCHES:
        return [ins.rel_target], True
    return [], True
