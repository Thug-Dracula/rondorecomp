#!/usr/bin/env python3
"""Read the oracle's CPU state and memory out of an emulator savestate.

The savestate is a flat list of [u16 BE name length][name][u32 LE size][data]
records.  The HuC6280 block holds PC, A, X, Y, P, S, MPR and the interrupt/timer
shadow; BaseRAM is the 8 KB work RAM, CDRAM the 64 KB Super CD-ROM2 RAM, VRAM the
video memory.

Usage:
  oracle_state.py STATE...            one summary line per state
  oracle_state.py STATE --detail      also dump registers, MPR, pad bytes, bank hashes
"""
import argparse
import re
import struct
import sys
import zlib


def records(buf):
    """Walk the state's record list: [u8 name length][name][u32 LE size][data].

    The length is a *single* byte.  An earlier version assumed a big-endian u16,
    which matched only the records that happened to be preceded by a zero byte --
    it silently missed MPR and picked up 32 KB blobs whose first byte looked like a
    name length.  Walking the list fixes both.
    """
    out = {}
    p = 0
    n = len(buf)
    while p + 5 <= n:
        ln = buf[p]
        if not (1 <= ln <= 40) or p + 1 + ln + 4 > n:
            p += 1
            continue
        name = buf[p + 1:p + 1 + ln]
        if not all(0x20 <= b < 0x7F for b in name):
            p += 1
            continue
        sz = struct.unpack("<I", buf[p + 1 + ln:p + 5 + ln])[0]
        if not (0 < sz <= n - (p + 5 + ln)):
            p += 1
            continue
        out.setdefault(name.decode(), []).append((p + 5 + ln, sz))
        p += 5 + ln + sz
    return out


def replay_end(buf):
    """Byte offset where the embedded replay payload ends (0 if there is none).

    The file carries TWO savestates: an RASTATE header, then an RPLY chunk whose
    payload is a 2VSB movie, then the live core state.  Every duplicated record name
    therefore appears twice -- the replay's *starting* state first, the live one
    second.  Picking [0] silently reads the wrong machine.
    """
    i = buf.find(b"RPLY")
    if i < 0:
        return 0
    j = buf.find(b"2VSB", i)
    if j < 0:
        return 0
    # The movie's size is a u32 LE at RPLY+8 (offset 16 in every capture checked:
    # $000360FD = 221,437 bytes, and the live BaseRAM record follows at 221,552).
    # Returning `j` (the *start* of the replay, 20) filtered nothing at all, so
    # live_records still handed back the replay's starting BaseRAM -- the first
    # occurrence -- and every work-RAM comparison in this project was made against
    # the wrong machine.  Measured on burst5/f1845: BaseRAM@160 has $DB=$00 and
    # 3,911 non-zero bytes (the replay's frame 0), BaseRAM@172712 has $DB=$68 and
    # 97 non-zero bytes (the live machine at pc $8285).  The TAM arithmetic in the
    # copier (`lda cmd; adc $DB`) only reaches bank $7C with $DB=$68, so the live
    # record is the one that agrees with the machine's own observed bank.
    try:
        size = struct.unpack("<I", buf[i + 8:i + 12])[0]
    except struct.error:
        return j
    if j + size > len(buf) or size == 0:
        return j
    return j + size


def live_records(buf):
    """Records from the live core state only (those after the replay payload)."""
    r = records(buf)
    cut = replay_end(buf)
    if not cut:
        return r
    out = {}
    for name, occ in r.items():
        live = [(o, s) for (o, s) in occ if o > cut]
        out[name] = live if live else occ
    return out


def load(path):
    buf = open(path, "rb").read()
    r = live_records(buf)

    def rec(name):
        if name not in r:
            return b""
        off, sz = r[name][0]
        return buf[off:off + sz]

    def one(name, default=0, want=None):
        """Pick the record with the expected size -- the files contain other
        records that happen to share short names ("A" appears as a 32 KB blob)."""
        for (off, sz) in sorted(r.get(name, [])):
            if want is not None and sz != want:
                continue
            if sz == 1:
                return buf[off]
            if sz == 2:
                return struct.unpack("<H", buf[off:off + 2])[0]
            return buf[off:off + sz]
        return default

    st = {
        "pc": one("PC", 0, 2), "a": one("A", 0, 1), "x": one("X", 0, 1),
        "y": one("Y", 0, 1), "p": one("P", 0, 1), "s": one("S", 0, 1),
        "mpr": list(one("MPR", bytes(8), 8)),
        "baseram": rec("BaseRAM"), "cdram": rec("CDRAM"), "vram": rec("VRAM"),
    }
    # the CD BIOS pages its own ROM banks at $C000/$E000; report which bank PC is in
    page = (st["pc"] >> 13) & 7
    st["pc_bank"] = st["mpr"][page] if len(st["mpr"]) > page else 0
    st["pc_page"] = page
    return st


def frame_of(path):
    m = re.search(r"f(\d+)_", path)
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("states", nargs="+")
    ap.add_argument("--detail", action="store_true")
    a = ap.parse_args()
    print("  %-7s %-6s %-5s %-3s %s" % ("frame", "PC", "bnk", "pg", "state"))
    for path in a.states:
        st = load(path)
        print("  %-7d $%04X  $%02X   %d   A=%02X X=%02X Y=%02X P=%02X S=%02X  "
              "baseram=%08X cdram=%08X vram=%08X"
              % (frame_of(path), st["pc"], st["pc_bank"], st["pc_page"],
                 st["a"], st["x"], st["y"], st["p"], st["s"],
                 zlib.crc32(st["baseram"]) & 0xFFFFFFFF,
                 zlib.crc32(st["cdram"]) & 0xFFFFFFFF,
                 zlib.crc32(st["vram"]) & 0xFFFFFFFF))
        if a.detail:
            print("          MPR=%s  pad $2228=%02X $222D=%02X"
                  % (" ".join("%02X" % b for b in st["mpr"]),
                     st["baseram"][0x228] if len(st["baseram"]) > 0x228 else 0,
                     st["baseram"][0x22D] if len(st["baseram"]) > 0x22D else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
