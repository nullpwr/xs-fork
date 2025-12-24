#!/bin/bash
# Full XS test suite. Each test runs under --interp and --vm separately,
# output is captured, and the two are diffed. Divergence between the
# backends fails the test even if each one individually "passes", which
# is how earlier sessions caught silent match-pattern bugs in the VM.
set -u
cd "$(dirname "$0")/.."

pass=0
fail=0
diverge=0
fails=""

# Skip cross-backend diff for tests whose output is intrinsically
# non-deterministic (timestamps, randomness, temp paths). These are
# still run end-to-end under both backends; we only bypass the diff.
SKIP_DIFF_NAMES=(
    "test_fs"              # tempfile paths differ per run
    "test_http_client"     # network; output can timestamp
    "test_provenance"      # plugin eval hooks: interp-only path
    "test_pipeline"        # plugin parser productions: interp-only
)

is_diff_skipped() {
    local n="$1"
    for s in "${SKIP_DIFF_NAMES[@]}"; do [ "$n" = "$s" ] && return 0; done
    return 1
}

scrub() {
    # Normalize obvious non-deterministic bits before diff.
    sed -E \
        -e 's|/tmp/xs_tmp_[A-Za-z0-9]+|/tmp/xs_tmp_X|g' \
        -e 's/[0-9]+\.[0-9]+e[+-][0-9]+/<float>/g' \
        -e 's/0x[0-9a-fA-F]+/<ptr>/g'
}

report_fail() {
    local kind="$1" name="$2" extra="$3"
    fail=$((fail + 1))
    fails="$fails\n  $kind: $name"
    echo "  FAIL  $name ($kind)"
    [ -n "$extra" ] && echo "$extra" | head -6 | sed 's/^/        /'
}

run_one() {
    local f="$1"
    local name
    name=$(basename "$f" .xs)

    # test_vm is the historical VM-only file; keep it VM-only.
    if [ "$name" = "test_vm" ]; then
        if out=$(./xs --vm "$f" 2>&1); then
            pass=$((pass + 1)); echo "  ok    $name (vm only)"
        else
            report_fail "VM" "$name" "$out"
        fi
        return
    fi

    local out_interp rc_interp out_vm rc_vm
    # Capture stdout for diffing; merge stderr for failure reporting only.
    local combo_interp combo_vm
    combo_interp=$(./xs --interp "$f" 2>/tmp/xs_err_i.txt); rc_interp=$?
    out_interp="$combo_interp"
    combo_vm=$(./xs --vm "$f" 2>/tmp/xs_err_v.txt);           rc_vm=$?
    out_vm="$combo_vm"

    if [ $rc_interp -ne 0 ] && [ $rc_vm -ne 0 ]; then
        report_fail "BOTH" "$name" "$out_interp"
        return
    fi
    if [ $rc_interp -ne 0 ]; then
        report_fail "INTERP" "$name" "$out_interp"
        return
    fi
    if [ $rc_vm -ne 0 ]; then
        report_fail "VM" "$name" "$out_vm"
        return
    fi

    if is_diff_skipped "$name"; then
        pass=$((pass + 1))
        echo "  ok    $name (both, diff skipped)"
        return
    fi

    local a b
    a=$(echo "$out_interp" | scrub)
    b=$(echo "$out_vm" | scrub)
    if [ "$a" != "$b" ]; then
        diverge=$((diverge + 1))
        fails="$fails\n  DIVERGE: $name"
        echo "  DIVERGE $name (interp and vm disagree)"
        diff <(echo "$a") <(echo "$b") | head -8 | sed 's/^/        /'
        return
    fi

    pass=$((pass + 1))
    echo "  ok    $name"
}

# 1. language tests
for f in tests/test_*.xs; do run_one "$f"; done
for f in tests/adversarial/test_*.xs; do run_one "$f"; done

# 2. examples (sanity: run each with default backend, no diff)
ex_pass=0
ex_fail=0
for f in examples/*.xs; do
    name=$(basename "$f")
    [ "$name" = "check_demo.xs" ] && continue
    out=$(./xs "$f" 2>&1)
    if [ $? -ne 0 ]; then
        ex_fail=$((ex_fail + 1))
        fails="$fails\n  FAIL: examples/$name"
        echo "  FAIL  examples/$name"
        echo "$out" | grep -E "assert|error" | head -2 | sed 's/^/        /'
    else
        ex_pass=$((ex_pass + 1))
    fi
done
if [ $ex_fail -eq 0 ]; then
    pass=$((pass + 1)); echo "  ok    examples ($ex_pass files)"
else
    fail=$((fail + ex_fail))
fi

# 3. CLI flag tests
if [ -f tests/test_cli.sh ]; then
    cli_output=$(bash tests/test_cli.sh 2>&1)
    cli_rc=$?
    cli_pass=$(echo "$cli_output" | grep -oP '\d+ passed' | grep -oP '\d+')
    if [ "$cli_rc" -eq 0 ]; then
        pass=$((pass + 1)); echo "  ok    test_cli (${cli_pass:-0} checks)"
    else
        report_fail "FAIL" "test_cli" "$(echo "$cli_output" | grep FAIL)"
    fi
fi

echo
echo "results: $pass passed, $fail failed, $diverge diverged"
if [ -n "$fails" ]; then
    echo -e "\nproblems:$fails"
    exit 1
fi
exit 0
