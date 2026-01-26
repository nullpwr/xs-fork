-- try / catch must intercept throws without unwinding past the catch,
-- and must expose the thrown value to the handler.

assert_eq(try { 1 } catch e { 2 }, 1)
assert_eq(try { throw "boom" } catch e { e }, "boom")

-- Rethrow from inner catch
let r = try {
    try {
        throw "x"
    } catch e {
        throw e ++ "!"
    }
} catch e { e }
assert_eq(r, "x!")

-- Finally / cleanup via nested try (no 'finally' keyword; defer is the equivalent)
var closed = false
fn run() {
    defer closed = true
    throw "err"
}
let caught = try { run() } catch e { e }
assert_eq(caught, "err")
assert_eq(closed, true)

-- Runtime division by zero is catchable
let safe = try { 10 / 0 } catch _ { -1 }
assert_eq(safe, -1)

println("CONFORMANCE OK")
