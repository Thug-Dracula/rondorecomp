#!/usr/bin/env python3
"""Convert an oracle savestate into the compact image xl_load_state() reads.

Layout (little-endian): magic "XLST1\\0\\0\\0", PC u16, A X Y S P u8, MPR[8],
12 VDC registers u16 (mwr cr bxr byr hsr hdr vsr vdr vcr dcr satb rcr),
512 palette u16, 32768 VRAM u16, 256 SAT u16, 196608 SysCardRAM, 8192 BaseRAM,
65536 CDRAM, then an OPTIONAL drive block (see below).

The last of the fixed fields is the console's *other* 64 KB of CD memory,
physical banks $80-$87.  The core keeps it separate from the 192 KB Super CD RAM
at $68-$7F (huc.cpp: `CDRAM` vs `SysCardRAM`, both in the save state), and the
two do not mirror each other.  Leaving it out of the image left those banks zero
in our port, which is where the stage code's sprite patterns live: the background
was right and every character sprite was missing.

THE DRIVE BLOCK (added 2026-10-29, the --from-state bypass route)
  A state taken while the CD unit has a command in flight cannot be continued
  without the drive: the card's own reader ($E95E) polls $1800 for $D0/$98/$88/$80
  and spins for ever on a drive that answers "bus free".  Measured: injecting the
  console's frame 28,200 state without this block leaves the machine in that loop
  at pc $E95E-$E974 for every frame of a 2,000-frame run.

  The block is written only when the state carries the SCSI records, and it is
  *trailing*, so older images load exactly as before (the loader stops after
  CDRAM).  Fields:

    magic "XDEV1\\0\\0\\0"
    u8  phase            our CD_PHASE_* (0 bus free, 1 command, 2 data in,
                         3 status, 4 message)
    u8  data_kind        0 none, 2 sectors
    u8  data_have        a sector (or response byte) is on the bus
    u8  req              the drive is asking for/offering a byte
    u8  port3            $1803 as the console has it
    u8  irq_mask         $1802 as the console has it
    u8  adpcm_lastcmd    $180D's latched value
    u8  adpcm_half       $1803 bit 2 source
    u8  adpcm_end        $1803 bit 3 source
    u8  cdda_playing     1/2 play/pause, derived from $1803 bit 4
    u8  status_byte      the byte the status phase offers
    u8  pad
    u32 read_lba         LBA of the sector in the FIFO (or the next one due)
    u32 read_count       sectors left including that one
    u32 data_pos         bytes of that sector already handed to the CPU
    u64 next_fill_delta  master clocks (3 per CPU cycle) until the next sector

THE SAVE-CARD BLOCK (added 2026-10-30)

  The same trailing mechanism, a second tag, for the machine's battery-backed
  2 KB backup RAM (the oracle's `SaveRAM` record).  It is a device of its own:
  the card's file manager addresses it as physical bank $F7, it survives a
  reset, and it is NOT any part of SysCardRAM/CDRAM.

    magic "XBRM1\\0\\0\\0"
    u8   2048 bytes, the card exactly as the oracle has it

  Present only when the state has a 2048-byte `SaveRAM` record.  It is appended
  AFTER the drive block, so the order is: fixed layout, [XDEV1], [XBRM1].  The
  fixed layout is unchanged and both blocks are optional, so every image written
  before this exists still loads byte-identically.
"""
import struct, sys
sys.path.insert(0, __file__.rsplit('/', 2)[0] + '/tools')
import oracle_state as os

REGS = ('mwr','cr','bxr','byr','hsr','hdr','vsr','vdr','vcr','dcr','satb','rcr')

# Mednafen's SCSI phases (cdrom/scsicd.cpp) -> ours (runtime.h, CD_PHASE_*).
PHASE_MAP = {0: 0, 1: 1, 2: 2, 4: 3, 5: 4, 6: 4}
CD_DATA_NONE, CD_DATA_SECTORS = 0, 2


def _int(r, buf, name, default=0, want=None):
    occ = r.get(name)
    if not occ:
        return default
    for off, sz in sorted(occ):
        if want is not None and sz != want:
            continue
        if sz == 1:
            return buf[off]
        if sz == 2:
            return struct.unpack('<H', buf[off:off + 2])[0]
        if sz == 4:
            return struct.unpack('<i', buf[off:off + 4])[0]
        return buf[off:off + sz]
    return default


def device_block(r, buf):
    """The drive's in-flight state, or b'' when the state has no SCSI section."""
    if 'CurrentPhase' not in r:
        return b''
    phase = _int(r, buf, 'CurrentPhase')
    din_in = _int(r, buf, 'din_in_count')
    sector_addr = _int(r, buf, 'SectorAddr')
    sector_count = _int(r, buf, 'SectorCount')
    timer = _int(r, buf, 'CDReadTimer')
    done = _int(r, buf, 'data_transfer_done')
    db = _int(r, buf, 'DB')
    port3 = _int(r, buf, 'port3')
    if 'port3' not in r and '_Port' in r:
        port = _int(r, buf, '_Port')
        if isinstance(port, (bytes, bytearray)) and len(port) >= 4:
            port3 = port[3]
    irq_mask = 0
    if '_Port' in r:
        port = _int(r, buf, '_Port')
        if isinstance(port, (bytes, bytearray)) and len(port) >= 3:
            irq_mask = port[2]

    kind = CD_DATA_NONE
    have = req = 0
    read_lba = read_count = data_pos = 0
    status_byte = 0

    if phase == 2:                                  # DATA_IN
        kind = CD_DATA_SECTORS
        if din_in > 0:
            read_lba = sector_addr - 1
            read_count = sector_count + 1
            data_pos = 2048 - din_in
            have = req = 1
        else:
            read_lba = sector_addr
            read_count = sector_count
    elif phase == 1 and sector_count > 0:           # COMMAND, a Read is armed
        kind = CD_DATA_SECTORS
        read_lba = sector_addr
        read_count = sector_count
    elif phase == 4:                                # STATUS
        status_byte = db
        have = req = 1

    out = bytearray(b'XDEV1\0\0\0')
    out += bytes([PHASE_MAP.get(phase, 0), kind, have, req,
                  port3 & 0xFF, irq_mask & 0xFF,
                  _int(r, buf, 'ADPCM.LastCmd') & 0xFF,
                  _int(r, buf, 'ADPCM.HalfReached') & 0xFF,
                  _int(r, buf, 'ADPCM.EndReached') & 0xFF,
                  1 if (port3 & 0x10) else 0,
                  status_byte & 0xFF, 0])
    out += struct.pack('<IIIQ', read_lba, read_count, data_pos,
                       max(timer, 0) if (kind == CD_DATA_SECTORS and not done) else 0)
    assert len(out) == 40, len(out)
    return bytes(out)


def bram_block(r, buf):
    """The machine's save card (the 2 KB backup RAM), or b'' when the state has none.

    THE STATE CARRIES IT AND THE IMAGE DID NOT, which is why a route injected from a
    savestate could not read a save file: the card's `bm_read` (`$E04E` = `$F8E3`)
    looks the name up in the directory at $8010 of the *backup RAM*, and an image
    without it hands the machine the power-on formatted-but-empty card
    (`runtime.h`, "THE BACKUP RAM POWERS ON FORMATTED, NOT BLANK"), so the answer is
    "no such file" and the game's `$3F83` ("a save exists") never sets.

    In the oracle's state it is the `SaveRAM` record inside the `HuC` section
    (`pce/huc.cpp`, `HuC_StateAction`: `SFARRAY(SaveRAM, ... 2048)`), 2048 bytes,
    written after PopRAM/TsushinRAM and before CDRAM.  It is a *separate* device
    from SysCardRAM and CDRAM, which is why the name does not appear anywhere else
    in the image.

    Emitted as a magic-tagged trailing block, the way XDEV1 was: the fixed layout is
    untouched, so an image without it loads exactly as before and a reader that does
    not know the tag stops at it."""
    if 'SaveRAM' not in r:
        return b''
    o, s = r['SaveRAM'][-1]
    if s != 2048:
        raise SystemExit("SaveRAM is %d bytes, expected 2048" % s)
    return b'XBRM1\0\0\0' + buf[o:o + s]


def main(src, dst):
    buf = open(src,'rb').read()
    r = os.live_records(buf)
    def blob(n):
        o,s = r[n][-1]
        return buf[o:o+s]
    out = bytearray(b'XLST1\0\0\0')
    st = os.load(src)
    out += struct.pack('<H', st['pc'])
    out += bytes([st['a'], st['x'], st['y'], st['s'], st['p']])
    out += bytes(st['mpr'][:8])
    for name in REGS:
        v = struct.unpack('<H', blob(name.upper())[:2])[0]
        out += struct.pack('<H', v)
    out += blob('color_table')[:1024]
    out += blob('VRAM')[:65536]
    out += blob('SAT')[:512]
    out += blob('SysCardRAM')[:196608]
    out += blob('BaseRAM')[:8192]
    out += blob('CDRAM')[:65536]
    dev = device_block(r, buf)
    out += dev
    brm = bram_block(r, buf)
    out += brm
    open(dst,'wb').write(out)
    print("wrote %s (%d bytes) from %s%s%s" %
          (dst, len(out), src, "  [drive block included]" if dev else
                              "  [no drive block: state has no SCSI section]",
           "  [save card included]" if brm else
           "  [no save card: state has no SaveRAM record]"))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
