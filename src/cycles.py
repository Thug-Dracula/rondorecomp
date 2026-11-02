#!/usr/bin/env python3
"""HuC6280 instruction cycle costs -- the ONE table, for both machines.

Who reads this:

  * the **translator** (`recompiler.py`) calls `cycles_for()` while emitting a unit
    and writes the result as one literal `xl_tick(c, N)` in the generated C;
  * the **C interpreter** (`interp.c`) does not know any numbers.  Its per-opcode
    rules are baked into `out/interp_tables.h` by `tools/gen_interp_tables.py`,
    which calls `cycle_rule()` for every opcode in the same 256-entry table the
    translator decodes with.

Why it is one module and not two tables: cycle identity is a test
(`tests/test_diff.py` compares cycle totals), and the interpreter is reachable by
the overlay guard on a normal run.
Two hand-written tables drift by one cycle on one opcode, every register test
still passes, and the frame cadence silently stops matching.  `tests/test_diff.py`
compares cycle totals as well as registers for exactly that reason.

Two shapes are not a single number, so a rule is a small tuple instead:

    fixed      one cost, whatever the operands
    branch     taken / not taken   (the translator ticks inside each emitted arm)
    bitbranch  taken / not taken   (bbr/bbs)
    block      base + per_byte * len for the five block-move opcodes, where a length
               of zero means 65536 iterations (the 16-bit counter is decremented to
               zero, which wraps: pcetech.txt, and the reference core's do-while)

The numbers themselves are the standard HuC6280 timings -- the PC Engine is one
cycle slower than the 6502 on memory operands because of the video chip's access
slots -- and are NOT yet measured against the oracle.  That was true before this
module existed and is unchanged by it; what this module guarantees is that both
execution paths charge exactly the same thing.
"""
import isa

# Rule kinds.  These numbers are part of the generated C (XK_* in
# out/interp_tables.h), so they travel with the table.
RULE_FIXED, RULE_BRANCH, RULE_BITBRANCH, RULE_BLOCK = 0, 1, 2, 3

CYCLES = {
    "branch_taken": 4, "branch_not": 2, "bra": 4, "bsr": 8,
    "jsr": 7, "rts": 7, "rti": 7, "jmp_abs": 4, "jmp_ind": 7,
    "bit_branch": 6, "bit_branch_taken": 8, "rmw_zp_bit": 7,
    "block_base": 17, "block_per_byte": 6,
}

MODE_CYCLES = {isa.M_OP: 2, isa.M_OP_A: 2, isa.M_NN: 2, isa.M_ZZ: 4, isa.M_ZZX: 4,
               isa.M_ZZY: 4, isa.M_HHLL: 5, isa.M_HHLLX: 5, isa.M_HHLLY: 5,
               isa.M_IZZ: 7, isa.M_IZZX: 7, isa.M_IZZY: 7, isa.M_NN_ZZ: 7,
               isa.M_NN_ZZX: 7, isa.M_NN_HHLL: 8, isa.M_NN_HHLLX: 8,
               isa.M_ZZ_HHLL: 7}

# The conditional branches.  `recompiler.BRANCH_COND` holds the same set with the
# C condition for each; it asserts the two agree at import time, so a branch added
# to one place and not the other is a build failure rather than a silent re-cost.
COND_BRANCHES = {"bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq"}

BLOCK_MOVES = {"tii", "tdd", "tin", "tia", "tai"}
FIVE_CYCLE = {"tam", "tma"}

# READ-MODIFY-WRITE ON A MEMORY OPERAND COSTS MORE THAN THE SAME MODE FOR A LOAD
# OR A STORE, AND THE TABLE DID NOT SAY SO.
#
# `MODE_CYCLES` is the cost of a *load* or *store* in each addressing mode.  An
# RMW instruction (asl/rol/lsr/ror/inc/dec, and tsb/trb, which are the same shape)
# reads, writes back and writes again, and the reference core charges it
# separately: `RMW_ZP` is `ADDCYC(5); LastCycle(); RdMem(); WrMem()` and `RMW_AB`
# is `ADDCYC(6); LastCycle(); ...`, against `LD_ZP`'s 3 and `LD_AB`'s 4 in the
# reference core's `huc6280.cpp`.  With LastCycle()'s one
# cycle that is 6 cycles on zero page and 7 on absolute, where this table charged
# the plain load/store cost (4 and 5) -- every `asl/ror $xx` in the game was two
# cycles cheap.
#
# Measured on the console's own trace, in the VRAM shift loop at $72:$8AA6 (16
# `ror $00`, two VDC port writes, two VDC port reads per iteration): the console
# spends 166 CPU cycles per iteration (240 + 258 master clocks between the two
# port writes, f=3802, c=1364178681 -> ...), this port 132.  Sixteen RMWs at +2
# is 32 of the 34; the other two are the two VDC port reads, which pay the access
# slot in the reference's IORead and did not pay it here (runtime.c, xl_rd_phys).
RMW_MNEMS = {"asl", "rol", "lsr", "ror", "inc", "dec", "tsb", "trb"}

# The zero-page bit instructions: read, modify one bit, write back, on a zero-page
# operand.  Seven cycles, as `RMW_ZP_B` charges them.
RMBBIT = {"rmb%d" % i for i in range(8)} | {"smb%d" % i for i in range(8)}
RMW_MODE_CYCLES = {isa.M_ZZ: 6, isa.M_ZZX: 6, isa.M_HHLL: 7, isa.M_HHLLX: 7}

# ST0/ST1/ST2 ARE FOUR CYCLES HERE, AND FIVE ON THE BUS.
#
# The published HuC6280 timing for ST0/ST1/ST2 is 5 cycles, and that 5 is the
# instruction *plus the VDC access slot*: the reference core writes it as
#
#     #define ST1 { ADDCYC(3); LastCycle(); WrMemPhysical(0x80000000|(0xFF*8192+2), x); }
#     // "Total cycles for ST0/ST1/ST2 is effectively 5 (4 here, +1 stealcycle in
#     //  the PC Engine memory handler logic)"
#
# i.e. 4 cycles of instruction and the fifth from `HuCPU.StealCycle()` in `IOWrite`
# (`case 0x0000: HuCPU.StealCycle(); vce->WriteVDC(...)`).  The steal is charged
# once, in the one place both C engines write the port (`xl_wr_phys`), so the table
# must carry the 4 and not the total -- carrying 5 charged every VDC port access
# six cycles instead of five.
#
# Measured, not assumed.  The game's frame-long delay loop at $69:$5BC2 is
# `st1 #$10 / st2 #$01 / inx / bne / dey / bpl`, so the console's own trace shows
# one ST in isolation: between the $0003 write's stamp and the next $0002 write's
# stamp there is exactly `ST + inx + bne` = 5 + 2 + 4 = 11 cycles = 33 master
# clocks (trace-cyc-full.log, f=24436, c=020AE0264D -> 020AE0266E).  The port, at
# 6 + 2 + 4 = 12, printed 36 (base-loop.log, f=36, c=0xC533E8 -> 0xC533FA -> the
# next ST1).  Five, not six.
ST_CYCLE = {"st0", "st1", "st2"}
TWO_CYCLE = {"nop", "set", "cla", "clx", "cly"}
# SXY/SAX/SAY/CSL/CSH ARE THREE, NOT TWO.
#
# Each one is `ADDCYC(2); LastCycle()` in the reference core -- two cycles of work
# plus the last-cycle charge -- and the published vectors say 3 for all five, with one
# dummy read.  They were in TWO_CYCLE, one cycle cheap, and that was invisible because
# the translator and the interpreter share this table: both agreed, and both were
# wrong.  (SingleStepTests/huc6280, opcodes 02/22/42/54/D4.)
#
# For CSL/CSH this 3 is the ADDCYC count and is correct as written: the charge is
# three cycles.  What the table cannot say is that the speed change falls *between*
# the charges -- CSL/CSH charge 2 at the old clock and 1 at the new (huc6280.cpp:247-248),
# so both engines charge them in two parts instead of taking this single tick
# (interp.c, recompiler.emit_body).  The rate each cycle is worth is in xl_tick().
THREE_CYCLE = {"csl", "csh", "sax", "say", "sxy"}
PUSH = {"pha", "phx", "phy", "php"}
PULL = {"pla", "plx", "ply", "plp"}


def _fixed(n):
    return (RULE_FIXED, n, 0)


def cycle_rule(ins):
    """The cost of one instruction as (kind, a, b).

    `ins` needs only `.mnem`, `.mode` and (for block moves) `.block`; the C
    generator passes a probe with a dummy block operand, because it only ever
    reads `kind` and the two rates.
    """
    m = ins.mnem
    if m in COND_BRANCHES:
        return (RULE_BRANCH, CYCLES["branch_taken"], CYCLES["branch_not"])
    if m in isa.BIT_BRANCHES:
        return (RULE_BITBRANCH, CYCLES["bit_branch_taken"], CYCLES["bit_branch"])
    if m == "bra":
        return _fixed(CYCLES["bra"])
    if m == "bsr":
        return _fixed(CYCLES["bsr"])
    if m == "jsr":
        return _fixed(CYCLES["jsr"])
    if m == "rts":
        return _fixed(CYCLES["rts"])
    if m == "rti":
        return _fixed(CYCLES["rti"])
    if m == "jmp":
        return _fixed(CYCLES["jmp_abs"] if ins.mode in (isa.M_HHLL, isa.M_BRANCH_OR_LABEL)
                      else CYCLES["jmp_ind"])
    if m in PUSH:
        return _fixed(3)
    if m in PULL:
        return _fixed(4)
    if m in BLOCK_MOVES:
        return (RULE_BLOCK, CYCLES["block_base"], CYCLES["block_per_byte"])
    if m in RMW_MNEMS and ins.mode in RMW_MODE_CYCLES:
        return _fixed(RMW_MODE_CYCLES[ins.mode])
    if m == "tma":
        # TMA is 4 (the reference core: ADDCYC(3) + LastCycle), TAM is 5.  They shared
        # one entry in FIVE_CYCLE, so every TMA was one cycle too expensive.  Vectors:
        # 43, 4 cycles in 2,191 of 2,191 runnable cases.
        return _fixed(4)
    if m in FIVE_CYCLE:
        return _fixed(5)
    if m in THREE_CYCLE:
        return _fixed(3)
    if m in RMBBIT:
        # RMBn/SMBn: RMW_ZP_B in the reference core is GetZP + ADDCYC(5) + ADDCYC(1) +
        # LastCycle = 7.  The table charged the plain zero-page cost (4), three short,
        # because these mnemonics are not in RMW_MNEMS and fell through to MODE_CYCLES.
        return _fixed(CYCLES["rmw_zp_bit"])
    if m in ST_CYCLE:
        return _fixed(4)            # + 1 for the VDC access slot, charged by the I/O write
    if m in TWO_CYCLE:
        return _fixed(2)
    return _fixed(MODE_CYCLES.get(ins.mode, 4))


def cycles_for(ins):
    """Cycle cost of one instruction, or None when the emitted body ticks itself."""
    kind, a, b = cycle_rule(ins)
    if kind in (RULE_BRANCH, RULE_BITBRANCH):
        return None                     # emitted per arm
    if kind == RULE_BLOCK:
        return a + b * (ins.block[2] or 65536)
    return a


def cost_of(rule, block_len=None, taken=None):
    """Resolve a rule to a number -- used by the Python reference and by tests."""
    kind, a, b = rule
    if kind in (RULE_BRANCH, RULE_BITBRANCH):
        return a if taken else b
    if kind == RULE_BLOCK:
        return a + b * (block_len or 65536)
    return a
