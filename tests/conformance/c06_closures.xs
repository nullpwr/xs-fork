-- Closures must capture by reference (shared state with the defining
-- scope) and persist their environment after the enclosing frame
-- returns.

fn counter() {
    var n = 0
    fn inc() { n = n + 1; n }
    inc
}

let c = counter()
assert_eq(c(), 1)
assert_eq(c(), 2)
assert_eq(c(), 3)

-- Each counter has its own state
let d = counter()
assert_eq(d(), 1)
assert_eq(c(), 4)

-- Closure over loop variable: captures current binding per iteration
let fns = []
var i = 0
while i < 3 {
    let j = i
    fns.push(fn() { j })
    i = i + 1
}
assert_eq(fns[0](), 0)
assert_eq(fns[1](), 1)
assert_eq(fns[2](), 2)

-- Mutual recursion via shared env
fn make_pair() {
    fn is_even(n) { if n == 0 { true } else { is_odd(n - 1) } }
    fn is_odd(n)  { if n == 0 { false } else { is_even(n - 1) } }
    (is_even, is_odd)
}
let (ev, od) = make_pair()
assert_eq(ev(10), true)
assert_eq(od(7), true)

-- Transitive capture through more than one nested function: the middle
-- frame must forward the outer binding to the inner closure even
-- though the middle body never reads it itself. This shape used to
-- trip the C and WASM AOT paths, where capture analysis only walked
-- one level of the lexical chain.
fn outer_decl() {
    var x = 100
    fn middle() {
        var y = 10
        fn inner() { x + y }
        inner
    }
    middle
}
assert_eq(outer_decl()()(), 110)

fn outer_lambda() {
    let x = 100
    let middle = fn() {
        let y = 10
        let inner = fn() { x + y }
        inner
    }
    middle
}
assert_eq(outer_lambda()()(), 110)

println("CONFORMANCE OK")
