"""Decoder for the published SingleStepTests/huc6280 binary vector files.

Format (reverse-engineered from the upstream transcode_json.py, verified against
the published file sizes and a hand-decoded first test):

  file   = u32 count, then `count` tests
  test   = i32 size (bytes of this test, from its own start)
           char name[50]        (NUL-padded)
           u32  opcode
           state initial, state final
           cycles
  state  = i32 size
           u8 A, X, Y, S, P
           u16 PC
           u8 MPR[8]
           i32 num_ram
           { u32 phys_addr, u8 value } * num_ram
  cycles = i32 size
           u32 num_cycles           (the instruction's cost)
           u32 n_records
           u32 word * n_records     RD bit0 | WR bit1 | DUMMY bit2 | addr[20:0] bit3..23 | data bit24..31

`size` fields are self-consistent: every test's declared size equals the bytes
consumed, which the loader checks.
"""
import os
import struct

DEFAULT_DIR = os.environ.get("VEC_DIR") or os.path.join(
    os.environ.get("REFDATA") or ".", "cpuvec", "bin")


def _state(b, p):
    (sz,) = struct.unpack_from("<i", b, p)
    start = p
    p += 4
    a, x, y, s, pl = struct.unpack_from("<5B", b, p)
    p += 5
    (pc,) = struct.unpack_from("<H", b, p)
    p += 2
    mpr = list(struct.unpack_from("<8B", b, p))
    p += 8
    (n,) = struct.unpack_from("<i", b, p)
    p += 4
    ram = []
    for _ in range(n):
        (addr,) = struct.unpack_from("<I", b, p)
        ram.append((addr, b[p + 4]))
        p += 5
    assert p - start == sz, (p - start, sz)
    return p, {"A": a, "X": x, "Y": y, "S": s, "P": pl, "PC": pc, "MPR": mpr,
               "RAM": ram}


def _cycles(b, p):
    # No size field here: the upstream transcoder computes the block length as
    # 4 (num_cycles) + 4 (n_records) + 4*n and reads the two words directly.
    start = p
    num_cycles, nrec = struct.unpack_from("<II", b, p)
    p += 8
    recs = []
    for _ in range(nrec):
        (w,) = struct.unpack_from("<I", b, p)
        p += 4
        rd = w & 1
        wr = (w >> 1) & 1
        dummy = (w >> 2) & 1
        addr = (w >> 3) & 0x1FFFFF
        data = (w >> 24) & 0xFF
        if not rd and not wr:
            addr = data = 0
        recs.append((addr, data, "r" if rd else "-", "w" if wr else "-",
                     "d" if dummy else "-"))
    assert p - start == 8 + 4 * nrec, (p - start, nrec)
    return p, num_cycles, recs


def load_file(path):
    b = open(path, "rb").read()
    (count,) = struct.unpack_from("<I", b, 0)
    p = 4
    out = []
    for _ in range(count):
        start = p
        (sz,) = struct.unpack_from("<i", b, p)
        p2 = p + 4
        name = b[p2:p2 + 50].decode("latin-1").strip()
        p2 += 50
        (op,) = struct.unpack_from("<I", b, p2)
        p2 += 4
        p2, ini = _state(b, p2)
        p2, fin = _state(b, p2)
        p2, ncyc, recs = _cycles(b, p2)
        assert p2 - start == sz, (name, p2 - start, sz)
        # keep the raw RAM lists, not a dict: the vector's pairs are a *complete*
        # description of the initial RAM contents (everything else is zero), and a
        # post-state comparison wants to know which addresses the test even looked at.
        out.append({"name": name, "opcode": op, "initial": ini, "final": fin,
                    "num_cycles": ncyc, "cycles": recs})
        p = p2
    assert p == len(b), (p, len(b))
    return out


def load(opcode, dir=DEFAULT_DIR):
    return load_file(os.path.join(dir, "%02x.json.bin" % opcode))


if __name__ == "__main__":
    import sys
    op = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x01
    tests = load(op)
    t = tests[0]
    print("opcode $%02X  %d tests" % (op, len(tests)))
    print("name", t["name"])
    print("initial", {k: v for k, v in t["initial"].items() if k != "RAM"},
          "RAM pairs", len(t["initial"]["RAM"]))
    print("final  ", {k: v for k, v in t["final"].items() if k != "RAM"},
          "RAM pairs", len(t["final"]["RAM"]))
    print("num_cycles", t["num_cycles"], "records", len(t["cycles"]))
    for c in t["cycles"][:12]:
        print("   ", c)
