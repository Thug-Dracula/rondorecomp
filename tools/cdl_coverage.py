#!/usr/bin/env python3
"""Measure the translated program against the content-keyed CDL.

This recomputes the coverage number from the generated C and a code/data log,
with these definitions:

  * executed code bytes of an image = its `op` and `arg` marks (union, runs of
    opcode + operand).  Summed over images, never deduplicated: the same offset
    in two overlays is two different bytes of program.
  * covered = executed code bytes inside the instruction extents of the units
    the translator emitted for that bank image.
  * the translator's own bank images are the 8 KB banks it built from its
    inputs, identified by their SHA-1 (``BankImages`` in recompiler.py).

**No coverage figure is written down here on purpose.** It moves every time the
translator emits more.  Run the tool for the current number; the last line of its
output is the figure, and `--json` writes it under `covered` / `executed_all`.

It reads the artifacts the CDL run wrote (read only):

  --cdl DIR      cdl-images.bin, cdl-imagebytes.bin, cdl-images.idx
  --keyset PATH  keyset.json (same content, as JSON)
  --cfile PATH   the generated C (default: out/boot.c)

Both unit-naming schemes are understood, so the same tool measures the build
before and after the CDL-driven expansion:

  u_BB_OOOO            bank + offset (the original emitter)
  u_<sha8>_pP_OOOO     image content hash + logical page + offset (current)

  python3 tools/cdl_coverage.py --cfile src/out/boot.c --json src/out/coverage.json
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # repo/tools
SRC = os.path.join(os.path.dirname(HERE), "src")           # the port
sys.path.insert(0, SRC)                                    # for isa

BMAP = 1024
BANK = 8192

OLD_UNIT = re.compile(r"^static void u_([0-9A-F]{2})_([0-9A-F]{4})\(Cpu \*c\)", re.M)
NEW_UNIT = re.compile(r"^static void u_([0-9a-f]{8})_p([0-7])_([0-9A-F]{4})\(Cpu \*c\)", re.M)
IMAGE_LINE = re.compile(
    r"^ \*   (\S+)\s+bank \$([0-9A-F]{2}) from \$([0-9A-F]{4,6}) \((\d+) bytes\)", re.M)
ARRAY = re.compile(r"^static const uint8_t (img|ref)_(\d+)\[\] = \{(.*?)\};", re.M | re.S)


def bit(bm, cat, off):
    return (bm[cat * BMAP + (off >> 3)] >> (off & 7)) & 1


def read_images(path):
    d = open(path, "rb").read()
    assert d[:4] == b"RCDL", "bad magic in %s" % path
    ver, n, bmap, ncat = struct.unpack_from("<IIII", d, 4)
    assert bmap == BMAP and ncat == 4, (bmap, ncat)
    off = 20
    imgs = []
    for _ in range(n):
        sha1 = d[off:off + 20].hex()
        off += 20
        cnt = struct.unpack_from("<4I", d, off)
        off += 16
        bm = d[off:off + 4 * BMAP]
        off += 4 * BMAP
        imgs.append({"sha1": sha1, "cnt": cnt, "bm": bm})
    assert off == len(d), (off, len(d))
    return imgs


def read_imagebytes(path):
    d = open(path, "rb").read()
    assert d[:5] == b"RCDLB", "bad magic in %s" % path
    off = 12
    out = {}
    while off + 20 + BANK <= len(d):
        sha1 = d[off:off + 20].hex()
        off += 20
        out[sha1] = d[off:off + BANK]
        off += BANK
    return out


def parse_c(path):
    """(images, units, units_by_image) from the generated C.

    units: {(bank, off)} for the old naming, or {('sha', off)}; returned as
    (kind, list) so the caller can key them.  units_by_image maps a bank image
    SHA-1 to the set of offsets that start a unit decoded from it.
    """
    src = open(path, errors="replace").read()
    images = []
    for m in IMAGE_LINE.finditer(src):
        images.append({"name": m.group(1), "bank": int(m.group(2), 16),
                       "base": int(m.group(3), 16), "size": int(m.group(4)),
                       "data": None})
    arrays = {}
    for m in ARRAY.finditer(src):
        arrays[int(m.group(2))] = bytes(int(x, 16)
                                        for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(3)))
    for i, img in enumerate(images):
        assert i in arrays, "array %d missing from %s" % (i, path)
        img["data"] = arrays[i]
        assert len(img["data"]) == img["size"], (img["name"], len(img["data"]), img["size"])

    old = {(int(a, 16), int(o, 16)) for a, o in OLD_UNIT.findall(src)}
    new = {(h, int(o, 16)) for h, p, o in NEW_UNIT.findall(src)}
    return images, old, new


def bank_images(images):
    """Reproduce BankImages.add(): each input image's bytes at (bank, logical base),
    spilling into the following banks at 8 KB boundaries."""
    banks = {}
    for img in images:
        pos, addr, b = 0, img["base"], img["bank"]
        while pos < len(img["data"]):
            off = addr & 0x1FFF
            chunk = min(BANK - off, len(img["data"]) - pos)
            buf = banks.setdefault(b & 0xFF, bytearray(BANK))
            buf[off:off + chunk] = img["data"][pos:pos + chunk]
            pos += chunk
            addr += chunk
            b += 1
    return banks


def len_table():
    sys.path.insert(0, SRC)
    from huc_table import TABLE
    return [t[1] for t in TABLE]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cdl", default=os.environ.get("CDL_DIR"),
                    help="directory holding the code/data log to measure "
                         "(default: $CDL_DIR)")
    ap.add_argument("--keyset", default=None,
                    help="keyset.json (default: <cdl>/keyset.json)")
    ap.add_argument("--cfile", default=os.path.join(SRC, "out", "boot.c"))
    ap.add_argument("--json", default=None, help="write the numbers here as JSON")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    keyset_path = a.keyset or os.path.join(a.cdl, "keyset.json")
    imgs = read_images(os.path.join(a.cdl, "cdl-images.bin"))
    ibytes = read_imagebytes(os.path.join(a.cdl, "cdl-imagebytes.bin"))
    keys = json.load(open(keyset_path))["images"]
    ximages, old_units, new_units = parse_c(a.cfile)
    LT = len_table()

    banks = bank_images(ximages)
    canon = {b: hashlib.sha1(bytes(buf)).hexdigest() for b, buf in banks.items()}
    by_sha = {i["sha1"]: i for i in imgs}

    # unit offsets per bank image.  Old naming: (bank, off) -> that bank's image.
    # New naming: (sha8, off) -> the image whose hash starts with sha8.
    per_image_units = {}
    for (b, o) in old_units:
        per_image_units.setdefault(canon.get(b), set()).add(o)
    sha8 = {}
    for h in set(canon.values()):
        assert h[:8] not in sha8 or sha8[h[:8]] == h, "sha8 collision: %s" % h[:8]
        sha8[h[:8]] = h
    unknown_hash = set()
    for (h8, o) in new_units:
        full = sha8.get(h8)
        if full is None:
            unknown_hash.add(h8)
            continue
        per_image_units.setdefault(full, set()).add(o)

    rows = []
    exec_total = trans_total = gap_total = 0
    gap_known = gap_new = 0
    for b in sorted(banks):
        sha = canon[b]
        img = by_sha.get(sha)
        if img is None:
            rows.append((b, sha, None, 0, 0, 0, 0, 0))
            continue
        bm = img["bm"]
        code = [o for o in range(BANK) if bit(bm, 0, o) or bit(bm, 1, o)]
        cover = bytearray(BANK)
        tb = 0
        for o in per_image_units.get(sha, ()):
            n = LT[banks[b][o]]
            for k in range(n):
                if o + k < BANK:
                    cover[o + k] = 1
            tb += n
        unit_off = per_image_units.get(sha, set())
        unt = sum(1 for o in code if not cover[o])
        # Split the gap: a byte whose instruction *start* the translator did emit
        # (a decoder disagreement) vs one it never emitted at all (undiscovered).
        unt_known = 0
        for o in code:
            if cover[o]:
                continue
            k = o
            while k >= 0 and k > o - 8 and not bit(bm, 0, k):
                k -= 1
            if k in unit_off:
                unt_known += 1
        gap_known += unt_known
        gap_new += unt - unt_known
        gap_total += unt
        exec_total += len(code)
        trans_total += tb
        rows.append((b, sha, len(code), tb, unt, unt_known, unt - unt_known, len(unit_off)))

    canon_set = set(canon.values())
    unseen = [i for i in imgs if i["sha1"] not in canon_set and (i["cnt"][0] or i["cnt"][1])]
    unseen_code = sum(i["cnt"][0] + i["cnt"][1] for i in unseen)
    all_code = sum(i["cnt"][0] + i["cnt"][1] for i in imgs)
    covered = exec_total - gap_total

    out = []
    out.append("CDL coverage of %s" % a.cfile)
    out.append("")
    out.append("| bank | image | units | executed | translated | untranslated |"
               " opcode known / unknown |")
    out.append("|---|---|---|---|---|---|---|")
    for (b, sha, ex, tb, unt, uk, un, nu) in rows:
        if ex is None:
            out.append("| $%02X | `%s` | %d | **no CDL image** | - | - | - |"
                       % (b, sha[:12], nu))
        else:
            out.append("| $%02X | `%s` | %d | %d | %d | **%d** | %d / %d |"
                       % (b, sha[:12], nu, ex, tb, unt, uk, un))
    out.append("")
    out.append("executed code bytes, all images (sum):        %d" % all_code)
    out.append("executed code bytes, translated images:       %d" % exec_total)
    out.append("  covered by the translation:                 %d" % covered)
    out.append("  not covered:                                %d" % gap_total)
    out.append("translated bytes in those images:             %d" % trans_total)
    out.append("executed code in untranslated images:          %d images, %d bytes"
               % (len(unseen), unseen_code))
    out.append("")
    out.append("COVERAGE %d of %d executed code bytes (%.1f%%)"
               % (covered, all_code, 100.0 * covered / all_code if all_code else 0.0))
    if unknown_hash:
        out.append("WARNING: %d unit hash prefixes name no bank image: %s"
                   % (len(unknown_hash), " ".join(sorted(unknown_hash))))
    text = "\n".join(out)
    print(text)
    if a.json:
        json.dump({"cfile": os.path.abspath(a.cfile),
                   "executed_all": all_code,
                   "executed_translated_images": exec_total,
                   "covered": covered,
                   "not_covered": gap_total,
                   "translated_bytes": trans_total,
                   "untranslated_images": len(unseen),
                   "untranslated_image_code_bytes": unseen_code,
                   "units": len(old_units) + len(new_units),
                   "per_bank": [
                       {"bank": b, "sha1": sha,
                        "units": nu,
                        "executed": ex, "translated": tb, "untranslated": unt}
                       for (b, sha, ex, tb, unt, _uk, _un, nu) in rows]},
                  open(a.json, "w"), indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
