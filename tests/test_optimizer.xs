-- optimizer: type specialization, inlining, GVN, SCCP

-- type specialization: int operations stay int
fn sum_ints(n) {
    var total = 0
    for i in 0..n { total = total + i }
    return total
}
assert_eq(sum_ints(100), 4950)
assert_eq(sum_ints(0), 0)
assert_eq(sum_ints(1), 0)
assert_eq(sum_ints(10), 45)

-- inlining: small function bodies get expanded
fn square(x) { x * x }
assert_eq(square(5), 25)
assert_eq(square(0), 0)
assert_eq(square(-3), 9)

fn double(x) { x + x }
assert_eq(double(7), 14)
assert_eq(double(0), 0)

fn identity(x) { x }
assert_eq(identity(42), 42)

-- constant folding through functions
fn add_const() { 2 + 3 }
assert_eq(add_const(), 5)

fn nested_const() { (2 + 3) * (4 - 1) }
assert_eq(nested_const(), 15)

-- GVN: redundant computation elimination
fn redundant(x) {
    let a = x * x
    let b = x * x
    return a + b
}
assert_eq(redundant(3), 18)
assert_eq(redundant(0), 0)
assert_eq(redundant(5), 50)

-- strength reduction: power of 2 multiply -> shift
fn times_eight(x) { x * 8 }
assert_eq(times_eight(3), 24)
assert_eq(times_eight(0), 0)
assert_eq(times_eight(10), 80)

fn div_four(x) { x / 4 }
assert_eq(div_four(16), 4)
assert_eq(div_four(0), 0)

-- algebraic simplification
fn add_zero(x) { x + 0 }
assert_eq(add_zero(42), 42)

fn mul_one(x) { x * 1 }
assert_eq(mul_one(42), 42)

fn mul_zero(x) { x * 0 }
assert_eq(mul_zero(99), 0)

-- dead code elimination
fn with_dead_code(x) {
    return x * 2
    let dead = 999
    return dead
}
assert_eq(with_dead_code(5), 10)

-- constant propagation
fn const_prop() {
    let a = 10
    let b = 20
    return a + b
}
assert_eq(const_prop(), 30)

-- boolean simplification
fn bool_simplify() {
    let a = true && true
    let b = false || true
    let c = true || false
    return a && b && c
}
assert_eq(bool_simplify(), true)

-- nested function calls with inlining
fn cube(x) { square(x) * x }
assert_eq(cube(3), 27)
assert_eq(cube(2), 8)

-- float type specialization
fn float_sum(n) {
    var total = 0.0
    for i in 0..n { total = total + 1.5 }
    return total
}
assert(float_sum(4) == 6.0, "float sum")

-- mixed operations
fn complex_calc(a, b) {
    let x = a * a
    let y = b * b
    let z = x + y
    return z
}
assert_eq(complex_calc(3, 4), 25)
assert_eq(complex_calc(0, 0), 0)

-- string concatenation
fn greet(name) {
    return "hello " ++ name
}
assert_eq(greet("world"), "hello world")

-- method calls (exercises inline cache)
let arr = [1, 2, 3, 4, 5]
assert_eq(arr.len(), 5)
assert_eq(arr.len(), 5)  -- second call should hit IC

let s = "hello"
assert_eq(s.len(), 5)
assert_eq(s.len(), 5)

-- loop invariant motion
fn licm_test(n) {
    let base = 10
    var total = 0
    for i in 0..n {
        let factor = base * 2
        total = total + factor
    }
    return total
}
assert_eq(licm_test(5), 100)

print("deep optimizer tests passed")
