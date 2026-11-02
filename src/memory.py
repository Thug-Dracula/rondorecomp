#!/usr/bin/env python3
"""Explicit address-space model of a PC Engine Super CD-ROM2 machine (M1 scope).

Why a model at all, instead of a flat 64 KB array: the machine has no MMU tricks,
but it *does* have eight 8 KB bank registers (MPR0..MPR7) that the program rewrites
at runtime with TAM, and two regions that are not memory at all (the hardware
register page and the CD BIOS).  If those three things are buried inside a flat
array, then a bank switch silently aliases code, and hardware access silently
looks like RAM.  Here every read and write goes through a method, so:

  * bank switching is visible (MPR writes are recorded),
  * hardware register access is observable and can later be routed to real shims,
  * CD BIOS calls are recorded rather than executed.

Layout (physical, 256 banks of 8 KB = 2 MB):

  bank $00-$1F   CD BIOS ROM (read-only; visible at $E000 when MPR7 = 0)
  bank $80-$9F   Super CD-ROM2 extra RAM ("CD work RAM"), 256 KB
  bank $F8-$FB   work RAM, visible at $2000-$3FFF when MPR1 = $F8
  bank $FF       hardware registers (VDC/VCE/PSG/timer/IRQ/CD), 8 KB page

Verified against the boot chain, not assumed:
  * the IPL loader reads $FFF5 from the BIOS ROM and pages banks $80.. in, then
    stops 20 sectors of disc data at $4000 -- so the CD work RAM base bank is
    $80 and the disc read really does land in banks $80+;
  * `st0/st1/st2` write to physical offsets 0/2/3 of bank $FF regardless of the
    current MPR mapping (that is what the hardware does), so they bypass the
    logical mapping here too.

Not modelled (deliberately, recorded so nobody assumes otherwise):
  * video timing, VDC/VCE behaviour (registers are stored, nothing renders)
  * PSG synthesis, CD audio, ADPCM
  * interrupts (nothing is ever raised; the I flag is just a bit)
  * cycle counts (no timing at all yet)
"""
import os

BANK_SIZE = 8192
BANK_COUNT = 256
PHYS_SIZE = BANK_SIZE * BANK_COUNT

IO_BANK = 0xFF
CDRAM_BASE = 0x80          # first bank of Super CD-ROM2 extra RAM
CDRAM_BANKS = 32           # 256 KB
ROM_BASE = 0x00            # BIOS occupies physical banks 0..31 (256 KB)
ROM_BANKS = 32
WORKRAM_BANK = 0xF8        # MPR1 value the IPL leaves behind (satb/zp at $2000)
ZP_BASE = 0x2000           # HuC6280 zero page is $2000-$20FF, not $0000
STACK_BASE = 0x2100        # stack is $2100-$21FF

# Hardware register offsets inside the bank-$FF page.
HW_VDC = (0x0000, 0x0007)
HW_VCE = (0x0400, 0x07FF)
HW_PSG = (0x0800, 0x0BFF)
HW_TIMER = (0x0C00, 0x0FFF)        # reload/counter (even) + control (odd), mirrored
HW_INPUT = (0x1000, 0x13FF)        # joypad port, one nibble per select
HW_IRQ = (0x1400, 0x17FF)          # $1402 mask (inverted), $1403 status; mirrored
HW_CD = (0x1800, 0x18FF)


def hw_region(off):
    if HW_VDC[0] <= off <= HW_VDC[1]:
        return "vdc"
    if HW_VCE[0] <= off <= HW_VCE[1]:
        return "vce"
    if HW_PSG[0] <= off <= HW_PSG[1]:
        return "psg"
    if HW_TIMER[0] <= off <= HW_TIMER[1]:
        return "timer"
    if HW_INPUT[0] <= off <= HW_INPUT[1]:
        return "input"
    if HW_IRQ[0] <= off <= HW_IRQ[1]:
        return "irq"
    if HW_CD[0] <= off <= HW_CD[1]:
        return "cd"
    return "other"


class Hardware:
    """The bank-$FF register page.  Records every access, models nothing."""

    def __init__(self, log=None):
        self.log = log
        self.regs = {}
        self.writes = []            # (offset, value) in order
        self.reads = []
        self.vdc_writes = []        # only $0000-$0007, the VDC port pair
        self.unknown = set()

    def read(self, off):
        val = self.regs.get(off, 0)
        self.reads.append((off, val))
        self.unknown.add(off)
        return val

    def write(self, off, val):
        self.regs[off] = val
        self.writes.append((off, val))
        if off <= 0x0007:
            self.vdc_writes.append((off, val))
        else:
            self.unknown.add(off)
        if self.log and off > 0x0007:
            self.log.log("hw write %s[$%04X] = $%02X" % (hw_region(off), off, val))

    def summary(self):
        out = {}
        for off, _ in self.writes:
            out[off] = out.get(off, 0) + 1
        return out


class Disc:
    """The extracted data track (flat 2048-byte payload sectors, 225-sector pregap).

    Record numbering used by the game/BIOS: payload offset = (record + 225) * 2048.
    The BIOS's own sector numbers are record + $100 (verified: the loader asks for
    sector $0123 and the boot program is record $23).
    """

    PREGAP = 225
    SECTOR = 2048

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        self.sector_count = len(self.data) // self.SECTOR

    def record_of_sector(self, sector):
        return sector - 0x100

    def read_record(self, record, count):
        off = (record + self.PREGAP) * self.SECTOR
        end = off + count * self.SECTOR
        if record < 0 or off < 0 or off >= len(self.data):
            return None
        return self.data[off:min(end, len(self.data))]


class AddressSpace:
    """64 KB logical space over 256 physical 8 KB banks, plus stub bookkeeping."""

    def __init__(self, log=None, disc=None, bios_path=None):
        self.log = log
        self.mem = bytearray(PHYS_SIZE)
        # IPL leaves: IO page at $0000, work RAM at $2000, BIOS at $E000.
        self.mpr = [IO_BANK, WORKRAM_BANK, CDRAM_BASE, CDRAM_BASE + 1,
                    CDRAM_BASE + 2, CDRAM_BASE + 3, CDRAM_BASE + 4, 0x00]
        self.hw = Hardware(log)
        self.disc = disc
        self.rom_present = False
        self.rom_writes_ignored = 0
        # One CPU cycle stolen per VDC register-port ACCESS, read or write (the
        # reference core's `HuCPU.StealCycle()`: IOWrite and IRead, case 0x0000 --
        # pce.cpp).  Counted at the choke points every I/O access passes through,
        # so refcpu.py charges it exactly where the generated C and the C
        # interpreter do -- a steal only one engine charges is a cycle-total
        # difference, and tests/test_diff.py is what catches it.  The gate is
        # $0000-$0003 because that is the window runtime.c dispatches to the VDC;
        # the chip's mirror over $0000-$03FF is not modelled by either C engine,
        # and this must match them rather than the chip.
        #
        # The VDC's own wait states (a data-port access stalling while the port is
        # busy) are NOT modelled here: this class records accesses and models
        # nothing, and the differential cases are short enough that the chip's port
        # window never closes.  They are measured where they matter -- the fill-loop
        # and burst-frame traces (video.c).
        self.io_steals = 0
        if bios_path and os.path.exists(bios_path):
            with open(bios_path, "rb") as f:
                rom = f.read(ROM_BANKS * BANK_SIZE)
            self.mem[0:len(rom)] = rom
            self.rom_present = True
        self.mpr_writes = []        # (mask, value) log of TAM operations

    # -- physical addressing -------------------------------------------------
    def phys(self, addr):
        addr &= 0xFFFF
        return self.mpr[addr >> 13] * BANK_SIZE + (addr & 0x1FFF)

    def phys_bank(self, addr):
        return self.mpr[(addr & 0xFFFF) >> 13]

    # -- byte access ---------------------------------------------------------
    def read(self, addr):
        addr &= 0xFFFF
        bank = self.mpr[addr >> 13]
        off = addr & 0x1FFF
        if bank == IO_BANK:
            if off <= 0x0003:
                self.io_steals += 1        # VDC register port: StealCycle
            return self.hw.read(off)
        return self.mem[bank * BANK_SIZE + off]

    def write(self, addr, val):
        addr &= 0xFFFF
        bank = self.mpr[addr >> 13]
        off = addr & 0x1FFF
        val &= 0xFF
        if bank == IO_BANK:
            if off <= 0x0003:
                self.io_steals += 1        # VDC register port: StealCycle
            self.hw.write(off, val)
            return
        if bank < ROM_BANKS and self.rom_present:
            self.rom_writes_ignored += 1
            return
        self.mem[bank * BANK_SIZE + off] = val

    def read_word(self, addr):
        return self.read(addr) | (self.read(addr + 1) << 8)

    def write_word(self, addr, val):
        self.write(addr, val & 0xFF)
        self.write(addr + 1, (val >> 8) & 0xFF)

    def read_phys(self, bank, off):
        return self.mem[(bank * BANK_SIZE + (off & 0x1FFF)) % PHYS_SIZE]

    def write_phys(self, bank, off, val):
        if bank == IO_BANK and (off & 0x1FFF) <= 0x0003:
            self.io_steals += 1            # ST0/ST1/ST2 land on the VDC too
        self.mem[(bank * BANK_SIZE + (off & 0x1FFF)) % PHYS_SIZE] = val & 0xFF

    # -- zero page (relocated: $2000 | zp) -----------------------------------
    @staticmethod
    def zp_addr(z):
        return ZP_BASE | (z & 0xFF)

    def read_zp(self, z):
        return self.read(ZP_BASE | (z & 0xFF))

    def write_zp(self, z, v):
        self.write(ZP_BASE | (z & 0xFF), v)

    def read_word_zp(self, z):
        """16-bit pointer from zero page. High byte wraps inside the page.

        (Matches the reference core: the pointer high byte is fetched at z+1 with
        8-bit arithmetic, so a pointer at $FF reads $FF then $00 of the page.)
        """
        lo = self.read_zp(z)
        hi = self.read_zp((z + 1) & 0xFF)
        return lo | (hi << 8)

    # -- banks ---------------------------------------------------------------
    def tam(self, mask, value):
        for i in range(8):
            if mask & (1 << i):
                self.mpr[i] = value & 0xFF
        self.mpr_writes.append((mask, value & 0xFF))

    def tma(self, mask):
        val = 0
        for i in range(8):
            if mask & (1 << i):
                val = self.mpr[i]
        return val

    # -- loading images ------------------------------------------------------
    def load_visible(self, base, data):
        for i, b in enumerate(data):
            self.write(base + i, b)

    def load_phys(self, bank, off, data):
        start = bank * BANK_SIZE + off
        self.mem[start:start + len(data)] = data

    def snapshot(self):
        return {
            "mpr": list(self.mpr),
            "crc": crc32_mem(self.mem),
            "rom_writes_ignored": self.rom_writes_ignored,
        }


def crc32_mem(mem):
    import zlib
    return zlib.crc32(mem) & 0xFFFFFFFF


class CDStub:
    """CD BIOS entries: recorded, not executed.

    The only modelled entry is the sector read at $E009.  Its parameter block was
    read off the loader's own code and confirmed by the fact that the bytes it
    moves are the boot program we already disassembled:

        $F8/$F9  sector count (mode 0/1) or byte count (mode $FE)
        $FA/$FB  destination address        (loader: $4000)
        $FC/$FD  destination bank           (loader: 0 -> CD RAM base $80)
        $FE      starting sector, 8 bits    (loader: $23 = record 35)
        $FF      transfer mode: 0/1 sectors, $FE bytes, $FF sectors

        Read off the BIOS's own dispatcher at $EC05 and its sector-count helper at
        $ED9F; the loader's read (mode 1, sector $23, 20 sectors to $4000) is the
        proof, because it reproduces the boot program byte for byte.

    Destination is resolved as physical banks ($80 + bank), 8 KB pages, starting at
    offset (dest_addr & $1FFF).  That is the model that makes the loader work: it
    pages banks $80-$83 in at $4000-$9FFF from the BIOS constant at $FFF5 = $80,
    so record $23's first byte has to appear at $4000.

    Every other entry returns A=0, X=0, C=0 and is logged as unmodelled.
    """

    READ_ENTRIES = {0xE009: "CD_READ"}
    OTHER_NAMES = {0xE000: "CD_BOOT", 0xE003: "CD_READ0", 0xE006: "CD_OTHER6",
                   0xE00C: "CD_?", 0xE00F: "CD_SEEK", 0xE05A: "CD_SYSREPORT"}

    def __init__(self, space, log=None):
        self.space = space
        self.log = log
        self.calls = []             # dicts, in order
        self.reads = []             # only the modelled sector reads

    def _params(self):
        sp = self.space
        # $FC/$FD/$FE are the record number H/M/L, most significant byte first
        # (Hudson BIOS manual, RESEARCH-pce-hardware.md 2.1).  `sector` is kept as
        # the low byte for existing callers; the read itself uses `record`.
        return {
            "count": sp.read_zp(0xF8) | (sp.read_zp(0xF9) << 8),
            "dest_addr": sp.read_zp(0xFA) | (sp.read_zp(0xFB) << 8),
            "dest_bank": sp.read_zp(0xFC) | (sp.read_zp(0xFD) << 8),
            "sector": sp.read_zp(0xFE),
            "record": ((sp.read_zp(0xFC) << 16) | (sp.read_zp(0xFD) << 8)
                       | sp.read_zp(0xFE)),
            "mode": sp.read_zp(0xFF),
        }

    def call(self, cpu, pc):
        """Handle entry at `pc`; returns nothing, resumes via RTS semantics."""
        params = self._params()
        rec = {"pc": pc, "name": self.OTHER_NAMES.get(pc, "CD_BIOS"),
               "a_in": cpu.a, "x_in": cpu.x, "y_in": cpu.y, "p_in": cpu.p,
               "params": params, "modelled": False, "bytes": 0, "record": None}
        if pc in self.READ_ENTRIES:
            rec["name"] = self.READ_ENTRIES[pc]
            rec.update(self._do_read(params))
        self.calls.append(rec)
        if self.log:
            self.log.log("CD BIOS call at $%04X %s params count=%d dest=$%04X bank=%d "
                         "sector=$%04X record=%s %s" % (
                             pc, rec["name"], params["count"], params["dest_addr"],
                             params["dest_bank"], params["sector"],
                             "-" if rec["record"] is None else "$%02X" % rec["record"],
                             "MODELLED %d bytes" % rec["bytes"] if rec["modelled"]
                             else "unmodelled (returns A=0,X=0,C=0)"))
        # BIOS returns: success, no error, system-card report 0 (Super CD-ROM2).
        cpu.a = 0
        cpu.x = 0
        cpu.p &= ~0x01
        cpu.pc = (self._pop16(cpu) + 1) & 0xFFFF

    # -- helpers -------------------------------------------------------------
    def _pop16(self, cpu):
        cpu.s = (cpu.s + 1) & 0xFF
        lo = self.space.read(STACK_BASE | cpu.s)
        cpu.s = (cpu.s + 1) & 0xFF
        hi = self.space.read(STACK_BASE | cpu.s)
        return lo | (hi << 8)

    def _do_read(self, params):
        count = params["count"]
        sector = params["record"]
        mode = params["mode"]
        if mode < 2 or mode == 0xFF:
            sectors = count & 0xFF
        elif mode == 0xFE:
            sectors = ((count & 0xFFFF) + 2047) // 2048
        else:
            out = {"modelled": False, "bytes": 0, "record": int(sector),
                   "error": "transfer mode $%02X not modelled" % mode}
            return out
        record = sector
        out = {"modelled": True, "bytes": 0, "record": record}
        count = sectors
        if self.space.disc is None:
            out["modelled"] = False
            out["error"] = "no disc attached"
            return out
        data = self.space.disc.read_record(record, count)
        if data is None:
            out["modelled"] = False
            out["error"] = "sector $%04X is not inside the data track" % sector
            return out
        bank0 = CDRAM_BASE + params["dest_bank"]
        off0 = params["dest_addr"] & 0x1FFF
        pos = 0
        bank, off = bank0, off0
        while pos < len(data):
            n = min(BANK_SIZE - off, len(data) - pos)
            self.space.load_phys(bank, off, data[pos:pos + n])
            pos += n
            bank += 1
            off = 0
        out["bytes"] = len(data)
        out["dest_banks"] = "%02X..%02X" % (bank0, bank - 1)
        if self.log:
            self.log.log("  -> copied %d bytes from record $%02X to banks $%02X..$%02X "
                         "(offset $%04X in first)" % (len(data), record, bank0,
                                                      bank - 1, off0))
        return out
