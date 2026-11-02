#!/usr/bin/env python3
"""Boot-chain test: the real loader, translated, must pull record $23 in.

Evidence chain (each step is checked, not assumed):

  1. The translated loader runs from $3000 to its `jmp $4000`.
  2. Its CD BIOS call is recorded with count=20, dest=$4000, bank=0,
     sector=$0123 -- record $23, exactly what dis/01_loader_3000.asm says.
  3. The 20 sectors really land in banks $80..$84 and are byte-identical to the
     boot program's own sectors on the disc (out/block/bootprog.bin).
  4. Banks $80..$83 are visible at $4000..$9FFF because the loader paged them in
     with TAM, so the jump target is the code we expect.
  5. A second, independent implementation (memory.py + refcpu.py, Python) reaches
     the same PC with the same memory CRC and the same call parameters.
"""
import os
import sys
import zlib

import harness as H
sys.path.insert(0, H.SRC)

from memory import AddressSpace, Disc, CDStub          # noqa: E402
import refcpu                                          # noqa: E402

LOADER = None
BOOTPROG = None


def run_reference(steps_limit=200000):
    """Same boot in the Python model: independent implementation, same claims."""
    space = AddressSpace(disc=Disc(H.DISC), bios_path=H.BIOS)
    space.load_visible(0x3000, open(LOADER, "rb").read())
    space.cd = CDStub(space)
    cpu = refcpu.RefCpu(space)
    cpu.pc = 0x3000
    cpu.s = 0xFF
    cpu.p = 0x24
    steps = 0
    while cpu.pc != 0x4000 and steps < steps_limit:
        cpu.step()
        steps += 1
        if cpu.pc >= 0xE000:          # RefCpu handles BIOS itself
            continue
    return space, cpu, steps


def main():
    H.skip_without_inputs()
    global LOADER, BOOTPROG
    BOOT = H.block_paths()
    LOADER, BOOTPROG = BOOT["loader"], BOOT["bootprog"]
    chk = H.Checker("boot chain (loader -> $4000)")

    b = H.run(["./build.sh"])
    if b.rc != 0:
        print(b.out)
        print(b.err)
        return 1

    dump = H.scratch("boot_mem.bin")
    r = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
               "--log", H.LOG, "--steps", "200000", "--stop-pc", "0x4000",
               "--dump", dump, "--hw"])
    state = H.parse_state(r.out)
    stop = H.parse_stop(r.out)
    calls = H.parse_calls(r.out)
    chk.ok(state is not None, "host produced a STATE line")
    if not state:
        print(r.out)
        return 1
    mem = H.parse_dump(dump)
    boot = open(BOOTPROG, "rb").read()

    # 1. the loader reached its jmp $4000
    chk.eq(stop["reason"] if stop else None, "stop-pc", "loader stopped at the jump target")
    chk.eq(stop["pc"] if stop else None, 0x4000, "stopped exactly at $4000")
    chk.eq(r.out.count("TRAP"), 0, "no untranslated code was reached")

    # 2. the CD read parameters
    reads = [c for c in calls if c["modelled"]]
    chk.eq(len(reads), 1, "exactly one modelled CD read")
    if reads:
        rd = reads[0]
        chk.eq(rd["pc"], 0xE009, "read went through BIOS entry $E009")
        chk.eq(rd["count"], 20, "sector count = 20 ($14)")
        chk.eq(rd["dest"], 0x4000, "destination address = $4000")
        chk.eq(rd["bank"], 0, "destination bank = 0 (CD RAM base $80)")
        chk.eq(rd["sector"], 0x23, "sector number = $23 (the record number)")
        chk.eq(rd["mode"], 0x01, "transfer mode 1: $F8 is a sector count")
        chk.eq(rd["record"], 0x23, "record $23 is the boot program")
        chk.eq(rd["bytes"], 40960, "40960 bytes transferred (20 x 2048)")
    # The loader's read uses transfer mode 1 (F8 = sector count); the boot program's
    # second read uses mode $FE (F8/F9 = byte count).  Both are asserted above and
    # in the "boot program" section below.
    chk.eq(reads[0]["mode"], 0x01, "loader read uses transfer mode 1")
    unmodelled = [c for c in calls if not c["modelled"]]
    chk.ok(any(c["pc"] == 0xE006 for c in unmodelled),
           "the early CD probe calls are logged as unmodelled")

    # 3. the bytes are the boot program we disassembled, bank by bank
    for i in range(5):
        bank = 0x80 + i
        got = mem[bank * 8192:(bank + 1) * 8192]
        want = boot[i * 8192:(i + 1) * 8192]
        chk.ok(got == want, "bank $%02X == bootprog[%d:%d] byte for byte"
               % (bank, i * 8192, (i + 1) * 8192))

    # 4. the loader's own TAM writes put those banks at $4000..$9FFF
    chk.eq(state["mpr"][2:6], [0x80, 0x81, 0x82, 0x83], "MPR2..MPR5 = $80..$83 after TAM")
    visible = b"".join(mem[(0x80 + i) * 8192:(0x81 + i) * 8192] for i in range(4))
    chk.ok(visible == boot[:32768], "visible $4000-$9FFF == bootprog[0:32768]")

    # 5. same run in the independent Python model
    space, cpu, py_steps = run_reference()
    chk.eq(cpu.pc, 0x4000, "reference model also reached $4000")
    chk.eq(list(space.mpr), state["mpr"], "reference MPRs identical")
    # Bank $FF is the hardware page: the C machine keeps it in Cpu.hw_regs and never
    # writes those bytes into its memory image, while the Python model writes
    # register stores into `mem` as well as into its Hardware object.  The two images
    # therefore cannot agree there -- by construction.  (This comparison used to
    # include the page and pass only because the loader's `st2 #$11` was mis-modelled
    # as "store A", with A = 0 at that point; the instant the immediate was honoured
    # the two models disagreed in a region the port deliberately keeps elsewhere.)
    py_mem = bytearray(space.mem)
    py_mem[0xFF * 8192:0x100 * 8192] = b"\x00" * 8192
    chk.eq(zlib.crc32(py_mem) & 0xFFFFFFFF, state["crc"],
           "reference memory CRC identical (whole 2 MB, hardware page aside)")
    chk.eq(py_steps, state["steps"], "reference executed the same instruction count")
    py_modelled = [c for c in space.cd.calls if c.get("modelled")]
    chk.eq(len(py_modelled), 1, "reference also modelled exactly one read")
    if py_modelled:
        p = py_modelled[0]["params"]
        chk.eq((p["count"], p["dest_addr"], p["dest_bank"], p["sector"], p["mode"]),
               (20, 0x4000, 0, 0x23, 0x01), "reference read parameters identical")

    # 6. bonus: the boot program's own code starts executing
    r2 = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
                "--log", H.LOG, "--steps", "40000"])
    st2 = H.parse_state(r2.out)
    chk.ok(st2 is not None and 0x4000 <= st2["pc"] < 0xE000,
           "with a 40000-step budget execution continues inside the boot program "
           "(pc=$%04X)" % (st2["pc"] if st2 else 0))
    chk.eq(r2.out.count("TRAP"), 0, "no trap while running into the boot program")

    # 7. the translated boot program now drives the video chip through our model
    r3 = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
                "--log", H.LOG, "--steps", "20000000", "--cycles", "2000000",
                "--pad", "0x08",                     # RUN: the program requires it
                "--frame", H.scratch("boot_video.ppm")])
    v = H.parse_video(r3.out)
    chk.ok(v is not None, "video summary present after 2,000,000 cycles")
    if v:
        chk.ok(v["vdc_writes"] > 20000,
               "translated code drove the video chip: %d register writes, "
               "%d VRAM word writes" % (v["vdc_writes"], v["vram_writes"]))
        chk.eq(v["bg"], 1, "the boot program turned the background on (BIOS $E096)")
        chk.eq(v["spr"], 1, "and the sprites on")
        st3 = H.parse_state_full(r3.out)
        chk.ok(st3 and 15 <= st3["frames"] <= 18,
               "16-ish frames of machine time elapsed (%s)"
               % (st3["frames"] if st3 else "?"))
        chk.ok(v["sat_dma"] >= 1,
               "the sprite-table DMA ran (%d times)" % v["sat_dma"])
    # 8. the boot program's second read: 24 sectors of record 3, mode $FE
    calls3 = H.parse_calls(r3.out)
    reads2 = [c for c in calls3 if c["pc"] == 0xE009 and c["count"] == 49152]
    chk.ok(bool(reads2), "the boot program issued its own CD read")
    if reads2:
        r2 = reads2[0]
        chk.eq(r2["sector"], 0x03, "sector $03 (record 3)")
        chk.eq(r2["mode"], 0xFE, "transfer mode $FE: the count is a byte count")
        chk.eq(r2["bytes"], 49152, "24 sectors = 49,152 bytes in one transfer")
        chk.eq(r2["dest"], 0x1000, "loaded to $1000 in the CD RAM bank")

    from PIL import Image
    img = Image.open(H.scratch("boot_video.ppm")).convert("RGB")
    colours = img.getcolors(maxcolors=512)
    chk.ok(colours and len(colours) > 1,
           "the frame the translated code produced is not a single colour "
           "(%d colours)" % (len(colours) if colours else -1))

    # 9. the pad: $E063 keeps the BIOS button tables, and the boot program's
    #    hand-off is gated on "RUN newly pressed" (bit 3 of $222D, the first pad's
    #    newly-pressed byte).  Pressed -> hand-off; not pressed -> it never gets
    #    there.  That is the derived meaning, not a guess: $222D is written by the
    #    BIOS pad routine at $E4D8 as (current ^ previous) & current.
    no_run = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
                    "--steps", "50000000", "--cycles", "2000000"])
    with_run = H.run(["./out/host", "run", "--disc", H.DISC, "--bios", H.BIOS,
                      "--steps", "50000000", "--cycles", "2000000",
                      "--pad", "0x08"])
    st_n = H.parse_state_full(no_run.out)
    st_r = H.parse_state_full(with_run.out)
    chk.ok(st_n and st_r, "both pad runs produced state")
    if st_n and st_r:
        chk.ok(st_n["steps"] > 2 * st_r["steps"],
               "without RUN the program stays in its per-frame loop (%d steps) "
               "and never takes the hand-off (%d steps with RUN)"
               % (st_n["steps"], st_r["steps"]))
    chk.ok("CALL pc=E063" in no_run.out, "the pad read is called through $E063")

    ok = chk.report("%d steps" % state["steps"])
    if st2:
        print("        note: boot program reached pc=$%04X after 40000 steps; "
              "it then spins on BIOS helpers we do not model" % st2["pc"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
