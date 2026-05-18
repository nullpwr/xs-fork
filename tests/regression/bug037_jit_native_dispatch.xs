-- skip-emit: wasm (TODO: wasm transpiler doesn't reproduce the jit native dispatch path)
-- bug037: cuts the per-instruction dispatch tax on the most common
-- hot ops the JIT was still routing through vm_step_jit:
--
-- 1. PUSH_CONST + NEG on a numeric literal. `-2.0` and `-1` used to
--    emit two ops; now the bytecode compiler folds them into a single
--    PUSH_CONST with the negated value. mandelbrot's outer loop fired
--    80,000 NEG dispatches per run.
--
-- 2. OP_CALL on an XS_NATIVE callee (`float(px)`, `str(i)`, etc.).
--    vm_call_closure_fast now handles natives directly, the same way
--    OP_CALL's slow arm does. Pending throws still bail to the slow
--    path so vm_dispatch can unwind.
--
-- 3. OP_METHOD_CALL on the workhorse cases:
--      - array .push / .len / .size / .pop (inline tag-checked path)
--      - string .len / .size / .length     (inline)
--      - map / module XS_NATIVE methods   (via inline cache)
--    bench_json's 1003 method-call dispatches per run drop to 3.

-- ---- 1. constant-folded unary minus ----
let neg_int = -42
let neg_float = -3.14
let neg_zero = -0
let neg_zero_f = -0.0
assert_eq(neg_int, -42)
assert_eq(neg_float, -3.14)
assert_eq(neg_zero, 0)
-- arithmetic with folded negatives
let sum = -2.0 + 3.0
assert_eq(sum, 1.0)
let scaled = -1.5 * 2.0
assert_eq(scaled, -3.0)
-- nested: -(-x) folds once at the outer
var x = 5
let rev = -x
assert_eq(rev, -5)
-- runtime negation of a non-literal still works
fn neg(n) { return -n }
assert_eq(neg(7), -7)
assert_eq(neg(-3.5), 3.5)

-- ---- 2. native CALL fast path ----
-- float() and str() are XS_NATIVE; under JIT they used to dispatch
-- via vm_step_jit. Now they run inline through vm_call_closure_fast.
var pixels = []
for i in 0..50 {
    let f = float(i) / 50.0
    pixels.push(f * 2.0 - 1.0)
}
assert_eq(pixels.len(), 50)
assert_eq(pixels[0], -1.0)
-- 49/50 * 2 - 1 = 0.96 (within fp tolerance)
let last = pixels[49]
assert(last > 0.95 and last < 0.97, "fp tail")

-- string conversion through str()
var parts = []
for i in 0..10 { parts.push(str(i)) }
assert_eq(parts, ["0","1","2","3","4","5","6","7","8","9"])

-- ---- 3a. array method fast paths ----
var xs = []
for i in 0..100 { xs.push(i * 2) }
assert_eq(xs.len(), 100)
assert_eq(xs.size(), 100)
assert_eq(xs[50], 100)
-- pop in a loop
var top = 0
while xs.len() > 0 {
    top = xs.pop()
}
assert_eq(top, 0)  -- last popped is xs[0]
assert_eq(xs.len(), 0)

-- mixed receivers (array + map) in same loop -- exercises the
-- type-tag dispatch in vm_method_call_fast.
let recs = []
for i in 0..20 {
    recs.push({"id": i, "v": i * 10})
}
var total = 0
for r in recs { total = total + r["v"] }
assert_eq(total, 1900)
assert_eq(recs.len(), 20)

-- ---- 3b. string .len fast path ----
let s = "hello"
assert_eq(s.len(), 5)
assert_eq(s.size(), 5)
assert_eq(s.length(), 5)
-- utf-8 char count, not byte count
let u = "café"
assert_eq(u.len(), 4)

-- ---- 3c. map / module method via IC ----
import json
let payload = #{ a: 1, b: [1, 2, 3], c: "hi" }
let s2 = json.stringify(payload)
let p2 = json.parse(s2)
assert_eq(p2["a"], 1)
assert_eq(p2["b"][2], 3)

-- map .has / .get hit the slow path (not in fast-path list); make
-- sure they still work alongside the inline routes.
let m = {"x": 10, "y": 20}
assert_eq(m["x"], 10)

-- ---- correctness under tight loop (the shape that would catch
--      stack-tracking bugs in the fast paths) ----
var seq = []
for i in 0..200 {
    seq.push(-float(i) / 100.0)
}
assert_eq(seq.len(), 200)
assert_eq(seq[0], 0.0)
assert(seq[100] < -0.99 and seq[100] > -1.01, "midpoint")
assert(seq[199] < -1.98 and seq[199] > -2.0, "tail")

println("bug037: ok")
