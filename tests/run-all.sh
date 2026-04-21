#!/usr/bin/env bash
# Top-level orchestrator. Runs every layer of the test architecture:
#
#   Layer 1 - unit tests on compiler internals       (make test-unit)
#   Layer 2 - end-to-end behavioural (existing suite)(tests/run.sh)
#   Layer 3 - negative tests                         (tests/negative/run.sh)
#   Layer 4 - property-based tests                   (tests/property/run.sh)
#   Layer 7 - golden/snapshot tests                  (tests/golden/run.sh)
#            +  regression corpus                    (tests/regression/*.xs)
#            +  conformance suite                    (tests/conformance/*.xs)
#
# Each layer prints its own summary. The orchestrator is bisect-friendly:
# the line "=== LAYER <n>: PASS ===" (or FAIL) is guaranteed to appear
# once per layer, so `git bisect run` can grep for the first FAIL.
#
# Environment:
#   FAST=1     skip slow layers (property, fuzz) for quick iterations
#   LAYERS="1 3 7"   run only these layers
set -u
cd "$(dirname "$0")/.."

LAYERS="${LAYERS:-1 2 3 4 7 R C}"
FAST="${FAST:-0}"

pass_total=0
fail_total=0

run_layer() {
    local n="$1"; shift
    local label="$1"; shift
    echo
    echo "=== LAYER $n: $label ==="
    if "$@"; then
        echo "=== LAYER $n: PASS ==="
        pass_total=$((pass_total + 1))
    else
        echo "=== LAYER $n: FAIL ==="
        fail_total=$((fail_total + 1))
    fi
}

run_xs_dir() {
    local dir="$1" label="$2"
    local pass=0 fail=0
    # Run every file against every backend; a regression or conformance
    # test has to agree across interp/vm/jit or it isn't really passing.
    # When a backend fails, print its full output so the CI log has
    # enough context to diagnose without re-running locally.
    for f in "$dir"/*.xs; do
        [ -f "$f" ] || continue
        local name base_rc=0 skip_jit=0
        name=$(basename "$f" .xs)
        # A test can opt out of the JIT leg by including a top-of-file
        # marker. Use this for regressions that exercise interp/vm
        # behaviour where the JIT lowering still needs work; the test
        # still runs under interp+vm so the bug stays locked in.
        if head -3 "$f" 2>/dev/null | grep -q "skip-backend: *jit"; then
            skip_jit=1
        fi
        for mode in interp vm jit; do
            if [ "$mode" = "jit" ] && [ $skip_jit -eq 1 ]; then continue; fi
            out=$(./xs --$mode "$f" 2>&1)
            rc=$?
            if [ $rc -ne 0 ]; then
                fail=$((fail + 1))
                base_rc=1
                echo "  FAIL  $name ($mode)"
                echo "$out" | sed 's/^/        /'
            else
                pass=$((pass + 1))
            fi
        done
        if [ $base_rc -eq 0 ]; then
            if [ $skip_jit -eq 1 ]; then
                echo "  ok    $name (interp+vm)"
            else
                echo "  ok    $name (interp+vm+jit)"
            fi
        fi
    done
    if [ $fail -eq 0 ]; then
        echo "[$label] $pass passed"
        return 0
    else
        echo "[$label] $fail/$((pass + fail)) failed"
        return 1
    fi
}

for L in $LAYERS; do
    case "$L" in
        1) run_layer 1 "unit tests"       make -s test-unit ;;
        2) run_layer 2 "end-to-end"       bash tests/run.sh ;;
        3) run_layer 3 "negative"         bash tests/negative/run.sh ;;
        4) if [ "$FAST" != "1" ]; then
               run_layer 4 "property"     bash tests/property/run.sh
           fi ;;
        7) run_layer 7 "golden snapshot"  bash tests/golden/run.sh ;;
        R) run_layer R "regression corpus" run_xs_dir tests/regression regression ;;
        C) run_layer C "conformance"      run_xs_dir tests/conformance conformance ;;
        *) echo "unknown layer: $L" >&2 ;;
    esac
done

echo
echo "=============================="
echo " $pass_total layers passed, $fail_total layers failed"
echo "=============================="
if [ $fail_total -ne 0 ]; then exit 1; fi
exit 0
