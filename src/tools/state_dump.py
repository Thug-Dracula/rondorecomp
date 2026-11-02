#!/usr/bin/env python3
"""Dump one emulator savestate's video state into a flat blob for the C renderer.

This is the bridge that lets the *real* renderer (video.c, the shim that ships) be
checked against the emulator's own picture: take the video chip's state at a frame,
hand it to our device model, render, and correlate with the screenshot the
emulator produced for the same frame.

Blob format (little-endian, no padding):
    0   "XLVDC1\\0\\0"        8 bytes
    8   12 x uint16          mwr cr bxr byr hsr hdr vsr vdr vcr dcr satb rcr
    32  512 x uint16         VCE palette
    1056  65536 bytes        VRAM (32768 words)
    66592 512 bytes          SAT (64 x 4 words, already DMA'd by the emulator)

Usage: state_dump.py <state> [--out blob] [--json summary.json]
"""
import argparse
import json
import re
import struct
import sys

MAGIC = b"XLVDC1\0\0"
FIELDS = ("mwr", "cr", "bxr", "byr", "hsr", "hdr", "vsr", "vdr", "vcr", "dcr",
          "satb", "rcr")


def records(buf):
    out = {}
    for m in re.finditer(rb"[A-Za-z_][A-Za-z0-9_]{0,40}", buf):
        p = m.start()
        if p < 2:
            continue
        if struct.unpack(">H", buf[p - 2:p])[0] != len(m.group(0)):
            continue
        q = p + len(m.group(0))
        if q + 4 > len(buf):
            continue
        sz = struct.unpack("<I", buf[q:q + 4])[0]
        if 0 < sz <= len(buf) - q - 4:
            out.setdefault(m.group(0).decode(), (q + 4, sz))
    return out


def u16(buf, rec, name, default=0):
    """Read a register record; savestate field sets differ between versions."""
    if name not in rec:
        return default
    off, sz = rec[name]
    if sz >= 2:
        return struct.unpack("<H", buf[off:off + 2])[0]
    return buf[off]


def load_state(path):
    buf = open(path, "rb").read()
    rec = records(buf)
    vram = buf[rec["VRAM"][0]:rec["VRAM"][0] + 65536]
    ct = buf[rec["color_table"][0]:rec["color_table"][0] + 1024]
    sat = buf[rec["SAT"][0]:rec["SAT"][0] + 512]

    # The state stores the *derived* register caches; prefer the raw registers
    # where the emulator keeps them, else rebuild from the caches.
    mwr = u16(buf, rec, "MWR")
    cr = u16(buf, rec, "CR_cache")
    # BXR is not in the savestate; BG_XOffset is its per-line latched value, which
    # equals BXR at the start of a line (verified: both 0 at the frames we use).
    bxr = u16(buf, rec, "BXR", u16(buf, rec, "BG_XOffset"))
    byr = u16(buf, rec, "BYR")
    hsr = u16(buf, rec, "HSR")
    dcr = u16(buf, rec, "DCR")
    satb = u16(buf, rec, "SATB")
    rcr = u16(buf, rec, "RCR", 0)
    hdr = (u16(buf, rec, "HDE_cache") << 8) | u16(buf, rec, "HDW_cache")
    vsr = (u16(buf, rec, "VDS_cache") << 8) | u16(buf, rec, "VSW_cache")
    vdr = u16(buf, rec, "VDW_cache")
    vcr = u16(buf, rec, "VCR_cache")

    return dict(vram=vram, pal=ct, sat=sat, mwr=mwr, cr=cr, bxr=bxr, byr=byr,
                hsr=hsr, hdr=hdr, vsr=vsr, vdr=vdr, vcr=vcr, dcr=dcr,
                satb=satb, rcr=rcr, rec=rec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("state")
    ap.add_argument("--out", default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    st = load_state(a.state)
    out = a.out or (a.state.rstrip("/").replace(".state", "") + ".vdc")
    blob = bytearray()
    blob += MAGIC
    for f in FIELDS:
        blob += struct.pack("<H", st[f] & 0xFFFF)
    blob += st["pal"][:1024]
    blob += st["vram"][:65536]
    blob += st["sat"][:512]
    with open(out, "wb") as fh:
        fh.write(bytes(blob))

    summary = {f: st[f] for f in FIELDS}
    summary["width"] = ((st["hdr"] & 0x7F) + 1) * 8
    summary["height"] = st["vdr"] & 0x1FF
    summary["mwr_bat_width"] = (32, 64, 128, 128)[(st["mwr"] >> 4) & 3]
    summary["mwr_bat_height"] = 64 if (st["mwr"] & 0x40) else 32
    summary["bg_on"] = bool(st["cr"] & 0x80)
    summary["spr_on"] = bool(st["cr"] & 0x40)
    summary["blob"] = out
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(summary, fh, indent=1)
    print("wrote %s (%d bytes): %dx%d, BAT %dx%d, BG %s SPR %s, BYR=%d BXR=%d"
          % (out, len(blob), summary["width"], summary["height"],
             summary["mwr_bat_width"], summary["mwr_bat_height"],
             "on" if summary["bg_on"] else "off",
             "on" if summary["spr_on"] else "off", st["byr"], st["bxr"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
