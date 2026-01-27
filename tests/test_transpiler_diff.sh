#!/bin/bash
# Transpiler diff tests: for each small program, run under the XS interpreter,
# the XS bytecode VM, the transpiled C binary, and the transpiled JS (via
# node, if present) and confirm all backends produce the same stdout.
#
# Catches the class of bug where a backend emits something that "compiles"
# but computes the wrong result at runtime.

cd "$(dirname "$0")/.."
pass=0
fail=0
have_node=0
if command -v node > /dev/null 2>&1; then have_node=1; fi

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

    # C backend: transpile, compile, run
    if ./xs --emit c "$xsfile" > "$tmp/prog.c" 2>/dev/null \
        && gcc -o "$tmp/prog_c" "$tmp/prog.c" -lm 2>/dev/null; then
        c_out=$("$tmp/prog_c" 2>&1)
        if [ "$interp_out" != "$c_out" ]; then
            fail=$((fail + 1))
            echo "  FAIL  $desc (interp vs c)"
            echo "        interp: $interp_out"
            echo "        c:      $c_out"
            rm -rf "$tmp"; return
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

echo ""
echo "transpiler diff: $pass passed, $fail failed"
[ $fail -eq 0 ] && exit 0 || exit 1
