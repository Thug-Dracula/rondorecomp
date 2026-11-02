# Analysis tooling

The tools that read the game's own bytes. The port in `src/` is built from what
these produce, but nothing here runs at build time except `mk_table.py`.

| file | what it does |
|---|---|
| `huc6280.py` | HuC6280 disassembler with recursive-descent code discovery: seed entry points, follow control flow, and see exactly which bytes ended up as code. A linear sweep of everything the descent did not claim shows data as data. |
| `mk_table.py` | re-derives the opcode table (`src/huc_table.py`) from [Etripator](https://github.com/pce-devel/Etripator)'s `opcodes.c`, so the decode cannot drift from a maintained table. Only needed when that table changes; point `ETRIPATOR` at a checkout. |
| `scan_code.py` | finds HuC6280 code in a disc image without knowing load addresses, using a branch-boundary test and a byte-frequency test; both fail on compressed or random data. |
| `decompress.py` | the VRAM/BAT "painting program" decoder — the stream format described in `format.md`, reversed from the kernel routine at `$644D`. |
| `vram_render.py` | renders 16-bit VRAM words as PC Engine 4bpp tiles (contact sheets), used to look at what a decoder produced. |
| `log.py` | line-buffered logger for these tools; writes `logs/re.log`, flushed per line, `RE_VERBOSE=1` for detail. |
| `dis/` | annotated disassembly of the boot chain: the IPL loader at `$3000`, the boot program at `$4000` (record `$23`), and the game kernel at `$4000` (record 67). Each listing is the output of `huc6280.py` with the entry points and comments added. |

## The disc's numbering

The data track is addressed in 2048-byte **records** (sectors) by the CD BIOS. The
payload file the port reads has the 225-sector pregap in front, so

```
payload offset = (record + 225) * 2048
```

Record 1 holds the IPL header (`"PC Engine CD-ROM SYSTEM"`), record 2 the stage-2 loader
that runs at `$3000`, record `$23` (35) the boot program, and record 67 the game kernel.
`src/tools/slice_disc.py` holds the table the build slices by.

## What the disassembly established

- The loader reads record `$23`, 20 sectors, to `$4000`, and jumps there. The bytes it
  lands are byte-identical to `dis/02_bootprog_4000.asm`'s image.
- The boot program then pages in the game kernel from record 67 and calls into it through
  a vector table; the kernel's own interpreter loop is at `$6476`.
- `format.md` describes one real decompressor found in the kernel, with the command set
  verified against the disassembly and its output decoded from real disc data.
