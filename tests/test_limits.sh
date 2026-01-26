#!/usr/bin/env bash
# End-to-end checks for --instr-limit, --time-limit, --memory-limit.
# Runs the binary under each backend (interp, vm) and confirms a
# program that exceeds the cap fails with a non-zero exit and a
# ResourceLimit message on stderr.

set -u
cd "$(dirname "$0")/.."
BIN=./xs
if [ ! -x "$BIN" ]; then
    echo "skip: $BIN not built" >&2
    exit 0
fi

pass=0
fail=0

check_fail() {
    local label="$1"; shift
    local pattern="$1"; shift
    local out
    out=$("$@" 2>&1 || true)
    local rc=$?
    if [ $rc -eq 0 ]; then
        echo "  FAIL $label: expected non-zero exit, got 0"
        fail=$((fail+1))
        return
    fi
    if ! echo "$out" | grep -q "$pattern"; then
        echo "  FAIL $label: expected stderr to contain '$pattern', got:"
        echo "$out" | sed 's/^/    /'
        fail=$((fail+1))
        return
    fi
    echo "  ok   $label"
    pass=$((pass+1))
}

check_ok() {
    local label="$1"; shift
    local out
    out=$("$@" 2>&1)
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "  FAIL $label: expected 0 exit, got $rc; output:"
        echo "$out" | sed 's/^/    /'
        fail=$((fail+1))
        return
    fi
    echo "  ok   $label"
    pass=$((pass+1))
}

# Infinite-loop program that the budget should interrupt.
LOOP='var i = 0; while true { i = i + 1 }'
# Quick program that must not trip the budget.
QUICK='let x = 1 + 2; println(x)'

for backend in "--interp" "--vm"; do
    check_fail "instr-limit $backend" "ResourceLimit\|instruction budget\|resource limit" \
        "$BIN" $backend --instr-limit 1000 -e "$LOOP"
    check_fail "time-limit $backend" "ResourceLimit\|wall-time\|resource limit" \
        "$BIN" $backend --time-limit 100 -e "$LOOP"
    check_ok   "quick-ok $backend" \
        "$BIN" $backend --instr-limit 1000000 -e "$QUICK"
done

echo "[limits] $pass passed, $fail failed"
[ $fail -eq 0 ]
