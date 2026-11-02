#!/usr/bin/env python3
"""Slice the game's data track into the images the translator needs.

The build works from one user-supplied file: the data track of the user's own
disc, as a flat sequence of 2048-byte sectors with the 225-sector pregap in
front (`inputs/data_payload.bin`; see README).  Everything the translator reads
is a slice of that file, taken by sector number, so no game data is stored in
this repository.

Two kinds of slice:

  * the boot chain -- the CD loader, the boot program and the game kernel, each
    a contiguous run of sectors (the loader's own `read` command names the boot
    program's start sector, and its length);
  * the game module's banks -- 24 banks of 8 KB, each one a contiguous run of
    four sectors, taken from the record table the loader builds at boot.

Usage:

    slice_disc.py PAYLOAD --out DIR          # write loader/bootprog/kernel/banks
    slice_disc.py PAYLOAD --check            # just verify the payload shape

Exit status: 0 on success, 1 if the payload is missing or too short for a slice
(a wrong or truncated dump reports here rather than deep inside the translator).
"""
import argparse
import os
import sys

SECTOR = 2048
PREGAP = 225                     # zero sectors ahead of record 0 in the payload

# name, first record, sector count
BOOT_CHAIN = [
    ("loader",   2,  1),         # IPL stage 2, runs at $3000
    ("bootprog", 35, 20),        # record $23, the loader reads 20 sectors to $4000
    ("kernel",   67, 6),         # the game kernel's first 6 sectors, at $4000
]

# bank -> disc record of its first sector; 8 KB = four 2048-byte sectors.
BANKS = {
    0x69: 67,  0x6A: 71,  0x6B: 193, 0x6C: 197, 0x6D: 225, 0x6E: 229,
    0x6F: 221, 0x70: 213, 0x71: 217, 0x72: 209, 0x73: 201, 0x74: 205,
    0x75: 77,   0x76: 81,  0x77: 85,  0x78: 97,  0x79: 101, 0x7A: 105,
    0x7B: 109, 0x7C: 113, 0x7D: 117, 0x7E: 121, 0x7F: 125,
}


def offset(record, sectors):
    """Payload byte range of `sectors` sectors starting at `record`."""
    start = (record + PREGAP) * SECTOR
    return start, start + sectors * SECTOR


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload", help="data track: 2048-byte sectors, pregap included")
    ap.add_argument("--out", help="directory to write the slices into")
    ap.add_argument("--check", action="store_true",
                    help="only report the payload's shape")
    a = ap.parse_args()

    try:
        payload = open(a.payload, "rb").read()
    except OSError as e:
        print("slice_disc: %s" % e, file=sys.stderr)
        return 1
    nsec = len(payload) // SECTOR
    print("%s: %d bytes, %d sectors of %d" % (a.payload, len(payload), nsec, SECTOR))
    if a.check or not a.out:
        return 0 if nsec else 1

    os.makedirs(a.out, exist_ok=True)
    failed = 0
    for name, rec, count in BOOT_CHAIN:
        lo, hi = offset(rec, count)
        if hi > len(payload):
            print("slice_disc: %s needs records %d..%d, payload ends at %d"
                  % (name, rec, rec + count - 1, nsec - 1 - PREGAP), file=sys.stderr)
            failed = 1
            continue
        path = os.path.join(a.out, name + ".bin")
        with open(path, "wb") as f:
            f.write(payload[lo:hi])
        print("  %-9s record %3d +%2d -> %s" % (name, rec, count, path))

    for bank, rec in sorted(BANKS.items()):
        lo, hi = offset(rec, 4)
        if hi > len(payload):
            print("slice_disc: bank $%02X needs record %d, payload ends at %d"
                  % (bank, rec, nsec - 1 - PREGAP), file=sys.stderr)
            failed = 1
            continue
        with open(os.path.join(a.out, "b%02X.bin" % bank), "wb") as f:
            f.write(payload[lo:hi])

    if failed:
        return 1
    print("  %d banks of 8192 bytes, records from the loader's own table" % len(BANKS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
