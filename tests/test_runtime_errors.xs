-- Expected-error tests. These exercise the compiler's and runtime's
-- error paths: the previous suite had nothing that failed on purpose,
-- so regressions like "yield outside generator silently works" slipped
-- through. Each assertion here pins a specific failure mode.

-- channel.recv on empty throws instead of returning null silently
let ch = channel()
var got_empty_err = false
try {
    ch.recv()
} catch e {
    got_empty_err = (e.kind == "ChannelEmpty")
}
assert(got_empty_err, "recv on empty channel must throw ChannelEmpty")

-- try_recv is the non-blocking form and returns null
let ch2 = channel()
assert_eq(ch2.try_recv(), null)

-- recv after send works
ch.send(1)
ch.send(2)
assert_eq(ch.recv(), 1)
assert_eq(ch.recv(), 2)

-- divide by zero returns null (and should print a runtime error to
-- stderr, but we cannot assert on stderr here)
let d = 10 / 0
assert_eq(d, null)
let m = 10 % 0
assert_eq(m, null)

println("test_runtime_errors: all passed")
