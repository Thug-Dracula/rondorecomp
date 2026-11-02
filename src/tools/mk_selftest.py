#!/usr/bin/env python3
"""Build the hand-checked self-test program and its expected-results table.

Why hand-written bytes and hand-written expectations: the point is to assert
behaviour a human worked out from the instruction meanings, not behaviour an
interpreter reported.  If the recompiler and the reference interpreter shared a
misreading, this test still catches it.

Each block ends with the same six instructions, which copy A, P, X and Y into a
record table at $DF00 + 4*block.  PHP pushes P with B set, and T has already been
cleared by the CPU, so the recorded P byte is (P | B).

Outputs:
  out/selftest.c             generated translation unit (via recompiler.py)
  out/selftest_expect.json   the claims, for tests/test_selftest.py
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.dirname(HERE)
sys.path.insert(0, XL)

import isa                                    # noqa: E402
import log as xlog                            # noqa: E402
import recompiler                             # noqa: E402

BASE = 0x8000
RECORD = 0xDF00
FLAG_C, FLAG_Z, FLAG_I, FLAG_D, FLAG_B, FLAG_T, FLAG_V, FLAG_N = 1, 2, 4, 8, 16, 32, 64, 128

# mnemonic+mode -> opcode.  Only what this test needs; anything unknown is an
# error, never a guess.
ENC = {
    ("lda", "imm"): 0xA9, ("lda", "zp"): 0xA5, ("lda", "abs"): 0xAD, ("lda", "izy"): 0xB1,
    ("sta", "zp"): 0x85, ("sta", "abs"): 0x8D,
    ("ldx", "imm"): 0xA2, ("ldy", "imm"): 0xA0,
    ("adc", "imm"): 0x69, ("sbc", "imm"): 0xE9,
    ("clc", None): 0x18, ("sec", None): 0x38, ("cld", None): 0xD8, ("sed", None): 0xF8,
    ("dec", "a"): 0x3A, ("dec", "zp"): 0xC6, ("inc", "zp"): 0xE6,
    ("dex", None): 0xCA, ("inx", None): 0xE8, ("nop", None): 0xEA,
    ("asl", "a"): 0x0A, ("lsr", "a"): 0x4A, ("rol", "a"): 0x2A,
    ("pha", None): 0x48, ("pla", None): 0x68, ("php", None): 0x08,
    ("stx", "abs"): 0x8E, ("sty", "abs"): 0x8C,
    ("tax", None): 0xAA, ("tay", None): 0xA8,
    ("set", None): 0xF4, ("tma", "imm"): 0x43, ("and", "imm"): 0x29,
    ("jsr", "abs"): 0x20, ("rts", None): 0x60,
    ("jmp", "abs"): 0x4C, ("jmp", "ind"): 0x6C,
    ("bne", "rel"): 0xD0, ("bra", "rel"): 0x80, ("bbs0", "zp_rel"): 0x8F,
    ("tam", "imm"): 0x53, ("st1", "imm"): 0x13, ("st2", "imm"): 0x23,
    ("tii", "block"): 0x73, ("tst", "imm_zp"): 0x83,
    ("smb3", "zp"): 0xB7, ("rmb0", "zp"): 0x07,
}

SIZES = {None: 1, "a": 1, "imm": 2, "zp": 2, "izy": 2, "rel": 2, "lo_label": 2,
         "hi_label": 2, "abs": 3, "ind": 3, "imm_zp": 3, "zp_rel": 3, "block": 7}

BRANCHES = {"bne", "bra", "beq"}


class AsmError(Exception):
    pass


def infer_mode(mnem, ops):
    """Addressing mode from how the operand is written (not from a table)."""
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
    # bare identifier: a label
    return ("rel" if mnem in BRANCHES else "abs"), t


def assemble(src, base):
    """Two-pass assembler over (mnemonic, operand) tuples and 'label:' strings."""
    labels = {}
    pc = base
    prog = []                     # (addr, mnem, mode, val, extra)
    for line in src:
        if isinstance(line, str):
            if not line.endswith(":"):
                raise AsmError("bare string line %r" % line)
            labels[line[:-1]] = pc
            continue
        mnem, ops = line
        if mnem == "tst":
            mask, addr = [x.strip() for x in ops.split(",")]
            mode = "imm_zp" if len(addr.lstrip("$")) <= 2 else "imm_abs"
            prog.append((pc, mnem, mode,
                         int(mask.lstrip("#$"), 16), int(addr.lstrip("$"), 16)))
            pc += 3
            continue
        if mnem == "bbs0":
            zp, lab = [x.strip() for x in ops.split(",")]
            prog.append((pc, mnem, "zp_rel", int(zp.lstrip("$"), 16), lab))
            pc += 3
            continue
        mode, val = infer_mode(mnem, ops) if ops is not None else (None, None)
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
                raise AsmError("bbs0: branch out of range")
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
        elif mode == "abs":
            target = labels[val] if isinstance(val, str) else val
            out += bytes([ENC[(mnem, mode)], target & 0xFF, (target >> 8) & 0xFF])
        elif mode == "ind" or mode == "izy":
            out += bytes([ENC[(mnem, mode)], val & 0xFF]) if mode == "izy" else \
                   bytes([ENC[(mnem, mode)], val & 0xFF, (val >> 8) & 0xFF])
        elif mode == "imm" or mode == "zp":
            out += bytes([ENC[(mnem, mode)], val & 0xFF])
        elif mode is None or mode == "a":
            out.append(ENC[(mnem, mode)])
        else:
            raise AsmError("cannot emit %s %s" % (mnem, mode))
    return bytes(out), labels


# --------------------------------------------------------------------------
# The claims.  Keys: a/x/y exact (None = don't care), mask+flags asserted on the
# recorded P byte (B and T are excluded from every mask), mem=[(visible, byte)],
# mpr2 (after a TAM), hw=[(register offset, exact write count)].
# --------------------------------------------------------------------------
def blocks():
    b = []

    b.append(("adc carry out", [
        ("clc", None), ("lda", "#$FF"), ("adc", "#$01"),
    ], dict(a=0x00, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N | FLAG_V,
            flags=FLAG_C | FLAG_Z,
            why="$FF + $01 = $100 -> A=0, carry out, Z set, V clear")))

    b.append(("adc signed overflow", [
        ("clc", None), ("lda", "#$7F"), ("adc", "#$01"),
    ], dict(a=0x80, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N | FLAG_V,
            flags=FLAG_N | FLAG_V,
            why="+127 + 1 overflows a signed byte -> A=$80, V set, C clear")))

    b.append(("sbc borrow", [
        ("sec", None), ("lda", "#$00"), ("sbc", "#$01"),
    ], dict(a=0xFF, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N | FLAG_V,
            flags=FLAG_N,
            why="0 - 1 borrows -> A=$FF, C clear, N set, V clear")))

    b.append(("decimal adc", [
        ("sed", None), ("clc", None), ("lda", "#$19"), ("adc", "#$01"), ("cld", None),
    ], dict(a=0x20, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N, flags=0,
            why="BCD $19 + $01 = $20, not $1A; no carry out")))

    b.append(("decimal sbc", [
        ("sed", None), ("sec", None), ("lda", "#$20"), ("sbc", "#$01"), ("cld", None),
    ], dict(a=0x19, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N, flags=FLAG_C,
            why="BCD $20 - $01 = $19, carry set (no borrow)")))

    b.append(("dex twice", [
        ("ldx", "#$05"), ("dex", None), ("dex", None),
    ], dict(a=None, x=0x03, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            why="X 5 -> 3; not zero, bit 7 clear")))

    b.append(("asl a", [
        ("clc", None), ("lda", "#$C0"), ("asl", "a"),
    ], dict(a=0x80, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N,
            flags=FLAG_C | FLAG_N, why="$C0 << 1 = $80, old bit 7 into carry")))

    b.append(("lsr a", [
        ("lda", "#$81"), ("lsr", "a"),
    ], dict(a=0x40, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N,
            flags=FLAG_C, why="$81 >> 1 = $40, old bit 0 into carry, N cleared")))

    b.append(("rol a through carry", [
        ("sec", None), ("lda", "#$01"), ("rol", "a"),
    ], dict(a=0x03, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N, flags=0,
            why="$01 rotated with carry in -> $03, old bit 7 clear so C=0")))

    b.append(("zero page is at $2000", [
        ("lda", "#$5A"), ("sta", "$42"), ("lda", "#$00"), ("lda", "$42"),
    ], dict(a=0x5A, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            mem=[(0x2042, 0x5A)],
            why="sta $42 must land at $2042 and read back as $5A")))

    b.append(("stack at $2100", [
        ("lda", "#$12"), ("pha", None),
        ("lda", "#$13"), ("pha", None),
        # read the two cells back from absolute addresses, so the proof does not
        # depend on nothing else touching the stack later
        ("lda", "$21FE"), ("tax", None),
        ("lda", "$21FF"), ("tay", None),
        ("lda", "#$00"),
    ], dict(a=0x00, x=0x13, y=0x12, mask=FLAG_Z, flags=FLAG_Z,
            why="the stack grows down from $21FF: $12 is pushed first and ends at "
                "$21FF, $13 second and ends at $21FE. X reads $21FE=$13, Y reads $21FF=$12")))

    b.append(("jsr / rts", [
        ("jsr", "sub"), ("sta", "$DE01"), ("lda", "#$00"),
    ], dict(a=0x00, x=None, y=None, mask=FLAG_Z, flags=FLAG_Z,
            mem=[(0xDE01, 0x99)],
            why="subroutine loads $99 and returns to the instruction after the call")))

    b.append(("indirect jump", [
        ("lda", "#<jtarget"), ("sta", "$DE10"),
        ("lda", "#>jtarget"), ("sta", "$DE11"),
        ("lda", "#$00"), ("jmp", "($DE10)"),
        "jtarget:", ("lda", "#$77"),
    ], dict(a=0x77, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            why="jmp ($DE10) must reach the label and run lda #$77")))

    b.append(("tam pages a bank in / tma reads it back", [
        ("lda", "#$A5"), ("tam", "#$04"), ("lda", "#$11"), ("sta", "$4000"),
        ("tma", "#$04"),
    ], dict(a=0xA5, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            mem=[(0x4000, 0x11)], mpr2=0xA5,
            why="tam #$04 sets MPR2=$A5 so the store lands in bank $A5; tma #$04 "
                "reads it back into A (TMA does not touch the flags, so the Z/N "
                "left by lda #$11 stay clear)")))

    b.append(("tii block move", [
        ("tii", "$DE20,$DE30,$0003"),
    ], dict(a=None, x=None, y=None, mask=0, flags=0,
            mem=[(0xDE30, 0x11), (0xDE31, 0x22), (0xDE32, 0x33), (0xDE33, 0x00)],
            why="length 3 copies exactly 3 bytes ($11 $22 $33): the instruction is a "
                "do-while, so the operand is the count, and a count of 0 means 65536. "
                "The kernel relies on this: it copies a 219-byte jump table with "
                "length $DB and uses the byte just past the end immediately.")))

    b.append(("bbs0 / smb3 / rmb0", [
        ("lda", "#$01"), ("sta", "$60"),
        ("bbs0", "$60,bhit"),
        ("lda", "#$EE"),
        "bhit:", ("lda", "#$55"),
        ("smb3", "$61"), ("rmb0", "$60"),
    ], dict(a=0x55, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            mem=[(0x2060, 0x00), (0x2061, 0x08)],
            why="bbs0 takes the branch, smb3 sets bit 3, rmb0 clears bit 0")))

    b.append(("loader VDC loop (real bytes)", [
        ("lda", "#$10"),
        "loop:", ("st1", "#$00"), ("st2", "#$00"), ("dec", "a"), ("bne", "loop"),
    ], dict(a=0x00, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=FLAG_Z,
            hw=[(2, 16), (3, 16)],
            why="the $3008 loop: 16 decrements, 16 st1 and 16 st2 writes")))

    b.append(("(zp),y", [
        ("lda", "#$AA"), ("sta", "$DE40"),
        ("lda", "#$40"), ("sta", "$70"),
        ("lda", "#$DE"), ("sta", "$71"),
        ("ldy", "#$00"), ("lda", "($70),y"),
    ], dict(a=0xAA, x=None, y=0x00, mask=FLAG_Z | FLAG_N, flags=FLAG_N,
            why="pointer $70 points at $DE40, so lda ($70),y reads $AA (bit 7 -> N)")))

    b.append(("tst takes N/V from memory", [
        ("lda", "#$00"), ("tst", "#$80,$50"),
    ], dict(a=0x00, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N | FLAG_V,
            flags=FLAG_N | FLAG_V,
            why="$2050=$C0: bit 7 -> N, bit 6 -> V, Z clear because $C0 & $80 != 0")))

    b.append(("T flag makes AND work on $2000+X", [
        ("lda", "#$F0"), ("sta", "$30"),
        ("lda", "#$5A"), ("ldx", "#$30"),
        ("set", None), ("and", "#$0F"),
    ], dict(a=0x5A, x=0x30, y=None, mask=FLAG_Z | FLAG_N, flags=FLAG_Z,
            mem=[(0x2030, 0x00)],
            why="set arms T for one instruction; AND then takes its operand from "
                "$2000+X ($F0), ANDs $0F -> $00, stores it there, and restores A=$5A. "
                "Z is set from the $00 result, so the flags survive the restore")))

    b.append(("decimal adc with carry out", [
        ("sed", None), ("clc", None), ("lda", "#$99"), ("adc", "#$01"), ("cld", None),
    ], dict(a=0x00, x=None, y=None, mask=FLAG_C | FLAG_Z | FLAG_N, flags=FLAG_C | FLAG_Z,
            why="BCD $99 + $01 = $00 with carry out and Z set")))

    b.append(("inc/dec memory", [
        ("lda", "#$0F"), ("sta", "$52"), ("inc", "$52"), ("inc", "$52"), ("dec", "$52"),
    ], dict(a=None, x=None, y=None, mask=FLAG_Z | FLAG_N, flags=0,
            mem=[(0x2052, 0x10)], why="$0F +1 +1 -1 = $10")))

    return b


PREAMBLE = [
    ("lda", "#$11"), ("sta", "$DE20"),
    ("lda", "#$22"), ("sta", "$DE21"),
    ("lda", "#$33"), ("sta", "$DE22"),
    ("lda", "#$44"), ("sta", "$DE23"),
    ("lda", "#$C0"), ("sta", "$50"),
]

SUB = [("lda", "#$99"), ("rts", None)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(XL, "out", "selftest.c"))
    ap.add_argument("--expect", default=os.path.join(XL, "out", "selftest_expect.json"))
    a = ap.parse_args()
    lg = xlog.announce()

    src = list(PREAMBLE)
    meta = []
    for i, (name, body, exp) in enumerate(blocks()):
        src.extend(body)
        src.extend([
            ("sta", "$%04X" % (RECORD + 4 * i)),
            ("php", None), ("pla", None),
            ("sta", "$%04X" % (RECORD + 4 * i + 1)),
            ("stx", "$%04X" % (RECORD + 4 * i + 2)),
            ("sty", "$%04X" % (RECORD + 4 * i + 3)),
        ])
        meta.append(dict(index=i, name=name, record=RECORD + 4 * i, expect=exp))

    src.append("done:")
    src.append(("bra", "done"))
    src.append("sub:")
    src.extend(SUB)

    code, labels = assemble(src, BASE)

    # Guard the hand-written assembler: the bytes must decode to the intended
    # instruction stream.  A wrong opcode here would quietly weaken the test.
    listing = []
    pos = 0
    while pos < len(code):
        ins = isa.decode(lambda x: code[x - BASE] if 0 <= x - BASE < len(code) else None,
                         BASE + pos)
        if ins is None:
            break
        listing.append("%04X  %s" % (ins.addr, ins.text))
        pos += ins.size
    if pos != len(code):
        raise SystemExit("listing stopped at %d of %d bytes" % (pos, len(code)))

    images = [{"bank": recompiler.DEFAULT_MPR[(BASE >> 13) & 7], "base": BASE,
               "data": code, "load": True, "name": "selftest"}]
    # jtarget is reached only through `jmp ($DE10)`, which ends linear discovery,
    # so it is named as an extra entry point -- exactly what a real port does with
    # a jump table or a vector table.
    text, tr, _space = recompiler.build_program(images, BASE, lg,
                                                extra_seeds=[labels["jtarget"]])
    with open(a.out, "w") as f:
        f.write(text)

    lg.log("selftest: %d blocks, %d bytes of code, %d instruction functions"
           % (len(meta), len(code), len(tr.units)))
    with open(a.expect, "w") as f:
        json.dump(dict(entry=BASE, record_base=RECORD, done_pc=labels["done"],
                       sub_pc=labels["sub"], blocks=meta, listing=listing), f, indent=1)
    print("wrote %s and %s (%d blocks, %d bytes)" % (a.out, a.expect, len(meta), len(code)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
