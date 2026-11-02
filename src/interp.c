/* interp.c -- a HuC6280 interpreter that executes the bytes actually in memory.
 *
 * Why this exists: a translated unit is keyed by (physical bank, offset) and is
 * built from the bytes the bank held *at build time*.  Rondo streams code over its
 * banks, so a unit can be entered when the bytes underneath it are no longer the
 * bytes it was built from, and the machine then runs a translation of something that
 * is not there.  The fix is a guard per bank plus a real interpreter to hand over to;
 * this file is the interpreter, and the guard lives in runtime.c (docs/ARCHITECTURE.md
 * section 6).
 *
 * It is a *port of the existing machine*, not a second one:
 *
 *   - the same `Cpu` struct, the same `xl_rd` / `xl_wr` memory map, so every store
 *     still passes the write barrier the guard will use, and every device still
 *     sees the same accesses in the same order;
 *   - the same flag/ALU helpers as the generated code (`xl_lda`, `xl_adc`,
 *     `xl_tick`, ...), so a fix in runtime.c reaches both paths;
 *   - the same opcode widths and addressing modes (`isa.py`, through
 *     out/interp_tables.h), so the PC advances identically;
 *   - the same cycle costs (`cycles.py`, the translator's own table, baked into
 *     the same generated header).  There is no second cost table here: this file
 *     contains no cycle numbers at all.
 *
 * Ordering inside a step deliberately mirrors one generated unit line for line --
 * body, one `xl_tick`, clear T, then set PC -- because `xl_tick` advances the VDC
 * and can itself write memory at a frame boundary.  Ticking in a different place
 * would be a real behaviour difference, not a stylistic one.
 *
 * What it is NOT: a second machine.  Three things route to it -- `XLATE_INTERP=1`
 * (every step, which is how the identity tests are run), the overlay guard (one bank
 * at a time, while that bank's live bytes disagree with its compiled image), and,
 * under `XLATE_BIOS_EXEC=1`, any pc whose page holds the user's System Card
 * (the card is executed, never translated).
 *
 * Two places where it is deliberately *more* faithful than the translated path,
 * both measured, neither reachable on a translated run that does not already stop:
 *
 *   - `xl_ram_trampoline` (a block-move routine the game builds in work RAM) is
 *     modelled by the dispatcher as one step costing no cycles at all; here those
 *     bytes are executed and cost what the table says.  Over the boot that is
 *     +1,308 cycles, with identical frames, interrupts and memory.
 *   - an instruction that runs past the end of a bank: the translator refuses to
 *     translate it (a conservative choice) and the machine then stops, whereas
 *     here the fetch simply continues into the next page, as the hardware does.
 *
 * Decode is from live memory through the live bank registers, which is the point:
 * if a bank has been overwritten, this runs what is there now, not what was there
 * when the unit was compiled.
 */
#include "runtime.h"
#include "interp_tables.h"

#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------- addressing -- */

/* Effective address of a memory operand.  Same expressions as recompiler.ea_expr()
 * and refcpu.RefCpu.ea(): zero page is $2000 (xl_zp*), and a zero-page pointer's
 * high byte wraps inside the page (xl_rd16_zp). */
static uint16_t ea_of(Cpu *c, uint8_t mode, const uint8_t *b)
{
    switch (mode) {
    case XMODE_ZZ:
        return xl_zp(b[1]);
    case XMODE_ZZX:
        return xl_zp_x(c, b[1]);
    case XMODE_ZZY:
        return xl_zp_y(c, b[1]);
    case XMODE_HHLL:
    case XMODE_BRANCH:
        return (uint16_t)(b[1] | (b[2] << 8));
    case XMODE_HHLLX:
        return (uint16_t)((uint16_t)(b[1] | (b[2] << 8)) + c->x);
    case XMODE_HHLLY:
        return (uint16_t)((uint16_t)(b[1] | (b[2] << 8)) + c->y);
    case XMODE_IZZ:
        return xl_rd16_zp(c, b[1]);
    case XMODE_IZZX:
        return xl_rd16_zp(c, (uint8_t)(b[1] + c->x));
    case XMODE_IZZY:
        return (uint16_t)(xl_rd16_zp(c, b[1]) + c->y);
    case XMODE_IHHLL:
        return xl_rd16(c, (uint16_t)(b[1] | (b[2] << 8)));
    case XMODE_IHHLLX:
        return xl_rd16(c, (uint16_t)((uint16_t)(b[1] | (b[2] << 8)) + c->x));
    /* TST's immediate-plus-address forms carry the address one byte later. */
    case XMODE_NN_ZZ:
        return xl_zp(b[2]);
    case XMODE_NN_ZZX:
        return xl_zp_x(c, b[2]);
    case XMODE_NN_HHLL:
        return (uint16_t)(b[2] | (b[3] << 8));
    case XMODE_NN_HHLLX:
        return (uint16_t)((uint16_t)(b[2] | (b[3] << 8)) + c->x);
    default:
        break;
    }
    return 0;
}

/* Operand of a read-type instruction: the immediate byte, or the byte at the
 * effective address.  Mirrors recompiler.value_expr(). */
static uint8_t operand(Cpu *c, const XOp *x, const uint8_t *b)
{
    if (x->mode == XMODE_NN)
        return b[1];
    return xl_rd(c, ea_of(c, x->mode, b));
}

/* Relative-branch target: signed byte counts from the end of the instruction. */
static uint16_t rel_target(uint16_t pc, unsigned size, uint8_t rel)
{
    int16_t off = (int16_t)(int8_t)rel;
    return (uint16_t)(pc + (int)size + (int)off);
}

/* The width of an opcode, from the same table the translator decodes with.
 * The trace needs it for one reason only (see trace.c): the console records a bus
 * access with the CPU's PC *after* the instruction's operand bytes were consumed,
 * so its `pc=` is start+size and ours would be start.  Reading the width from the
 * generated table keeps that from becoming a second copy of isa.py. */
unsigned xl_op_size(uint8_t op)
{
    return xl_ops[op].size;
}

typedef uint8_t (*RmwFn)(Cpu *, uint8_t);

/* Read-modify-write: A form, or memory through the same helpers the generated
 * code calls (read, transform, write back -- including the barrier).
 *
 * The memory form does NOT write here: it hands the address and the new value
 * back to the caller, which ticks the instruction's cycles and only then stores.
 * The store's bus cycle is the instruction's last one (reference huc6280.cpp:
 * `ADDCYC(n); LastCycle(); WrMem(...)`), and a store that lands n cycles early
 * moves a mid-frame VDC write up to eight dots on the raster. */
static int rmw(Cpu *c, uint8_t mode, const uint8_t *b, RmwFn fn,
               uint16_t *ea_out, uint8_t *v_out)
{
    uint16_t ea;
    if (mode == XMODE_OP_A) {
        c->a = fn(c, c->a);
        return 0;
    }
    ea = ea_of(c, mode, b);
    *ea_out = ea;
    *v_out = fn(c, xl_rd(c, ea));
    return 1;
}

/* An opcode the machine cannot execute: neither the translator nor this
 * interpreter runs BRK or an undefined byte.  Same two chances the dispatcher
 * gives before it stops, in the same order. */
static int no_instruction(Cpu *c)
{
    uint8_t bank = c->mpr[c->pc >> 13];
    if (xl_ram_trampoline(c))
        return 1;
    return xl_untranslated(c, bank, c->pc);
}

/* ---------------------------------------------------------------- execute -- */

int xl_interp_step(Cpu *c)
{
    uint16_t pc = c->pc;
    uint16_t nxt;
    uint8_t op, i;
    uint8_t b[8] = { 0 };      /* operand bytes; every slot up to size-1 is filled */
    const XOp *x;
    int taken = 0;
    unsigned block_len = 0;
    /* A store's bus cycle is the instruction's last one: the body fills these in
     * and the store itself happens after xl_tick(), below. */
    uint16_t late_ea = 0;
    uint8_t late_v = 0;
    int late = 0;
    /* Set by the instructions that charge their own time instead of taking the
     * single end-of-step charge: ST0/ST1/ST2 (cycles before the port write) and
     * CSL/CSH (two ticks, one on each side of the speed switch). */
    int ticked = 0;

    /* $E000-$FFFF is the CD BIOS: modelled here unless XLATE_BIOS_EXEC=1 and the page
     * really holds the supplied System Card, in which case the bytes below are the
     * card's own code and the rest of this function executes them. */
    if (pc >= 0xE000 && !xl_pc_in_rom(c)) {
        xl_bios_call(c);
        return 1;
    }

    op = xl_rd(c, pc);
    x = &xl_ops[op];
    b[0] = op;
    for (i = 1; i < x->size; i++)
        b[i] = xl_rd(c, (uint16_t)(pc + i));
    nxt = (uint16_t)(pc + x->size);

    /* XLATE_BM_PROBE: the System Card's file-manager entry, with its parameter
     * block.  Diagnostic only (a handful of addresses, off by default). */
    {
        static int bm = -1;
        if (bm < 0) {
            const char *e = getenv("XLATE_BM_PROBE");
            bm = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (bm && c->log &&
            (pc == 0xF8E3 || pc == 0xF8F7 || pc == 0xF923 || pc == 0xF943 ||
             pc == 0xF94B || pc == 0xF950 || pc == 0xFB96 || pc == 0xFBFA ||
             pc == 0xFC32 || pc == 0xFC42 || pc == 0xFC49 ||
             pc == 0x8048 || pc == 0x804B || pc == 0x804E || pc == 0x804F ||
             pc == 0x8059 || pc == 0x805B ||
             pc == 0x8748 || pc == 0x874B || pc == 0x874D ||
             pc == 0x40E7 || pc == 0x40EA || pc == 0x40ED ||
             pc == 0x6C24 || pc == 0x6C40)) {
            fprintf(c->log, "bm: pc $%04X bank $%02X frame %u A=$%02X X=$%02X Y=$%02X "
                            "mpr=%02X%02X%02X%02X%02X%02X%02X%02X "
                            "par F8-FF=%02X%02X %02X%02X %02X%02X %02X%02X "
                            "$22B3=%02X%02X bram=%02X%02X%02X%02X dir=%02X%02X%02X%02X\n",
                    pc, c->mpr[pc >> 13], (unsigned)c->vdc.frame, c->a, c->x, c->y,
                    c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
                    c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7],
                    c->mem[0xF8], c->mem[0xF9], c->mem[0xFA], c->mem[0xFB],
                    c->mem[0xFC], c->mem[0xFD], c->mem[0xFE], c->mem[0xFF],
                    c->mem[0x2B3], c->mem[0x2B4],
                    c->bram[0], c->bram[1], c->bram[2], c->bram[3],
                    c->bram[0x10], c->bram[0x11], c->bram[0x14], c->bram[0x15]);
            fflush(c->log);
        }
    }

    /* ST0/ST1/ST2 do nothing but write the hardware page, and that write is the
     * instruction's last cycle, so the cycles are charged before the body runs --
     * the same order the translator emits for LATE_STORE (recompiler.py). */
    if ((x->mnem == XM_ST0 || x->mnem == XM_ST1 || x->mnem == XM_ST2) &&
        x->kind == XK_FIXED) {
        xl_tick(c, (unsigned)x->a);
        ticked = 1;
    }

    switch (x->mnem) {
    /* -- loads ------------------------------------------------------------ */
    case XM_LDA: xl_lda(c, operand(c, x, b)); break;
    case XM_LDX: xl_ldx(c, operand(c, x, b)); break;
    case XM_LDY: xl_ldy(c, operand(c, x, b)); break;

    /* -- stores ----------------------------------------------------------- */
    case XM_STA: late_ea = ea_of(c, x->mode, b); late_v = c->a; late = 1; break;
    case XM_STX: late_ea = ea_of(c, x->mode, b); late_v = c->x; late = 1; break;
    case XM_STY: late_ea = ea_of(c, x->mode, b); late_v = c->y; late = 1; break;
    case XM_STZ: late_ea = ea_of(c, x->mode, b); late_v = 0;    late = 1; break;

    /* -- ALU -------------------------------------------------------------- */
    case XM_ADC: xl_adc(c, operand(c, x, b)); break;
    case XM_SBC: xl_sbc(c, operand(c, x, b)); break;
    case XM_AND: xl_and(c, operand(c, x, b)); break;
    case XM_ORA: xl_ora(c, operand(c, x, b)); break;
    case XM_EOR: xl_eor(c, operand(c, x, b)); break;
    case XM_CMP: xl_cmp(c, operand(c, x, b)); break;
    case XM_CPX: xl_cpx(c, operand(c, x, b)); break;
    case XM_CPY: xl_cpy(c, operand(c, x, b)); break;
    case XM_BIT: xl_bit(c, operand(c, x, b)); break;
    case XM_TST: xl_tst(c, xl_rd(c, ea_of(c, x->mode, b)), b[1]); break;

    /* -- read-modify-write ------------------------------------------------ */
    case XM_ASL: late = rmw(c, x->mode, b, xl_asl, &late_ea, &late_v); break;
    case XM_LSR: late = rmw(c, x->mode, b, xl_lsr, &late_ea, &late_v); break;
    case XM_ROL: late = rmw(c, x->mode, b, xl_rol, &late_ea, &late_v); break;
    case XM_ROR: late = rmw(c, x->mode, b, xl_ror, &late_ea, &late_v); break;
    case XM_INC: late = rmw(c, x->mode, b, xl_inc, &late_ea, &late_v); break;
    case XM_DEC: late = rmw(c, x->mode, b, xl_dec, &late_ea, &late_v); break;

    /* -- test-and-set / test-and-reset ------------------------------------ */
    case XM_TSB:
    case XM_TRB: {
        uint16_t ea = ea_of(c, x->mode, b);
        uint8_t v = xl_rd(c, ea);
        c->p &= (uint8_t)~(XL_Z | XL_V | XL_N);
        /* Z is the BIT-style test (A & M) for BOTH TSB and TRB -- the rule Bruce
         * Clark's "65C02 Opcodes" and the WDC W65C02S datasheet give, and what the
         * published vectors (SingleStepTests/huc6280) assert.  N and V come from the
         * memory operand instead, which is what the reference emulator does and what
         * neither the datasheet nor the vectors do.  For TSB the two Z rules agree;
         * for TRB they do not, and that is the one flag where this port knowingly
         * differs from the reference -- see docs/REFERENCE.md. */
        if ((uint8_t)(v & c->a) == 0)
            c->p |= XL_Z;
        c->p |= (uint8_t)(v & (XL_N | XL_V));
        late_ea = ea;
        late_v = (x->mnem == XM_TSB) ? (uint8_t)(v | c->a)
                                     : (uint8_t)(v & (uint8_t)~c->a);
        late = 1;
        break;
    }

    /* -- flags ------------------------------------------------------------ */
    case XM_SEC: c->p |= XL_C; break;
    case XM_SED: c->p |= XL_D; break;
    case XM_SEI: c->p |= XL_I; break;
    case XM_CLC: c->p &= (uint8_t)~XL_C; break;
    case XM_CLD: c->p &= (uint8_t)~XL_D; break;
    case XM_CLI: c->p &= (uint8_t)~XL_I; break;
    case XM_CLV: c->p &= (uint8_t)~XL_V; break;

    /* -- register transfers ----------------------------------------------- */
    case XM_TAX: c->x = c->a; xl_nz(c, c->x); break;
    case XM_TAY: c->y = c->a; xl_nz(c, c->y); break;
    case XM_TXA: c->a = c->x; xl_nz(c, c->a); break;
    case XM_TYA: c->a = c->y; xl_nz(c, c->a); break;
    case XM_TSX: c->x = c->s; xl_nz(c, c->x); break;
    case XM_TXS: c->s = c->x; break;
    case XM_INX: c->x = (uint8_t)(c->x + 1); xl_nz(c, c->x); break;
    case XM_DEX: c->x = (uint8_t)(c->x - 1); xl_nz(c, c->x); break;
    case XM_INY: c->y = (uint8_t)(c->y + 1); xl_nz(c, c->y); break;
    case XM_DEY: c->y = (uint8_t)(c->y - 1); xl_nz(c, c->y); break;
    case XM_CLA: c->a = 0; break;
    case XM_CLX: c->x = 0; break;
    case XM_CLY: c->y = 0; break;
    case XM_SAX: { uint8_t t = c->a; c->a = c->x; c->x = t; break; }
    case XM_SAY: { uint8_t t = c->a; c->a = c->y; c->y = t; break; }
    case XM_SXY: { uint8_t t = c->x; c->x = c->y; c->y = t; break; }

    /* -- stack ------------------------------------------------------------ */
    case XM_PHA: xl_push(c, c->a); break;
    case XM_PHX: xl_push(c, c->x); break;
    case XM_PHY: xl_push(c, c->y); break;
    case XM_PHP: xl_push(c, (uint8_t)((c->p & (uint8_t)~XL_T) | XL_BF)); break;
    case XM_PLA: c->a = xl_pop(c); xl_nz(c, c->a); break;
    case XM_PLX: c->x = xl_pop(c); xl_nz(c, c->x); break;
    case XM_PLY: c->y = xl_pop(c); xl_nz(c, c->y); break;
    case XM_PLP: c->p = xl_pop(c); break;

    /* -- conditional branches (the tick happens in the arm, as emitted) ---- */
    case XM_BPL: taken = !(c->p & XL_N); break;
    case XM_BMI: taken = (c->p & XL_N) != 0; break;
    case XM_BVC: taken = !(c->p & XL_V); break;
    case XM_BVS: taken = (c->p & XL_V) != 0; break;
    case XM_BCC: taken = !(c->p & XL_C); break;
    case XM_BCS: taken = (c->p & XL_C) != 0; break;
    case XM_BNE: taken = !(c->p & XL_Z); break;
    case XM_BEQ: taken = (c->p & XL_Z) != 0; break;

    /* -- jumps, calls, returns -------------------------------------------- */
    case XM_BRA:
        c->pc = rel_target(pc, x->size, b[x->size - 1]);
        break;
    case XM_BSR:
        /* pushes the address of the last byte; RTS adds one */
        xl_push16(c, (uint16_t)(pc + 1));
        c->pc = rel_target(pc, x->size, b[x->size - 1]);
        break;
    case XM_JSR:
        xl_push16(c, (uint16_t)(pc + 2));
        c->pc = (uint16_t)(b[1] | (b[2] << 8));
        break;
    case XM_RTS:
        c->pc = (uint16_t)(xl_pop16(c) + 1);
        break;
    case XM_RTI:
        xl_rti(c);
        break;
    case XM_JMP:
        if (x->mode == XMODE_HHLL || x->mode == XMODE_BRANCH)
            c->pc = (uint16_t)(b[1] | (b[2] << 8));
        else
            c->pc = ea_of(c, x->mode, b);
        break;

    /* -- zero-page bit set/reset ------------------------------------------ */
    case XM_RMB0: case XM_RMB1: case XM_RMB2: case XM_RMB3:
    case XM_RMB4: case XM_RMB5: case XM_RMB6: case XM_RMB7: {
        uint16_t ea = xl_zp(b[1]);
        uint8_t bit = (uint8_t)(x->mnem - XM_RMB0);
        late_ea = ea;
        late_v = (uint8_t)(xl_rd(c, ea) & (uint8_t)~(1u << bit));
        late = 1;
        break;
    }
    case XM_SMB0: case XM_SMB1: case XM_SMB2: case XM_SMB3:
    case XM_SMB4: case XM_SMB5: case XM_SMB6: case XM_SMB7: {
        uint16_t ea = xl_zp(b[1]);
        uint8_t bit = (uint8_t)(x->mnem - XM_SMB0);
        late_ea = ea;
        late_v = (uint8_t)(xl_rd(c, ea) | (uint8_t)(1u << bit));
        late = 1;
        break;
    }

    /* -- zero-page bit branches ------------------------------------------- */
    case XM_BBR0: case XM_BBR1: case XM_BBR2: case XM_BBR3:
    case XM_BBR4: case XM_BBR5: case XM_BBR6: case XM_BBR7:
    case XM_BBS0: case XM_BBS1: case XM_BBS2: case XM_BBS3:
    case XM_BBS4: case XM_BBS5: case XM_BBS6: case XM_BBS7: {
        uint8_t bit = (uint8_t)((x->mnem <= XM_BBR7) ? (x->mnem - XM_BBR0)
                                                     : (x->mnem - XM_BBS0));
        unsigned want = (x->mnem >= XM_BBS0) ? 1u : 0u;
        unsigned have = ((unsigned)xl_rd(c, xl_zp(b[1])) >> bit) & 1u;
        taken = (have == want);
        break;
    }

    /* -- mapper and hardware ---------------------------------------------- */
    case XM_TAM: xl_tam(c, b[1]); break;
    case XM_TMA: xl_tma(c, b[1]); break;
    /* ST0/ST1/ST2 carry the VDC port value as an *immediate*, not in A -- the
     * translator emits `xl_st0(c, 0x%02X)` from the operand byte, and A is left
     * untouched.  (refcpu.py used to write A here; the forced-interpreter frame
     * test is what surfaced it, because the port-0 address register ended up
     * different and every VRAM upload after it landed elsewhere.) */
    case XM_ST0: xl_st0(c, b[1]); break;
    case XM_ST1: xl_st1(c, b[1]); break;
    case XM_ST2: xl_st2(c, b[1]); break;

    /* -- block moves ------------------------------------------------------- */
    case XM_TII:
    case XM_TDD:
    case XM_TIN:
    case XM_TIA:
    case XM_TAI: {
        uint16_t src = (uint16_t)(b[1] | (b[2] << 8));
        uint16_t dst = (uint16_t)(b[3] | (b[4] << 8));
        int kind = XL_TII;
        block_len = (unsigned)(b[5] | (b[6] << 8));
        if (x->mnem == XM_TDD) kind = XL_TDD;
        else if (x->mnem == XM_TIN) kind = XL_TIN;
        else if (x->mnem == XM_TIA) kind = XL_TIA;
        else if (x->mnem == XM_TAI) kind = XL_TAI;
        xl_bmt(c, kind, src, dst, (uint16_t)block_len);
        break;
    }

    /* -- no-ops ------------------------------------------------------------ */
    case XM_SET: c->p |= XL_T; break;
    /* CSL/CSH ARE NOT NO-OPS: they switch the CPU between 7.16 MHz and 1.79 MHz.
     *
     * The reference core writes them as
     *
     *     #define CSL { ADDCYC(2); speed = 0; REDOSPEEDCACHE(); LastCycle(); }
     *     #define CSH { ADDCYC(2); speed = 1; REDOSPEEDCACHE(); LastCycle(); }
     *     // "Note: CSL/CSH's speed timing changes take effect for the last CPU
     *     //  cycle of CSL/CSH.  Be cautious not to change the order here"
     *                                       -- mednafen pce/huc6280.cpp:245-248
     *
     * so the two cycles of the body are charged at the OLD speed and the one cycle
     * of LastCycle() at the NEW one.  Two ticks in that order, and `ticked` stops
     * the generic end-of-step charge from adding three more at the new speed.  The
     * instruction's ADDCYC total is still 3, which is what vectors.py and
     * cycles.py say; what changes is how much time those three cycles buy. */
    case XM_CSL: xl_tick(c, 2); xl_speed_set(c, 0); xl_tick(c, 1); ticked = 1; break;
    case XM_CSH: xl_tick(c, 2); xl_speed_set(c, 1); xl_tick(c, 1); ticked = 1; break;
    case XM_NOP: break;

    /* -- not executable ---------------------------------------------------- */
    case XM_BRK:
        /* BRK is a real instruction: it pushes and vectors (see xl_brk).  Only a
         * byte that is not an instruction at all is "no instruction". */
        return xl_brk(c);
    case XM_DB:
    default:
        return no_instruction(c);
    }

    /* ---- cycles, T flag, PC: the same three lines a generated unit ends with -- */
    if (ticked) {
        /* ST0/ST1/ST2: charged above, before their write. */
    } else if (late && x->kind == XK_FIXED) {
        /* The store is the instruction's last bus cycle: charge the cycles, then
         * write (recompiler.LATE_STORE / LATE_RMW emit the same order). */
        xl_tick(c, (unsigned)x->a);
        xl_wr(c, late_ea, late_v);
    } else if (x->kind == XK_BRANCH || x->kind == XK_BITBRANCH) {
        /* The translator emits the tick inside each arm, after the PC is set. */
        if (taken) {
            c->pc = rel_target(pc, x->size, b[x->size - 1]);
            xl_tick(c, (unsigned)x->a);
        } else {
            c->pc = nxt;
            xl_tick(c, (unsigned)x->b);
        }
    } else if (x->kind == XK_BLOCK) {
        /* base + per_byte * len, the same expression cycles_for() gives; a length of
         * zero is 65536 iterations, which is what the counter does when it wraps. */
        unsigned n = block_len ? block_len : 65536u;
        xl_tick(c, (unsigned)((unsigned)x->a + (unsigned)x->b * n));
    } else {
        xl_tick(c, (unsigned)x->a);
    }

    /* Every instruction clears T except these three; the emitter does the same. */
    if (x->mnem != XM_RTI && x->mnem != XM_PLP && x->mnem != XM_SET)
        c->p &= (uint8_t)~XL_T;

    /* PC: the body set it for jumps, calls, returns and branches. */
    switch (x->mnem) {
    case XM_BRA: case XM_BSR: case XM_JSR: case XM_RTS: case XM_RTI: case XM_JMP:
        break;
    default:
        if (x->kind != XK_BRANCH && x->kind != XK_BITBRANCH)
            c->pc = nxt;
        break;
    }
    return 1;
}
