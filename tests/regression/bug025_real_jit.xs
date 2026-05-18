-- skip-emit: wasm (TODO: wasm transpiler doesn't reproduce the jit/native emit path under wasm)
-- bug025: --jit was previously a fork-and-exec of the system C
-- compiler (AOT in disguise). It now emits real x86-64 machine code
-- into mmap(PROT_EXEC) memory and executes it in-process by driving
-- vm_step through a tight call/test/jz loop. Output must be byte-
-- identical with --vm and --interp (verified by tests/run.sh
-- triple-diff).
fn fib(n) {
    return if n < 2 { n } else { fib(n - 1) + fib(n - 2) }
}
assert_eq(fib(15), 610)

let arr = [1, 2, 3, 4, 5].map(|x| x * x)
assert_eq(arr, [1, 4, 9, 16, 25])

var n = 0
for i in 1..=100 { n = n + i }
assert_eq(n, 5050)

try {
    let _ = 1 / 0
} catch e {
    assert_eq(e.kind, "division by zero")
}

println("bug025: ok")
