# Architecture

Rondorecomp is a port built from the game's own machine code. Nothing in the game is
rewritten: at build time the HuC6280 code on the disc is translated into C, and at run time
the console under it is a set of C models. This document follows one frame through the
machine, and names the file each part lives in.

```
   disc track + System Card image (yours)
            │
            │  src/build.sh → src/tools/slice_disc.py
            ▼
   src/out/block/*.bin    the boot chain and the game module's banks, sliced by sector
            │
            │  src/recompiler.py  (+ isa.py, cycles.py, keyset.json)
            ▼
   src/out/boot.c         one C function per instruction unit, plus a two-level dispatcher
            │
            │  gcc, with runtime.c video.c interp.c trace.c portstate.c host.c
            ▼
   src/out/host           headless machine: steps the CPU, renders frames, prints state
```

Everything below is in `src/` unless a path says otherwise.

## 1. The program being translated

The PC Engine addresses memory through eight 8 KB bank registers (MPR), and Rondo streams
code over the same addresses as it loads levels: bank `$DB` at `$4000` and bank `$80` at
`$4000` are different programs. Everything in the translator follows from that.

A **unit** is one instruction's worth of translated code, keyed by `(physical bank,
offset)`. A translated function looks like this (`runtime.h` explains the shape):

```c
static void i_3008(Cpu *c) {        /* lda #$10 */
    xl_lda(c, 0x10);
    c->p &= ~XL_T;
    c->pc = 0x300A;
}
```

All the semantics — flags, addressing modes, cycle costs, the memory map — live in `runtime.c`
and `runtime.h`, once, rather than being inlined into each of the 15,000 generated
units.

## 2. The translator — `recompiler.py`

Inputs: the sliced bank images, the System Card image (for its vectors), and a code/data log.

- **Recursive descent from seeds.** Entry points come from `boot_seeds.txt` /
  `stage_seeds.txt` (addresses the dispatcher can be observed to jump to) and from the boot
  vectors. Anything the descent does not reach is not emitted as code.
- **A code/data log closes the descent's blind spots.** A computed jump or an entry the
  program builds at run time is unreachable statically, so the build also takes a
  **content-keyed code/data log**: a map from the SHA-1 of an 8 KB bank image to the offsets
  executed as opcodes in that exact image. It is stored as hashes and offset ranges only
  (`data/keyset.json`) — no game bytes. The translator still decides where instructions
  start and end; the log only says which bytes were executed, and every disagreement is
  reported. Missing key set is a build failure, not a silent loss of coverage
  (`CDL_KEYSET=none` opts out deliberately).
- **Dispatch resolves the bank at run time.** `_emit_dispatcher()` writes a two-level
  switch: on `c->mpr[pc >> 13]` (the bank now mapped), then on the full logical address. A
  bank mapped somewhere it was not translated for therefore fails loudly instead of running
  the wrong program.
- **Every instruction re-dispatches.** Units never fall through into each other: each ends
  by setting `c->pc`, and the switch resolves the next unit afresh, so a `TAM` takes effect
  on the very next instruction.
- **The translator tracks MPRs statically while it descends**, so it stops at a bank
  boundary it can compute and says so when a `TAM` value is unknown.

Output is one C file plus the registration of every image it was built from
(`xl_stale_image`), which is what the overlay guard compares against (section 6).

Two tables are generated, never hand-written:

| table | source | used by |
|---|---|---|
| instruction widths and addressing modes | `isa.py` | the translator and `interp.c` |
| cycle costs | `cycles.py` | the translator and `interp.c` |

`src/tools/gen_interp_tables.py` bakes both into `out/interp_tables.h`. One table means the
translated and interpreted paths cannot disagree about a cost or a width — which is the
whole basis of the differential test.

## 3. The runtime and the hardware models — `runtime.c`, `runtime.h`

`Cpu` holds the registers, the 2 MB physical memory (256 banks), the device state, and the
counters the tests read. The memory map is physical: CD RAM at banks `$80`–`$87`, work RAM
at `$F8`, backup RAM at `$F7`, the I/O page at bank `$FF`, the System Card at `$E000`–`$FFFF`.

- **The clock is `xl_tick(c, cycles)`.** Every instruction charges its cost there; the VDC
  model advances its raster from that number, and frame boundaries, timer ticks and
  interrupts fall out of it. There is no second clock and no wall-clock tie anywhere.
- **The CD-ROM unit is a device, not a stub.** `$1800`–`$180F` implements the register
  interface (REQ/ACK handshake, phases, command decoder, sector streaming at the drive's
  own rate, seek pricing, CD-DA playback state) so that the System Card's own driver can
  drive it. The numbers it uses are the reference emulator's, cited in place; see
  [REFERENCE.md](REFERENCE.md).
- **The System Card is modelled, or executed, or both.** By default the BIOS entry points
  the game calls (`$E009` read, `$E006` …) are modelled in C: their observable effects on
  memory, registers and the drive. With `XLATE_BIOS_EXEC=1` the machine executes the user's
  actual System Card instead, instruction by instruction, through the interpreter
  (section 5) — the card is never translated, because its bytes are not ours to translate.
  The two paths are compared against each other by the tests.
- **Interrupts** are delivered from the tick: IRQ1 (VDC), timer, and the raster-compare
  line come through the vector table with the timing the model computes, not at the frame
  boundary.
- **Backup RAM** (2 KB of battery-backed save data) is a real region, loadable from and
  dumpable to a file: it is an input like the disc, and a comparison that does not give both
  machines the same card is comparing two different consoles.

## 4. The video path — `video.c`, `video.h`

The VDC (HuC6270) and VCE (colour encoder) are modelled at the register level, and the
renderer draws a frame from the state the game's code left behind:

- address/data ports with the low/high latch, the memory-increment modes, SAT DMA,
  VRAM-to-VRAM DMA, status bits and the raster counter;
- the raster advances with CPU time, line by line, so a register written during vblank or
  mid-frame takes effect on the correct line;
- the renderer follows the measured format — BAT layout, 4bpp tile bitplane order, sprite
  attribute table and 16×16 pattern layout, palette word format. Those constants are listed
  at the top of `video.h`, each with where it was verified.

The renderer is deliberately separable: `host render --state BLOB --out frame.ppm` loads a
dumped video state and draws one frame, which is how the renderer is compared with a
reference screenshot without involving the CPU at all.

## 5. The interpreter — `interp.c`

A real HuC6280 interpreter that executes the bytes actually in memory. It is not a second
machine: the same `Cpu`, the same `xl_rd`/`xl_wr` memory map, the same flag helpers, the same
generated width and cycle tables. Its step order deliberately mirrors one generated unit
line for line (body, one tick, clear `T`, set PC), because the tick can itself write memory
at a frame boundary.

It has three jobs:

1. `XLATE_INTERP=1` forces every step through it, which is how the translated path is
   checked for exact equivalence (registers, flags, cycles, frames, memory).
2. The **overlay guard** (section 6) routes one bank at a time to it.
3. Under `XLATE_BIOS_EXEC=1`, any PC in a page holding the user's System Card.

## 6. The overlay guard — `runtime.c`

A translated unit is built from the bytes a bank held at build time. Rondo loads code over
its banks while it runs, so a unit can be entered when the bytes underneath it are no longer
the bytes it was built from — the machine would then execute a translation of something that
is not there.

The guard closes that: every compiled image is registered (`xl_stale_image`), and on entry
to a bank the guard compares the live bytes against that image, per bank *generation* (a
bank's generation advances when something writes to it). While they disagree, that bank's
execution is routed to the interpreter, which decodes what is really in memory. When the
image is reloaded and the bytes match again, the translation resumes.

The guard never guesses: a bank with no compiled image (the System Card, a bank the
translator never covered) is not guardable and is never routed. Every run prints the
translated/interpreted split, so a run that leaned on the interpreter cannot be quoted as
pure. `XLATE_GUARD=0` disables the routing and leaves the report-only instrument in place,
which is how "on and off agree on a run that never touches a changed bank" is measured.

## 7. The test host — `host.c`

No window, no SDL, no input devices. The host reads files, steps the machine, writes
frames, and prints machine-readable lines (`STATE …`, `PAD …`, `CALL …`, `VIDEO …`,
`BANK …`) that the Python tests parse. It is both the way the port is run today and the
instrument the whole test suite is written against.

`portstate.c` emits the port's observable state in the same field layout a reference
capture uses, so two captures can be compared field by field instead of by eye.
`trace.c` emits an I/O-access trace in the same format a reference emulator's tracer writes
(see `docs/TRACE-FORMAT.md`), so a timing question becomes a diff.

## 8. One frame, start to finish

1. `xl_cpu_step()` picks a path: the System Card (if executing it), the guard's interpretation
   of a stale bank, the interpreter if forced, otherwise the translated dispatcher.
2. The dispatcher resolves `(bank now mapped, pc)` to one unit. The unit calls runtime
   helpers, each of which charges cycles through `xl_tick`.
3. `xl_tick` advances the VDC's raster. On a line match it may raise the raster-compare
   interrupt; at the end of the field it completes the frame, latches the joypad state the
   host scheduled for that frame, applies any scheduled console reset, and raises IRQ1.
4. The game's interrupt handler runs as translated code, writes VDC registers and VRAM, and
   returns.
5. When the host is asked for a frame, `video.c` renders the current VDC/VCE state to a PPM;
   when it is asked for state, `portstate.c` prints the fields `harness.py` parses.
6. The Python tests compare those lines — and whole-RAM dumps, and pictures — against a
   reference emulator's capture. That comparison is the subject of
   [VERIFICATION.md](VERIFICATION.md).
