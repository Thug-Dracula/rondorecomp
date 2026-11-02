#!/usr/bin/env bash
# run_all.sh -- the whole suite: build, hand-checked self-test, boot chain, video
# renderer, interrupts/timing, differential test.
#
# Run it from anywhere; it works in src/ (where build.sh and out/ live) and calls
# the test programs by path.
#
# Exits non-zero if any stage fails.  A stage that cannot run because the user has
# supplied no game data, or because it compares against a reference capture tree
# this repository does not ship, exits 77 and is reported as SKIP (see README and
# docs/VERIFICATION.md).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../src" && pwd)
cd "$SRC"

echo "=== test suite ====================================================="
echo "log: $SRC/out/xlate.log"
echo

fail=0
skipped=0
run() {
    local name="$1"; shift
    local rc
    echo "--- $name"
    "$@"
    rc=$?
    case "$rc" in
        0)  echo ;;
        77) echo "*** $name SKIPPED"
            skipped=$((skipped + 1))
            echo ;;
        *)  echo "*** $name FAILED"
            fail=1
            echo ;;
    esac
}

run "build (warnings are errors)" ./build.sh
run "self-test (hand-checked expectations)" python3 "$HERE/test_selftest.py"
run "boot chain (loader -> record \$23 -> \$4000)" python3 "$HERE/test_boot.py"
run "video renderer vs reference frames" python3 "$HERE/test_video.py"
run "interrupts + timing (translated code)" python3 "$HERE/test_irq.py"
run "bank-keyed translation (same address, two banks)" python3 "$HERE/test_bank.py"
run "frame vs reference (which layer diverges)" python3 "$HERE/test_frame.py"
run "input replay (a schedule of pad windows)" python3 "$HERE/test_input.py"
run "reference state comparison (PC/banks/memory)" python3 "$HERE/test_oracle_state.py"
run "differential (generated C and C interpreter vs Python reference)" \
    python3 "$HERE/test_diff.py" --total 2688 --batch 384
run "forced interpretation (same frames as the translation)" \
    python3 "$HERE/test_interp.py"
run "overlay guard (a stale bank is interpreted, then released)" \
    python3 "$HERE/test_guard.py"

if [ "$fail" -eq 0 ]; then
    echo "=== all stages passed ($skipped skipped) ==========================="
else
    echo "=== FAILURES ABOVE ================================================="
fi
exit "$fail"
