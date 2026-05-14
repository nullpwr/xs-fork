-- duration is a real first-class type. ns is the canonical unit.

assert_eq(typeof(5s), "duration")
assert_eq(typeof(100ms), "duration")
assert_eq(typeof(1ns), "duration")
assert_eq(typeof(2us), "duration")
assert_eq(typeof(2.5s), "duration")

-- equality and ordering
assert_eq(1s, 1s)
assert(500ms < 1s)
assert(2h > 90m)
assert(1d == 24h)

-- nanosecond accessors round-trip
assert_eq((5s).ns, 5000000000)
assert_eq((1ms).ns, 1000000)
assert_eq((1us).ns, 1000)
assert_eq((1ns).ns, 1)

-- coarser accessors are floats
assert_eq((1500ms).s, 1.5)
assert_eq((90s).m, 1.5)

-- arithmetic
assert_eq(2s + 500ms, 2500ms)
assert_eq(1m - 30s, 30s)
assert_eq((100ms) * 3, 300ms)
assert_eq(2 * (250ms), 500ms)
assert_eq((2s) / 4, 500ms)
assert_eq((1s) / (250ms), 4.0)

-- compound and float forms
assert_eq(1m30s, 90s)
assert_eq(2.5s, 2500ms)
assert_eq(0.5s, 500ms)

-- repr matches the smallest readable form
assert_eq(str(5s), "5s")
assert_eq(str(100ms), "100ms")
assert_eq(str(1ns), "1ns")
assert_eq(str(1500ns), "1.5us")
assert_eq(str(1m30s), "1m30s")
assert_eq(str(2500ms), "2.5s")
assert_eq(str(0s), "0s")

println("all literal tests passed")
