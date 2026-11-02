#!/usr/bin/env python3
"""A small, independent HuC6280 interpreter: the reference for differential tests.

This is *not* the deliverable.  It exists so the generated C can be checked
against something that was written separately: if both agree on tens of thousands
of randomly chosen instruction sequences, the recompiler's code generation (the
mechanical, error-prone half) is very likely right.

What it does NOT prove: that the *semantics* are right.  Both implementations
follow the same documentation, so a shared misreading would pass unnoticed.  The
semantics were taken from the HuC6280 core used by the emulator that is our
accuracy oracle (Beetle PCE / Mednafen `pce/huc6280.cpp` + `huc6280_ops.inc`,
fetched 2026-10-26), including its quirks:

  * zero page is at $2000-$20FF and the stack at $2100-$21FF;
  * the T flag makes AND/ORA/EOR/ADC (but not SBC) operate on $2000+X and then
    restore A, and T is cleared at the end of almost every instruction;
  * TST sets N/V from the *memory* byte and Z from (memory & immediate) == 0;
  * decimal ADC does not touch V.

Deliberately unsupported (raise): BRK and the undefined opcodes.  No interrupts.
"""
import cycles
import isa
from memory import ZP_BASE, STACK_BASE, IO_BANK

FLAG_C, FLAG_Z, FLAG_I, FLAG_D, FLAG_B, FLAG_T, FLAG_V, FLAG_N = 1, 2, 4, 8, 16, 32, 64, 128


class Unsupported(Exception):
    pass


class RefCpu:
    def __init__(self, space):
        self.space = space
        self.a = self.x = self.y = 0
        self.s = 0xFF
        self.p = FLAG_I | FLAG_B          # $24 at reset
        self.pc = 0
        self.steps = 0
        self.cycles = 0
        # THE SPEED MODE (CSL/CSH).  `cycles` is the master-clock timeline divided by
        # three -- the C engines' `c->cycles` -- so a charged ADDCYC cycle is worth
        # 3 master clocks normally and 12 in slow mode, and everything that adds to
        # `cycles` goes through charge() to apply that.  The mode starts at the
        # normal rate, which is the state the machines under test start in
        # (runtime.c: xl_reset() seeds normal, and every vector case copies it).
        self.speed = 1
        self.speed_shift = 0
        self._self_cycled = False  # CSL/CSH charged themselves, in two parts
        self.next_pc = 0
        self._taken = False        # set by the branch handlers: a branch whose
                                   # target *is* the next instruction is still a
                                   # taken branch, and costs 4 cycles, not 2
        self._abackup = 0

    # -- flags ---------------------------------------------------------------
    def nz(self, v):
        v &= 0xFF
        if v == 0:
            self.p = (self.p & ~(FLAG_N | FLAG_Z)) | FLAG_Z
        elif v & 0x80:
            self.p = (self.p & ~(FLAG_N | FLAG_Z)) | FLAG_N
        else:
            self.p = self.p & ~(FLAG_N | FLAG_Z)

    def setf(self, flag, on):
        if on:
            self.p |= flag
        else:
            self.p &= ~flag

    # -- stack ---------------------------------------------------------------
    def push(self, v):
        self.space.write(STACK_BASE | self.s, v & 0xFF)
        self.s = (self.s - 1) & 0xFF

    def pop(self):
        self.s = (self.s + 1) & 0xFF
        return self.space.read(STACK_BASE | self.s)

    # -- addressing ----------------------------------------------------------
    def ea(self, ins):
        """Effective address for a memory operand (None for immediates)."""
        sp, m, b = self.space, ins.mode, ins.b
        if m == isa.M_ZZ:
            return ZP_BASE | b[1]
        if m == isa.M_ZZX:
            return ZP_BASE | ((b[1] + self.x) & 0xFF)
        if m == isa.M_ZZY:
            return ZP_BASE | ((b[1] + self.y) & 0xFF)
        if m == isa.M_HHLL:
            return b[1] | (b[2] << 8)
        if m == isa.M_HHLLX:
            return ((b[1] | (b[2] << 8)) + self.x) & 0xFFFF
        if m == isa.M_HHLLY:
            return ((b[1] | (b[2] << 8)) + self.y) & 0xFFFF
        if m == isa.M_IZZ:
            ptr = sp.read_word_zp(b[1])
            return ptr
        if m == isa.M_IZZX:
            ptr = sp.read_word_zp((b[1] + self.x) & 0xFF)
            return ptr
        if m == isa.M_IZZY:
            ptr = sp.read_word_zp(b[1])
            return (ptr + self.y) & 0xFFFF
        if m == isa.M_IHHLL:
            a = b[1] | (b[2] << 8)
            return sp.read(a) | (sp.read((a + 1) & 0xFFFF) << 8)
        if m == isa.M_IHHLLX:
            a = ((b[1] | (b[2] << 8)) + self.x) & 0xFFFF
            return sp.read(a) | (sp.read((a + 1) & 0xFFFF) << 8)
        # TST's immediate-plus-address forms: the address is the second operand
        if m == isa.M_NN_ZZ:
            return ZP_BASE | b[2]
        if m == isa.M_NN_ZZX:
            return ZP_BASE | ((b[2] + self.x) & 0xFF)
        if m == isa.M_NN_HHLL:
            return b[2] | (b[3] << 8)
        if m == isa.M_NN_HHLLX:
            return ((b[2] | (b[3] << 8)) + self.x) & 0xFFFF
        raise Unsupported("ea: mode %s for %s" % (isa.MODE_NAMES.get(m, m), ins.mnem))

    def operand(self, ins):
        """Value of the operand for read-type instructions."""
        if ins.mode == isa.M_NN:
            return ins.b[1]
        if ins.mode in (isa.M_NN_ZZ, isa.M_NN_ZZX, isa.M_NN_HHLL, isa.M_NN_HHLLX):
            return ins.b[1]          # the immediate byte of TST
        return self.space.read(self.ea(ins))

    def store(self, ins, val):
        self.space.write(self.ea(ins), val & 0xFF)

    # -- cycle charging ------------------------------------------------------
    def charge(self, n):
        """Charge n ADDCYC cycles at the current speed.

        The reference core's conversion is `int master = (x * 3) << shift`
        (huc6280.h:227) with `shift = (speed ^ 1) << 1` (huc6280.h:209), so a charged
        cycle is 3 master clocks at 7.16 MHz and 12 at 1.79 MHz.  `self.cycles` counts
        master/3, i.e. `n << shift`.  Every direct addition to `self.cycles` in this
        file goes through here -- the instruction's own cost, the T-flag's three extra
        cycles, decimal mode's extra cycle, and the VDC's port steals -- because each
        of them is an ADDCYC in the core and all of them scale together.
        """
        self.cycles += n << self.speed_shift

    # -- T-flag prefixes -----------------------------------------------------
    #
    # THE T FLAG COSTS THREE EXTRA CYCLES, and they are charged here so that this
    # reference and the two C engines agree: `TPREFIX`/`TPOSTFIX` in the reference
    # core are `ADDCYC(1); RdMem(...)` and `WrMem(...); LastCycle()` (2 more), and
    # the published vectors show +3 on every T-set instruction (e.g. ORA $zp, T=1:
    # 7 cycles against 4).  runtime.c charges them in its own tprefix/tpostfix.
    def tprefix(self):
        if self.p & FLAG_T:
            self._abackup = self.a
            self.charge(1)
            self.a = self.space.read(ZP_BASE | self.x)

    def tpostfix(self):
        if self.p & FLAG_T:
            self.space.write(ZP_BASE | self.x, self.a)
            self.charge(2)
            self.a = self._abackup

    # -- execution -----------------------------------------------------------
    def step(self):
        if self.pc >= 0xE000:
            self.space.cd.call(self, self.pc)
            self.steps += 1
            return True
        ins = isa.decode(self.space.read, self.pc)
        if ins is None:
            raise Unsupported("no decode at $%04X" % self.pc)
        fn = HANDLERS.get(ins.mnem)
        if fn is None:
            raise Unsupported("%s at $%04X (opcode $%02X)" % (ins.mnem, ins.addr, ins.op))
        self.next_pc = (ins.addr + ins.size) & 0xFFFF
        self._taken = False
        self._self_cycled = False
        steals_before = self.space.io_steals
        fn(self, ins)
        # Cycles, from the SAME table the translator emits and the C interpreter
        # reads (`cycles.py`).  Counted here so the three-way differential can
        # compare totals, not just registers: a cost difference on one opcode is
        # invisible in A/X/Y and fatal to frame timing.
        #
        # CSL/CSH are the exception: their three cycles straddle the speed change, so
        # the handlers charge them in two parts and set `_self_cycled` (runtime.c and
        # recompiler.py do the same thing with two xl_tick() calls).
        rule = cycles.cycle_rule(ins)
        if self._self_cycled:
            pass
        elif rule[0] in (cycles.RULE_BRANCH, cycles.RULE_BITBRANCH):
            self.charge(rule[1] if self._taken else rule[2])
        elif rule[0] == cycles.RULE_BLOCK:
            self.charge(rule[1] + rule[2] * (ins.block[2] or 65536))
        else:
            self.charge(rule[1])
        # The VDC stalls the CPU one cycle per access (the reference core's
        # `HuCPU.StealCycle()` in IOWrite, case 0x0000).  Counted from the same
        # choke point the two C engines use, so the cycle totals of the three
        # implementations stay comparable -- a steal that only one of them charges
        # is a cycle-total difference, and this test is what would catch it.
        self.charge(self.space.io_steals - steals_before)
        self.pc = self.next_pc & 0xFFFF
        if ins.mnem not in ("rti", "plp", "set"):
            self.p &= ~FLAG_T
        self.steps += 1
        return True

    def run(self, limit):
        for _ in range(limit):
            self.step()
        return self

    def state(self):
        return {"a": self.a, "x": self.x, "y": self.y, "s": self.s, "p": self.p,
                "pc": self.pc, "mpr": list(self.space.mpr)}


# ---------------------------------------------------------------- handlers --
def h_ora(c, ins):
    c.tprefix()
    c.a |= c.operand(ins)
    c.nz(c.a)
    c.tpostfix()


def h_and(c, ins):
    c.tprefix()
    c.a &= c.operand(ins)
    c.nz(c.a)
    c.tpostfix()


def h_eor(c, ins):
    c.tprefix()
    c.a ^= c.operand(ins)
    c.nz(c.a)
    c.tpostfix()


def h_adc(c, ins):
    c.tprefix()
    v = c.operand(ins)
    if c.p & FLAG_D:
        lo = (c.a & 0x0F) + (v & 0x0F) + (c.p & 1)
        if lo >= 0x0A:
            lo += 0x06
        tot = lo + (c.a & 0xF0) + (v & 0xF0)
        if tot >= 0xA0:
            tot += 0x60
        carry = 1 if (tot & 0xFF00) else 0
        c.a = tot & 0xFF
        c.p &= ~(FLAG_Z | FLAG_N | FLAG_C)
        c.p |= FLAG_C if carry else 0
        c.nz(c.a)
        c.charge(1)          # decimal mode is one cycle longer (LastCycle in the ref)
    else:
        tot = c.a + v + (c.p & 1)
        vflag = 1 if (~(c.a ^ v) & (c.a ^ tot) & 0x80) else 0
        carry = 1 if tot > 0xFF else 0
        c.a = tot & 0xFF
        c.p &= ~(FLAG_Z | FLAG_N | FLAG_C | FLAG_V)
        c.p |= (FLAG_Z if c.a == 0 else (FLAG_N if c.a & 0x80 else 0))
        c.p |= FLAG_C if carry else 0
        c.p |= FLAG_V if vflag else 0
    c.tpostfix()


def h_sbc(c, ins):
    v = c.operand(ins)
    if c.p & FLAG_D:
        m = (c.a & 0x0F) - (v & 0x0F) - ((c.p & 1) ^ 1)
        n = (c.a >> 4) - (v >> 4) - ((m >> 4) & 1)
        res = ((n << 4) | (m & 0x0F)) & 0xFF
        if m & 0x10:
            res = (res - 0x06) & 0xFF
        if n & 0x10:
            res = (res - 0x60) & 0xFF
        c.a = res
        c.p &= ~(FLAG_Z | FLAG_N | FLAG_C)
        c.p |= FLAG_C if not ((n >> 4) & 1) else 0
        c.nz(c.a)
        c.charge(1)          # decimal mode is one cycle longer (LastCycle in the ref)
    else:
        diff = c.a - v - ((c.p & 1) ^ 1)
        vflag = 1 if ((c.a ^ diff) & (c.a ^ v) & 0x80) else 0
        c.a = diff & 0xFF
        c.p &= ~(FLAG_Z | FLAG_N | FLAG_C | FLAG_V)
        c.p |= (FLAG_Z if c.a == 0 else (FLAG_N if c.a & 0x80 else 0))
        c.p |= 0 if diff < 0 else FLAG_C
        c.p |= FLAG_V if vflag else 0


def _cmp(c, reg, v):
    t = (reg - v) & 0x1FF
    c.nz((reg - v) & 0xFF)
    c.setf(FLAG_C, reg >= v)


def h_cmp(c, ins):
    _cmp(c, c.a, c.operand(ins))


def h_cpx(c, ins):
    _cmp(c, c.x, c.operand(ins))


def h_cpy(c, ins):
    _cmp(c, c.y, c.operand(ins))


def h_bit(c, ins):
    v = c.operand(ins)
    c.p &= ~(FLAG_Z | FLAG_V | FLAG_N)
    if (v & c.a) == 0:
        c.p |= FLAG_Z
    c.p |= v & (FLAG_N | FLAG_V)


def h_lda(c, ins):
    c.a = c.operand(ins)
    c.nz(c.a)


def h_ldx(c, ins):
    c.x = c.operand(ins)
    c.nz(c.x)


def h_ldy(c, ins):
    c.y = c.operand(ins)
    c.nz(c.y)


def h_sta(c, ins):
    c.store(ins, c.a)


def h_stx(c, ins):
    c.store(ins, c.x)


def h_sty(c, ins):
    c.store(ins, c.y)


def h_stz(c, ins):
    c.store(ins, 0)


def _rmw(c, ins, fn):
    a = c.ea(ins)
    c.space.write(a, fn(c, c.space.read(a)))


def h_asl(c, ins):
    def op(cpu, v):
        cpu.setf(FLAG_C, v & 0x80)
        v = (v << 1) & 0xFF
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_lsr(c, ins):
    def op(cpu, v):
        cpu.setf(FLAG_C, v & 1)
        v >>= 1
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_rol(c, ins):
    def op(cpu, v):
        carry_in = 1 if cpu.p & FLAG_C else 0
        cpu.setf(FLAG_C, v & 0x80)
        v = ((v << 1) & 0xFF) | carry_in
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_ror(c, ins):
    def op(cpu, v):
        carry_in = 1 if cpu.p & FLAG_C else 0
        cpu.setf(FLAG_C, v & 1)
        v = (v >> 1) | (0x80 if carry_in else 0)
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_inc(c, ins):
    def op(cpu, v):
        v = (v + 1) & 0xFF
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_dec(c, ins):
    def op(cpu, v):
        v = (v - 1) & 0xFF
        cpu.nz(v)
        return v
    if ins.mode == isa.M_OP_A:
        c.a = op(c, c.a)
    else:
        _rmw(c, ins, op)


def h_tsb(c, ins):
    a = c.ea(ins)
    v = c.space.read(a)
    c.p &= ~(FLAG_Z | FLAG_V | FLAG_N)
    # Z is the BIT-style test (A & M) for BOTH TSB and TRB -- not the memory
    # operation.  Bruce Clark, "65C02 Opcodes"; WDC W65C02S datasheet.  N and V
    # come from the memory operand, as the reference emulator computes them.
    if (v & c.a) == 0:
        c.p |= FLAG_Z
    c.p |= v & (FLAG_N | FLAG_V)
    c.space.write(a, v | c.a)


def h_trb(c, ins):
    a = c.ea(ins)
    v = c.space.read(a)
    c.p &= ~(FLAG_Z | FLAG_V | FLAG_N)
    if (v & c.a) == 0:
        c.p |= FLAG_Z
    c.p |= v & (FLAG_N | FLAG_V)
    c.space.write(a, v & ~c.a)


def h_tst(c, ins):
    v = c.space.read(c.ea(ins))
    imm = ins.b[1]
    c.p &= ~(FLAG_Z | FLAG_V | FLAG_N)
    if (v & imm) == 0:
        c.p |= FLAG_Z
    c.p |= v & (FLAG_N | FLAG_V)


def h_rmb(c, ins):
    bit = int(ins.mnem[-1])
    a = c.ea(ins)
    c.space.write(a, c.space.read(a) & ~(1 << bit))


def h_smb(c, ins):
    bit = int(ins.mnem[-1])
    a = c.ea(ins)
    c.space.write(a, c.space.read(a) | (1 << bit))


def h_bbr(c, ins):
    bit = int(ins.mnem[-1])
    if not (c.space.read(ZP_BASE | ins.b[1]) & (1 << bit)):
        c._taken = True
        c.next_pc = ins.rel_target


def h_bbs(c, ins):
    bit = int(ins.mnem[-1])
    if c.space.read(ZP_BASE | ins.b[1]) & (1 << bit):
        c._taken = True
        c.next_pc = ins.rel_target


def _branch(flag, want):
    def h(c, ins):
        if bool(c.p & flag) == want:
            c._taken = True
            c.next_pc = ins.rel_target
    return h


def h_bra(c, ins):
    c._taken = True
    c.next_pc = ins.rel_target


def h_bsr(c, ins):
    # pushes addr+1 (the last byte); RTS adds 1, landing on the next instruction
    ret = (ins.addr + 1) & 0xFFFF
    c.push(ret >> 8)
    c.push(ret & 0xFF)
    c.next_pc = ins.rel_target


def h_jsr(c, ins):
    ret = (ins.addr + 2) & 0xFFFF      # last byte, as the hardware pushes
    c.push(ret >> 8)
    c.push(ret & 0xFF)
    c.next_pc = ins.abs16


def h_rts(c, ins):
    lo = c.pop()
    hi = c.pop()
    c.next_pc = ((lo | (hi << 8)) + 1) & 0xFFFF


def h_rti(c, ins):
    c.p = c.pop()
    lo = c.pop()
    hi = c.pop()
    c.next_pc = lo | (hi << 8)


def h_jmp(c, ins):
    if ins.mode in (isa.M_HHLL, isa.M_BRANCH_OR_LABEL):
        c.next_pc = ins.abs16
    elif ins.mode == isa.M_IHHLL:
        a = ins.abs16
        c.next_pc = c.space.read(a) | (c.space.read((a + 1) & 0xFFFF) << 8)
    elif ins.mode == isa.M_IHHLLX:
        a = (ins.abs16 + c.x) & 0xFFFF
        c.next_pc = c.space.read(a) | (c.space.read((a + 1) & 0xFFFF) << 8)
    else:
        raise Unsupported("jmp mode")


def h_brk(c, ins):
    raise Unsupported("brk (no interrupt model)")


def h_pha(c, ins):
    c.push(c.a)


def h_php(c, ins):
    # PHP clears T before pushing (so the stacked byte never has T set)
    c.push((c.p & ~FLAG_T) | FLAG_B)


def h_phx(c, ins):
    c.push(c.x)


def h_phy(c, ins):
    c.push(c.y)


def h_pla(c, ins):
    c.a = c.pop()
    c.nz(c.a)


def h_plp(c, ins):
    c.p = c.pop()


def h_plx(c, ins):
    c.x = c.pop()
    c.nz(c.x)


def h_ply(c, ins):
    c.y = c.pop()
    c.nz(c.y)


def h_tax(c, ins):
    c.x = c.a
    c.nz(c.x)


def h_txa(c, ins):
    c.a = c.x
    c.nz(c.a)


def h_tay(c, ins):
    c.y = c.a
    c.nz(c.y)


def h_tya(c, ins):
    c.a = c.y
    c.nz(c.a)


def h_tsx(c, ins):
    c.x = c.s
    c.nz(c.x)


def h_txs(c, ins):
    c.s = c.x


def h_inx(c, ins):
    c.x = (c.x + 1) & 0xFF
    c.nz(c.x)


def h_iny(c, ins):
    c.y = (c.y + 1) & 0xFF
    c.nz(c.y)


def h_dex(c, ins):
    c.x = (c.x - 1) & 0xFF
    c.nz(c.x)


def h_dey(c, ins):
    c.y = (c.y - 1) & 0xFF
    c.nz(c.y)


def _flag_handler(flag, on):
    def h(c, ins):
        c.setf(flag, on)
    return h


def h_cla(c, ins):
    c.a = 0


def h_clx(c, ins):
    c.x = 0


def h_cly(c, ins):
    c.y = 0


def h_sax(c, ins):
    c.a, c.x = c.x, c.a


def h_say(c, ins):
    c.a, c.y = c.y, c.a


def h_sxy(c, ins):
    c.x, c.y = c.y, c.x


def h_tam(c, ins):
    c.space.tam(ins.b[1], c.a)


def h_tma(c, ins):
    c.a = c.space.tma(ins.b[1])


def _st(c, ins, off):
    # ST0/ST1/ST2 write the *immediate* operand byte to a VDC port; A is not read
    # and not changed.  This used to write c.a, which the generated C never did --
    # the forced-interpreter frame test found it (VRAM uploads went to the wrong
    # address because port 0 held A instead of the immediate).
    c.space.write_phys(IO_BANK, off, ins.b[1])


def h_st0(c, ins):
    _st(c, ins, 0x0000)


def h_st1(c, ins):
    _st(c, ins, 0x0002)


def h_st2(c, ins):
    _st(c, ins, 0x0003)


def h_set(c, ins):
    c.p |= FLAG_T


def h_nop(c, ins):
    pass


def h_csl(c, ins):
    # huc6280.cpp:247  CSL = { ADDCYC(2); speed = 0; REDOSPEEDCACHE(); LastCycle(); }
    # The two body cycles are charged at the OLD speed, the last-cycle one at the
    # NEW speed -- the reference's own comment says the change "take[s] effect for
    # the last CPU cycle of CSL/CSH", and warns about the order.
    c.charge(2)
    c.speed = 0
    c.speed_shift = 2
    c.charge(1)
    c._self_cycled = True


def h_csh(c, ins):
    # huc6280.cpp:248  CSH = { ADDCYC(2); speed = 1; REDOSPEEDCACHE(); LastCycle(); }
    c.charge(2)
    c.speed = 1
    c.speed_shift = 0
    c.charge(1)
    c._self_cycled = True


def _bmt(c, ins, kind):
    """Block move.  A/X/Y are saved on the stack and restored, as on hardware."""
    src, dst, n = ins.block
    sp = c.space
    c.push(c.y)
    c.push(c.a)
    c.push(c.x)
    # do-while: the body runs `len` times; a zero length wraps to 65536.
    for i in range(n if n else 65536):
        if kind == "tii":
            sp.write(dst, sp.read(src))
            src = (src + 1) & 0xFFFF
            dst = (dst + 1) & 0xFFFF
        elif kind == "tdd":
            sp.write(dst, sp.read(src))
            src = (src - 1) & 0xFFFF
            dst = (dst - 1) & 0xFFFF
        elif kind == "tin":
            sp.write(dst, sp.read(src))
            src = (src + 1) & 0xFFFF
        elif kind == "tia":
            # destination alternates between dest and dest+1; source walks forward
            sp.write((dst + (i & 1)) & 0xFFFF, sp.read(src))
            src = (src + 1) & 0xFFFF
        elif kind == "tai":
            # destination walks forward; source alternates
            sp.write(dst, sp.read((src + (i & 1)) & 0xFFFF))
            dst = (dst + 1) & 0xFFFF
    c.x = c.pop()
    c.a = c.pop()
    c.y = c.pop()


HANDLERS = {
    "ora": h_ora, "and": h_and, "eor": h_eor, "adc": h_adc, "sbc": h_sbc,
    "cmp": h_cmp, "cpx": h_cpx, "cpy": h_cpy, "bit": h_bit,
    "lda": h_lda, "ldx": h_ldx, "ldy": h_ldy,
    "sta": h_sta, "stx": h_stx, "sty": h_sty, "stz": h_stz,
    "asl": h_asl, "lsr": h_lsr, "rol": h_rol, "ror": h_ror,
    "inc": h_inc, "dec": h_dec, "tsb": h_tsb, "trb": h_trb, "tst": h_tst,
    "rmb0": h_rmb, "rmb1": h_rmb, "rmb2": h_rmb, "rmb3": h_rmb,
    "rmb4": h_rmb, "rmb5": h_rmb, "rmb6": h_rmb, "rmb7": h_rmb,
    "smb0": h_smb, "smb1": h_smb, "smb2": h_smb, "smb3": h_smb,
    "smb4": h_smb, "smb5": h_smb, "smb6": h_smb, "smb7": h_smb,
    "bbr0": h_bbr, "bbr1": h_bbr, "bbr2": h_bbr, "bbr3": h_bbr,
    "bbr4": h_bbr, "bbr5": h_bbr, "bbr6": h_bbr, "bbr7": h_bbr,
    "bbs0": h_bbs, "bbs1": h_bbs, "bbs2": h_bbs, "bbs3": h_bbs,
    "bbs4": h_bbs, "bbs5": h_bbs, "bbs6": h_bbs, "bbs7": h_bbs,
    "bpl": _branch(FLAG_N, False), "bmi": _branch(FLAG_N, True),
    "bvc": _branch(FLAG_V, False), "bvs": _branch(FLAG_V, True),
    "bcc": _branch(FLAG_C, False), "bcs": _branch(FLAG_C, True),
    "bne": _branch(FLAG_Z, False), "beq": _branch(FLAG_Z, True),
    "bra": h_bra, "bsr": h_bsr, "jsr": h_jsr, "rts": h_rts, "rti": h_rti,
    "jmp": h_jmp, "brk": h_brk,
    "pha": h_pha, "php": h_php, "phx": h_phx, "phy": h_phy,
    "pla": h_pla, "plp": h_plp, "plx": h_plx, "ply": h_ply,
    "tax": h_tax, "txa": h_txa, "tay": h_tay, "tya": h_tya,
    "tsx": h_tsx, "txs": h_txs,
    "inx": h_inx, "iny": h_iny, "dex": h_dex, "dey": h_dey,
    "cla": h_cla, "clx": h_clx, "cly": h_cly,
    "sax": h_sax, "say": h_say, "sxy": h_sxy,
    "tam": h_tam, "tma": h_tma,
    "st0": h_st0, "st1": h_st1, "st2": h_st2,
    "set": h_set, "nop": h_nop, "csl": h_csl, "csh": h_csh,
    "clc": _flag_handler(FLAG_C, False), "sec": _flag_handler(FLAG_C, True),
    "cli": _flag_handler(FLAG_I, False), "sei": _flag_handler(FLAG_I, True),
    "clv": _flag_handler(FLAG_V, False),
    "cld": _flag_handler(FLAG_D, False), "sed": _flag_handler(FLAG_D, True),
}


def _mk_bmt(kind):
    return lambda c, ins: _bmt(c, ins, kind)


HANDLERS.update({"tii": _mk_bmt("tii"), "tdd": _mk_bmt("tdd"),
                 "tin": _mk_bmt("tin"), "tia": _mk_bmt("tia"),
                 "tai": _mk_bmt("tai")})
