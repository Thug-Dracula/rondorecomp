#!/usr/bin/env python3
"""A very small HuC6280 assembler for hand-written test programs.

Only the encodings the tests need, resolved from the operand syntax (so a typo
becomes an error rather than a silently wrong opcode).  Every generated program is
disassembled back with isa.py by its caller, so a bad encoding shows up as a
mismatch between what the source says and what the bytes decode to.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.dirname(HERE)
sys.path.insert(0, XL)

ENC = {
    ("lda", "imm"): 0xA9, ("lda", "zp"): 0xA5, ("lda", "abs"): 0xAD,
    ("lda", "izy"): 0xB1, ("lda", "absx"): 0xBD, ("lda", "zpx"): 0xB5,
    ("sta", "zp"): 0x85, ("sta", "abs"): 0x8D, ("sta", "absx"): 0x9D,
    ("ldx", "imm"): 0xA2, ("ldx", "abs"): 0xAE, ("ldy", "imm"): 0xA0,
    ("adc", "imm"): 0x69, ("sbc", "imm"): 0xE9, ("and", "imm"): 0x29,
    ("ora", "imm"): 0x09, ("eor", "imm"): 0x49, ("cmp", "imm"): 0xC9,
    ("cpx", "imm"): 0xE0, ("cpy", "imm"): 0xC0,
    ("clc", None): 0x18, ("sec", None): 0x38, ("cld", None): 0xD8,
    ("sed", None): 0xF8, ("cli", None): 0x58, ("sei", None): 0x78,
    ("clv", None): 0xB8, ("set", None): 0xF4,
    ("dec", "a"): 0x3A, ("dec", "zp"): 0xC6, ("dec", "abs"): 0xCE,
    ("inc", "zp"): 0xE6, ("inc", "abs"): 0xEE, ("inc", "a"): 0x1A,
    ("dex", None): 0xCA, ("inx", None): 0xE8, ("dey", None): 0x88, ("iny", None): 0xC8,
    ("asl", "a"): 0x0A, ("lsr", "a"): 0x4A, ("rol", "a"): 0x2A, ("ror", "a"): 0x6A,
    ("pha", None): 0x48, ("pla", None): 0x68, ("php", None): 0x08,
    ("phx", None): 0xDA, ("phy", None): 0x5A, ("plx", None): 0xFA, ("ply", None): 0x7A,
    ("stx", "abs"): 0x8E, ("sty", "abs"): 0x8C, ("stz", "abs"): 0x9C,
    ("tax", None): 0xAA, ("tay", None): 0xA8, ("txa", None): 0x8A, ("tya", None): 0x98,
    ("tsx", None): 0xBA, ("txs", None): 0x9A,
    ("jsr", "abs"): 0x20, ("rts", None): 0x60, ("rti", None): 0x40,
    ("jmp", "abs"): 0x4C, ("jmp", "ind"): 0x6C, ("nop", None): 0xEA,
    ("bne", "rel"): 0xD0, ("beq", "rel"): 0xF0, ("bra", "rel"): 0x80,
    ("bcc", "rel"): 0x90, ("bcs", "rel"): 0xB0, ("bpl", "rel"): 0x10,
    ("bmi", "rel"): 0x30, ("bvc", "rel"): 0x50, ("bvs", "rel"): 0x70,
    ("bbs0", "zp_rel"): 0x8F, ("bbs7", "zp_rel"): 0xFF,
    ("bbr0", "zp_rel"): 0x0F, ("bbr7", "zp_rel"): 0x7F,
    ("tam", "imm"): 0x53, ("tma", "imm"): 0x43,
    ("st0", "imm"): 0x03, ("st1", "imm"): 0x13, ("st2", "imm"): 0x23,
    ("tii", "block"): 0x73, ("tdd", "block"): 0xC3, ("tin", "block"): 0xD3,
    ("tia", "block"): 0xE3, ("tai", "block"): 0xF3,
    ("tst", "imm_zp"): 0x83, ("smb3", "zp"): 0xB7, ("rmb0", "zp"): 0x07,
}

SIZES = {None: 1, "a": 1, "imm": 2, "zp": 2, "zpx": 2, "izy": 2, "rel": 2,
         "lo_label": 2, "hi_label": 2, "abs": 3, "absx": 3, "ind": 3,
         "imm_zp": 3, "zp_rel": 3, "block": 7}

BRANCHES = {"bne", "beq", "bra", "bcc", "bcs", "bpl", "bmi", "bvc", "bvs"}


class AsmError(Exception):
    pass


def infer_mode(mnem, ops):
    t = ops.strip()
    if t.startswith("#<"):
        return "lo_label", t[2:]
    if t.startswith("#>"):
        return "hi_label", t[2:]
    if t.startswith("#"):
        return "imm", int(t[1:].lstrip("$"), 16)
    if t == "a":
        return "a", None
    if t.startswith("(") and t.endswith("),y"):
        return "izy", int(t[2:-3].lstrip("$"), 16)
    if t.startswith("(") and t.endswith(")"):
        return "ind", int(t[2:-1].lstrip("$"), 16)
    if t.count(",") == 2:
        return "block", [int(x.strip().lstrip("$"), 16) for x in t.split(",")]
    if t.startswith("$"):
        digits = t[1:]
        return ("zp" if len(digits) <= 2 else "abs"), int(digits, 16)
    return ("rel" if mnem in BRANCHES else "abs"), t


def assemble(src, base):
    labels = {}
    pc = base
    prog = []
    for line in src:
        if isinstance(line, str):
            if not line.endswith(":"):
                raise AsmError("bare string line %r" % line)
            labels[line[:-1]] = pc
            continue
        mnem, ops = line
        if mnem == "tst":
            mask, addr = [x.strip() for x in ops.split(",")]
            prog.append((pc, mnem, "imm_zp", int(mask.lstrip("#$"), 16),
                         int(addr.lstrip("$"), 16)))
            pc += 3
            continue
        if mnem in ("bbs0", "bbs7", "bbr0", "bbr7"):
            zp, lab = [x.strip() for x in ops.split(",")]
            prog.append((pc, mnem, "zp_rel", int(zp.lstrip("$"), 16), lab))
            pc += 3
            continue
        mode, val = infer_mode(mnem, ops) if ops is not None else (None, None)
        key_mode = "imm" if mode in ("lo_label", "hi_label") else mode
        if (mnem, key_mode) not in ENC:
            raise AsmError("no encoding for %s with mode %s (operand %r)"
                           % (mnem, mode, ops))
        prog.append((pc, mnem, mode, val, None))
        pc += SIZES[mode]

    out = bytearray()
    for (addr, mnem, mode, val, extra) in prog:
        if mode == "rel":
            target = labels[val] if isinstance(val, str) else val
            rel = target - (addr + 2)
            if not -128 <= rel <= 127:
                raise AsmError("%s: branch to %s out of range" % (mnem, val))
            out += bytes([ENC[(mnem, "rel")], rel & 0xFF])
        elif mode == "zp_rel":
            rel = labels[extra] - (addr + 3)
            if not -128 <= rel <= 127:
                raise AsmError("%s: branch out of range" % mnem)
            out += bytes([ENC[(mnem, "zp_rel")], val, rel & 0xFF])
        elif mode == "imm_zp":
            out += bytes([ENC[(mnem, "imm_zp")], val, extra])
        elif mode == "lo_label":
            out += bytes([ENC[(mnem, "imm")], labels[val] & 0xFF])
        elif mode == "hi_label":
            out += bytes([ENC[(mnem, "imm")], (labels[val] >> 8) & 0xFF])
        elif mode == "block":
            s, d, n = val
            out += bytes([ENC[(mnem, "block")], s & 0xFF, s >> 8, d & 0xFF, d >> 8,
                          n & 0xFF, n >> 8])
        elif mode == "abs" or mode == "absx":
            target = labels[val] if isinstance(val, str) else val
            out += bytes([ENC[(mnem, mode)], target & 0xFF, (target >> 8) & 0xFF])
        elif mode == "ind":
            out += bytes([ENC[(mnem, mode)], val & 0xFF, (val >> 8) & 0xFF])
        elif mode == "imm" or mode == "zp" or mode == "zpx" or mode == "izy":
            out += bytes([ENC[(mnem, mode)], val & 0xFF])
        elif mode is None or mode == "a":
            out.append(ENC[(mnem, mode)])
        else:
            raise AsmError("cannot emit %s %s" % (mnem, mode))
    return bytes(out), labels
