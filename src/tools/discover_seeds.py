#!/usr/bin/env python3
"""Find the entry points a translated program needs, by running it.

Static recompilation cannot follow `jmp ($xxxx)`: the target lives in memory. So the
dispatcher reports what it could not reach (`UNTRANSLATED bank=.. pc=..`), this tool
adds those addresses as entry points, rebuilds, and runs again until nothing new
appears. It is the loop the fallback path exists for.

Two programs can be discovered this way:

  (default)      the boot chain: rebuilds ./build.sh, runs out/host from the loader,
                 writes src/boot_seeds.txt
  --state IMG    a stage: rebuilds ./build.sh stage IMG (which translates the bank
                 images the dumped RAM state holds, not the disc's), runs
                 out/host_stage --from-state IMG, writes src/stage_seeds.txt

Both files are sources (part of the build), one BANK:ADDR per line, plus comments.
"""
import argparse
import os
import re
import subprocess
import sys

XL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # src/
REPO = os.path.dirname(XL)
INPUTS = os.environ.get("INPUTS") or os.path.join(REPO, "inputs")
DISC = os.environ.get("DISC") or os.path.join(INPUTS, "data_payload.bin")
BIOS = os.environ.get("BIOS") or os.path.join(INPUTS, "syscard3.pce")
REFDATA = os.environ.get("REFDATA") or os.path.join(REPO, "refdata")
REPLAY = os.path.join(REFDATA, "replays", "press_into_gameplay.replay")
SEEDS = os.path.join(XL, "boot_seeds.txt")
STAGE_SEEDS = os.path.join(XL, "stage_seeds.txt")


def load_seeds(path):
    if not os.path.exists(path):
        return []
    out = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if line:
            out.append(line)
    return out


def build(seeds, stage_state=None):
    cmd = ["./build.sh"] if stage_state is None else ["./build.sh", "stage", stage_state]
    if seeds:
        os.environ["XLATE_BOOT_SEEDS"] = " ".join(seeds)
    r = subprocess.run(cmd, cwd=XL, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit("build failed")
    # build.sh reads the env var; if it is not honoured, rebuild by hand
    return r


def press_args():
    """The oracle's own press cadence, taken from the replay it played."""
    out = subprocess.run([sys.executable, "tools/replay_presses.py", REPLAY,
                          "--as-args"], cwd=XL, capture_output=True, text=True).stdout
    return out.split()


def run(cycles=1450000000):
    r = subprocess.run(["./out/host", "run", "--disc", DISC, "--bios", BIOS,
                        "--steps", "400000000", "--cycles", str(cycles)]
                       + press_args(),
                       cwd=XL, capture_output=True, text=True)
    return r.stdout


def run_stage(state, cycles, steps):
    """The stage host: no disc, no BIOS -- the state supplies every bank it uses."""
    r = subprocess.run(["./out/host_stage", "run", "--from-state", state,
                        "--steps", str(steps), "--cycles", str(cycles)],
                       cwd=XL, capture_output=True, text=True)
    if r.returncode not in (0, 1, 2, 3):
        print(r.stdout, r.stderr)
        raise SystemExit("stage run failed (exit %d)" % r.returncode)
    return r.stdout


def parse(out):
    """New entry points and the run's own numbers, from one run's report."""
    new = []
    for m in re.finditer(r"^UNTRANSLATED bank=([0-9A-F]{2}) pc=([0-9A-F]{4})", out, re.M):
        s = "0x%s:0x%s" % (m.group(1), m.group(2))
        if s not in new:
            new.append(s)
    st = re.search(r"^STATE .*frames=(\d+)", out, re.M)
    split = re.search(r"^SPLIT translated=(\d+) interpreted=(\d+) total=(\d+)", out, re.M)
    guard = re.search(r"^GUARD firings=(\d+) routed_banks=(\d+)", out, re.M)
    stop = re.search(r"^STOP reason=(\w+)", out, re.M)
    return {
        "new": new,
        "frames": st.group(1) if st else "?",
        "split": split.groups() if split else ("?", "?", "?"),
        "guard": guard.groups() if guard else ("?", "?"),
        "stop": stop.group(1) if stop else "budget",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--state", default=None,
                    help="an XLST1 image (tools/mkstate.py): discover the stage's "
                         "entry points instead of the boot chain's")
    ap.add_argument("--iters", type=int, default=20)
    ap.add_argument("--cycles", type=int, default=None)
    ap.add_argument("--steps", type=int, default=400000000)
    a = ap.parse_args()

    seeds_path = STAGE_SEEDS if a.state else SEEDS
    cycles = a.cycles if a.cycles else (40000000 if a.state else 1450000000)

    for iteration in range(a.iters):
        seeds = load_seeds(seeds_path)
        build(seeds, a.state)
        out = run_stage(a.state, cycles, a.steps) if a.state else run(cycles)
        info = parse(out)
        print("iteration %d: %d seeds, new %s, stop %s, frames %s, "
              "split translated/interpreted/total %s, guard firings/routed %s"
              % (iteration + 1, len(seeds), info["new"] or "none", info["stop"],
                 info["frames"], "/".join(info["split"]), "/".join(info["guard"])))
        new = [s for s in info["new"] if s not in seeds]
        if not new:
            if info["stop"] in ("steps", "budget"):
                # Nothing new *and* the run used its whole budget: that is not
                # convergence, it is the budget running out.  Saying "converged" here
                # would claim coverage the run never reached.
                print("budget-limited: the run used its whole budget at frame %s; "
                      "raise --cycles/--steps to explore further" % info["frames"])
            else:
                print("converged: nothing new to translate")
            return 0
        with open(seeds_path, "a") as f:
            for s in new:
                f.write("%s  # discovered: reached at run time, not by static descent\n"
                        % s)
    print("did not converge in %d iterations" % a.iters)
    return 1


if __name__ == "__main__":
    sys.exit(main())
