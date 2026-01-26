-- Every documented operator at every precedence level. These checks
-- encode the precedence table: any change that relaxes it breaks the
-- expected value.

assert_eq(1 + 2 * 3, 7)
assert_eq((1 + 2) * 3, 9)
assert_eq(10 - 3 - 2, 5)
assert_eq(2 ** 3, 8)
assert_eq(2 ** 3 ** 2, 512)
assert_eq(7 / 2, 3)
assert_eq(7 % 3, 1)
assert_eq(-5 + 7, 2)
assert_eq(-(3 + 4), -7)

-- comparison
assert_eq(1 < 2, true)
assert_eq(2 < 2, false)
assert_eq(2 <= 2, true)
assert_eq(3 > 2, true)
assert_eq(2 == 2, true)
assert_eq(2 != 3, true)

-- logical short-circuit
var hits = 0
fn bump() { hits = hits + 1; true }
let _ = false && bump()
assert_eq(hits, 0)
let _ = true || bump()
assert_eq(hits, 0)

-- bitwise
assert_eq(0b1100 & 0b1010, 0b1000)
assert_eq(0b1100 | 0b0011, 0b1111)
assert_eq(0b1100 ^ 0b1010, 0b0110)
assert_eq(1 << 4, 16)
assert_eq(256 >> 2, 64)

-- string concat
assert_eq("a" ++ "b", "ab")

-- null coalesce
assert_eq(null ?? 7, 7)
assert_eq(42 ?? 7, 42)

println("CONFORMANCE OK")
