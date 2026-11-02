#!/usr/bin/env python3
"""Build a HuC6280 opcode table in Python from Etripator's opcodes.c.

Etripator (github.com/pce-devel/Etripator) carries a hand-checked 256-entry
opcode table.  Rather than retyping it, we parse it straight out of the C
source so the Python disassembler cannot drift from a known-good table.

The result is checked in at `src/huc_table.py`, so a build does not
need Etripator; run this only to re-derive it after Etripator's table changes.
Point ETRIPATOR at a checkout of that repository.

Output: src/huc_table.py  ->  TABLE[opcode] = (mnemonic, size, mode)
"""
import re
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ETR = os.environ.get("ETRIPATOR", os.path.join(HERE, "Etripator"))
SRC = os.path.join(ETR, "opcodes.c")
OUT = os.path.normpath(os.path.join(HERE, "..", "src", "huc_table.py"))


def main():
    txt = open(SRC, encoding="utf-8", errors="replace").read()

    # --- opcode table: /* 00 */  { "brk ", 1 ,  0 }, ---
    body = txt[txt.index("static Opcode pce_opcode[256]"):]
    body = body[:body.index("};")]
    rows = re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', body)
    if len(rows) != 256:
        sys.exit("expected 256 opcodes, got %d" % len(rows))

    names = [r[0].strip() for r in rows]
    sizes = [int(r[1]) for r in rows]
    modes = [int(r[2]) for r in rows]

    # ---- corrections to the upstream table, each proved by a published vector ----
    #
    # $36 is ROL $zp,X, not ROL $zp.  Etripator's opcodes.c says `{ "rol ", 2 , 7 },
    # // ROL ZZ` and it is the odd one out in its own family: $16 asl zp,X, $56 lsr
    # zp,X, $76 ror zp,X, $D6 dec zp,X and $F6 inc zp,X are all mode 8, and the
    # SingleStepTests/huc6280 vectors for $36 access (operand + X) in every one of
    # 2,500 cases.  Left wrong, `rol $xx,X` reads and writes the unindexed address.
    modes[0x36] = 8

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# generated from Etripator opcodes.c -- do not edit\n")
        f.write("NAMES = %r\n" % (names,))
        f.write("SIZES = %r\n" % (sizes,))
        f.write("MODES = %r\n\n" % (modes,))
        f.write("TABLE = [(NAMES[i], SIZES[i], MODES[i]) for i in range(256)]\n")

    print("wrote %s (%d opcodes)" % (OUT, len(rows)))
    print("sample: 0x20 -> %r  0x6C -> %r  0xE3 -> %r" %
          (rows[0x20], rows[0x6C], rows[0xE3]))


if __name__ == "__main__":
    main()
