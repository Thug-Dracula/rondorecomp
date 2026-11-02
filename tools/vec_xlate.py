#!/usr/bin/env python3
"""vec_xlate.py -- run sampled published vectors through the RECOMPILED C as well.

WHY.  Step 5 of the job: the interpreter is now measured against an outside source, but
the translator is only ever measured against the interpreter (`tests/test_diff.py`).  A
vector that disagrees tells us nothing about *which* engine is wrong on its own; a
vector run through both engines gives three answers that are all useful:

  * interpreter agrees with the vector, translator agrees too  -> translator proven here
  * both differ from the vector in the same way                -> interpreter (or model)
  * the two engines differ from each other                     -> translator bug

HOW IT WORKS.  For each opcode a few vectors are sampled (deterministically: first,
last, and two spread through the file).  Each becomes a `recompiler.py --cases` spec:
the instruction's bytes at the vector's own physical location, the vector's registers,
MPRs, PC and every RAM pair.  `recompiler.py --cases` then emits one translation unit
per case, and tools/case_host.c runs each case twice in one process -- once through
`xl_interp_step()`, once through the dispatcher (`xl_cpu_step()`) -- printing both.

The single-instruction guarantee: the case's `seeds` include the address just past the
instruction, so the translator stops the unit there instead of continuing into whatever
bytes follow (all zero in a case).  A case whose unit came out with the wrong bytes --
two sampled vectors landing on the same (bank, offset), so one overwrote the other --
is dropped and counted; that is a property of this harness, not of the port.

Sample K vectors per opcode: `--per-opcode`.  Whole set: 256 opcodes x K.  One build,
one compile, because every case carries its own MPR mapping.

  ./tools/vec_xlate.py --per-opcode 4
  ./tools/vec_xlate.py --opcodes 07,0f,43,53 --per-opcode 6 --verbose
"""
import argparse
import json
import os
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))          # repo/tools
SRC = os.path.join(os.path.dirname(HERE), "src")           # the port
sys.path.insert(0, SRC)
sys.path.insert(0, HERE)

import isa            # noqa: E402
import vec            # noqa: E402
import vec_suite as V  # noqa: E402  (the interpreter-side comparison, reused)
import recompiler      # noqa: E402

OUT = os.path.join(SRC, "out")


def sample(tests, k):
    """Deterministic spread: first, last, and evenly spaced in between."""
    if k >= len(tests):
        return list(range(len(tests)))
    if k == 1:
        return [0]
    idx = sorted({round(i * (len(tests) - 1) / (k - 1)) for i in range(k)})
    return idx


def spec_for(cid, t):
    """A --cases spec that reproduces this vector's initial state exactly."""
    ini = t["initial"]
    pc = ini["PC"]
    page = (pc >> 13) & 7
    bank = ini["MPR"][page]
    off = pc & 0x1FFF
    size = isa.TABLE[ini["RAM"] and t["opcode"] or t["opcode"]][1]
    code = []
    ram = dict(ini["RAM"])
    for i in range(size):
        a = bank * 8192 + off + i
        if a not in ram:
            return None, "instruction bytes cross out of the bank"
        code.append(ram[a])
    if code[0] != t["opcode"]:
        return None, "opcode byte does not match the vector's opcode field"
    if off + size > 8192:
        return None, "instruction crosses the bank end"
    patches = [[a // 8192, a % 8192, [v]] for (a, v) in ini["RAM"]]
    return {
        "id": cid,
        "code": code,
        "code_bank": bank,
        "code_off": off,
        "pc": pc,
        "mpr": list(ini["MPR"]),
        "a": ini["A"], "x": ini["X"], "y": ini["Y"], "s": ini["S"], "p": ini["P"],
        "steps": 1,
        "patches": patches,
        "seeds": [pc + size] if pc + size < 0x10000 else [],
    }, None


def build(specs, path):
    text, tr = recompiler.build_cases(specs, path)
    return tr


def compile_host(case_c, binary):
    cmd = ["gcc", "-std=c11", "-O0", "-Wall", "-Wextra", "-DXLATE_BUS_REC",
           "-I", SRC, "-I", OUT,
           os.path.join(SRC, "runtime.c"), os.path.join(SRC, "video.c"),
           os.path.join(SRC, "interp.c"), os.path.join(SRC, "trace.c"),
           case_c, os.path.join(HERE, "case_host.c"), "-o", binary, "-lm"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    if p.returncode != 0:
        raise SystemExit("compile failed:\n%s\n%s" % (p.stdout[-4000:], p.stderr[-4000:]))
    if "warning:" in p.stderr:
        print(p.stderr[-2000:])
    return binary


def parse(out_text):
    """{id: {"INTERP": d, "TRANSLATED": d}}"""
    res = {}
    for line in out_text.splitlines():
        if not line.startswith("CASE "):
            continue
        f = line.split()
        cid = int(f[1])
        tag = f[2]
        d = {}
        for fld in f[3:]:
            k, _, v = fld.partition("=")
            d[k] = v
        bus = []
        if d.get("BUS"):
            for item in d["BUS"].split(","):
                a, v, rw = item.split(":")
                bus.append((int(a, 16), int(v, 16), rw))
        d["bus"] = bus
        d["cyc"] = int(d["CYC"])
        d["refused"] = d["REFUSED"] == "1"
        d["interpsteps"] = int(d["INTERPSTEPS"])
        d["crcmem"] = int(d["CRCMEM"], 16)
        for k in ("A", "X", "Y", "S", "P", "PC"):
            d[k] = int(d[k], 16)
        d["MPR"] = [int(d["MPR"][i:i + 2], 16) for i in range(0, 16, 2)]
        res.setdefault(cid, {})[tag] = d
    return res


def state_diff(a, b):
    """Fields that differ between two engine results, RAM compared through the bus."""
    bad = []
    for k in ("A", "X", "Y", "S", "P", "PC"):
        if a[k] != b[k]:
            bad.append(k)
    if a["MPR"] != b["MPR"]:
        bad.append("MPR")
    if a["cyc"] != b["cyc"]:
        bad.append("cycles")
    # RAM is compared by a CRC over the whole 2 MB, not by the bus log: the bus ring
    # holds 4,096 accesses, and an interpreter that also fetches its operand bytes
    # reaches fewer of a long block move's writes than the translator does, which
    # would show up as a fake difference.  The CRC cannot run out.
    if a["crcmem"] != b["crcmem"]:
        bad.append("RAM")
    # The bus *reads* are deliberately not compared between the two engines: the
    # translator emits the operand bytes as constants, so it never performs the
    # fetches the interpreter performs, and a difference there is the translation
    # being a translation -- not a defect.  Bus activity is compared against the
    # *vector* instead, by vec_suite.py.
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opcodes", default=None)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--per-opcode", type=int, default=4)
    ap.add_argument("--json", default=None)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    ops = list(range(256)) if a.all else \
        [int(x, 16) for x in (a.opcodes or "07,0f,43,53,1a,02,00,20,40,60").replace(",", " ").split()]

    specs, meta = [], {}
    dropped = Counter()
    for op in ops:
        try:
            tests = vec.load(op, V.VEC_DIR)
        except FileNotFoundError:
            dropped["no vector file"] += 1
            continue
        run, _sk = V.select(tests)
        for j in sample(range(len(run)), a.per_opcode):
            cid = op * 1000 + j
            spec, why = spec_for(cid, run[j])
            if spec is None:
                dropped[why] += 1
                continue
            specs.append(spec)
            meta[cid] = {"opcode": op, "index": j, "vector": run[j]}

    print("%d cases from %d opcodes (%s)"
          % (len(specs), len(ops), ", ".join("%s x%d" % kv for kv in dropped.items()) or "none dropped"))

    case_c = os.path.join(OUT, "cases", "vec.c")
    os.makedirs(os.path.dirname(case_c), exist_ok=True)
    tr = build(specs, case_c)

    # A case whose unit carries different bytes than the vector asked for is a harness
    # collision (two sampled vectors share a physical location): drop it, say so.
    keep = []
    for spec in specs:
        key = (spec["code_bank"], spec["code_off"])
        ins = tr.units.get(key)
        if ins is None:
            dropped["no translated unit (undefined opcode or refused)"] += 1
            continue
        if list(ins.b[:len(spec["code"])]) != spec["code"]:
            dropped["collision at (bank, offset)"] += 1
            continue
        if key in tr.delegated:
            dropped["unit delegates to the interpreter (crosses a bank)"] += 1
            continue
        keep.append(spec)
    if len(keep) != len(specs):
        print("kept %d of %d cases: %s"
              % (len(keep), len(specs), ", ".join("%s x%d" % kv for kv in dropped.items())))
        specs = keep
        tr = build(specs, case_c)

    binary = compile_host(case_c, os.path.join(OUT, "vec_xlate_host"))
    p = subprocess.run([binary], capture_output=True, text=True, timeout=3600)
    if p.returncode != 0:
        raise SystemExit("case_host failed: %s" % p.stderr[-2000:])
    res = parse(p.stdout)

    rows, causes = [], Counter()
    for spec in specs:
        cid = spec["id"]
        r = res[cid]
        ip, tl = r["INTERP"], r["TRANSLATED"]
        vb = V.compare(meta[cid]["vector"], ip)
        tb = V.compare(meta[cid]["vector"], tl)
        d = state_diff(ip, tl)
        verdict = "agree" if not d else "differ:" + ",".join(d)
        if d:
            causes[verdict] += 1
        delegated = tl["interpsteps"] > ip["interpsteps"]
        rows.append({"id": cid, "opcode": meta[cid]["opcode"],
                     "index": meta[cid]["index"],
                     "vector_vs_interp": [b[0] for b in vb],
                     "vector_vs_translated": [b[0] for b in tb],
                     "engines": verdict, "delegated": delegated,
                     "cyc_interp": ip["cyc"], "cyc_translated": tl["cyc"],
                     "cyc_vector": meta[cid]["vector"]["num_cycles"]})
        if a.verbose and d:
            print("  $%02X #%-4d %-16s interp: A=%02X P=%02X PC=%04X cyc=%d  "
                  "translated: A=%02X P=%02X PC=%04X cyc=%d"
                  % (meta[cid]["opcode"], meta[cid]["index"], verdict,
                     ip["A"], ip["P"], ip["PC"], ip["cyc"],
                     tl["A"], tl["P"], tl["PC"], tl["cyc"]))

    n = len(rows)
    same = sum(1 for r in rows if r["engines"] == "agree")
    both_no_vec = sum(1 for r in rows if r["vector_vs_interp"] and
                      r["vector_vs_translated"] == r["vector_vs_interp"] and
                      r["engines"] == "agree")
    print("\n%d case(s): interpreter and translator agree on %d (%.1f%%)"
          % (n, same, 100.0 * same / n if n else 0.0))
    print("   of those, %d disagree with the vector in exactly the same fields "
          "(interpreter/model, not translator)" % both_no_vec)
    print("   %d case(s) fall back to the interpreter on the translated side"
          % sum(1 for r in rows if r["delegated"]))
    for k, v in causes.most_common():
        print("   %4d  %s" % (v, k))
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
        print("wrote %s" % a.json)
    return 0 if same == n else 1


if __name__ == "__main__":
    sys.exit(main())
