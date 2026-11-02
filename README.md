# Rondorecomp

> ## ⚠ Work in progress — not playable
>
> This is an active research project, published for reference and discussion. **It does not
> play the game end to end, and it is not ready to be used as a port.** It boots from a real
> System Card, renders the intro and the file-select screens, and reaches the prologue level —
> but it stops, diverges or renders incorrectly in places, sound is not implemented at all,
> and much of the game has never been verified.
>
> Read the "Status" section below before expecting anything to work. There is no support, no
> release and no schedule.

A native port of *Castlevania: Rondo of Blood* (PC Engine CD), built by **static
recompilation**. The game's HuC6280 machine code is translated into C at build time — the
console's own bytes are the source, nothing is rewritten by hand — and the hardware it runs
on (CPU, VDC/VCE video, CD-ROM drive, System Card services, interrupts, timer, joypad) is
modelled in C.

**You need your own System Card image and your own disc. Nothing is bundled, and nothing
may be: this repository contains no game data of any kind.** No ROM, no BIOS, no disc track,
no audio, no screenshots, no assets. The build slices what it needs out of the two files you
supply, at build time.

**Licence: MIT** — see [LICENSE](LICENSE).
**Built on other people's work** — see [ACKNOWLEDGEMENTS](ACKNOWLEDGEMENTS.md): Geargrafx is the
reference this port reproduces, and the CPU test vectors, hardware notes and prior-art surveys
below are what make the verification possible.
**No game assets are included or distributed** — *Castlevania: Rondo of Blood* and everything
in it are © Konami, and this project is not affiliated with or endorsed by Konami.

## Status

Honest summary, because the banner above is not a formality:

| | |
|---|---|
| **Works** | boots from a user-supplied System Card; the CPU, video, CD and pad layers run; the opening cutscene, the file-select screens and the prologue level are reached and render correctly across the windows that have been measured |
| **Not implemented** | **sound** — no PSG, no CD-DA playback, no ADPCM. The engine has no audio output at all |
| **Known divergences** | a small phase difference against the reference in the intro; two sprite positions and a backdrop column differ in the file-select screens; the prologue cut drifts by ~14 frames |
| **Unverified** | most of the game. Roughly a tenth of a full playthrough has been compared frame by frame against a reference emulator |
| **No** | release, installer, controller support, save states, widescreen, or any promise of completion |

Correctness is defined against a specific reference emulator, not against the hardware, and is
measured frame by frame (see `docs/VERIFICATION.md`). The test suite is real but partial: run
`./tests/run_all.sh` and read what passes and what skips.

## What you need

- A **System Card 3.0** image (`syscard3.pce`, 256 KB) — the CD-ROM BIOS the game requires.
- Your own **Rondo of Blood** disc's data track as one flat file: 2048-byte user sectors in
  order, with the 225 zero sectors that precede record 0 in front of them
  (`data_payload.bin`, 21,245,952 bytes for the Japanese release).
- `gcc` with C11 support (tested with GCC 16.2), `python3` 3.8+ (tested with 3.14), and
  `bash`. `numpy` and `Pillow` are needed only by the image/capture tools and a few tests.

Both files go in `inputs/` at the repository root, which is git-ignored:

```
inputs/syscard3.pce
inputs/data_payload.bin
```

Or point at them from the environment, wherever they live:

```sh
BIOS=/path/to/syscard3.pce DISC=/path/to/data_payload.bin ./src/build.sh
```

A wrong or truncated dump is caught immediately: `python3 src/tools/slice_disc.py
inputs/data_payload.bin --check` reports the file's size in sectors, and the build refuses a
payload too short for a slice it needs.

## Build

```sh
cd src
./build.sh                       # translate the boot chain, compile the test host
```

About half a minute (26 s measured here). **Warnings are treated as errors** — the build captures the compiler
output and stops if any line contains `warning:`. The output is `out/host`, a headless
machine: no window, no audio, no input devices.

Other targets, from `src/`:

```sh
./build.sh selftest              # a hand-checked sequence      -> out/selftest_host
./build.sh banktest              # same address, two banks      -> out/bank_*_host
./build.sh irqtest               # interrupt/timing sequence    -> out/irqtest_host
./build.sh cases SPECS.json OUT  # generated differential cases -> OUT
./build.sh stage STATE.img       # translate a dumped RAM image -> out/host_stage
```

`stage` takes a state image: `src/tools/mkstate.py` converts a reference emulator's
savestate into the port's own `XLST1` format, and `src/tools/state_banks.py` slices it into
physical banks.

Two tables are generated on every build — `out/interp_tables.h`, from `isa.py` (widths,
addressing modes) and `cycles.py` (costs) — and written atomically, because two builds of
one tree would otherwise let one compile a header the other is halfway through writing.

## Run

```sh
cd src
./out/host run --disc ../inputs/data_payload.bin --bios ../inputs/syscard3.pce \
               --steps 50000000 --cycles 20000000 --pad 0x08 --frame /tmp/frame.ppm
```

`--pad 0x08` holds RUN, which the boot program waits for before handing control to the game.
The frame is written as a binary PPM. Other switches: `--trace` (every PC), `--hw`
(hardware-register summary), `--dump` (whole RAM at the end), `--press FROM:TO:MASK` and
`--pressfile` (timed pad windows), `--resets` (console resets by frame).

## What works today

- The **boot chain** runs as translated code: the disc loader, the boot program and the
  game kernel all execute from the recompiled C, driven by the CD-ROM model and the System
  Card's own entry points.
- The **intro renders**: the game's own code uploads its VRAM maps, sets its palettes, loads
  its data through the drive and draws the title and intro sequence, frame by frame.
- **Sound is not implemented**, and no stage is playable yet. The port reaches the game; it
  does not yet play it.

## Tests

```sh
./tests/run_all.sh               # build + eleven stages
python3 tests/test_boot.py       # or one stage on its own
```

The suite exits **0** whether the reference-dependent stages ran or skipped. A stage that
cannot run prints one line naming what is missing and exits 77, which `run_all.sh` reports
as **SKIP** — never as a pass:

| what is missing | stages that skip |
|---|---|
| a reference capture tree (`REFDATA`) | `test_video`, `test_frame`, `test_input`, `test_oracle_state`, `test_interp` |
| your System Card / disc (`BIOS`, `DISC`) | every stage that boots or translates |

With `BIOS` and `DISC` set and no `REFDATA`, seven of the twelve stages run. With a full
reference tree as well, all twelve run. `test_bios_exec.py` and `test_bios_reset.py` are
separate programs outside the suite, for the "execute the System Card" route.

The reference tree is user-supplied too; its layout is in
[docs/VERIFICATION.md](docs/VERIFICATION.md), which also explains the method — state and
picture comparison against a pinned reference emulator, whole-RAM diffs, and fault injection.

## Instrumentation switches

Environment variables, all off unless named; none changes the machine except the two that
choose which engine runs.

| variable | effect |
|---|---|
| `XLATE_LOG=PATH` | where the port's log goes (default `logs/xlate.log`), line-flushed, size-capped |
| `XLATE_TRACE_OUT=PATH` | an rce-trace v1 I/O trace (`docs/TRACE-FORMAT.md`); `XLATE_TRACE_FROM`/`_TO`/`_REGIONS`/`_ALL` narrow it |
| `--trace` / `--hw` | per-PC trace and a hardware-write summary on stdout |
| `--dump PATH`, `XLATE_RAMDUMP` | whole-RAM dumps, in the reference's byte layout |
| `--frame PATH`, `XLATE_FRAME_DIR` + `XLATE_FRAME_EVERY` | rendered frames (PPM) |
| `XLATE_FRAMELOG` | one line per frame: screen variable and pad latch |
| `XLATE_PORTSTATE_DIR` | the port's full observable state per checkpoint, as JSON |
| `XLATE_INTERP=1` | every step through the interpreter (same machine, other engine) |
| `XLATE_BIOS_EXEC=1` | execute your System Card instead of modelling it; `XLATE_BIOS_RESET=1` enters it through the reset vector |
| `XLATE_GUARD=0`, `XLATE_STALE=0` | overlay guard routing off / stale-bank instrument off |

`CDL_KEYSET=none` builds without the code/data key set: the translation then contains only
what the static descent reaches (about 19 % of the executed code), and the run stops at the
first entry point it cannot reach. `tools/cdl_coverage.py` measures the difference.

## Layout

| path | what it is |
|---|---|
| `src/` | the port: translator, interpreter, runtime and hardware models, renderer, headless host, `build.sh` |
| `src/tools/` | build-time helpers: the disc slicer, the table generator, the case/self-test generators, the state converters |
| `tests/` | the test programs, `harness.py` and `run_all.sh` |
| `tools/` | analysis utilities: comparison video, whole-RAM comparison, the CPU-vector suite, coverage |
| `re/` | the disassembly and analysis tooling the port was derived from |
| `docs/` | [architecture](docs/ARCHITECTURE.md), [reference behaviours](docs/REFERENCE.md), [verification](docs/VERIFICATION.md), [trace format](docs/TRACE-FORMAT.md) |

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) first: it follows one frame through the
machine.

## Licence of individual files

Everything here is MIT except `src/huc_table.py`, which is machine-extracted from
[Etripator](https://github.com/pce-devel/Etripator) (GPL-3.0) and carries that licence in
its header.
