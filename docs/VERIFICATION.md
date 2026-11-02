# How correctness is established

The port is checked against a **reference emulator**, not against a feeling about how
the game looks. This document describes the method: what is compared, against what, and
which comparisons are allowed to count.

## 1. What "correct" means

One reference implementation is the target — Geargrafx 1.8.6, pinned, with its power-on
randomness fixed (see [REFERENCE.md](REFERENCE.md)). The port is correct where it agrees
with that machine, on two channels:

- **state** — registers, the bank registers, and every byte of memory the machine has:
  the 64 KB of CD RAM, the 8 KB of work RAM, the video chip's VRAM, the palette, the
  sprite table;
- **picture** — the frame the video chip would show, compared pixel by pixel against the
  reference's own frame at the same instant.

Neither alone is enough. A picture can look right while the machine is in the wrong state
(the same sprite in the same place on a different screen), and state can agree while the
raster is a line out of step.

A note on names: the code and some tools call the reference machine **the oracle**
(`src/tools/oracle_state.py`, `tests/test_oracle_state.py`, and comments throughout). It is the same
thing — one reference implementation, the one named in REFERENCE.md.

## 2. Instruments

| instrument | what it gives you |
|---|---|
| `host` `STATE` line | registers, bank registers, a CRC of the whole 2 MB physical memory, step count, cycles, frames, interrupt counts |
| `--dump` / `XLATE_RAMDUMP` | whole-RAM snapshots in the reference's own byte layout, so a diff needs no conversion |
| `--frame` / `XLATE_FRAME_DIR` | rendered frames as PPM, at chosen frames or every N frames |
| `XLATE_FRAMELOG` | one line per frame: the game's screen variable and its pad latch |
| `XLATE_TRACE_OUT` | an I/O-access trace in the reference tracer's format (`docs/TRACE-FORMAT.md`) |
| `portstate.c` (`XLATE_PORTSTATE_DIR`) | the port's observable state in the reference capture's field layout, one JSON document per checkpoint |
| `XLATE_INTERP=1` | every step through the interpreter, for equivalence testing |
| `XLATE_GUARD=0` | the overlay guard's routing off, with its report-only instrument on |

All of them are off unless asked for, and none of them changes the machine: the only
switches that alter behaviour are `XLATE_INTERP` (same machine, different engine) and
`XLATE_BIOS_EXEC` (the user's System Card instead of the modelled BIOS).

## 3. The test suite

`tests/run_all.sh` runs twelve stages. Each is a separate program and can be run
on its own.

| stage | what it establishes |
|---|---|
| `test_selftest.py` | a hand-checked sequence: 64 expectations written down from the instruction definitions, not from the port's output |
| `test_boot.py` | the translated disc loader really loads record `$23` into banks `$80`–`$83`, byte-identical to the disc's own sectors, and reaches `$4000`; a second, independent implementation in Python (`memory.py` + `refcpu.py`) reaches the same PC with the same memory CRC |
| `test_video.py` | the renderer, given a dumped video state, reproduces the reference's frame for the same state (correlation per capture) |
| `test_irq.py` | interrupts from translated code: the vector is taken, the handler runs, the frame rate is the machine's |
| `test_bank.py` | the same address in two different banks executes two different programs — the property the whole translation rests on |
| `test_frame.py` | the boot chain's own output: CD data, VRAM writes, palette, display enable, and which code ran, each checked separately so a low correlation is explained rather than reported |
| `test_input.py` | an input schedule is delivered at the frame it names, and the game sees an edge, not a level |
| `test_oracle_state.py` | the port's bank numbering and PC agree with the reference's savestates, phase by phase |
| `test_diff.py` | 2,688 generated sequences run through three implementations — the generated C, the C interpreter, and the Python reference — must agree on registers, memory CRC **and cycle total** |
| `test_interp.py` | forcing every step through the interpreter produces the same frames and the same state as the translation |
| `test_guard.py` | a bank whose bytes were replaced is interpreted, then released when the translation's bytes come back |

Two more tests exist for the System Card route and are not part of the default suite:
`test_bios_exec.py` (execute the card instead of modelling it) and `test_bios_reset.py`
(enter it through the reset vector, and take interrupts like the console).

A stage that needs data this repository does not ship prints **one line naming what is
missing** and exits **77**, which `run_all.sh` reports as SKIP — never as a pass, and never
as a failure. The suite still exits 0:
```
SKIP  no reference savestates (refdata/captures/*.state): set REFDATA=<dir>
```

Stages that need a **reference capture tree** look for it under `$REFDATA`
(default `refdata/`):

```
refdata/shots/     fNNNNNNNN.raw     reference frames, 256x232 RGB555
refdata/ram.bin    per-frame work RAM, 8192 bytes per frame
refdata/latch.txt  "frame $31 $222D" per frame (optional)
refdata/captures/  *.state           reference savestates
refdata/states/    *.img             RAM images converted by src/tools/mkstate.py
refdata/replays/   *.replay          input schedules
refdata/ramdump/   *.bin             whole-RAM dumps at frame boundaries
```

The suite passes with none of it present (seven stages run, five skip) and with a full
tree present (all twelve run).

## 4. The deterministic-domain rule

A reference capture is only evidence where the reference is **deterministic**. Geargrafx
seeds an RNG from the wall clock, and that seed decides visible state from the first
instruction, so the reference is pinned: same source, same version, seed replaced by a
constant (see REFERENCE.md). Even then, determinism is *measured*, never assumed:

- the run is split into **2,000-frame segments**;
- each segment is run three times, and its state compared against itself;
- the segment is **DETERMINISTIC** if all three agree — a divergence inside it is a defect
  in the port — or **NON-DETERMINISTIC**, in which case it supports source-conformance
  only, and no first-divergence figure may be quoted from it.

No frame number is ever quoted without its segment's verdict beside it. A number from a
non-deterministic segment is a number about the weather.

## 5. Whole-RAM diffs

Frame-level comparison tells you *that* two machines differ; a whole-RAM diff tells you
*where*. The port dumps memory in the same layout the reference capture uses, so a diff
is a byte compare with no conversion:

```
0x00000 .. 0x3FFFF   CD RAM, physical banks $68..$87 (32 x 8 KB)
0x40000 .. 0x41FFF   work RAM, physical bank $F8 (8 KB)
```

Two things consume those dumps. `tools/lockstep/wram_cmp.py` compares the whole 8 KB of
work RAM frame by frame against a reference RAM stream and prints, per frame, how many
bytes differ and the first differing address. And the port's state document
(`portstate.c`) reports VRAM, the palette, the sprite table, CD RAM and work RAM as
separate fields, so a comparison names the subsystem before it names an address. The point
is that "the graphics differ" becomes "VRAM word `$1445` differs at frame 11,119", which
is a question that can be answered.

## 6. Fault injection

A test that cannot fail proves nothing, so several checks deliberately break something:

- **Stale overlay.** `test_guard.py` runs a route on which the boot program reloads three
  program banks under itself, and asserts both that the guard fires (with the bank, the
  frame and the byte count) and that it *releases*; the same run with the guard off must
  stop untranslated earlier. A guard that only holds would make the machine an
  interpreter with a translation bolted on.
- **Forced interpretation.** `test_interp.py` sends every instruction through the
  interpreter, which must produce the same frames as the translation — a difference is a
  bug in one engine, not a measurement.
- **Refused opcodes.** An unknown opcode is refused by both engines through the same
  path, and `host cdprobe` asserts that the drive refuses an unknown command with a named
  CHECK CONDITION rather than guessing.
- **Input schedule check.** `tools/lockstep/pad_from_ref.py --check` reconstructs the
  buttons from per-frame RAM and compares the result against an independent input log:
  a shifted reconstruction disagrees on most windows instead of none.

## 7. What this does not prove

- It proves agreement with one emulator, not with the hardware. Where that emulator is
  wrong about the machine, this port is wrong with it, deliberately.
- The random differential corpus excludes the instructions a slot-sized test cannot run
  safely (`brk`, `rts`/`rti`, `jsr`, absolute `jmp`, `tam`/`tma`, `st0`–`st2`); those are
  covered by hand-written cases instead, and the self-test is the independent check on
  the semantics the emulator core supplied.
- No sound is compared, because no sound is implemented.
- A green segment says the two machines were the same for 2,000 frames. It says nothing
  about frame 2,001.
