# Behaviours this port reproduces on purpose

The port targets **one** reference implementation, not "the hardware": **Geargrafx 1.8.6**
(`github.com/drhelius/Geargrafx`, GPL-3.0, Copyright © 2024 Ignacio Sanchez). It is an
independent PC Engine emulator — not a port or fork of Mednafen or Mesen — so where it and
the hardware manual disagree, this port follows Geargrafx, and where it and this port
disagree, this port is wrong.

That decision is only affordable if it is written down and never widened after the fact.
So: every behaviour below is here because it was read in the reference's own source, or
measured repeatedly from it, and each carries the file it came from. If the port looks
wrong and its behaviour is in this list, it is the target. If it looks wrong and is not in
this list, it is a bug.

One further pin, because it is a property of the reference and not of the game: Geargrafx
seeds a core RNG from `time(NULL)` (`src/geargrafx_core.cpp:108`), which decides the
power-on palette (`src/huc6260.cpp:182-196`), the reset `A/X/Y/S/P`, and a 0–1 frame delay
before the drive's first sector. The port therefore targets a **deterministic build of the
same source with that seed fixed**, and reproduces that machine's power-on state. A capture
without its seed recorded is not evidence.

---

## CPU

| behaviour | why | where |
|---|---|---|
| `TSB`/`TRB` take **N and V from the memory operand**, not from the `A & M` test the datasheet specifies | the reference's shipping branch does this, and its unit-test branch does not; the port targets the shipping binary | `src/huc6280_opcodes_inline.h:463`, `:485` |
| the **shipping** build's semantics throughout, never the `GG_TESTING` build's | `GG_TESTING` is defined only by the reference's own unit-test harness and replaces memory, the `T` flag and `ADC` with test doubles | `src/memory_inline.h:47`, `src/huc6280_inline.h:67`, `:373`, `src/huc6280_opcodes_inline.h:28` |
| the `T` flag is **cleared after every instruction**; `SET`/`CLR` gate a following zero-page read-modify-write | shipping behaviour; the test build leaves `P` alone | `src/huc6280_inline.h:67` |
| `BRK` costs 8 cycles, not 7 | the reference charges the interrupt-acknowledge cycle | `src/huc6280_opcodes_inline.h` |
| undefined opcodes **execute** at the reference's cost (2 cycles) instead of stopping the machine | 22 opcodes are undocumented but implemented; a stray byte does not freeze the reference | `src/huc6280_opcodes_inline.h:695` |
| `CSL`/`CSH` switch 7.16 MHz ↔ 1.79 MHz, and a cycle is 3 master clocks at full speed and 12 in slow mode | the clock is what every timing measurement is expressed in | `src/huc6280.cpp` |

## Video

| behaviour | why | where |
|---|---|---|
| a frame is **263 lines**, because Rondo writes `VCECR = $04` and sets the blur bit | the line count is a function of the VCE control register, not a constant; 263 is this game's | `src/huc6260.cpp` (`k_huc6260_total_lines[]`) |
| **palette RAM is not zeroed at power-on** | the reference leaves VCE memory in its randomised power-on state, and the first frames of a cold boot otherwise differ | `src/huc6260.cpp:182-196` |
| a **VCE access costs one extra CPU cycle** | the reference charges it; without it every palette write lands a cycle early | `src/huc6260.cpp` |
| **VDC DMA moves one word per 4 clocks** with `LENR+1` semantics, and the SAT transfer raises its status bit on the last word | the transfer's duration is visible to code that polls it | `src/huc6270.cpp` |

## Disc drive

| behaviour | why | where |
|---|---|---|
| **`$D8` (set audio play position) holds the bus BUSY until its seek completes**, priced from a distance table in **milliseconds** and converted at 21,477,273 Hz | the drive is busy for ~36 frames on the intro's own `$D8`; returning status immediately puts the game a frame-block ahead of the reference | `src/scsi_controller.cpp`, `src/cdrom_media.cpp:359`, `src/cdrom_common.h:74` |
| a **`Read(6)` idles CD-DA** and leaves the audio head frozen where the music had it | a read is priced from the head's real position, and after the read the head is wherever the transfer left it | `src/scsi_controller.cpp:550`, `src/cdrom_audio_inline.h:210`, `:334` |
| every sector a read fetches **advances the media head by one** (`lba + 1`) | the next command is priced from there, so a following read is a zero-distance move | `src/cdrom_cuebin_image.cpp:288` |
| **`Read(6)` costs a seek plus one sector period**, with no multi-sector preload | there is no pre-spin of the first sectors; the game's own read burst only makes sense with this cost model | `src/scsi_controller.cpp` |
| a data byte **leaves the drive's FIFO one at a time**, each held `15 × 3` master clocks of ACK | a 2,048-byte sector takes 92,160 master clocks to leave, whoever is reading it | `src/scsi_controller.cpp` |
| **`$1804` reads back the byte last written to it** | the card's own reset routine is a read-modify-write on it and then reads it back | `src/scsi_controller.cpp` |
| the drive answers **NOT READY until it has spun up**, then GOOD | the card's boot code tests for exactly that condition and retries | `src/scsi_controller.cpp` |

## Console

| behaviour | why | where |
|---|---|---|
| the joypad's upper four bits are **`$30` plus `$80` when no CD unit is attached** | they are the machine's identity bits, not open bus; a game is entitled to ask | `src/input.cpp` (`INPUT_Read`) |
| **backup RAM is battery-backed and loaded at boot** | a save file left behind by an earlier run is an *input*: without it the two machines are different consoles from the first frame | `src/geargrafx_core.cpp` |

---

## Where the port is known to differ

Listed rather than hidden. Each is a defect against the reference until measured otherwise;
none of them is a decision.

- **`Read(6)` startup sector count.** The port charges 8 (a figure inherited from an older
  core); the reference's own figure has not been read out yet.
- **Drive ready timing.** The port models a 60-frame not-ready window from power-on; whether
  the reference models a motor at all is unmeasured.
- **Frame boundary.** The port declares a frame nine lines before the end of the field
  (`field_end - 9`), inherited from the first reference it was calibrated against; the
  reference declares it at the wrap onto line 256. Same period, different tick position.
- **`TRB`'s Z flag.** The port computes it from `A & M` (the datasheet's test) rather
  than from the result, so this one flag differs from the reference. For `TSB` the two
  rules agree.
- **Line count register.** The port fixes 263 lines rather than deriving the count from
  the blur bit; identical for this game, different for one that clears it.
- **Line numbering.** The port's line numbers are picture-relative; the reference's are
  field-relative, 14 lines apart. Comparisons that use line numbers must account for it,
  and the port does not yet model the reference's VDS/VDW/VCR/VSW line machine.
- **Sound.** Not implemented. The PSG and the ADPCM unit are simply absent, so no sound
  comparison exists yet.
