#!/usr/bin/env python3
"""Full opcode-cycle audit: our cycles.py against the reference core's own table.

The timing ground truth is a reference core's own opcode table -- point `--ref`
(or REF_CORE) at its `pce/` directory (`huc6280.cpp` + `huc6280_ops.inc`).  That core
has no per-instruction cycle array: each opcode case invokes a macro, and each macro
charges its own cycles with `ADDCYC(n)`, plus
`LastCycle()`, which itself charges 1 (`void HuC6280::LastCycle(void) { ... ADDCYC(1); }`).

So the reference cost of an opcode = sum of ADDCYC(n) over the macro body, +1 per
LastCycle(), + the cost of any macro passed as an argument (`IMP(DEY)`, `RMW_ZP(ROR)`,
`LD_ZP(BBRi(...))`, `LD_IM_COMPLEX(ST0)`).

    python3 tools/audit_cycles.py [--ref DIR]

Exit status is 1 when any comparable opcode disagrees.  Not comparable, and skipped:
the five block moves (a base plus a per-byte rate, reported separately), WAI (waits for
an interrupt), and the two-clock TAM/TMA/ST0/ST1/ST2 shapes are compared as the core
charges them.  The VDC/VCE port steal (`IOWrite`'s `HuCPU.StealCycle()`) is not part of
either number: both sides charge it at the access, so it cancels.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # repo/tools
SRC = os.path.join(os.path.dirname(HERE), "src")           # the port
sys.path.insert(0, SRC)
sys.path.insert(0, HERE)

import isa        # noqa: E402
import cycles     # noqa: E402

REF = os.environ.get("REF_CORE") or os.path.join(
    os.environ.get("REFDATA") or ".", "mednafen", "pce")

# Names that are C/API, not instruction macros: never add their cost.
NOT_A_MACRO = {'ADDCYC', 'ADDCYC_MASTER', 'LastCycle', 'RdMem', 'WrMem', 'RdOp',
               'WrOp', 'GetAB', 'GetZP', 'GetZPI', 'GetIX', 'GetIY', 'GetIND',
               'GetABI', 'PUSH', 'POP', 'if', 'while', 'for', 'do', 'sizeof',
               'assert', 'uint8', 'uint16', 'uint32', 'int8', 'int32', 'IRQSample'}


def load_macros(*paths):
    """#define NAME(args) body  ->  {NAME: body}, continuation lines joined."""
    out = {}
    for p in paths:
        src = open(p).read().replace('\\\n', ' ')
        for m in re.finditer(r'^#define\s+(\w+)\(([^)]*)\)\s*(.*)$', src, re.M):
            out[m.group(1)] = m.group(3)
        # argument-less macros are written `#define NAME<TAB>{ ... }`
        for m in re.finditer(r'^#define\s+(\w+)[ \t]+(\{.*)$', src, re.M):
            out.setdefault(m.group(1), m.group(2))
    return out


def macro_cost(name, macros, depth=0):
    """Cycles one macro charges, or None when it cannot be resolved statically."""
    if depth > 8 or name in NOT_A_MACRO:
        return None
    body = macros.get(name)
    if body is None:
        return None
    if re.search(r'ADDCYC\((?!\d)', body):
        return None                     # variable count (block moves)
    total = sum(int(x) for x in re.findall(r'ADDCYC\((\d+)\)', body))
    total += body.count('LastCycle()')
    for m in re.finditer(r'\b([A-Z][A-Z0-9_]*)\s*\(', body):
        sub = m.group(1)
        if sub in NOT_A_MACRO or sub not in macros:
            continue
        c = macro_cost(sub, macros, depth + 1)
        if c is None:
            return None
        total += c
    return total


def case_cost(body, macros):
    """Cost of one opcode's case body: the invoked macro plus its macro arguments."""
    m = re.search(r'\b([A-Z][A-Z0-9_]*)\s*\(', body)
    if not m:
        return None
    total = macro_cost(m.group(1), macros)
    if total is None:
        return None
    # arguments that are themselves macros (IMP(DEY), RMW_ZP(ROR), ...)
    depth = 1
    i = m.end()
    while i < len(body) and depth:
        if body[i] == '(':
            depth += 1
        elif body[i] == ')':
            depth -= 1
        i += 1
    for a in re.finditer(r'\b([A-Z][A-Z0-9_]{1,})\b', body[m.end():i - 1]):
        sub = a.group(1)
        if sub in NOT_A_MACRO or sub not in macros:
            continue
        c = macro_cost(sub, macros)
        if c is None:
            return None
        total += c
    return total


def opcode_costs(macros, opsfile):
    src = open(opsfile).read().replace('\\\n', ' ')
    out = {}
    for m in re.finditer(r'case (0x[0-9A-Fa-f]{2}):(.*?)(?=case 0x|\Z)', src, re.S):
        out[int(m.group(1), 16)] = case_cost(m.group(2), macros)
    return out


def main():
    ref = REF
    if '--ref' in sys.argv:
        ref = sys.argv[sys.argv.index('--ref') + 1]
    if not os.path.isfile(os.path.join(ref, 'huc6280.cpp')):
        sys.exit("audit_cycles: no reference core at %r -- pass --ref DIR or set "
                 "REF_CORE to a checkout's pce/ directory\n"
                 "(needs huc6280.cpp and huc6280_ops.inc)" % ref)
    macros = load_macros(os.path.join(ref, 'huc6280.cpp'))
    refc = opcode_costs(macros, os.path.join(ref, 'huc6280_ops.inc'))
    bad, skipped, cmp = [], [], 0
    for op in range(256):
        name, size, mode = isa.TABLE[op]
        ins = isa.Ins(0, op, name, size, mode, [0] * size)
        rule = cycles.cycle_rule(ins)
        r = refc.get(op)
        if r is None or rule[0] == cycles.RULE_BLOCK:
            skipped.append((op, name, r))
            continue
        ours = cycles.cost_of(rule)
        cmp += 1
        if ours != r:
            bad.append((op, name, isa.MODE_NAMES.get(mode, mode), ours, r))
    print('%d of 256 opcodes compared, %d skipped (block moves / dynamic)' %
          (cmp, len(skipped)))
    print('%d disagree' % len(bad))
    for op, name, mode, ours, r in bad:
        print('  $%02X %-5s %-9s ours %d  ref %d  (%+d)' % (op, name, mode, ours, r, r - ours))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
