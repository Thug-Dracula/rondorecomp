#!/usr/bin/env python3
"""HuC6280 machine code -> compilable C, keyed by (bank, address).

Design, and why:

  * **A translation unit per (bank, offset).**  The CPU has eight 8 KB bank
    registers, and this game pages different code over the same addresses with
    `TAM` (the overlay loader at $309C does exactly that).  Keying translation by
    logical address cannot represent that: bank $DB at $4000 and bank $80 at $4000
    are *different programs*.  So each bank keeps its own code, and a unit is
    identified by the physical bank plus the offset inside it.

  * **Dispatch resolves the bank at run time, from the live MPR value.**  The
    dispatcher is a two-level switch: bank first, then the full logical address,
    so a unit is only selected when its bank is mapped at the page it was
    translated for.  A bank mapped somewhere unexpected does not silently run the
    wrong code; it falls through to the untranslated path and says so.

  * **Bank switches are honoured because every instruction is re-dispatched.**
    Units never fall through into each other in C; each one ends by setting the
    PC, and the switch resolves the next one afresh.  So a `TAM` takes effect on
    the very next instruction.

  * **The translator tracks MPRs statically while it descends**, so it knows when
    a linear run has crossed into another bank: at that point it stops (never
    translating the wrong module) and continues in the new bank if the value is
    known.  `lda #imm` and reads from the BIOS ROM are what it can resolve; when a
    `TAM` value is unknown it keeps the previous bank and *says so* in the log.

  * Recursive descent from explicit seeds decides what is code; anything never
    reached is never emitted.

  * **A content-keyed code/data log closes the descent's blind spots.**  Descent
    can only follow control flow it can resolve, so it misses code reached by a
    computed jump or by an entry the program builds at run time.  The console's
    own execution says where the code is: `--cdl-keyset` takes a code/data log
    produced by a full instrumented run, whose key is the
    SHA-1 of an 8 KB bank image and whose value marks every offset executed as an
    opcode *in that exact image*.  For every bank whose image hash is a key, the
    translator decodes from each opcode mark with the same ISA table descent uses,
    so operand lengths are respected: a mark that falls inside an instruction the
    decoder already emitted is not a start (and is counted), an instruction that
    decodes to an undefined opcode or a mnemonic the emitter does not support is
    rejected and reported rather than emitted, and every disagreement between the
    log's split and the decoder's is reported with its address.  Ground truth is
    the console; the decoder still decides, and the log says what it decided.

  * **A unit's name is its identity, not its discovery order.**  `u_<sha8>_p<page>_<off>`:
    the content hash of the bank image it was decoded from, the logical page it was
    translated for (branch targets and fall-through PCs are logical addresses), and
    its offset inside the bank.  Adding a discovery therefore never renames or
    renumbers a unit that already existed, which is what a patch, a symbol file or a
    CDL key is written against.

  * Each unit ends by clearing the T flag (the CPU does that after almost every
    instruction -- SET, RTI and PLP are the exceptions), and flag arithmetic lives
    in runtime.c rather than being inlined, so one fix applies everywhere.
"""
import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import isa                                          # noqa: E402
import cycles                                       # noqa: E402
import log as xlog                                  # noqa: E402
from memory import (BANK_SIZE, PHYS_SIZE, ZP_BASE, STACK_BASE, IO_BANK,      # noqa: E402
                    CDRAM_BASE, WORKRAM_BANK, ROM_BANKS, AddressSpace)

# The bank registers a program starts with in this model.  Must match xl_reset()
# in runtime.c: I/O at $0000, work RAM at $2000, CD RAM at $4000-$BFFF, BIOS at
# $E000.  Also the mapping the translator assumes at its entry points.
DEFAULT_MPR = [IO_BANK, WORKRAM_BANK, CDRAM_BASE + 0, CDRAM_BASE + 1,
               CDRAM_BASE + 2, CDRAM_BASE + 3, CDRAM_BASE + 4, 0x00]

# Mnemonics the emitter cannot translate yet (no interrupt model / undefined opcodes).
UNSUPPORTED = {"brk", ".db"}

# Instructions that overwrite A, which invalidates the translator's idea of what A
# holds (it uses that to follow `lda #value; tam #mask` bank switches).
A_WRITERS = {"adc", "sbc", "and", "ora", "eor", "pla", "txa", "tya", "tma",
             "sax", "say", "tst"}


def ea_expr(ins):
    """C expression for the effective address of a memory operand."""
    m, b = ins.mode, ins.b
    if m == isa.M_ZZ:
        return "xl_zp(0x%02X)" % b[1]
    if m == isa.M_ZZX:
        return "xl_zp_x(c, 0x%02X)" % b[1]
    if m == isa.M_ZZY:
        return "xl_zp_y(c, 0x%02X)" % b[1]
    if m == isa.M_HHLL:
        return "0x%04X" % (b[1] | (b[2] << 8))
    if m == isa.M_HHLLX:
        return "(uint16_t)(0x%04X + c->x)" % (b[1] | (b[2] << 8))
    if m == isa.M_HHLLY:
        return "(uint16_t)(0x%04X + c->y)" % (b[1] | (b[2] << 8))
    if m == isa.M_IZZ:
        return "xl_rd16_zp(c, 0x%02X)" % b[1]
    if m == isa.M_IZZX:
        return "xl_rd16_zp(c, (uint8_t)(0x%02X + c->x))" % b[1]
    if m == isa.M_IZZY:
        return "(uint16_t)(xl_rd16_zp(c, 0x%02X) + c->y)" % b[1]
    if m == isa.M_IHHLL:
        return "xl_rd16(c, 0x%04X)" % (b[1] | (b[2] << 8))
    if m == isa.M_IHHLLX:
        return "xl_rd16(c, (uint16_t)(0x%04X + c->x))" % (b[1] | (b[2] << 8))
    if m == isa.M_NN_ZZ:
        return "xl_zp(0x%02X)" % b[2]
    if m == isa.M_NN_ZZX:
        return "xl_zp_x(c, 0x%02X)" % b[2]
    if m == isa.M_NN_HHLL:
        return "0x%04X" % (b[2] | (b[3] << 8))
    if m == isa.M_NN_HHLLX:
        return "(uint16_t)(0x%04X + c->x)" % (b[2] | (b[3] << 8))
    raise ValueError("no effective address for mode %s" % isa.MODE_NAMES.get(m, m))


def value_expr(ins):
    """C expression for the operand of a read-type instruction."""
    if ins.mode == isa.M_NN:
        return "0x%02X" % ins.b[1]
    return "xl_rd(c, %s)" % ea_expr(ins)


# Cycle costs live in one place now: `cycles.py`, which the C interpreter also
# consumes (through out/interp_tables.h, generated at build time).  This module
# keeps no numbers of its own -- see the note at the top of that file.
CYCLES = cycles.CYCLES

BRANCH_COND = {
    "bpl": "!(c->p & XL_N)", "bmi": "(c->p & XL_N)",
    "bvc": "!(c->p & XL_V)", "bvs": "(c->p & XL_V)",
    "bcc": "!(c->p & XL_C)", "bcs": "(c->p & XL_C)",
    "bne": "!(c->p & XL_Z)", "beq": "(c->p & XL_Z)",
}

# The cycle table decides which mnemonics are conditional branches; this module
# decides the C condition for each.  If the two sets ever disagree, one of them
# would silently cost a branch as a fixed 4 cycles -- so fail the build instead.
if set(BRANCH_COND) != cycles.COND_BRANCHES:
    raise SystemExit("recompiler: BRANCH_COND and cycles.COND_BRANCHES disagree: %r"
                     % (set(BRANCH_COND) ^ cycles.COND_BRANCHES,))

cycles_for = cycles.cycles_for

# Mnemonic -> (helper, kind) for the simple read-operand ALU group.
ALU1 = {
    "adc": "xl_adc", "sbc": "xl_sbc", "and": "xl_and", "ora": "xl_ora",
    "eor": "xl_eor", "cmp": "xl_cmp", "cpx": "xl_cpx", "cpy": "xl_cpy",
    "bit": "xl_bit",
}
LOAD1 = {"lda": "xl_lda", "ldx": "xl_ldx", "ldy": "xl_ldy"}
STORE1 = {"sta": "c->a", "stx": "c->x", "sty": "c->y"}
RMW1 = {"asl": "xl_asl", "lsr": "xl_lsr", "rol": "xl_rol", "ror": "xl_ror",
        "inc": "xl_inc", "dec": "xl_dec"}

# THE STORE HAPPENS ON THE INSTRUCTION'S LAST CYCLE, NOT ITS FIRST.
#
# The reference core adds the cycles and then performs the bus access:
#
#     #define ST1 { ADDCYC(3); LastCycle(); WrMemPhysical(0x80000000|(0xFF*8192+2), x); }
#     #define ST_AB { ... ADDCYC(4); LastCycle(); WrMem(...); }
#                                                    -- huc6280.cpp:127, 405
#
# so the write's own bus cycle is the instruction's last one.  This port used to
# emit `xl_wr(...)` and then `xl_tick(c, N)`, i.e. every store landed N cycles
# early.  In work RAM that is invisible; in the VDC it moves the write up to six
# CPU cycles (eight dots) earlier in the raster, which is enough to move a VDC
# write across a scanline and shift the picture by a dot.
#
# `LATE_STORE` is the group whose whole body is the write (tick first, then write);
# `LATE_RMW` reads first, ticks, and only then writes back.
LATE_STORE = {"sta", "stx", "sty", "stz", "st0", "st1", "st2"}
LATE_RMW = set(RMW1) | {"tsb", "trb"} | {"rmb%d" % i for i in range(8)} \
           | {"smb%d" % i for i in range(8)}


def emits_own_tick(ins):
    """True when emit_body() has already emitted this instruction's xl_tick()."""
    if ins.mnem in ("csl", "csh"):
        # Two ticks, one on each side of the speed change (see emit_body).
        return True
    if ins.mnem in LATE_RMW:
        # `asl a` and friends touch no memory, so they keep the ordinary shape.
        return not (ins.mnem in RMW1 and ins.mode == isa.M_OP_A)
    return False
FLAG_SET = {"sec": "XL_C", "sed": "XL_D", "sei": "XL_I"}
FLAG_CLR = {"clc": "XL_C", "cld": "XL_D", "cli": "XL_I", "clv": "XL_V"}


def emit_body(ins):
    """Return (lines, sets_pc) for one instruction, or None if unsupported."""
    m = ins.mnem
    nxt = (ins.addr + ins.size) & 0xFFFF

    if m in UNSUPPORTED:
        return None

    if m in LOAD1:
        return ["%s(c, %s);" % (LOAD1[m], value_expr(ins))], False

    if m in STORE1:
        return ["xl_wr(c, %s, %s);" % (ea_expr(ins), STORE1[m])], False

    if m == "stz":
        return ["xl_wr(c, %s, 0);" % ea_expr(ins)], False

    if m in ALU1:
        return ["%s(c, %s);" % (ALU1[m], value_expr(ins))], False

    if m == "tst":
        return ["xl_tst(c, xl_rd(c, %s), 0x%02X);" % (ea_expr(ins), ins.b[1])], False

    if m in RMW1:
        if ins.mode == isa.M_OP_A:
            return ["c->a = %s(c, c->a);" % RMW1[m]], False
        ea = ea_expr(ins)
        return ["{",
                "    uint16_t ea = (uint16_t)(%s);" % ea,
                "    uint8_t v = %s(c, xl_rd(c, ea));" % RMW1[m],
                "    xl_tick(c, %d);" % cycles_for(ins),
                "    xl_wr(c, ea, v);",
                "}"], False

    if m == "tsb" or m == "trb":
        mask = "|" if m == "tsb" else "& (uint8_t)~"
        return ["{",
                "    uint16_t ea = (uint16_t)(%s);" % ea_expr(ins),
                "    uint8_t v = xl_rd(c, ea);",
                "    c->p &= (uint8_t)~(XL_Z | XL_V | XL_N);",
                # Z is the BIT-style test (A & M), for BOTH TSB and TRB -- it is not
                # the memory operation: Bruce Clark, "65C02 Opcodes", and the WDC
                # W65C02S datasheet, corroborated by SingleStepTests/huc6280. N and V
                # come from the memory operand, as the reference emulator computes
                # them. TRB's Z is the one flag where this port knowingly differs
                # from the reference -- see docs/REFERENCE.md.
                "    if ((uint8_t)(v & c->a) == 0) c->p |= XL_Z;",
                "    c->p |= (uint8_t)(v & (XL_N | XL_V));",
                "    xl_tick(c, %d);" % cycles_for(ins),
                "    xl_wr(c, ea, (uint8_t)(v %s c->a));" % mask,
                "}"], False

    if m in FLAG_SET:
        return ["c->p |= %s;" % FLAG_SET[m]], False
    if m in FLAG_CLR:
        return ["c->p &= (uint8_t)~%s;" % FLAG_CLR[m]], False

    if m in ("tax", "tay"):
        reg = m[2].upper()
        return ["c->%s = c->a;" % reg.lower(), "xl_nz(c, c->%s);" % reg.lower()], False
    if m in ("txa", "tya"):
        src = m[1].lower()
        return ["c->a = c->%s;" % src, "xl_nz(c, c->a);"], False
    if m == "tsx":
        return ["c->x = c->s;", "xl_nz(c, c->x);"], False
    if m == "txs":
        return ["c->s = c->x;"], False
    if m in ("inx", "dex", "iny", "dey"):
        reg = m[2].lower()
        op = "+ 1" if m.startswith("in") else "- 1"
        return ["c->%s = (uint8_t)(c->%s %s);" % (reg, reg, op),
                "xl_nz(c, c->%s);" % reg], False
    if m in ("cla", "clx", "cly"):
        return ["c->%s = 0;" % m[2].lower()], False
    if m == "sax":
        return ["{ uint8_t t = c->a; c->a = c->x; c->x = t; }"], False
    if m == "say":
        return ["{ uint8_t t = c->a; c->a = c->y; c->y = t; }"], False
    if m == "sxy":
        return ["{ uint8_t t = c->x; c->x = c->y; c->y = t; }"], False

    if m in ("pha", "phx", "phy"):
        return ["xl_push(c, c->%s);" % m[2].lower()], False
    if m == "php":
        # PHP clears T before pushing, so the pushed byte never has T set.
        return ["xl_push(c, (uint8_t)((c->p & (uint8_t)~XL_T) | XL_BF));"], False
    if m == "pla":
        return ["c->a = xl_pop(c);", "xl_nz(c, c->a);"], False
    if m == "plx":
        return ["c->x = xl_pop(c);", "xl_nz(c, c->x);"], False
    if m == "ply":
        return ["c->y = xl_pop(c);", "xl_nz(c, c->y);"], False
    if m == "plp":
        return ["c->p = xl_pop(c);"], False

    if m in BRANCH_COND:
        return ["if (%s) { c->pc = 0x%04X; xl_tick(c, %d); } else { c->pc = 0x%04X; xl_tick(c, %d); }"
                % (BRANCH_COND[m], ins.rel_target, CYCLES["branch_taken"],
                   nxt, CYCLES["branch_not"])], True
    if m == "bra":
        return ["c->pc = 0x%04X;" % ins.rel_target], True
    if m == "bsr":
        return ["xl_push16(c, 0x%04X);" % ((ins.addr + 1) & 0xFFFF),
                "c->pc = 0x%04X;" % ins.rel_target], True
    if m == "jsr":
        # 6502 convention: push the address of the instruction's last byte, so
        # RTS (pop + 1) lands on the next instruction.
        return ["xl_push16(c, 0x%04X);" % ((ins.addr + 2) & 0xFFFF),
                "c->pc = 0x%04X;" % ins.abs16], True
    if m == "rts":
        return ["c->pc = (uint16_t)(xl_pop16(c) + 1);"], True
    if m == "rti":
        # The three pops live in xl_rti() so the trace can see every interrupt
        # return in one place (t=IRQE).  Same three statements, same order.
        return ["xl_rti(c);"], True
    if m == "jmp":
        if ins.mode in (isa.M_HHLL, isa.M_BRANCH_OR_LABEL):
            return ["c->pc = 0x%04X;" % ins.abs16], True
        return ["c->pc = %s;" % ea_expr(ins)], True

    if m in ("rmb0", "rmb1", "rmb2", "rmb3", "rmb4", "rmb5", "rmb6", "rmb7"):
        bit = int(m[-1])
        return ["{",
                "    uint16_t ea = xl_zp(0x%02X);" % ins.b[1],
                "    uint8_t v = (uint8_t)(xl_rd(c, ea) & 0x%02X);" % (0xFF ^ (1 << bit)),
                "    xl_tick(c, %d);" % cycles_for(ins),
                "    xl_wr(c, ea, v);",
                "}"], False
    if m in ("smb0", "smb1", "smb2", "smb3", "smb4", "smb5", "smb6", "smb7"):
        bit = int(m[-1])
        return ["{",
                "    uint16_t ea = xl_zp(0x%02X);" % ins.b[1],
                "    uint8_t v = (uint8_t)(xl_rd(c, ea) | 0x%02X);" % (1 << bit),
                "    xl_tick(c, %d);" % cycles_for(ins),
                "    xl_wr(c, ea, v);",
                "}"], False
    if m.startswith("bbr") or m.startswith("bbs"):
        bit = int(m[-1])
        want = 0 if m.startswith("bbr") else 1
        test = "!(xl_rd(c, xl_zp(0x%02X)) & 0x%02X)" % (ins.b[1], 1 << bit)
        if want:
            test = "xl_rd(c, xl_zp(0x%02X)) & 0x%02X" % (ins.b[1], 1 << bit)
        return ["if (%s) { c->pc = 0x%04X; xl_tick(c, %d); } else { c->pc = 0x%04X; xl_tick(c, %d); }"
                % (test, ins.rel_target, CYCLES["bit_branch_taken"], nxt,
                   CYCLES["bit_branch"])], True

    if m == "tam":
        return ["xl_tam(c, 0x%02X);" % ins.b[1]], False
    if m == "tma":
        return ["xl_tma(c, 0x%02X);" % ins.b[1]], False
    if m == "st0":
        return ["xl_st0(c, 0x%02X);" % ins.b[1]], False
    if m == "st1":
        return ["xl_st1(c, 0x%02X);" % ins.b[1]], False
    if m == "st2":
        return ["xl_st2(c, 0x%02X);" % ins.b[1]], False

    if m in ("tii", "tdd", "tin", "tia", "tai"):
        src, dst, ln = ins.block
        return ["xl_bmt(c, XL_%s, 0x%04X, 0x%04X, 0x%04X);"
                % (m.upper(), src, dst, ln)], False

    if m == "set":
        return ["c->p |= XL_T;"], False
    # CSL/CSH: the ONE instruction whose two tick boundaries fall on opposite sides
    # of a state change.  Reference core, huc6280.cpp:245-248 --
    #
    #     #define CSL { ADDCYC(2); speed = 0; REDOSPEEDCACHE(); LastCycle(); }
    #     #define CSH { ADDCYC(2); speed = 1; REDOSPEEDCACHE(); LastCycle(); }
    #     // "the speed timing changes take effect for the last CPU cycle"
    #
    # so the body's two cycles are charged at the old speed and LastCycle()'s one at
    # the new.  Emitting them here (and not letting emit_function add the usual
    # single tick) keeps both engines, and the Python reference, on the same two
    # edges.  The instruction still costs 3 charged cycles -- cycles.py is right --
    # but in slow mode those 3 cycles are 36 master clocks, not 9.
    if m == "csl" or m == "csh":
        return ["xl_tick(c, 2);",
                "xl_speed_set(c, %d);" % (0 if m == "csl" else 1),
                "xl_tick(c, 1);"], False
    if m == "nop":
        return [], False

    return None


def emit_function(ins, name):
    res = emit_body(ins)
    if res is None:
        return None
    body, sets_pc = res
    lines = ["static void %s(Cpu *c) {   /* %s */" % (name, ins.text)]
    cyc = cycles_for(ins)
    # A store's bus cycle is the instruction's last cycle (see LATE_STORE).
    tick_first = cyc is not None and ins.mnem in LATE_STORE
    if tick_first:
        lines.append("    xl_tick(c, %d);" % cyc)
    lines.extend("    " + ln for ln in body)
    if cyc is not None and not tick_first and not emits_own_tick(ins):
        lines.append("    xl_tick(c, %d);" % cyc)
    # The CPU clears T at the end of every instruction except these three.
    if ins.mnem not in ("rti", "plp", "set"):
        lines.append("    c->p &= (uint8_t)~XL_T;")
    if not sets_pc:
        lines.append("    c->pc = 0x%04X;" % ((ins.addr + ins.size) & 0xFFFF))
    lines.append("}")
    return "\n".join(lines)


def unit_name(img_sha, page, off):
    """A unit's stable identity: bank image content hash, logical page, offset.

    Not the discovery order: adding a discovery must never rename or renumber a
    unit that already existed, because a patch, a symbol reference or a CDL key is
    written against the name.
    """
    return "u_%s_p%d_%04X" % (img_sha[:8], page, off)


class BankImages:
    """The bytes of each physical bank, independent of any one mapping.

    Translation needs this because a bank can be mapped at different pages at
    different times; the *bytes* belong to the bank, not to the address.
    """

    def __init__(self):
        self.banks = {}
        self.have = {}           # per bank: which bytes an input image covered
        self._sha = {}

    def add(self, bank, base, data):
        """Place `data` at logical `base`, continuing into the next banks."""
        pos = 0
        addr = base
        b = bank
        while pos < len(data):
            page = (addr >> 13) & 7
            off = addr & 0x1FFF
            chunk = min(BANK_SIZE - off, len(data) - pos)
            buf = self.banks.setdefault(b & 0xFF, bytearray(BANK_SIZE))
            have = self.have.setdefault(b & 0xFF, bytearray(BANK_SIZE))
            buf[off:off + chunk] = data[pos:pos + chunk]
            have[off:off + chunk] = b"\x01" * chunk
            self._sha.pop(b & 0xFF, None)
            pos += chunk
            addr += chunk
            b += 1
        return True

    def read(self, bank, off):
        buf = self.banks.get(bank & 0xFF)
        if buf is None or off >= BANK_SIZE:
            return None
        return buf[off]

    def present(self, bank):
        return (bank & 0xFF) in self.banks

    def sha1(self, bank):
        """SHA-1 of the 8 KB image this bank was built from (None if absent).

        This is the CDL's key.  For a bank only partly covered by an input the
        uncovered bytes are whatever the buffer starts as (zero), which is why
        `complete()` exists: only a fully covered bank can be *identified* with a
        CDL image, and only such a bank may take CDL-discovered code.
        """
        b = bank & 0xFF
        if b not in self.banks:
            return None
        if b not in self._sha:
            self._sha[b] = hashlib.sha1(bytes(self.banks[b])).hexdigest()
        return self._sha[b]

    def complete(self, bank):
        have = self.have.get(bank & 0xFF)
        return have is not None and all(have)


class Translation:
    """Bank-aware translation: one unit per (bank, offset inside the bank).

    A unit is named by the content of the bank image it was decoded from, the
    logical page it was translated for and its offset -- see `unit_name()`.
    """

    def __init__(self, log, images, initial_mpr=None, rom=None):
        self.log = log
        self.images = images
        self.units = {}          # (bank, off) -> Ins
        self.page_of = {}        # (bank, off) -> logical page translated for
        self.blocked = set()     # (bank, off) that cannot be translated
        self.unsupported = []
        self.mpr = list(initial_mpr if initial_mpr is not None else DEFAULT_MPR)
        self.initial_mpr = list(self.mpr)   # where every seed's walk starts
        self.rom = rom           # BIOS image (banks 0..31), for constant reads
        self.known_a = None
        self.assumptions = []    # times the translator had to guess a bank
        self.conflicts = []      # same bank+offset reached via two different pages
        self.unknown_targets = []
        self.delegated = set()   # (bank, off) whose bytes cross into the next bank
        self.cdl = {"banks": [], "emitted": 0, "rejected": 0, "delegated": 0,
                    "inside": 0, "disagree": 0, "already": 0, "skipped": []}

    # -- identity --------------------------------------------------------
    def name(self, bank, off):
        """The unit's stable name -- see `unit_name()`."""
        sha = self.images.sha1(bank)
        if sha is None:
            raise SystemExit("recompiler: unit at bank $%02X offset $%04X has no image"
                             % (bank & 0xFF, off))
        return unit_name(sha, self.page_of[(bank, off)], off)

    # -- fetching through the tracked mapping ----------------------------
    def _read_at(self, addr):
        bank = self.mpr[(addr >> 13) & 7]
        if bank is None:
            return None
        return self.images.read(bank, addr & 0x1FFF)

    def _const_read(self, addr):
        """A value we can know at translation time, or None."""
        bank = self.mpr[(addr >> 13) & 7]
        if bank is None or self.rom is None or bank >= ROM_BANKS:
            return None
        idx = bank * BANK_SIZE + (addr & 0x1FFF)
        if idx >= len(self.rom):
            return None
        return self.rom[idx]

    # -- static MPR bookkeeping ------------------------------------------
    def _track_a(self, ins):
        if ins.mnem == "lda":
            if ins.mode == isa.M_NN:
                self.known_a = ins.b[1]
            elif ins.mode in (isa.M_HHLL, isa.M_BRANCH_OR_LABEL):
                self.known_a = self._const_read(ins.abs16)
            else:
                self.known_a = None
        elif ins.mnem in A_WRITERS or ins.mnem in ("inc", "dec", "asl", "lsr",
                                                   "rol", "ror") and ins.mode == isa.M_OP_A:
            self.known_a = None
        elif ins.mnem in ("ldx", "ldy", "stx", "sty", "sta", "stz", "nop", "cmp",
                          "cpx", "cpy", "bit", "pha", "php", "phx", "phy",
                          "inx", "dex", "iny", "dey", "tax", "tay", "tsx", "txs",
                          "clc", "sec", "cli", "sei", "clv", "cld", "sed",
                          "tam", "tma", "jmp", "jsr", "rts", "rti", "bra", "bsr",
                          "st0", "st1", "st2", "cla", "clx", "cly", "set",
                          "csl", "csh", "nop"):
            pass
        elif ins.mnem.startswith("b") or ins.mnem.startswith("rmb") or \
                ins.mnem.startswith("smb"):
            pass
        else:
            self.known_a = None

    def _tam_target(self, ins):
        """New bank for the *current page* after this instruction, if it matters."""
        if ins.mnem != "tam":
            return None
        mask = ins.b[1]
        new = self.known_a
        changed = []
        for i in range(8):
            if mask & (1 << i):
                self.mpr[i] = new
                changed.append(i)
        return changed

    # -- descent ---------------------------------------------------------
    def descend(self, seeds, limit=400000):
        """seeds: iterable of (bank, offset, page).

        The walk keeps the *logical* address and re-resolves bank and page from
        the tracked MPRs at every step, which is what makes a run that crosses an
        8 KB boundary continue into the next bank instead of the next address.

        Every seed starts from the mapping the caller established (`--mpr` from the
        savestate for a stage build, the loader images for the boot chain), and the
        walk then tracks the `tam`s it meets.  The mapping is *shared* across seeds:
        that is a known weakness -- a walk that meets a `tam` whose value is data
        leaves that page unknown, and a later walk that crosses the same page stops
        with "bank unknown", so adding an entry point can cost translations
        elsewhere (measured: 187 units between two builds of the same state).  It is
        kept because it translates more of the real program than resetting per seed
        (11,326 against 10,398 units on the same seed list): a seed reached at run
        time arrives with the later mapping, which the shared state has often
        already learned.  The discovery loop therefore is not monotone; it converges
        because a lost address is reported again at run time.
        """
        todo = [(b, o, p, True) for (b, o, p) in seeds]
        while todo:
            bank, off, page, is_seed = todo.pop()
            if is_seed:
                # An entry point implies a mapping: that bank is mapped at that
                # page when the program arrives here.  Everything reachable from
                # this seed is then resolved against it.
                self.mpr[page] = bank
            cur = (page << 13) | off
            # A seed names the bank its code lives in; that is authoritative for
            # the first instruction.  After that, bank and page are re-resolved
            # from the tracked MPRs, which is what makes a run that crosses an 8 KB
            # boundary continue into the next bank instead of the next address.
            seed_bank = bank
            while True:
                off = cur & 0x1FFF
                page = (cur >> 13) & 7
                if seed_bank is not None:
                    bank = seed_bank
                    seed_bank = None
                elif self.mpr[page] is None:
                    self.assumptions.append(
                        ("?", cur, page, "bank unknown at $%04X" % cur))
                    break
                else:
                    bank = self.mpr[page]
                key = (bank & 0xFF, off)
                if key in self.units or key in self.blocked:
                    prev = self.page_of.get(key)
                    if prev is not None and prev != page:
                        self.conflicts.append((key, prev, page))
                    break
                # decode from *this unit's bank*, not through the tracked mapping:
                # a seed may name a bank that is not the one currently mapped.
                base_off = off

                def _fetch(addr, _bank=bank, _base=base_off):
                    o = addr & 0x1FFF
                    if o < _base:            # ran past the end of the bank
                        return None
                    return self.images.read(_bank, o)

                ins = isa.decode(_fetch, cur)
                if ins is None:
                    # The only way decode fails inside a bank is a fetch past its end.
                    # Get the opcode's width from the table and decide there: the
                    # instruction is real, its operand bytes are just not in this bank.
                    op = self.images.read(bank, off)
                    if op is None:
                        break
                    _mnem, _size, _mode = isa.TABLE[op]
                    if _size > BANK_SIZE - off:
                        # The instruction's bytes cross into the next bank, which the
                        # machine pages in at run time and which this translation is
                        # not keyed to (the kernel's bank $69 ends with `beq` at $5FFF
                        # and its displacement is the first byte of the next bank).  A
                        # unit is emitted that hands this one step to the interpreter,
                        # which fetches through the live bank registers and charges the
                        # same cycle table -- no operand is frozen from bytes that may
                        # have been reloaded since.
                        self.units[key] = isa.Ins(cur, op, _mnem.strip(), _size, _mode,
                                                  [op] + [0] * (_size - 1))
                        self.page_of[key] = page
                        self.delegated.add(key)
                        self.log.log("bank $%02X: instruction at $%04X crosses into the "
                                     "next bank; one interpreted step" % (bank, cur))
                    break
                if ins.mnem in UNSUPPORTED:
                    self.blocked.add(key)
                    self.unsupported.append(ins)
                    break
                if len(self.units) >= limit:
                    self.log.log("descent: hit %d unit limit" % limit)
                    return self.units
                self.units[key] = ins
                self.page_of[key] = page

                follow, falls = isa.flow(ins)
                for t in follow:
                    tpage = (t >> 13) & 7
                    tbank = self.mpr[tpage]
                    if tbank is None:
                        self.unknown_targets.append((bank, cur, t))
                    else:
                        todo.append((tbank, t & 0x1FFF, tpage, False))

                self._track_a(ins)
                changed = self._tam_target(ins)

                if not falls:
                    break
                nxt = cur + ins.size
                if changed and page in changed:
                    # The bank under our feet just changed: the next instruction
                    # belongs to the new bank, so the linear run stops here and the
                    # continuation is queued as its own seed.
                    nbank = self.mpr[page]
                    if nbank is None:
                        self.assumptions.append(
                            (bank, cur, page, "TAM value unknown; kept the old bank"))
                        cur = nxt
                        continue
                    todo.append((nbank, nxt & 0x1FFF, page, False))
                    break
                cur = nxt

        return self.units

    # -- code the console says is code -----------------------------------
    def _page_for(self, bank):
        """The logical page to translate a bank's CDL-discovered units for.

        A unit's emitted C depends on the page: branch targets, JSR pushes and the
        fall-through PC are *logical* addresses.  So the page is part of the unit's
        identity and must be the one the dispatcher will see.  Prefer the page this
        bank's units already use (agreement with the descent, no conflict), else a
        page the tracked mapping has the bank at, else give up and say so.
        """
        seen = {}
        for (b, _o), p in self.page_of.items():
            if b == (bank & 0xFF):
                seen[p] = seen.get(p, 0) + 1
        if seen:
            return max(sorted(seen), key=lambda p: seen[p])
        mapped = [p for p in range(8) if self.mpr[p] == (bank & 0xFF)]
        return mapped[0] if mapped else None

    def expand_cdl(self, keyset, log, limit=400000):
        """Emit a unit for every offset the CDL marks as an opcode.

        Only for a bank whose 8 KB image is *identified* by its SHA-1 in the key
        set: the marks belong to that exact image, so they mean nothing for a bank
        holding different bytes (which is the superposition error an address-keyed
        log makes -- see the CDL report, section 4).

        The CDL says where an instruction *starts*; this function still decides
        how long it is, from the image and the ISA table, and reports every place
        the two disagree.  A mark inside an instruction already emitted is not a
        start.  An instruction that decodes to an undefined opcode, or to a
        mnemonic the emitter cannot translate, is rejected -- never emitted.
        """
        total = {"banks": [], "emitted": 0, "rejected": 0, "delegated": 0,
                 "inside": 0, "disagree": 0, "skipped": [], "already": 0}
        rejected_list = []
        disagree_list = []

        for bank in sorted(self.images.banks):
            sha = self.images.sha1(bank)
            entry = keyset.get(sha) if sha else None
            if entry is None:
                continue
            if not self.images.complete(bank):
                total["skipped"].append((bank, sha, "bank image is only partly known"))
                log.log("cdl: bank $%02X image %s is only partly covered by its inputs; "
                        "not expanded" % (bank, sha[:12]))
                continue
            page = self._page_for(bank)
            if page is None:
                total["skipped"].append((bank, sha, "no page the bank is mapped at"))
                log.log("cdl: bank $%02X image %s is a key but no page maps it; not expanded"
                        % (bank, sha[:12]))
                continue

            ops = set()
            for start, n in entry.get("opcode", ()):
                ops.update(range(start, start + n))

            buf = bytes(self.images.banks[bank])

            def _fetch(addr, _buf=buf):
                return _buf[addr & 0x1FFF]

            # Every offset already inside an emitted instruction, so a mark that is
            # an operand byte of one is not mistaken for a start.
            covered = bytearray(BANK_SIZE)
            for (b, o), ins in self.units.items():
                if b != bank:
                    continue
                for k in range(min(ins.size, BANK_SIZE - o)):
                    covered[o + k] = 1

            emitted = rejected = delegated = inside = disagree = already = 0
            off = 0
            while off < BANK_SIZE:
                if off not in ops:
                    off += 1
                    continue
                key = (bank, off)
                if key in self.units:
                    # The descent already has this one.
                    already += 1
                    off += 1
                    continue
                if key in self.blocked:
                    off += 1
                    continue
                if covered[off]:
                    # A mark inside an instruction the descent already emitted: not
                    # a start.  The decoder has decided the split here, and this is
                    # the count that says the two disagree.
                    inside += 1
                    disagree_list.append((bank, (page << 13) | off,
                                          "the log marks an opcode inside an "
                                          "instruction the descent emitted"))
                    off += 1
                    continue
                if len(self.units) >= limit:
                    log.log("cdl: hit %d unit limit" % limit)
                    break
                cur = (page << 13) | off
                op = buf[off]
                _mnem, _size, _mode = isa.TABLE[op]
                if _size > BANK_SIZE - off:
                    # The operand bytes are in the next bank, which the machine pages
                    # in at run time: one interpreted step, exactly as the descent
                    # does for the same case.  Never decoded from wrapped bytes --
                    # that would be a false instruction built out of the bank's start.
                    self.units[key] = isa.Ins(cur, op, _mnem.strip(), _size, _mode,
                                              [op] + [0] * (_size - 1))
                    self.page_of[key] = page
                    self.delegated.add(key)
                    delegated += 1
                    for k in range(_size):
                        if off + k < BANK_SIZE:
                            covered[off + k] = 1
                    off += _size
                    continue
                ins = isa.decode(_fetch, cur)
                if ins is None:
                    # Cannot happen: the width was checked against the bank end and
                    # the fetch always answers.  A build failure, not a silent skip.
                    raise SystemExit("recompiler: $%04X in bank $%02X decoded to "
                                     "nothing inside its own image" % (cur, bank))
                if ins.mnem in UNSUPPORTED:
                    rejected += 1
                    rejected_list.append((bank, cur, ins.text))
                    self.blocked.add(key)
                    self.unsupported.append(ins)
                    for k in range(ins.size):
                        if off + k < BANK_SIZE:
                            covered[off + k] = 1
                    off += ins.size
                    continue
                # The decoder decides the length; if that swallows an opcode mark
                # the log's split and the decoder's disagree, and the address is
                # named rather than hidden.
                for k in range(1, ins.size):
                    if (off + k) in ops:
                        disagree += 1
                        disagree_list.append(
                            (bank, cur, "decodes to %d bytes; the log marks an opcode "
                                        "at +%d" % (ins.size, k)))
                        break
                self.units[key] = ins
                self.page_of[key] = page
                emitted += 1
                for k in range(min(ins.size, BANK_SIZE - off)):
                    covered[off + k] = 1
                off += ins.size

            total["banks"].append((bank, sha, len(ops), emitted, rejected,
                                   delegated, inside, disagree, already))
            total["emitted"] += emitted
            total["rejected"] += rejected
            total["delegated"] += delegated
            total["inside"] += inside
            total["disagree"] += disagree
            total["already"] += already

        self.cdl = total
        if total["banks"]:
            log.log("cdl: %d bank images identified by content; %d opcode marks, "
                    "%d new units, %d already translated, %d marks inside an emitted "
                    "instruction, %d rejected, %d cross-bank steps delegated, "
                    "%d decoder/log disagreements"
                    % (len(total["banks"]),
                       sum(r[2] for r in total["banks"]),
                       total["emitted"], total["already"], total["inside"],
                       total["rejected"], total["delegated"], total["disagree"]))
            for (bank, sha, nops, em, rj, dg, ins_, dis, al) in total["banks"]:
                log.log("  cdl: bank $%02X image %s: %d opcode marks -> %d new units, "
                        "%d already, %d inside, %d rejected, %d delegated, %d disagree"
                        % (bank, sha[:12], nops, em, al, ins_, rj, dg, dis))
        else:
            log.log("cdl: no bank image matches a key in the set; nothing added")
        for (bank, addr, why) in rejected_list[:10]:
            log.log("  cdl rejected: bank $%02X $%04X %s" % (bank, addr, why))
        for (bank, addr, why) in disagree_list[:10]:
            log.log("  cdl disagreement: bank $%02X $%04X %s" % (bank, addr, why))
        return self.units

    def stats(self):
        return {"instructions": len(self.units),
                "bytes": sum(i.size for i in self.units.values()),
                "unsupported": len(self.unsupported),
                "delegated": len(self.delegated),
                "banks": sorted({b for (b, _o) in self.units})}


def c_bytes(name, data, per_line=16):
    out = ["static const uint8_t %s[] = {" % name]
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        out.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    out.append("};")
    return "\n".join(out)


def _emit_dispatcher(parts, tr):
    """Two-level switch: bank, then the logical address inside that page.

    Switching on the *logical* address rather than the offset is what makes a bank
    mapped at an unexpected page fail loudly instead of running the wrong code.
    """
    by_bank = {}
    for (bank, off), ins in tr.units.items():
        page = tr.page_of[(bank, off)]
        by_bank.setdefault(bank, []).append((page << 13 | off, bank, off))

    parts.append("int xlate_step(Cpu *c)")
    parts.append("{")
    parts.append("    unsigned bank;")
    parts.append("    /* $E000-$FFFF is the CD BIOS: modelled, never translated.  Under")
    parts.append("       XLATE_BIOS_EXEC=1 the page holds the user's System Card and")
    parts.append("       xl_cpu_step() has already routed it to the interpreter, so nothing")
    parts.append("       arrives here that should have run; the test keeps the model for a")
    parts.append("       page that is NOT the card (a game bank mapped at $E000). */")
    parts.append("    if (c->pc >= 0xE000 && !xl_pc_in_rom(c)) { xl_bios_call(c); return 1; }")
    parts.append("    bank = c->mpr[c->pc >> 13];")
    parts.append("    switch (bank) {")
    for bank in sorted(by_bank):
        parts.append("    case 0x%02X:      /* %d instruction units */" % (bank, len(by_bank[bank])))
        parts.append("        switch (c->pc) {")
        for (pc, _b, off) in sorted(by_bank[bank]):
            parts.append("        case 0x%04X: %s(c); return 1;" % (pc, tr.name(bank, off)))
        parts.append("        }")
        parts.append("        break;")
    parts.append("    }")
    parts.append("    /* Code the program builds in work RAM at run time (a patched block")
    parts.append("       move, for instance) cannot be translated ahead of time. */")
    parts.append("    if (xl_ram_trampoline(c))")
    parts.append("        return 1;")
    parts.append("    return xl_untranslated(c, (uint8_t)bank, c->pc);")
    parts.append("}")
    parts.append("")


def build_program(images, entry, log, extra_seeds=(), initial_mpr=None, rom=None,
                  keyset=None):
    """images: list of dicts {bank, base, data, load(bool), name}.

    `extra_seeds` are additional *logical* entry points (a handler reached only by
    an indirect jump, for example).  Their bank is resolved from the tracked
    mapping, so they land in the right module.

    `keyset` is the content-keyed CDL (SHA-1 of a bank image -> offsets marked as
    opcode).  It runs *after* the descent and adds the code the descent could not
    reach; see `Translation.expand_cdl`.
    """
    banks = BankImages()
    for img in images:
        banks.add(img["bank"], img["base"], img["data"])

    # A *loaded* image at (bank, address) implies that bank is mapped at that page when
    # the program starts, so the emitted init keeps code and machine in agreement.
    # Code-only images are bytes the machine supplies later, so they must NOT dictate
    # the boot mapping (they would silently change the initial bank registers).
    mpr = list(initial_mpr if initial_mpr is not None else DEFAULT_MPR)
    for img in images:
        if img.get("load"):
            mpr[(img["base"] >> 13) & 7] = img["bank"] & 0xFF

    tr = Translation(log, banks, initial_mpr=mpr, rom=rom)
    # An image is also an entry point unless it is marked `seed: False`.  A bank taken
    # from a running machine is often mapped at a page the state does not show, so
    # seeding it would pin its units to a guessed page and make the real placement a
    # "conflict" that the first guess wins.  Such a bank is supplied as *bytes only*:
    # its code is translated when something actually maps it in (`tam`) or when a
    # discovered entry point names it.
    seeds = [(img["bank"], img["base"] & 0x1FFF, (img["base"] >> 13) & 7)
             for img in images if img.get("seed", True)]
    for s in extra_seeds:
        if isinstance(s, tuple):
            _kind, bank, addr = s
            page = (addr >> 13) & 7
            seeds.append((bank, addr & 0x1FFF, page))
            continue
        page = (s >> 13) & 7
        bank = tr.mpr[page]
        if bank is None:
            log.log("seed $%04X: bank unknown, skipped" % s)
        else:
            seeds.append((bank, s & 0x1FFF, page))
    tr.descend(seeds)
    if keyset is not None:
        tr.expand_cdl(keyset, log)

    st = tr.stats()
    log.log("translated %d instruction units in banks %s (%d bytes), "
            "%d unsupported, %d cross-bank steps delegated, %d page conflicts, "
            "%d unresolved bank assumptions"
            % (st["instructions"],
               " ".join("$%02X" % b for b in st["banks"]),
               st["bytes"], st["unsupported"], st["delegated"], len(tr.conflicts),
               len(tr.assumptions)))
    for (key, prev, page) in tr.conflicts[:5]:
        log.log("  conflict: bank $%02X offset $%04X reached on page %d and %d"
                % (key[0], key[1], prev, page))
    for (bank, addr, page, why) in tr.assumptions[:5]:
        # `bank` is the marker "?" when the page's bank was never established.
        which = ("$%02X" % bank) if isinstance(bank, int) else str(bank)
        log.log("  bank assumption at $%04X (bank %s): %s" % (addr, which, why))
    for ins in tr.unsupported[:10]:
        log.log("  unsupported: $%04X %s" % (ins.addr, ins.text))

    parts = ["/* Generated by src/recompiler.py -- do not edit.",
             " * Bank-keyed translation: one unit per (bank image, page, offset).",
             " * Unit names are content-keyed: u_<image sha1[:8]>_p<page>_<offset>.",
             " * The code/data log added %d units in banks %s."
             % (tr.cdl["emitted"],
                " ".join("$%02X" % b for (b, _s, _n, _e, _r, _d, _i, _g, _a)
                         in tr.cdl["banks"]) or "none")]
    for img in images:
        parts.append(" *   %-36s bank $%02X from $%04X (%d bytes)"
                     % (img["name"], img["bank"], img["base"], len(img["data"])))
    parts.append(" */")
    parts.append('#include "runtime.h"')
    parts.append("")

    for i, img in enumerate(images):
        if img.get("load"):
            parts.append(c_bytes("img_%d" % i, img["data"]))
        else:
            # Code images are not loaded into the machine at init, but the stale-bank
            # instrument needs the bytes the translation was built from (report only).
            parts.append(c_bytes("ref_%d" % i, img["data"]))
        parts.append("")

    for (bank, off) in sorted(tr.units):
        if (bank, off) in tr.delegated:
            parts.append("static void %s(Cpu *c) {   /* %s */"
                         % (tr.name(bank, off), tr.units[(bank, off)].text))
            parts.append("    /* This instruction's operand bytes are in the next bank, "
                         "which the machine pages in at run time. */")
            parts.append("    if (xl_interp_step(c)) c->interp_steps++;")
            parts.append("}")
            parts.append("")
            continue
        fn = emit_function(tr.units[(bank, off)], tr.name(bank, off))
        if fn is None:
            continue
        parts.append(fn)
        parts.append("")

    parts.append("void xlate_init_state(Cpu *c)")
    parts.append("{")
    parts.append("    xl_reset(c);")
    for (i, img) in enumerate(images):
        if not img.get("load"):
            continue
        parts.append("    /* %s: %d bytes into bank $%02X from $%04X */"
                     % (img["name"], len(img["data"]), img["bank"],
                        img["base"] & 0x1FFF))
        parts.append("    xl_load_phys(c, 0x%02X, 0x%04X, img_%d, sizeof img_%d);"
                     % (img["bank"], img["base"] & 0x1FFF, i, i))
    parts.append("    /* Register the bytes each bank's translation was built from.")
    parts.append("       Report-only instrument: it compares against these, writes nothing. */")
    for i, img in enumerate(images):
        ref = ("img_%d" if img.get("load") else "ref_%d") % i
        parts.append("    xl_stale_image(c, 0x%02X, 0x%04X, %s, sizeof %s);"
                     % (img["bank"], img["base"] & 0x1FFF, ref, ref))
    for i in range(8):
        parts.append("    c->mpr[%d] = 0x%02X;" % (i, mpr[i] & 0xFF))
    parts.append("    c->pc = 0x%04X;" % entry)
    parts.append("}")
    parts.append("")

    _emit_dispatcher(parts, tr)
    return "\n".join(parts), tr, banks


# ------------------------------------------------------------------ cases --
def build_cases(specs, out_path=None):
    """Emit a translation unit holding N independent random-test cases.

    Each case's code sits in its own bank at its own offset, which is the same
    keying the real program uses; several cases can share a bank at different
    offsets.
    """
    parts = ["/* Generated by src/recompiler.py --cases: %d random cases. */"
             % len(specs), '#include "runtime.h"', ""]
    entries = []
    for i, spec in enumerate(specs):
        code = bytes(spec["code"])
        data = bytes(spec.get("data", b""))
        parts.append(c_bytes("c%d_code" % i, code))
        if data:
            parts.append(c_bytes("c%d_data" % i, data))
        parts.append("static void case_init_%d(Cpu *c)" % i)
        parts.append("{")
        parts.append("    xl_reset(c);")
        for r in range(8):
            parts.append("    c->mpr[%d] = 0x%02X;" % (r, spec["mpr"][r]))
        parts.append("    c->a = 0x%02X; c->x = 0x%02X; c->y = 0x%02X; c->s = 0x%02X;"
                     % (spec["a"], spec["x"], spec["y"], spec["s"]))
        parts.append("    c->p = 0x%02X; c->pc = 0x%04X;" % (spec["p"], spec["pc"]))
        parts.append("    xl_load_phys(c, 0x%02X, 0x%04X, c%d_code, sizeof c%d_code);"
                     % (spec["code_bank"], spec.get("code_off", 0), i, i))
        if data:
            parts.append("    xl_load_phys(c, 0x%02X, 0x%04X, c%d_data, sizeof c%d_data);"
                         % (spec["data_bank"], spec["data_off"], i, i))
        for (pb, poff, blob) in spec.get("patches", []):
            parts.append("    { static const uint8_t p[] = {%s}; "
                         "xl_load_phys(c, 0x%02X, 0x%04X, p, sizeof p); }"
                         % (", ".join("0x%02X" % b for b in blob), pb, poff))
        parts.append("}")
        parts.append("")
        entries.append((spec["id"], i, spec["steps"]))

    banks = BankImages()
    for spec in specs:
        # Cases share banks, so merge into the bank rather than replacing it:
        # overwriting would leave every case but the last with the wrong bytes.
        buf = banks.banks.setdefault(spec["code_bank"], bytearray(BANK_SIZE))
        off = spec.get("code_off", 0)
        buf[off:off + len(spec["code"])] = bytes(spec["code"])

    master = Translation(xlog.announce(), banks)
    for spec in specs:
        per = Translation(xlog.announce(), banks, initial_mpr=spec["mpr"])
        page = (spec["pc"] >> 13) & 7
        seeds = [(spec["code_bank"], spec["pc"] & 0x1FFF, page)]
        for s in spec.get("seeds", []):
            spage = (s >> 13) & 7
            sbank = spec["mpr"][spage]
            if sbank is not None:
                seeds.append((sbank, s & 0x1FFF, spage))
        per.descend(seeds)
        for key, ins in per.units.items():
            master.units.setdefault(key, ins)
            master.page_of.setdefault(key, per.page_of[key])

    for (bank, off) in sorted(master.units):
        fn = emit_function(master.units[(bank, off)], master.name(bank, off))
        if fn:
            parts.append(fn)
            parts.append("")

    if entries:
        parts.append("static const XlateCase case_table[] = {")
        for (cid, i, steps) in entries:
            parts.append("    { %d, case_init_%d, %dUL }," % (cid, i, steps))
        parts.append("};")
    else:
        parts.append("static const XlateCase case_table[1] = { { 0, 0, 0UL } };")
    parts.append("const XlateCase *xlate_cases(unsigned *count)")
    parts.append("{")
    parts.append("    *count = %du;" % len(entries))
    parts.append("    return case_table;")
    parts.append("}")
    parts.append("")
    _emit_dispatcher(parts, master)
    text = "\n".join(parts)
    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
    return text, master


# -------------------------------------------------------------------- cli --
def parse_image_arg(s):
    """BANK:ADDR=PATH, e.g. 0x80:0x4000=bootprog.bin -- the bank matters now."""
    try:
        loc, path = s.split("=", 1)
        bank, addr = loc.split(":")
        return {"bank": int(bank, 0), "base": int(addr, 0), "path": path}
    except ValueError:
        raise argparse.ArgumentTypeError("expected BANK:ADDR=PATH, got %r" % s)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", action="append", default=[], type=parse_image_arg,
                    metavar="BANK:ADDR=PATH",
                    help="translate and load into the initial state, in bank BANK")
    ap.add_argument("--code", action="append", default=[], type=parse_image_arg,
                    metavar="BANK:ADDR=PATH",
                    help="translate only (the machine supplies the bytes at runtime)")
    ap.add_argument("--bytes", action="append", default=[], type=parse_image_arg,
                    metavar="BANK:ADDR=PATH",
                    help="translate only, and do not treat it as an entry point (bytes "
                         "a `tam` or a discovered seed may reach)")
    ap.add_argument("--rom", default=None,
                    help="BIOS image, so `lda $FFF5`-style constants can be resolved")
    ap.add_argument("--entry", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--mpr", default=None, metavar="B0,B1,...,B7",
                    help="the eight bank registers the program starts with, e.g. "
                         "FF,F8,69,6A,6B,6D,6E,00 (taken from a savestate). Every "
                         "entry point's walk starts from this mapping, so a seed that "
                         "meets an unknowable `tam` cannot poison the others")
    ap.add_argument("--seed", action="append", default=[], metavar="[BANK:]ADDR",
                    help="extra entry point, e.g. 0x80:0x46FD or 0x46FD")
    ap.add_argument("--cdl-keyset", default=None, metavar="PATH",
                    help="content-keyed code/data log (keyset.json): for every bank "
                         "whose 8 KB image is a key, translate every offset the log "
                         "marks as an opcode. 'none' disables it")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default=None, help="JSON case specs (differential tests)")
    a = ap.parse_args()

    lg = xlog.announce()
    if a.cases:
        with open(a.cases) as f:
            specs = json.load(f)
        text, tr = build_cases(specs, a.out)
        print("wrote %s (%d cases, %d instruction units in %d banks)"
              % (a.out, len(specs), len(tr.units),
                 len({b for (b, _o) in tr.units})))
        return 0

    images = []
    for item in a.image:
        images.append({"bank": item["bank"], "base": item["base"],
                       "data": open(item["path"], "rb").read(), "load": True,
                       "name": os.path.basename(item["path"])})
    for item in a.code:
        images.append({"bank": item["bank"], "base": item["base"],
                       "data": open(item["path"], "rb").read(), "load": False,
                       "name": os.path.basename(item["path"])})
    for item in a.bytes:
        images.append({"bank": item["bank"], "base": item["base"],
                       "data": open(item["path"], "rb").read(), "load": False,
                       "seed": False, "name": os.path.basename(item["path"])})
    if not images:
        ap.error("need at least one --image or --code")
    entry = a.entry if a.entry is not None else images[0]["base"]
    rom = open(a.rom, "rb").read() if a.rom else None
    mpr0 = None
    if a.mpr:
        vals = [int(v, 16) for v in a.mpr.replace("0x", "").split(",")]
        if len(vals) != 8:
            ap.error("--mpr needs eight values, got %d" % len(vals))
        mpr0 = vals

    seeds = []
    for raw in a.seed:
        if ":" in raw:
            b, addr = raw.split(":")
            seeds.append(("bank", int(b, 0), int(addr, 0)))
        else:
            seeds.append(("addr", 0, int(raw, 0)))
    keyset = None
    if a.cdl_keyset and a.cdl_keyset != "none":
        if not os.path.exists(a.cdl_keyset):
            ap.error("--cdl-keyset %s does not exist (or pass 'none')" % a.cdl_keyset)
        with open(a.cdl_keyset) as f:
            ks = json.load(f)
        if ks.get("format") != "rondorecomp content-keyed CDL keyset v1":
            ap.error("%s is not a content-keyed CDL keyset (format %r)"
                     % (a.cdl_keyset, ks.get("format")))
        keyset = ks["images"]
        lg.log("cdl: %d content keys from %s" % (len(keyset), a.cdl_keyset))
    text, tr, _banks = build_program(images, entry, lg, seeds, rom=rom,
                                     initial_mpr=mpr0, keyset=keyset)
    with open(a.out, "w") as f:
        f.write(text)
    st = tr.stats()
    print("wrote %s (%d instruction units, banks %s, %d bytes of code; "
          "%d from the code/data log in %d bank images)"
          % (a.out, st["instructions"],
             " ".join("$%02X" % b for b in st["banks"]), st["bytes"],
             tr.cdl["emitted"], len(tr.cdl["banks"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
