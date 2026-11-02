# Trace line format (`rce-trace v1`)

This is the format `XLATE_TRACE_OUT` writes (see `trace.c`), and the format a
reference emulator's tracer writes, so the two can be compared line for line.

One line per event, append-only, written in the order the CPU made the accesses.
Every line starts with `t=<kind>` followed by `key=value` fields separated by
single spaces, so a parser can split on whitespace and on `=` without quoting.

The header line `# rce-trace v1 ...` marks the start of a run; `#` lines are
comments.

## Kinds

| `t=` | meaning | fields |
|------|---------|--------|
| `FRAME` | start of an emulated frame | `f`,`n`,`pc`,`mpr` |
| `R` | read from the hardware I/O page | `f`,`pc`,`pa`,`la`,`off`,`r`,`v` |
| `W` | write to the hardware I/O page | `f`,`pc`,`pa`,`la`,`off`,`r`,`v` |
| `IRQ` | CPU took an interrupt | `f`,`n`,`pc`,`mpr`,`vec`,`k` |
| `IRQE` | CPU returned from an interrupt (RTI) | `f`,`n`,`pc`,`mpr` |
| `CDCMD` | write to `$1800`, the CD unit's start/SEL register | `f`,`n`,`pc`,`mpr`,`cmd` |

## Fields

| field | meaning |
|-------|---------|
| `f` | frame number, counted by the core from power-on (frame 0 is the first emulated frame) |
| `n` | monotonic record counter, present on `FRAME`/`IRQ`/`IRQE`/`CDCMD` only; gap-tolerant, useful for ordering across collapsed runs |
| `pc` | CPU program counter (logical, 16-bit hex) at the moment of the access |
| `mpr` | the 8 MPR bank registers as 16 hex digits, `MPR0` first. **Present only on `FRAME`, `IRQ`, `IRQE`, `CDCMD`** — for bus lines the value in force is the one from the preceding frame marker/event on the same frame |
| `pa` | physical bus address, 21-bit hex (`$1FE000`-`$1FFFFF` is the I/O page) |
| `la` | the logical address the CPU used (16-bit), before MPR translation |
| `off` | `pa & 0x1FFF`, the offset inside the I/O page — this is the number a hardware manual calls `$0000`-`$1FFF` |
| `r` | region tag, from `off`: `vdc` `$0000-$03FF`, `vce` `$0400-$07FF`, `psg` `$0800-$0BFF`, `timer` `$0C00-$0FFF`, `joy` `$1000-$13FF`, `irq` `$1400-$17FF`, `cd` `$1800-$1BFF`, `exp` `$1C00-$1FFF` |
| `v` | for `W`: the byte written. For `R`: the byte returned by the device. For `IRQ`: the vector address. For `CDCMD`: the command byte |
| `vec` | interrupt vector the CPU fetched (`$FFF8` = IRQ1/VDC, `$FFFA` = timer, `$FFF6` = IRQ2 or BRK) |
| `k` | interrupt kind: `irq1`, `irq2`, `timer`, `brk` |
| `cmd` | the byte written to `$1800` (see the warning below — this is **not** the CD command opcode) |
| `rep` | repeat count, see below |
| `cyc` | cycle length, see below |

`v` is always two hex digits for bus lines, so `v=00` is unambiguous.

## Collapsed repeats (`rep=` / `cyc=`)

The recorder collapses repeated access patterns so that the BIOS's idle loops do
not bury the interesting traffic. Two cases:

* `rep=N` **without** `cyc` — the single access on that line happened `N` times in
  a row (same `pc`, `pa`, `la`, `v`).
* `rep=N cyc=P` — this line is the first of a group of `P` consecutive lines that
  together form a cycle, and that cycle repeated `N` times as a unit. The other
  `P-1` lines of the group carry the same `rep=N cyc=P`.

To replay the exact stream:

```
if no rep:        emit the line once
elif no cyc:      emit the line rep times
else:             read the next cyc lines as one group, emit the group rep times
```

`rep` is omitted when it is 1. A missing `cyc` means "this line alone".

Worked example — the BIOS idle loop is a 6-record cycle repeated 19 times:

```
t=W f=299 pc=C93E pa=1FE000 la=0000 off=0000 r=vdc v=00 rep=19 cyc=6
t=W f=299 pc=C943 pa=1FE002 la=0002 off=0002 r=vdc v=48 rep=19 cyc=6
...
```

## What is *not* in the trace
* **The PSG (`$0800-$0BFF`) and the joypad (`$1000-$13FF`) are opt-in**: they are
  recorded only when the region mask names them.
* **DMA transfers are not expanded.** A VDC DMA writes VRAM without touching the
  data port, so you see the `LENR` write that starts it and nothing else.
* CPU reads from RAM/ROM are not hardware accesses and are not recorded; only the
  I/O page is. Writes to the I/O page are recorded even when the device ignores
  them.
