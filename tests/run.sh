#!/bin/bash
# Full XS test suite. Each test runs under --interp and --vm separately,
# output is captured, and the two are diffed. Divergence between the
# backends fails the test even if each one individually "passes", which
# is how earlier sessions caught silent match-pattern bugs in the VM.
set -u
cd "$(dirname "$0")/.."

# Under ASan+UBSan, LeakSanitizer would otherwise turn every clean
# run into a failure because the VM compiler and a handful of other
# components don't free their per-run working memory before exit --
# those are real leaks but process-lifetime, not per-request, so
# they'd swamp the signal from a newly-introduced bug. Report them
# to stderr but don't inherit the failing exit code. The
# tests/lsan.supp file has an initial pass at suppressing a few of
# the easy ones; expand it as they get fixed.
export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=0:suppressions=$PWD/tests/lsan.supp:print_suppressions=0}"

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

# ASan / UBSan inflate the C stack with shadow memory and instrumentation,
# so the interpreter's tree-walker blows through its recursion budget on
# the 10k-deep adversarial test well before the XS-level StackOverflow
# guard can fire. Under sanitizers we skip the cross-backend diff for
# this test; the VM and JIT still run it end-to-end and get the
# expected StackOverflow path.
if readelf -d ./xs 2>/dev/null | grep -q asan; then
    SKIP_DIFF_NAMES+=("test_deep_recursion")
fi

# JIT runs every test. The JIT emits real x86-64 machine code in
# mmap'd executable memory and drives the same VM dispatch as --vm,
# so output is identical by construction.
SKIP_JIT_NAMES=()

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
    [ -n "$extra" ] && echo "$extra" | head -40 | sed 's/^/        /'
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

    local out_interp rc_interp out_vm rc_vm out_jit rc_jit
    # Capture stdout for diffing; merge stderr for failure reporting only.
    local combo_interp combo_vm combo_jit
    combo_interp=$(./xs --interp "$f" 2>/tmp/xs_err_i.txt); rc_interp=$?
    out_interp="$combo_interp"
    combo_vm=$(./xs --vm "$f" 2>/tmp/xs_err_v.txt);           rc_vm=$?
    out_vm="$combo_vm"
    # JIT compiles to C via the transpiler; features the transpiler
    # doesn't yet cover (plugins, some concurrency primitives, async
    # runtime, DB, provenance hooks) will fail to compile. Those
    # tests stay on interp+vm only. Everything else is triple-checked.
    local run_jit=1
    # Empty-array expansion under `set -u` trips bash 3.2 (macOS) and
    # older bash builds on CI Windows, so guard the length.
    if [ "${#SKIP_JIT_NAMES[@]}" -gt 0 ]; then
        for skip in "${SKIP_JIT_NAMES[@]}"; do
            if [ "$name" = "$skip" ]; then run_jit=0; break; fi
        done
    fi
    if [ $run_jit -eq 1 ]; then
        combo_jit=$(./xs --jit "$f" 2>/tmp/xs_err_j.txt); rc_jit=$?
        out_jit="$combo_jit"
        # If the JIT itself reports it could not emit code (e.g. an
        # unsupported platform) it falls back to the VM and prints a
        # marker. Treat that as a transparent skip rather than a real
        # JIT comparison.
        if grep -q "falling back to VM" /tmp/xs_err_j.txt 2>/dev/null; then
            run_jit=0
        fi
    fi

    # Build a combined stdout+stderr blob for the failure reports so the
    # actual diagnostic message is visible in CI. Non-zero exits with
    # silent stderr (e.g. a Windows-only shutdown abort) are unreadable
    # otherwise.
    dump_fail() {
        local out="$1" errfile="$2"
        printf '%s\n' "$out"
        if [ -s "$errfile" ]; then
            printf -- '--- stderr ---\n'
            cat "$errfile"
        fi
    }
    if [ $rc_interp -ne 0 ] && [ $rc_vm -ne 0 ]; then
        report_fail "BOTH" "$name" "$(dump_fail "$out_interp" /tmp/xs_err_i.txt)"
        return
    fi
    if [ $rc_interp -ne 0 ]; then
        report_fail "INTERP" "$name" "$(dump_fail "$out_interp" /tmp/xs_err_i.txt)"
        return
    fi
    if [ $rc_vm -ne 0 ]; then
        report_fail "VM" "$name" "$(dump_fail "$out_vm" /tmp/xs_err_v.txt)"
        return
    fi
    if [ $run_jit -eq 1 ] && [ $rc_jit -ne 0 ]; then
        report_fail "JIT" "$name" "$(dump_fail "$out_jit" /tmp/xs_err_j.txt)"
        return
    fi

    if is_diff_skipped "$name"; then
        pass=$((pass + 1))
        echo "  ok    $name (both, diff skipped)"
        return
    fi

    local a b c
    a=$(echo "$out_interp" | scrub)
    b=$(echo "$out_vm" | scrub)
    if [ "$a" != "$b" ]; then
        diverge=$((diverge + 1))
        fails="$fails\n  DIVERGE: $name"
        echo "  DIVERGE $name (interp and vm disagree)"
        diff <(echo "$a") <(echo "$b") | head -8 | sed 's/^/        /'
        return
    fi
    if [ $run_jit -eq 1 ]; then
        c=$(echo "$out_jit" | scrub)
        if [ "$a" != "$c" ]; then
            diverge=$((diverge + 1))
            fails="$fails\n  DIVERGE-JIT: $name"
            echo "  DIVERGE $name (jit disagrees)"
            diff <(echo "$a") <(echo "$c") | head -8 | sed 's/^/        /'
            return
        fi
    fi

    pass=$((pass + 1))
    if [ $run_jit -eq 1 ]; then
        echo "  ok    $name (interp+vm+jit)"
    else
        echo "  ok    $name"
    fi
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

# 4. Resource-limit CLI tests
if [ -f tests/test_limits.sh ]; then
    lim_output=$(bash tests/test_limits.sh 2>&1)
    lim_rc=$?
    lim_pass=$(echo "$lim_output" | grep -oP '\d+ passed' | grep -oP '\d+')
    if [ "$lim_rc" -eq 0 ]; then
        pass=$((pass + 1)); echo "  ok    test_limits (${lim_pass:-0} checks)"
    else
        report_fail "FAIL" "test_limits" "$(echo "$lim_output" | grep FAIL)"
    fi
fi

echo
echo "results: $pass passed, $fail failed, $diverge diverged"
if [ -n "$fails" ]; then
    echo -e "\nproblems:$fails"
    exit 1
fi
exit 0
