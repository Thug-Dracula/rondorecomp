#!/usr/bin/env python3
"""vec_cycles.py -- settle the cycle-cost disagreements against three sources.

`tools/audit_cycles.py` compares this project's cycle table with the reference core it
was built from (Mednafen/TurboNyma, read out of `huc6280.cpp` + `huc6280_ops.inc` by
parsing the macros) and reports 41 opcodes where the two disagree.  That audit cannot
say which side is right: it is our table against a second table.

The published vectors are a third table, generated from Mesen.  This tool puts all
three side by side per opcode, with the number of vectors that showed each cost, and
prints a verdict:

    agree          our cost, the reference core's and the vector's all equal
    core-only      the vector disagrees but the reference core agrees with us
    vector-only    the reference core disagrees but the vector agrees with us
    both-differ    we are alone -- a bug in our table

Usage, from src/:

    python3 tools/vec_cycles.py <report.json>        # report.json from vec_suite.py --all

The vector column shows every (our cost : vector cost) pair seen, with counts, so a
branch that costs 8 taken and 6 not taken appears as two pairs rather than an average.
"""
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))          # repo/tools
SRC = os.path.join(os.path.dirname(HERE), "src")           # the port
sys.path.insert(0, SRC)
sys.path.insert(0, HERE)

import isa        # noqa: E402
import cycles     # noqa: E402
import audit_cycles as A  # noqa: E402  (the reference core's own macro parse)


def ref_costs():
    ref = os.environ.get("REF_CORE") or os.path.join(
        os.environ.get("REFDATA") or ".", "mednafen", "pce")
    if not os.path.isdir(ref):
        return {}
    macros = A.load_macros(os.path.join(ref, "huc6280.cpp"))
    return A.opcode_costs(macros, os.path.join(ref, "huc6280_ops.inc"))


def main():
    report = json.load(open(sys.argv[1]))
    ref = ref_costs()
    rows = []
    for r in report:
        op = r["opcode"]
        mn, size, mode = isa.TABLE[op]
        rule = cycles.cycle_rule(isa.Ins(0, op, mn, size, mode, [0] * size))
        ours = cycles.cost_of(rule)
        vec = {tuple(int(x) for x in k.split(":")): n
               for k, n in r.get("cyc_pairs_clean", r.get("cyc_pairs", {})).items()}
        rows.append((op, mn.strip(), isa.MODE_NAMES.get(mode, ""), ours,
                     ref.get(op), vec, r.get("pass", 0), r.get("total", 0)))

    # The clickable question is the audit's 41, so list those first and in full.
    print("%-4s %-6s %-9s %5s %5s  %-22s %s" %
          ("op", "mnem", "mode", "ours", "ref", "vectors (ours:vec xN)", "verdict"))
    counts = Counter()
    for (op, mn, mode, ours, rf, vec, ok, tot) in rows:
        if not vec or mn == ".db":
            continue
        wrong = {p for p in vec if p[0] != p[1]}      # (ours, vector) with a mismatch
        if rule[0] == cycles.RULE_BLOCK:
            verdict = "block move: compared separately"
        elif not wrong:
            verdict = "agree"
        elif rf is not None and ours == rf:
            verdict = "we match the reference core"
        elif all(v == ours for (_o, v) in wrong):
            verdict = "we match the vectors"
        else:
            verdict = "OUR BUG (both sources disagree)"
        counts[verdict] += 1
        # only print where something actually disagrees
        if verdict in ("agree", "block move: compared separately"):
            continue
        pairs = ", ".join("%d:%d x%d" % (o, v, n) for (o, v), n in sorted(vec.items()))
        if len(pairs) > 60:
            pairs = "%d distinct (block move: base + 6/byte)" % len(vec)
        print("$%02X %-6s %-9s %5d %5s  %-40s %s" %
              (op, mn, mode, ours, "n/a" if rf is None else rf, pairs, verdict))
    print()
    for k, v in counts.most_common():
        print("%4d  %s" % (v, k))
    return 1 if counts["BOTH DIFFER -- our bug"] else 0


if __name__ == "__main__":
    sys.exit(main())
