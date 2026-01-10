#!/bin/bash
# Adversarial tests: deep recursion, huge strings, pathological match, etc.
# Run explicitly via `bash tests/adversarial/run.sh`. These are stricter than
# the main suite and meant to be paired with `make asan` / `make ubsan`.

cd "$(dirname "$0")/../.."

# Same LSan policy as the main suite: report leaks but don't fail the
# process over one-shot shutdown leaks that the OS reclaims anyway.
export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=0:print_suppressions=0}"

pass=0
fail=0

for f in tests/adversarial/test_*.xs; do
    name=$(basename "$f" .xs)
    out=$(./xs "$f" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        fail=$((fail + 1))
        echo "  FAIL  $name (exit $rc)"
        echo "$out" | tail -3
    else
        pass=$((pass + 1))
        echo "  ok    $name"
    fi
done

# stdin bounds bug: piping source through /dev/stdin must not trip glibc fortify
got=$(echo 'println(1)' | ./xs /dev/stdin 2>&1)
if [ "$got" = "1" ]; then
    pass=$((pass + 1))
    echo "  ok    stdin pipe"
else
    fail=$((fail + 1))
    echo "  FAIL  stdin pipe: $got"
fi

# unhandled throw at top level must exit non-zero
echo 'throw "boom"' > /tmp/adv_throw.xs
./xs /tmp/adv_throw.xs > /dev/null 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    pass=$((pass + 1))
    echo "  ok    unhandled throw exit code"
else
    fail=$((fail + 1))
    echo "  FAIL  unhandled throw did not exit non-zero"
fi
rm -f /tmp/adv_throw.xs

echo ""
echo "adversarial: $pass passed, $fail failed"
[ $fail -eq 0 ] && exit 0 || exit 1
