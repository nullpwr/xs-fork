-- Generators: fn* functions produce iterables via yield. They must
-- suspend at each yield and resume in place on the next .next() call.

fn* range3() {
    yield 1
    yield 2
    yield 3
}

let g = range3()
assert_eq(g.next(), 1)
assert_eq(g.next(), 2)
assert_eq(g.next(), 3)

-- Iterate via for-in over a generator
fn* squares(n) {
    var i = 1
    while i <= n {
        yield i * i
        i = i + 1
    }
}

var total = 0
for v in squares(4) { total = total + v }
assert_eq(total, 30)

-- Generators are independent instances
let a = range3()
let b = range3()
assert_eq(a.next(), 1)
assert_eq(b.next(), 1)
assert_eq(a.next(), 2)

println("CONFORMANCE OK")
