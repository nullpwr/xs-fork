-- bug054: wrapping decorators -- @memoize / @retry / @trace / @timed
-- transform the bound fn into a dispatcher that intercepts the call.
-- Trigger decorators only register side-effects; wrapping ones must
-- actually rewire the call path so the inner function sees its
-- arguments through the wrapper.

var fib_calls = 0
@memoize
fn fib(n) {
    fib_calls = fib_calls + 1
    if n < 2 { return n }
    return fib(n-1) + fib(n-2)
}
assert_eq(fib(10), 55)
assert_eq(fib_calls, 11)
-- second call hits the cache: fib_calls must not advance.
assert_eq(fib(10), 55)
assert_eq(fib_calls, 11)

var attempts = 0
@retry(5)
fn flaky() {
    attempts = attempts + 1
    if attempts < 3 { throw "fail" }
    return "ok"
}
assert_eq(flaky(), "ok")
assert_eq(attempts, 3)

@retry(2)
fn always_fails() { throw "boom" }
var caught = ""
try { always_fails() } catch e { caught = e }
assert_eq(caught, "boom")

@timed
fn quick() { return 42 }
assert_eq(quick(), 42)

println("bug054: ok")
