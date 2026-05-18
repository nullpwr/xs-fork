-- skip-emit: wasm (TODO: Duration is a new value tag with its own arithmetic / comparison / repr fast paths; the wasm runtime helpers ship int / float / string / array / map / range only, so `5s + 500ms` etc all collapse to null)
-- bug044: duration is a real first-class type rather than a sugared
-- number of milliseconds, and the time-literal suffixes (ns, us, ms, s,
-- m, h, d) work without the old `use literals duration` opt-in. The
-- suffix only applies when adjacent to the number, so a bare `s`/`m`
-- on the next line stays an identifier.

assert_eq(typeof(5s), "duration")
assert_eq(typeof(2.5s), "duration")

-- subsecond accessors round-trip through ns
assert_eq((5s).ns, 5000000000)
assert_eq((1ms).ns, 1000000)
assert_eq((1us).ns, 1000)
assert_eq((1ns).ns, 1)

-- arithmetic and comparisons preserve the type
assert_eq(2s + 500ms, 2500ms)
assert_eq(1m - 30s, 30s)
assert_eq((100ms) * 3, 300ms)
assert_eq(2 * (250ms), 500ms)
assert_eq((2s) / 4, 500ms)
assert_eq((1s) / (250ms), 4.0)
assert(500ms < 1s)
assert(1d == 24h)

-- repr picks the smallest readable form
assert_eq(str(5s), "5s")
assert_eq(str(100ms), "100ms")
assert_eq(str(1ns), "1ns")
assert_eq(str(1500ns), "1.5us")
assert_eq(str(1m30s), "1m30s")
assert_eq(str(2500ms), "2.5s")
assert_eq(str(0s), "0s")

-- adjacency: a unit-suffix-named identifier on a separate line stays
-- an identifier, otherwise `var f = 4.0\ns = ...` would silently turn
-- the float into 4 seconds.
fn neg(x) { return -x }
var f = 4.0
var sum = 0.0
for i in 1..10 {
    f = f * 1.1
    sum = sum + neg(f)
}
assert(sum < 0.0)

println("bug044: ok")
