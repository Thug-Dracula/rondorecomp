#!/usr/bin/env python3
"""Shared helpers for the xlate tests: run the host, parse its output."""
import os
import re
import subprocess
import sys
import tempfile

#: repo/tests, repo/src (the port: build.sh, out/, the Python modules) and repo/src/tools
#: (build-time helpers).  Everything below is derived from these three.
TESTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TESTS)
SRC = os.path.join(REPO, "src")
TOOLS = os.path.join(SRC, "tools")
sys.path.insert(0, SRC)

# The two inputs the user supplies (see README): the System Card image and the
# game's data track.  Neither is in this repository.  Override either with
# DISC=/... or BIOS=/..., or put both in inputs/.
INPUTS = os.environ.get("INPUTS") or os.path.join(REPO, "inputs")
DISC = os.environ.get("DISC") or os.path.join(INPUTS, "data_payload.bin")
BIOS = os.environ.get("BIOS") or os.path.join(INPUTS, "syscard3.pce")
LOG = os.path.join(SRC, "out", "xlate.log")
BLOCK = os.path.join(SRC, "out", "block")

#: Where tests write their scratch files: $XLATE_SCRATCH, or a directory under the
#: system temp dir.  One shared root, so a run leaves everything in one place.
SCRATCH = os.environ.get("XLATE_SCRATCH") or os.path.join(
    tempfile.gettempdir(), "rondorecomp")


def scratch(name):
    """A path inside the scratch root, directory created."""
    path = os.path.join(SCRATCH, name)
    os.makedirs(os.path.dirname(path) or SCRATCH, exist_ok=True)
    return path


#: exit status a test uses when it cannot run for want of user-supplied data.
SKIP = 77


def have_inputs():
    return os.path.isfile(DISC) and os.path.isfile(BIOS)


def skip_unless(cond, what):
    """One line naming what is missing, then exit 77 (run_all.sh reports SKIP)."""
    if cond:
        return
    print("SKIP  %s" % what)
    sys.exit(SKIP)


def skip_without_inputs():
    skip_unless(have_inputs(),
                "no System Card image or disc: set BIOS=<syscard3.pce> and "
                "DISC=<data_payload.bin> (README)")


#: The reference-capture tree, if the user has one: states, screenshots, RAM dumps
#: and input schedules taken from a reference emulator (see docs/VERIFICATION.md).
#: Every stage that needs it skips when it is absent.
REFDATA = os.environ.get("REFDATA") or os.path.join(REPO, "refdata")


def ref(*parts):
    """A path inside the reference-capture tree."""
    return os.path.join(REFDATA, *parts)


def ref_present(*parts):
    p = ref(*parts)
    return os.path.exists(p) and (not os.path.isdir(p) or bool(os.listdir(p)))


def block_paths():
    """Paths to the boot chain's three images, sliced from the user's data track.

    build.sh writes these too; slicing again here keeps a test runnable on its
    own, and is the same call either way (tools/slice_disc.py).
    """
    names = ("loader", "bootprog", "kernel")
    paths = {n: os.path.join(BLOCK, n + ".bin") for n in names}
    if all(os.path.isfile(p) for p in paths.values()):
        return paths
    os.makedirs(BLOCK, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(TOOLS, "slice_disc.py"),
                    DISC, "--out", BLOCK], check=True, capture_output=True)
    return paths

FLAG_C, FLAG_Z, FLAG_I, FLAG_D, FLAG_B, FLAG_T, FLAG_V, FLAG_N = 1, 2, 4, 8, 16, 32, 64, 128


class Result:
    def __init__(self, rc, out, err, cmd):
        self.rc, self.out, self.err, self.cmd = rc, out, err, cmd

    def __repr__(self):
        return "<Result rc=%d cmd=%s>" % (self.rc, " ".join(self.cmd))


def run(cmd, cwd=None, timeout=600, env=None):
    p = subprocess.run(cmd, cwd=cwd or SRC, capture_output=True, text=True,
                       timeout=timeout, env=env)
    return Result(p.returncode, p.stdout, p.stderr, cmd)


def parse_state(text):
    m = re.search(r"^STATE a=([0-9A-F]{2}) x=([0-9A-F]{2}) y=([0-9A-F]{2}) "
                  r"s=([0-9A-F]{2}) p=([0-9A-F]{2}) pc=([0-9A-F]{4}) "
                  r"mpr=([0-9A-F]{16}) steps=(\d+) crc=([0-9A-F]{8}) "
                  r"bios=(\d+)", text, re.M)
    if not m:
        return None
    return {"a": int(m.group(1), 16), "x": int(m.group(2), 16),
            "y": int(m.group(3), 16), "s": int(m.group(4), 16),
            "p": int(m.group(5), 16), "pc": int(m.group(6), 16),
            "mpr": [int(m.group(7)[i:i + 2], 16) for i in range(0, 16, 2)],
            "steps": int(m.group(8)), "crc": int(m.group(9), 16),
            "bios": int(m.group(10))}


def parse_state_full(text):
    """STATE line including the timing fields added with the video shim."""
    m = re.search(r"^STATE a=([0-9A-F]{2}) x=([0-9A-F]{2}) y=([0-9A-F]{2}) "
                  r"s=([0-9A-F]{2}) p=([0-9A-F]{2}) pc=([0-9A-F]{4}) "
                  r"mpr=([0-9A-F]{16}) steps=(\d+) crc=([0-9A-F]{8}) bios=(\d+) "
                  r"cycles=(\d+) frames=(\d+) irq=(\d+) vblank=(\d+)", text, re.M)
    if not m:
        return None
    return {"a": int(m.group(1), 16), "x": int(m.group(2), 16),
            "y": int(m.group(3), 16), "s": int(m.group(4), 16),
            "p": int(m.group(5), 16), "pc": int(m.group(6), 16),
            "mpr": [int(m.group(7)[i:i + 2], 16) for i in range(0, 16, 2)],
            "steps": int(m.group(8)), "crc": int(m.group(9), 16),
            "bios": int(m.group(10)), "cycles": int(m.group(11)),
            "frames": int(m.group(12)), "irq": int(m.group(13)),
            "vblank": int(m.group(14))}


def parse_video(text):
    m = re.search(r"^VIDEO vdc_writes=(\d+) vram_writes=(\d+) vdc_reads=(\d+) "
                  r"dma=(\d+) sat_dma=(\d+) vblank_irq=(\d+) rcr_irq=(\d+) "
                  r"cr=([0-9A-F]{4}) mwr=([0-9A-F]{4}) vdw=(\d+) hdr=([0-9A-F]{4}) "
                  r"bg=(\d) spr=(\d)", text, re.M)
    if not m:
        return None
    return {"vdc_writes": int(m.group(1)), "vram_writes": int(m.group(2)),
            "vdc_reads": int(m.group(3)), "dma": int(m.group(4)),
            "sat_dma": int(m.group(5)), "vblank_irq": int(m.group(6)),
            "rcr_irq": int(m.group(7)), "cr": int(m.group(8), 16),
            "mwr": int(m.group(9), 16), "vdw": int(m.group(10)),
            "hdr": int(m.group(11), 16), "bg": int(m.group(12)),
            "spr": int(m.group(13))}


def parse_calls(text):
    calls = []
    for m in re.finditer(r"^CALL pc=([0-9A-F]{4}) count=(\d+) dest=([0-9A-F]{4}) "
                         r"bank=(\d+) sector=([0-9A-F]{2}) mode=([0-9A-F]{2}) "
                         r"record=(-?\d+) modelled=(\d+) bytes=(\d+)$", text, re.M):
        calls.append({"pc": int(m.group(1), 16), "count": int(m.group(2)),
                      "dest": int(m.group(3), 16), "bank": int(m.group(4)),
                      "sector": int(m.group(5), 16), "mode": int(m.group(6), 16),
                      "record": int(m.group(7)), "modelled": m.group(8) == "1",
                      "bytes": int(m.group(9))})
    return calls


def parse_stop(text):
    m = re.search(r"^STOP reason=(\S+) pc=([0-9A-F]{4})$", text, re.M)
    return None if not m else {"reason": m.group(1), "pc": int(m.group(2), 16)}


def parse_hw(text):
    out = {}
    for m in re.finditer(r"\[\$([0-9A-F]{4})\]=(?:\d+)x", text):
        pass
    for m in re.finditer(r"\[\$([0-9A-F]{4})\]=(\d+)x", text):
        out[int(m.group(1), 16)] = int(m.group(2))
    return out


def parse_dump(path):
    with open(path, "rb") as f:
        return f.read()


def phys(mem, mpr, addr):
    """Resolve a visible address through a given MPR set."""
    return mpr[(addr >> 13) & 7] * 8192 + (addr & 0x1FFF)


class Checker:
    def __init__(self, name):
        self.name = name
        self.failures = []
        self.checks = 0

    def ok(self, cond, msg):
        self.checks += 1
        if not cond:
            self.failures.append(msg)
        return cond

    def eq(self, got, want, what):
        return self.ok(got == want, "%s: got %r, want %r" % (what, got, want))

    def report(self, extra=""):
        status = "PASS" if not self.failures else "FAIL"
        print("%s  %-28s %3d checks %s" % (status, self.name, self.checks, extra))
        for f in self.failures[:20]:
            print("    - " + f)
        if len(self.failures) > 20:
            print("    ... and %d more" % (len(self.failures) - 20))
        return not self.failures
