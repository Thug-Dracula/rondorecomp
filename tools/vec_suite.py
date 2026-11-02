#!/usr/bin/env python3
"""vec_suite.py -- compare this machine's interpreter with the published HuC6280
single-step vectors (github.com/SingleStepTests/huc6280, Mesen-generated).

WHY THIS EXISTS
---------------
`interp.c` executes the game's original bytes and is the reference side of every
comparison in PLAN-VERIFICATION.md.  Until now it had only ever been checked against
the *translator*, which shares its tables and therefore its misunderstandings.  This
is the first check against a source outside the codebase.

WHAT A VECTOR IS
----------------
Each opcode has a file of 2,500 independent tests.  A test gives the complete initial
machine state (A, X, Y, S, P, PC, the eight MPR registers, and the RAM pairs that are
non-zero), the complete final state, `num_cycles`, and the per-cycle bus pins:

    [physical address, data, "r--"]      a real read
    [physical address, data, "-w-"]      a real write
    [physical address, data, "r-d"]      a *dummy* read (Mesen models it; we do not)
    [0, 0, "---"]                        an internal cycle, no bus access

RAM addresses are 21-bit *physical* addresses, the same (bank << 13 | offset) space
`Cpu.mem` is laid out in.

HOW A TEST IS RUN
-----------------
`out/vec_host` (tools/vec_host.c) builds the machine with `xl_reset()`, writes the
vector's registers, MPRs, PC and RAM pairs, calls `xl_cpu_step()` exactly once with
XLATE_INTERP=1, and prints the resulting registers, the cycle delta, and -- when built
with -DXLATE_BUS_REC -- every physical (address, byte, direction) the CPU touched.

WHAT IS COMPARED, IN ORDER
--------------------------
    registers (A X Y S P PC MPR)  -> disagreement class "registers"
    final RAM pairs               -> "ram"
    cycle count                   -> "cycles"
    bus activity                  -> "bus"

A case that matches on all four is a pass.  Everything else is classified, never
lumped: see classify() for the classes and their meanings.

  ./tools/vec_suite.py --opcodes 03,ad --json out/vec-03-ad.json
  ./tools/vec_suite.py --all --json out/cpuvec-report.json
  ./tools/vec_suite.py --opcodes 03 --limit 50 --verbose
  ./tools/vec_suite.py --selftest          # fault injection: prove the harness fails

ADDING AN OPCODE is `--opcodes 5a`; nothing else.  The vector file is found by opcode
number, the ISA entry (mnemonic, width, mode) comes from isa.py, and the harness
protocol is opcode-agnostic.
"""
import argparse
import json
import os
import subprocess
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))          # repo/tools
SRC = os.path.join(os.path.dirname(HERE), "src")           # the port
sys.path.insert(0, SRC)
sys.path.insert(0, HERE)

import isa                                     # noqa: E402  (the port's own ISA table)
import vec                                     # noqa: E402  (the vector decoder)

VEC_DIR = vec.DEFAULT_DIR
VEC_HOST = os.path.join(SRC, "out", "vec_host")

# The port models $E000-$FFFF as the CD BIOS (interp.c: xl_bios_call), so a vector whose
# PC lives there would measure the BIOS model, not the opcode.  Those are counted and
# reported as not-run, never silently dropped.
BIOS_PAGE = 0xE000
IO_BANK = 0xFF          # the port's I/O page
BRAM_BANK = 0xF7        # the port's battery-backed save RAM


# --------------------------------------------------------------- selection --
def select(tests):
    """Split the vectors for one opcode into runnable cases and skip reasons."""
    run, skipped = [], Counter()
    for t in tests:
        ini = t["initial"]
        if ini["PC"] >= BIOS_PAGE:
            skipped["pc in $E000-$FFFF (the port models this page as the CD BIOS)"] += 1
            continue
        fetch_bank = ini["MPR"][(ini["PC"] >> 13) & 7]
        if fetch_bank in (IO_BANK, BRAM_BANK):
            skipped["opcode fetched from a device bank the port models "
                    "($F7 save RAM, $FF I/O)"] += 1
            continue
        run.append(t)
    return run, skipped


def banks_touched(t):
    """Physical banks a test reads or writes (RAM pairs plus bus records)."""
    out = set()
    for (a, _v) in t["initial"]["RAM"]:
        out.add(a >> 13)
    for c in t["cycles"]:
        if c[2] != "-" or c[3] != "-":
            out.add(c[0] >> 13)
    return out


# ----------------------------------------------------------------- driving --
def build_input(tests):
    lines = []
    for t in tests:
        ini = t["initial"]
        mpr = 0
        for b in ini["MPR"]:
            mpr = (mpr << 8) | b
        pairs = " ".join("%X:%X" % (a, v) for (a, v) in ini["RAM"])
        lines.append("%02X %02X %02X %02X %02X %04X %016X %d %s"
                     % (ini["A"], ini["X"], ini["Y"], ini["S"], ini["P"],
                        ini["PC"], mpr, len(ini["RAM"]), pairs))
    return "\n".join(lines) + "\n"


def run_host(tests, workdir):
    os.makedirs(workdir, exist_ok=True)
    inp = os.path.join(workdir, "cases.txt")
    outp = os.path.join(workdir, "result.txt")
    with open(inp, "w") as f:
        f.write(build_input(tests))
    env = dict(os.environ, XLATE_INTERP="1")
    for k in ("XLATE_TRACE_OUT", "XLATE_RAMSTREAM", "XLATE_FRAMELOG",
              "XLATE_RAMDUMP", "XLATE_SATSTREAM", "XLATE_STEP_FROM"):
        env.pop(k, None)
    with open(inp) as fin, open(outp, "w") as fout:
        p = subprocess.run([VEC_HOST, outp], stdin=fin, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True, env=env, timeout=1800)
    if p.returncode != 0:
        raise SystemExit("vec_host failed: %s" % (p.stderr[-2000:],))
    res = {}
    with open(outp) as f:
        for line in f:
            if not line.startswith("CASE "):
                continue
            fields = line.split()
            n = int(fields[1])
            d = {}
            for fld in fields[2:]:
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
            for k in ("A", "X", "Y", "S", "P", "PC"):
                d[k] = int(d[k], 16)
            d["MPR"] = [int(d["MPR"][i:i + 2], 16) for i in range(0, 16, 2)]
            res[n] = d
    return res


# ------------------------------------------------------------- comparison --
P_B = 0x10          # the B-flag storage bit; see compare() below

def compare(t, r, op=None):
    """Return a list of (field, expected, got) disagreements; empty means pass.

    P is compared with bit 4 masked.  That bit is the B flag's storage bit, and the
    two sides keep it in different places: the port keeps the last pushed B bit in P
    and sets it again on PHP, while the vector set (Mesen) never has it set in the P
    it publishes -- so a PLP/RTI that restores a pushed byte 0xF0 reads back as 0xF0
    here and 0xE0 there, which is a representation difference and not a semantic one.
    It is measured, not hidden: `p_bit4` in the result counts the cases that differ
    only there, and the report says how many there were.
    """
    bad = []
    fin = t["final"]
    for key in ("A", "X", "Y", "S", "PC"):
        if r[key] != fin[key]:
            bad.append((key, fin[key], r[key]))
    if (r["P"] ^ fin["P"]) & ~P_B:
        bad.append(("P", fin["P"], r["P"]))
    elif r["P"] != fin["P"]:
        r["p_bit4"] = True
    if r["MPR"] != fin["MPR"]:
        bad.append(("MPR", fin["MPR"], r["MPR"]))

    # RAM: the vector lists every address that is non-zero in the final state.  Our
    # value there is the last byte we wrote, or the initial byte if we did not write.
    writes = {}
    for (a, v, rw) in r["bus"]:
        if rw == "w":
            writes[a] = v
    init = dict(t["initial"]["RAM"])
    final = dict(fin["RAM"])
    for (a, want) in final.items():
        got = writes.get(a, init.get(a, 0))
        if got != want:
            bad.append(("RAM:$%X" % a, want, got))
    if op not in BLOCK_OPS:
        # (a block move's own transfer runs past the vector set's 500-pair cap; the
        # extra writes are the transfer continuing, not a difference.  The cap is
        # the vector set's, stated in its README.)
        for a in writes:
            if a not in final:
                bad.append(("RAM:$%X (not in the vector's RAM list)" % a, None, writes[a]))

    if r["cyc"] != t["num_cycles"]:
        bad.append(("cycles", t["num_cycles"], r["cyc"]))

    # Bus activity: the vector's records with a real direction, in order, against ours.
    # Dummy accesses are counted separately because this machine does not model them at
    # all -- that is a fact about the model, not a mismatch to be scored as an error.
    want = [(c[0], c[1], "r" if c[2] == "r" else "w")
            for c in t["cycles"]
            if (c[2] != "-" or c[3] != "-") and c[4] != "d"]
    if op in BLOCK_OPS:
        if want != r["bus"][:len(want)]:
            bad.append(("bus (first %d records)" % len(want), want, r["bus"][:len(want)]))
    elif want != r["bus"]:
        bad.append(("bus", want, r["bus"]))
    return bad


def has_dummy(t):
    return any(c[4] == "d" for c in t["cycles"])


# ---- the causes, each one established by looking at the failing vectors ----
#
# These are not "guesses from the fields that differ": every one was checked against
# the reference core's source, and where they disagree the reference is named.

BLOCK_OPS = {0x73, 0xC3, 0xD3, 0xE3, 0xF3}    # tii tdd tin tia tai
ST_OPS = {0x03, 0x13, 0x23}                   # st0 st1 st2


def classify(t, r, bad, mnem, op):
    dev = bool(banks_touched(t) & {IO_BANK, BRAM_BANK})
    fields = {b[0].split(":")[0] for b in bad}
    tag = " [operand in a device bank]" if dev else ""

    if r["refused"]:
        return "The port refuses undefined opcodes (22 of them)"
    if op in ST_OPS and fields <= {"cycles"}:
        return "VDC access slot: +1 cycle here, not in the vector"
    if mnem == "brk":
        return "BRK: PC+1 / 7 cycles here and in the reference core, PC+2 / 8 in the vector"
    if mnem == "jsr" and fields == {"bus"}:
        return "bus order: JSR pushes before fetching the high operand byte"
    if mnem in ("tsb", "trb") and fields <= {"P", "bus"}:
        # Settled 2026-10-30: the vector set and the datasheet are right, and the
        # Mednafen-derived cores (and Mesen2, and Geargrafx shipping) are wrong in
        # two different ways. The port now implements (A & M) for both. This
        # class is retained only so an older binary still classifies.
        return "TSB/TRB Z flag: vector and datasheet formula; the reference core is wrong"
    if mnem == "tma":
        return "TMA with several operand bits set: highest MPR here, lowest in the vector"
    if op in BLOCK_OPS:
        if "cycles" in fields:
            return "block move: cycle count differs"
        return ("block move: cycles agree; the vector stops at 500 RAM pairs and "
                "pushes before the operand fetches")
    if dev:
        return "operand in a device bank ($F7 save RAM / $FF I/O page)"
    if mnem in ("adc", "sbc") and (t["initial"]["P"] & 0x08):
        return "decimal mode: invalid BCD input, reformatted differently" + tag
    if fields == {"bus"} and has_dummy(t):
        return "dummy read/write in the vector (this port does not model them)"
    if fields == {"cycles"}:
        return "cycles differ"
    if fields & {"A", "X", "Y", "S", "P", "PC", "MPR", "RAM"}:
        return "registers/RAM disagree"
    return "unclassified"


# ------------------------------------------------------------------- table --
def run_opcode(op, workdir, limit=None, verbose=False):
    mn, size, mode = isa.TABLE[op]
    name = mn.strip()
    mname = isa.MODE_NAMES.get(mode, str(mode))
    try:
        tests = vec.load(op, VEC_DIR)
    except FileNotFoundError:
        return {"opcode": op, "name": name, "mode": mname, "error": "no vector file"}
    run, skipped = select(tests)
    if limit:
        run = run[:limit]
    if not run:
        return {"opcode": op, "name": name, "mode": mname, "skipped": dict(skipped),
                "total": 0, "pass": 0, "fail": 0, "causes": {}, "examples": [],
                "device_bank": 0, "dummy": 0, "p_bit4": 0, "cyc_pairs": {},
                "cyc_pairs_clean": {}}
    res = run_host(run, os.path.join(workdir, "%02x" % op))
    causes = Counter()
    examples = defaultdict(list)
    cyc_pairs = Counter()          # "ours:vector" -> cases, over every case
    cyc_pairs_clean = Counter()    # the same, excluding cases on a device bank
    passes = device = dummy = bit4 = 0
    for i, t in enumerate(run):
        r = res[i]
        bad = compare(t, r, op)
        cyc_pairs["%d:%d" % (r["cyc"], t["num_cycles"])] += 1
        if not (banks_touched(t) & {IO_BANK, BRAM_BANK}):
            cyc_pairs_clean["%d:%d" % (r["cyc"], t["num_cycles"])] += 1
        dummy += 1 if has_dummy(t) else 0
        if any(b >> 13 in (IO_BANK, BRAM_BANK) for b in banks_touched(t)):
            device += 1
        if r.get("p_bit4"):
            bit4 += 1
        if not bad:
            passes += 1
            continue
        cause = classify(t, r, bad, name, op)
        causes[cause] += 1
        if len(examples[cause]) < 6:
            examples[cause].append({
                "name": t["name"],
                "fields": [{"field": f, "want": w, "got": g} for (f, w, g) in bad[:6]],
            })
    return {"opcode": op, "name": name, "mode": mname, "skipped": dict(skipped),
            "total": len(run), "pass": passes, "fail": len(run) - passes,
            "causes": dict(causes), "examples": dict(examples),
            "device_bank": device, "dummy": dummy, "p_bit4": bit4,
            "cyc_pairs": dict(cyc_pairs), "cyc_pairs_clean": dict(cyc_pairs_clean)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opcodes", help="comma/space separated hex opcodes, e.g. 03,ad")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--work", default=os.path.join(SRC, "out", "vec_work"))
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true",
                    help="fault injection: mutate one expected field and require the "
                         "harness to name exactly that field")
    a = ap.parse_args()

    if a.selftest:
        return selftest(a.work)

    if a.all:
        ops = list(range(256))
    elif a.opcodes:
        ops = [int(x, 16) for x in a.opcodes.replace(",", " ").split()]
    else:
        ap.error("give --opcodes or --all")

    rows = []
    for op in ops:
        row = run_opcode(op, a.work, a.limit, a.verbose)
        rows.append(row)
        if "error" in row:
            print("$%02X %-6s %s" % (op, row["name"], row["error"]))
            continue
        print("$%02X %-6s %-10s %5d run  %5d agree  %4d differ   %s"
              % (op, row["name"], row["mode"], row["total"], row["pass"],
                 row["fail"],
                 ", ".join("%s x%d" % (k, v) for k, v in row["causes"].items())))
        if a.verbose:
            for cause, exs in row["examples"].items():
                for e in exs[:3]:
                    print("      %-40s %s" % (e["name"], e["fields"][:3]))

    tot_run = sum(r.get("total", 0) for r in rows)
    tot_pass = sum(r.get("pass", 0) for r in rows)
    agreeing = [r for r in rows if r.get("fail") == 0 and r.get("total")]
    print("\n%d opcodes, %d vectors run, %d agree (%.2f%%), %d differ"
          % (len(rows), tot_run, tot_pass,
             100.0 * tot_pass / tot_run if tot_run else 0.0, tot_run - tot_pass))
    print("%d opcodes entirely clean" % len(agreeing))
    causes = Counter()
    for r in rows:
        for k, v in r.get("causes", {}).items():
            causes[k] += v
    for k, v in causes.most_common():
        print("   %6d  %s" % (v, k))
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
        print("wrote %s" % a.json)
    return 0 if tot_run == tot_pass else 1


# ------------------------------------------------------------ fault inject --
def selftest(workdir):
    """Prove the harness can fail, and that it fails on the field that was planted.

    Four plants, each in a different compared channel:
      * one bit of the expected A,
      * one bit of the expected PC,
      * one byte of the expected final RAM,
      * the expected cycle count (+1).
    Each must be reported, and the reported field must be the one planted.
    """
    ok = True
    op = 0xAD                       # LDA $hhll: a clean, single-address instruction
    tests = vec.load(op, VEC_DIR)[:50]
    run, _ = select(tests)
    res = run_host(run, os.path.join(workdir, "selftest"))
    base_bad = sum(1 for i, t in enumerate(run) if compare(t, res[i]))
    print("selftest: %d of %d unmodified vectors disagree (baseline %d)"
          % (base_bad, len(run), base_bad))

    def plant(field, mutate):
        bad_at = []
        named = Counter()
        for i, t in enumerate(run[:20]):
            t2 = json.loads(json.dumps(t))
            mutate(t2)
            bad = compare(t2, res[i])
            if bad:
                bad_at.append(i)
                for (f, _w, _g) in bad:
                    named[f.split(":")[0] if f.startswith("RAM") else f] += 1
        hit = len(bad_at)
        good = hit == 20
        want = field.split(":")[0]
        got = [k for k in named if k == want or (want == "RAM" and k == "RAM")]
        named_ok = bool(got) and not [k for k in named if k != want]
        print("  plant %-14s reported on %2d/20, names %-8s -> %s"
              % (field, hit, ",".join(sorted(named)) or "-",
                 "OK" if (good and named_ok) else "FAILED"))
        return good and named_ok

    def m_a(t):  t["final"]["A"] ^= 0x01
    def m_pc(t): t["final"]["PC"] = (t["final"]["PC"] + 0x10) & 0xFFFF
    def m_cy(t): t["num_cycles"] += 1
    def m_ram(t):
        a = t["final"]["RAM"][0][0]
        v = t["final"]["RAM"][0][1]
        for i, (aa, vv) in enumerate(t["final"]["RAM"]):
            if aa != t["initial"]["RAM"][0][0] or vv != t["initial"]["RAM"][0][1]:
                a, v = aa, vv
                t["final"]["RAM"][i] = (aa, vv ^ 0x01)
                return
        t["final"]["RAM"][0] = (a, v ^ 0x01)

    for field, fn in (("A", m_a), ("PC", m_pc), ("cycles", m_cy), ("RAM", m_ram)):
        ok = plant(field, fn) and ok
    print("selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
