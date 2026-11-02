#!/usr/bin/env bash
# build_vec_host.sh -- build out/vec_host, the CPU-vector harness that
# tools/vec_suite.py drives.
#
# This binary is deliberately NOT part of the port's own build: it links the
# interpreter with -DXLATE_BUS_REC so every physical (address, byte, direction)
# the CPU touches is recorded, and it has its own main().
#
# REBUILD IT AFTER ANY CHANGE TO runtime.c, interp.c, cycles.py OR THE TABLES.
# A stale vec_host keeps reporting the previous machine, and the symptom is not
# an error -- it is a fix that appears to have had no effect. That cost one
# debugging cycle on 2026-10-30: a TSB/TRB flag correction was measured against
# a binary built before it.
#
#   ./tools/build_vec_host.sh
#   python3 tools/vec_suite.py --all
set -euo pipefail
cd "$(dirname "$0")/../src"

if [ ! -f out/interp_tables.h ]; then
    echo "build_vec_host: out/interp_tables.h missing -- run ./build.sh first" >&2
    exit 1
fi

gcc -std=c11 -O2 -I. -Iout -DXLATE_BUS_REC -o out/vec_host \
    ../tools/vec_host.c runtime.c interp.c video.c trace.c portstate.c

echo "built out/vec_host"
