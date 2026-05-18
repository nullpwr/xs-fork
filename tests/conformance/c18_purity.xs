-- Static purity inference. Every fn / lambda gets stamped with a
-- bit during sema; the runtime exposes the bit through `__pure?(f)`,
-- and `@memoize` / `@retry` use the same bit to refuse impure
-- decoratees at decoration time.

import math
import time

-- value-only lambdas
assert_eq(__pure?(fn(x) { x + 1 }), true)
assert_eq(__pure?(fn(x, y) { x * y + 1 }), true)
assert_eq(__pure?(fn() { 42 }), true)

-- print / I/O escapes
assert_eq(__pure?(fn() { print("x") }), false)
assert_eq(__pure?(fn() { println("hi") }), false)

-- local let-bound mutation that does not escape: pure
assert_eq(__pure?(fn() {
    let arr = []
    arr.push(1)
    arr.push(2)
    arr
}), true)

-- mutating a parameter is observable to the caller: impure
assert_eq(__pure?(fn(arr) {
    arr.push(1)
    arr
}), false)

-- spawn / perform / await are all impure
assert_eq(__pure?(fn() { spawn fn() {} }), false)

effect Foo { foo() -> int }
assert_eq(__pure?(fn() { perform Foo.foo() }), false)

-- pure stdlib calls keep purity
assert_eq(__pure?(fn(x) { math.sqrt(x) }), true)
assert_eq(__pure?(fn(s) { s.to_upper() }), true)

-- impure stdlib calls flip purity
assert_eq(__pure?(fn() { time.now() }), false)

-- transitive purity through named callees
fn pure_helper(n) { n + 1 }
fn impure_helper() { print("noise") }

fn calls_pure(n) { pure_helper(n) + 1 }
fn calls_impure() { impure_helper() }

assert_eq(__pure?(pure_helper), true)
assert_eq(__pure?(impure_helper), false)
assert_eq(__pure?(calls_pure), true)
assert_eq(__pure?(calls_impure), false)

-- mutual recursion: pure if both pure, impure if either impure
fn mr_a(n) { if n <= 0 { return 0 } mr_b(n - 1) + 1 }
fn mr_b(n) { if n <= 0 { return 0 } mr_a(n - 1) + 2 }
assert_eq(__pure?(mr_a), true)
assert_eq(__pure?(mr_b), true)

fn mri_a(n) { if n <= 0 { print("base"); return 0 } mri_b(n - 1) + 1 }
fn mri_b(n) { if n <= 0 { return 0 } mri_a(n - 1) + 2 }
assert_eq(__pure?(mri_a), false)
assert_eq(__pure?(mri_b), false)

-- non-callables are reported as not-pure
assert_eq(__pure?(42), false)
assert_eq(__pure?("hi"), false)
assert_eq(__pure?(null), false)
assert_eq(__pure?([1, 2]), false)

-- @memoize accepts a pure body and the cache works as before
@memoize fn slow(n) { n * 2 + 1 }
assert_eq(slow(7), 15)
assert_eq(slow(7), 15)
assert_eq(slow(8), 17)

-- @retry accepts a pure body
@retry(3) fn flaky(x) { x + 1 }
assert_eq(flaky(10), 11)

-- The decoration-time gate itself is exercised by the negative
-- corpus; this leg only verifies that the bit `@memoize` reads is
-- the same one `__pure?` exposes. Same input, both refuse: the
-- impurity propagates from the body through the inferred bit, and
-- attempting to memoize would land on PurityError.
fn bad_memo() { print("noise") }
fn bad_retry() { print("noise") }
assert_eq(__pure?(bad_memo), false)
assert_eq(__pure?(bad_retry), false)

println("CONFORMANCE OK")
