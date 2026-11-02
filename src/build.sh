#!/usr/bin/env bash
# build.sh -- regenerate the translated C and compile the headless test host.
#
#   ./build.sh                      boot chain (loader + boot program) -> out/host
#   ./build.sh stage <state.img>    translate the code a dumped RAM image holds
#                                   (a savestate converted by tools/mkstate.py)
#                                   -> out/host_stage
#   ./build.sh cases <specs.json> <out-binary>
#   ./build.sh selftest             hand-checked sequence host -> out/selftest_host
#
# INPUTS.  Two files, both the user's: the System Card image and the game's data
# track (README says where to put them).  Nothing of the game is in this
# repository; every byte the translator reads is sliced out of those two files at
# build time (tools/slice_disc.py).
#
#   BIOS=/path/to/syscard3.pce  DISC=/path/to/data_payload.bin  ./build.sh
#
# Warnings are treated as failures: the compiler output is captured and the build
# stops if any line contains "warning:".  A translated program that only compiles
# because nobody looked at the warnings is not built.
set -euo pipefail
cd "$(dirname "$0")"

ROOT=$(cd .. && pwd)
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion}
WARNFLAGS="-Wall -Wextra -Wpedantic -Wshadow"
INPUTS=${INPUTS:-$ROOT/inputs}
BIOS=${BIOS:-$INPUTS/syscard3.pce}
DISC=${DISC:-$INPUTS/data_payload.bin}
BLOCK=out/block
LOADER=$BLOCK/loader.bin
BOOTPROG=$BLOCK/bootprog.bin

for f in "$BIOS" "$DISC"; do
    if [ ! -f "$f" ]; then
        echo "BUILD FAILED: input not found: $f" >&2
        echo "  This repository ships no game data.  Put your own System Card image" >&2
        echo "  and data track in inputs/ (or set BIOS=/... DISC=/...).  See README." >&2
        exit 1
    fi
done

# The content-keyed code/data log: a record of which bytes of an 8 KB bank image
# are code, keyed by the SHA-1 of that image, produced by a fully instrumented run
# of the game (a CDL).  The key set itself is tracked at keyset.json: it holds
# only SHA-1 hashes and offset ranges -- no game bytes -- so the build is
# reproducible from a clean checkout.
#
# It exists because the static descent cannot reach code entered through a computed
# jump or an entry the program builds at run time.  A missing key set is therefore
# a BUILD FAILURE, not a silent loss of coverage.  CDL_KEYSET=none opts out
# deliberately: the translation then has only what the descent reaches, which is
# measured at 19.4 % of the executed code bytes, and the run stops where the first
# unreachable entry point is.  ../tools/cdl_coverage.py measures the difference.
CDL_KEYSET=${CDL_KEYSET:-keyset.json}        # the script has already cd'd to its own dir
if [ "$CDL_KEYSET" != "none" ] && [ ! -f "$CDL_KEYSET" ]; then
    echo "BUILD FAILED: code/data key set not found: $CDL_KEYSET" >&2
    echo "  Set CDL_KEYSET=none to build without one, or point CDL_KEYSET at a key" >&2
    echo "  set in the format keyset.json uses (sha1 of an 8 KB bank image ->" >&2
    echo "  offsets marked code).  Without one the translator misses every address" >&2
    echo "  the descent cannot reach." >&2
    exit 1
fi

mkdir -p out

# The interpreter's opcode and cycle tables are generated, never hand-written: one
# source for widths/modes (isa.py, the translator's own table) and one for costs
# (cycles.py, whose numbers the translator emits).  A second hand-written table is
# how frame-exactness gets lost while every test still passes.
#
# WRITTEN ATOMICALLY, and that is not decoration.  Two builds of one tree (two agents,
# two shells) both regenerate this file; a plain redirect lets one compile a header the
# other is halfway through writing, and the symptom is not a build error but a *cycle
# total* that disagrees with the translated code by two per read-modify-write.  Write
# to a private name and rename: a reader then always sees a whole table, and the last
# writer wins atomically.
echo "[tables] generating the interpreter's opcode/cycle tables"
python3 tools/gen_interp_tables.py --out "out/interp_tables.h.$$"
mv -f "out/interp_tables.h.$$" out/interp_tables.h

fail_on_warning() {
    local logfile="$1"
    if grep -q "warning:" "$logfile"; then
        echo "BUILD FAILED: compiler warnings (treated as errors):" >&2
        grep -n "warning:" "$logfile" | head -40 >&2
        return 1
    fi
    return 0
}

compile() {   # compile <out> <sources...>
    local out="$1"; shift
    local logf
    logf=$(mktemp)
    echo "  cc -> $out"
    if ! "$CC" $FLAGS_UNIT -I. -Iout "$@" -o "$out" 2>"$logf"; then
        echo "BUILD FAILED: compiler error:" >&2
        cat "$logf" >&2
        rm -f "$logf"
        return 1
    fi
    if ! fail_on_warning "$logf"; then
        cat "$logf" >&2
        rm -f "$logf"
        return 1
    fi
    if [ -s "$logf" ]; then
        echo "  (compiler notes: $(wc -l < "$logf") lines)"
        cat "$logf"
    fi
    rm -f "$logf"
}

build_boot() {
    FLAGS_UNIT="$CFLAGS"
    echo "[1/2] translating the boot chain"
    # Entry points beyond the loader and the boot program's start: `jmp ($xxxx)`
    # targets that static discovery cannot follow.  tools/discover_seeds.py fills
    # this file (a source file, not a build output) by running the program and
    # reading the dispatcher's own report.
    # Every image the translator reads is sliced out of the user's data track here,
    # every build: the boot chain's three images and the 24 banks of the game
    # module, by sector number (tools/slice_disc.py holds the table).
    mkdir -p out/block
    python3 tools/slice_disc.py "$DISC" --out out/block
    SEEDARGS=()
    if [ -f boot_seeds.txt ]; then
        while IFS= read -r line; do
            line="${line%%#*}"
            line="${line//[[:space:]]/}"
            if [ -n "$line" ]; then
                SEEDARGS+=(--seed "$line")
            fi
        done < boot_seeds.txt
    fi
    python3 recompiler.py \
        --image "0xF8:0x3000=$LOADER" \
        --code  "0x80:0x4000=$BOOTPROG" \
        --code  "0x69:0x2000=out/block/b69.bin" \
        --code  "0x6A:0x4000=out/block/b6A.bin" \
        --code  "0x6B:0x6000=out/block/b6B.bin" \
        --code  "0x6C:0x8000=out/block/b6C.bin" \
        --code  "0x6D:0xA000=out/block/b6D.bin" \
        --code  "0x6E:0xC000=out/block/b6E.bin" \
        --code  "0x6F:0xE000=out/block/b6F.bin" \
        --code  "0x70:0x10000=out/block/b70.bin" \
        --code  "0x71:0x12000=out/block/b71.bin" \
        --code  "0x72:0x14000=out/block/b72.bin" \
        --code  "0x73:0x16000=out/block/b73.bin" \
        --code  "0x74:0x18000=out/block/b74.bin" \
        --code  "0x75:0x1A000=out/block/b75.bin" \
        --code  "0x76:0x1C000=out/block/b76.bin" \
        --code  "0x77:0x1E000=out/block/b77.bin" \
        --code  "0x78:0x20000=out/block/b78.bin" \
        --code  "0x79:0x22000=out/block/b79.bin" \
        --code  "0x7A:0x24000=out/block/b7A.bin" \
        --code  "0x7B:0x26000=out/block/b7B.bin" \
        --code  "0x7C:0x28000=out/block/b7C.bin" \
        --code  "0x7D:0x2A000=out/block/b7D.bin" \
        --code  "0x7E:0x2C000=out/block/b7E.bin" \
        --code  "0x7F:0x2E000=out/block/b7F.bin" \
        --rom "$BIOS" \
        --cdl-keyset "$CDL_KEYSET" \
        --entry 0x3000 \
        "${SEEDARGS[@]}" \
        --out out/boot.c
    echo "[2/2] compiling"
    compile out/host runtime.c video.c interp.c trace.c portstate.c out/boot.c host.c
    echo "built out/host"
}

build_stage() {
    local img="$1"
    FLAGS_UNIT="$CFLAGS"
    echo "[1/3] slicing the live state into physical banks"
    mkdir -p out/stage
    python3 tools/state_banks.py "$img" --out out/stage

    echo "[2/3] translating the stage banks (from the state's own bytes)"
    # `stage_seeds.txt` is the stage's counterpart of boot_seeds.txt: entry points the
    # static descent cannot follow, discovered by running (tools/discover_seeds.py
    # --state).  A bank taken from a live machine is often mapped at a page the state
    # does not show, so most banks go in as bytes only (--bytes) and their code is
    # translated where the code's own `tam` or a discovered seed puts it.
    local ARGS=()
    while IFS= read -r line; do
        if [ -n "$line" ]; then ARGS+=(--bytes "$line"); fi
    done < out/stage/bytes.args
    while IFS= read -r line; do
        if [ -n "$line" ]; then ARGS+=(--code "$line"); fi
    done < out/stage/mapped.args
    local SEEDARGS=()
    STAGE_PC=$(sed -n 's/^pc=//p' out/stage/meta.txt)
    STAGE_MPR=$(sed -n 's/^mpr=//p' out/stage/meta.txt | tr ' ' ',')
    if [ -f stage_seeds.txt ]; then
        while IFS= read -r line; do
            line="${line%%#*}"
            line="${line//[[:space:]]/}"
            if [ -n "$line" ]; then
                SEEDARGS+=(--seed "$line")
            fi
        done < stage_seeds.txt
    fi
    python3 recompiler.py \
        "${ARGS[@]}" \
        --rom "$BIOS" \
        --cdl-keyset "$CDL_KEYSET" \
        --entry "0x${STAGE_PC}" \
        --mpr "$STAGE_MPR" \
        "${SEEDARGS[@]}" \
        --out out/stage.c
    echo "[3/3] compiling"
    compile out/host_stage runtime.c video.c interp.c trace.c portstate.c out/stage.c host.c
    echo "built out/host_stage  (run: ./out/host_stage run --from-state $img ...)"
}

build_cases() {
    local specs="$1" outbin="$2"
    FLAGS_UNIT="-std=c11 -O0 -Wall -Wextra -Wpedantic"
    mkdir -p out/cases
    local stem
    stem=$(basename "$outbin" .bin)
    python3 recompiler.py --cases "$specs" --out "out/cases/$stem.c" >/dev/null
    compile "$outbin" runtime.c video.c interp.c trace.c portstate.c "out/cases/$stem.c" host.c -DXLATE_WITH_CASES
}

build_selftest() {
    FLAGS_UNIT="$CFLAGS"
    python3 tools/mk_selftest.py --out out/selftest.c
    compile out/selftest_host runtime.c video.c interp.c trace.c portstate.c out/selftest.c host.c
}

case "${1:-boot}" in
    boot|"")  build_boot ;;
    stage)
        if [ $# -lt 2 ]; then
            echo "usage: $0 stage <state.img>   (the XLST1 image tools/mkstate.py writes)" >&2
            exit 64
        fi
        build_stage "$2"
        ;;
    cases)    build_cases "$2" "$3" ;;
    selftest) build_selftest ;;
    banktest)
        FLAGS_UNIT="$CFLAGS"
        python3 tools/mk_banktest.py >/dev/null
        for t in unit klo khi; do
            compile "out/bank_${t}_host" runtime.c video.c interp.c trace.c portstate.c "out/bank_${t}.c" host.c
        done
        ;;
    irqtest)
        FLAGS_UNIT="$CFLAGS"
        python3 tools/mk_irqtest.py >/dev/null
        compile out/irqtest_host runtime.c video.c interp.c trace.c portstate.c out/irqtest.c host.c
        ;;
    *) echo "usage: $0 [boot|stage <state.img>|cases <specs.json> <out>|selftest|irqtest|banktest]" >&2; exit 64 ;;
esac
