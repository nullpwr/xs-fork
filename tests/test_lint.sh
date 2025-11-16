#!/bin/bash
# lint tests: verify the linter catches various issues
cd "$(dirname "$0")/.."
pass=0
fail=0

check_lint() {
    local desc="$1"
    local file="$2"
    local pattern="$3"
    local output
    output=$(./xs lint "$file" 2>&1)
    if echo "$output" | grep -qi "$pattern"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL  $desc (expected '$pattern')"
        echo "        got: $(echo "$output" | head -1)"
    fi
}

check_lint "unused variable" \
    tests/lint_samples/unused_var.xs \
    "unused variable"

check_lint "unreachable code" \
    tests/lint_samples/unreachable.xs \
    "unreachable"

check_lint "self comparison" \
    tests/lint_samples/self_compare.xs \
    "comparison.*itself"

check_lint "constant condition" \
    tests/lint_samples/constant_cond.xs \
    "constant condition"

check_lint "double negation" \
    tests/lint_samples/double_neg.xs \
    "double negation"

check_lint "shadowed variable" \
    tests/lint_samples/shadowed.xs \
    "shadowed"

echo ""
echo "lint tests: $pass passed, $fail failed"
[ $fail -eq 0 ] && exit 0 || exit 1
