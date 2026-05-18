#!/bin/bash
# Transpiler diff tests: for each small program, run under the XS interpreter,
# the XS bytecode VM, the transpiled C binary (at -O0 and -O2), the
# transpiled JS (via node, if present), and the transpiled WASM (via
# wasmtime, if present). Confirm all backends produce the same stdout.
#
# Catches the class of bug where a backend emits something that "compiles"
# but computes the wrong result at runtime, including the slippery cases
# where the program passes at -O0 but the optimiser exposes signed-overflow
# UB at -O2.

cd "$(dirname "$0")/.."
# If this runs against an ASan build, don't let the process-lifetime
# leaks the sanitizer prints to stderr (which we capture as part of
# stdout via 2>&1) randomize the diff. The suppressions file covers the
# known process-wide allocations; fresh leaks would still surface as
# a real ASan error, not as diff churn.
if [ -f tests/lsan.supp ]; then
    export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=0:suppressions=$PWD/tests/lsan.supp:print_suppressions=0}"
fi
pass=0
fail=0
have_node=0
have_wasmtime=0
if command -v node     > /dev/null 2>&1; then have_node=1;     fi
if command -v wasmtime > /dev/null 2>&1; then have_wasmtime=1; fi

one_diff() {
    local desc="$1"
    local src="$2"
    local tmp=$(mktemp -d)
    local xsfile="$tmp/prog.xs"
    printf '%s\n' "$src" > "$xsfile"

    local interp_out vm_out c_out js_out
    interp_out=$(./xs "$xsfile" 2>&1)
    vm_out=$(./xs --vm "$xsfile" 2>&1)

    if [ "$interp_out" != "$vm_out" ]; then
        fail=$((fail + 1))
        echo "  FAIL  $desc (interp vs vm)"
        echo "        interp: $interp_out"
        echo "        vm:     $vm_out"
        rm -rf "$tmp"; return
    fi

    # C backend at -O0: transpile, compile, run
    if ./xs --emit c "$xsfile" > "$tmp/prog.c" 2>/dev/null \
        && gcc -o "$tmp/prog_c" "$tmp/prog.c" -lm 2>/dev/null; then
        c_out=$("$tmp/prog_c" 2>&1)
        if [ "$interp_out" != "$c_out" ]; then
            fail=$((fail + 1))
            echo "  FAIL  $desc (interp vs c -O0)"
            echo "        interp: $interp_out"
            echo "        c:      $c_out"
            rm -rf "$tmp"; return
        fi
        # Also at -O2: catches signed-overflow UB that -O0 hides. The
        # released binary is built with -O2 and end users compile
        # `--emit c` output with whatever flags they like.
        if gcc -O2 -o "$tmp/prog_co" "$tmp/prog.c" -lm 2>/dev/null; then
            local co_out
            co_out=$("$tmp/prog_co" 2>&1)
            if [ "$interp_out" != "$co_out" ]; then
                fail=$((fail + 1))
                echo "  FAIL  $desc (interp vs c -O2)"
                echo "        interp: $interp_out"
                echo "        c -O2:  $co_out"
                rm -rf "$tmp"; return
            fi
        fi
    fi

    # JS backend: transpile and run if node is present
    if [ "$have_node" = "1" ]; then
        if ./xs --emit js "$xsfile" > "$tmp/prog.js" 2>/dev/null; then
            js_out=$(node "$tmp/prog.js" 2>&1)
            if [ "$interp_out" != "$js_out" ]; then
                fail=$((fail + 1))
                echo "  FAIL  $desc (interp vs js)"
                echo "        interp: $interp_out"
                echo "        js:     $js_out"
                rm -rf "$tmp"; return
            fi
        fi
    fi

    # WASM backend: transpile and run via wasmtime if present
    if [ "$have_wasmtime" = "1" ]; then
        if ./xs --emit wasm "$xsfile" > "$tmp/prog.wasm" 2>/dev/null; then
            local wa_out
            wa_out=$(wasmtime "$tmp/prog.wasm" 2>&1)
            if [ "$interp_out" != "$wa_out" ]; then
                fail=$((fail + 1))
                echo "  FAIL  $desc (interp vs wasm)"
                echo "        interp: $interp_out"
                echo "        wasm:   $wa_out"
                rm -rf "$tmp"; return
            fi
        fi
    fi

    pass=$((pass + 1))
    rm -rf "$tmp"
}

one_diff "arithmetic" \
'println(2 + 3 * 4)'

one_diff "if/else expr" \
'let x = 10
if x > 5 { println("big") } else { println("small") }'

one_diff "fn + recursion" \
'fn fib(n) { if n <= 1 { return n } return fib(n - 1) + fib(n - 2) }
println(fib(10))'

one_diff "array len" \
'let a = [1, 2, 3, 4]
println(a.len())'

one_diff "string interp" \
'let name = "xs"
println("hello {name}")'

one_diff "match with guard" \
'fn describe(val) {
    match val {
        0 => "zero"
        x if x < 0 => "negative"
        _ => "positive"
    }
}
println(describe(-5))'

one_diff "closure" \
'let make_adder = fn(n) { fn(x) { x + n } }
let add5 = make_adder(5)
println(add5(3))'

# Probes added after v1.2.14 surfaced backend divergences that the
# conformance suite's coverage missed. Each one is a reduced repro of
# a real bug; keep them so the next round of transpiler work can't
# regress us back into trapped-but-passing-on-conformance territory.

one_diff "transitive closure (fn-decl)" \
'fn outer() {
    var x = 100
    fn middle() {
        var y = 10
        fn inner() { x + y }
        inner
    }
    middle
}
println(outer()()())'

one_diff "transitive closure (lambda)" \
'fn outer() {
    let x = 100
    let middle = fn() {
        let y = 10
        let inner = fn() { x + y }
        inner
    }
    middle
}
println(outer()()())'

one_diff "enum with-arg constructor" \
'enum Maybe { None, Some(int) }
fn unwrap(m) { match m { Maybe::Some(x) => x  Maybe::None => -1 } }
println(unwrap(Maybe::Some(42)))
println(unwrap(Maybe::None))'

one_diff "bigint literal" \
'println(99999999999999999999)'

one_diff "bigint via overflow at compile-aware optimisation" \
'let huge = 10 ** 30
println(huge.to_str())
println((huge * huge).to_str().len())'

echo ""
echo "transpiler diff: $pass passed, $fail failed"
[ $fail -eq 0 ] && exit 0 || exit 1
