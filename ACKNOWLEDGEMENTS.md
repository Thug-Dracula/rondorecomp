# Acknowledgements

This project stands on other people's work. None of the code below was copied into this
repository — everything here was written for this project — but the *knowledge*, the
measurements and the test data came from these sources, and they are the reason it works at
all.

If any of them are wrong about something, this project is probably wrong about it too.

---

## The reference implementation

**[Geargrafx](https://github.com/drhelius/Geargrafx)** — Ignacio Sanchez (drhelius), GPL-3.0.

The single most important influence. This port reproduces *Geargrafx's* behaviour, deliberately and
completely: where the port and Geargrafx disagree, the port is considered wrong. Its source was read
as a specification — the CPU, the VDC/VCE video chips, the CD-ROM/SCSI layer, the ADPCM unit, the
timer and the interrupt model — and reimplemented in C. **No Geargrafx source code is present here**,
and none was copied; the relationship is "read it, then write your own", which is why this project can
carry a different licence.

Its idiosyncrasies are reproduced on purpose rather than corrected. They are listed, with the source
lines that justify each one, in [`docs/REFERENCE.md`](docs/REFERENCE.md).

## Documentation and prior art

**[PC Engine Recomp](https://github.com/rages4calm/pcenginerecomp)** — MIT.

A static-recompilation toolkit for HuCard and CD-ROM² games, read as **documentation only** during the
design of this port's translator, overlay handling and hardware model. Its notes were treated as
leads to be verified against a running machine, never as facts, and nothing was taken from it but
understanding. Its own published limits are recorded beside its claims.

**[SingleStepTests/huc6280](https://github.com/SingleStepTests/huc6280)** — published single-step CPU
test vectors.

The most useful external artefact in the whole project. 555,220 vectors, each an independent machine
state with its complete expected result, generated outside this project and outside every emulator
this project compares against. They corrected this port's CPU from 61% agreement to 86% and found
nine real defects that no amount of internal testing had caught.

**Paul Clifford's PC Engine technical notes** (`pcetech.txt`) and the **Hu7 CD-ROM BIOS manual** —
the hardware documentation that explains what the machine is *supposed* to do, and the only way to
tell a bug from a quirk.

**Bruce Clark's 65C02 opcode reference** and the **WDC W65C02S datasheet** — the CPU's documented
behaviour, used to identify where the emulators disagree with the silicon.

**Shadoff / `pce-devel`** — the `PCECD_seek` measurements, a distance-resolved table of real CD drive
seek times. Used as a diagnostic for how long the drive should take, even though the final reference
for this port is an emulator rather than the hardware.

## Tools used to verify

**[BizHawk](https://github.com/TASEmulators/BizHawk) 2.9.1 / TurboNyma** (Mednafen 1.29 running
under Waterbox) — the 131,357-frame capture that this project measured against before it switched to
Geargrafx as its reference. Much of the verification design — frame-by-frame memory comparison,
first-divergence measurement, picture diffing — was built against that corpus.

**[Mednafen](https://mednafen.github.io/)** — an early timing reference for CPU cycles and CD command
behaviour, and the emulator BizHawk's TurboNyma core is built from.

**[Etripator](https://github.com/bbbradsmith/etripator)** — disassembly tooling, and its example
System Card listings, which named several BIOS routines this project would otherwise have modelled by
guessing.

**MAME** and **ares** — consulted as independent cross-checks wherever the emulators disagreed.

## On licences

Geargrafx is GPL-3.0, Mednafen and MAME are GPL, and this project is MIT. That combination is only
possible because **no code was copied**: these projects were read to learn behaviour, and every line
in this repository was written here. If you believe something here is derived from GPL code rather
than from observed behaviour, please open an issue — it would be a mistake worth correcting.

*Castlevania: Rondo of Blood* and everything in it are © Konami. This project ships none of it and is
not affiliated with or endorsed by Konami.
