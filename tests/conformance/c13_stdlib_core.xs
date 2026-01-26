-- Core standard-library operations on arrays, maps, strings, numbers.
-- These are the shared contract every backend agrees on.

-- array ops
assert_eq([1, 2, 3].map(fn(x) { x * 2 }), [2, 4, 6])
assert_eq([1, 2, 3, 4].filter(fn(x) { x % 2 == 0 }), [2, 4])
assert_eq([1, 2, 3].reduce(0, fn(a, b) { a + b }), 6)
assert_eq([3, 1, 2].sort(), [1, 2, 3])
assert_eq([1, 2, 3].reverse(), [3, 2, 1])
assert_eq([1, 2].concat([3, 4]), [1, 2, 3, 4])
assert_eq([1, 2, 3].join(","), "1,2,3")

-- string ops
assert_eq("hello".upper(), "HELLO")
assert_eq("  hi  ".trim(), "hi")
assert_eq("a,b,c".split(","), ["a", "b", "c"])
assert_eq("abc".starts_with("ab"), true)
assert_eq("abc".ends_with("bc"), true)
assert_eq("abc".contains("b"), true)
assert_eq("abc".replace("b", "X"), "aXc")

-- map ops
let m = #{"a": 1, "b": 2}
assert_eq(m.keys().sort(), ["a", "b"])
assert_eq(m.get("a"), 1)
assert_eq(m.has("c"), false)

-- number ops
assert_eq((5).abs(), 5)
assert_eq((-5).abs(), 5)
assert_eq((3.7).floor(), 3)
assert_eq((3.2).ceil(), 4)

println("CONFORMANCE OK")
