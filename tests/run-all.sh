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
    for f in "$dir"/*.xs; do
        [ -f "$f" ] || continue
        if out=$(./xs "$f" 2>&1); then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "  FAIL  $(basename "$f")"
            echo "$out" | tail -3 | sed 's/^/        /'
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
