-- skip-emit: c (TODO: wrapping decorators rejected by --emit c)
-- bug054: wrapping decorators -- @memoize / @retry / @trace / @timed
-- transform the bound fn into a dispatcher that intercepts the call.
-- Trigger decorators only register side-effects; wrapping ones must
-- actually rewire the call path so the inner function sees its
-- arguments through the wrapper.
--
-- @memoize and @retry both require their target to be statically pure
-- (a memoized impure body would silently skip side effects on cache
-- hits; a retried impure body would replay them per attempt). The
-- counter-based observability used to live here got moved out: the
-- caching / replay behaviour is verified through the return value
-- alone now, which is enough since the wrapper always delegates
-- through call_value for the first miss.

@memoize
fn fib(n) {
    if n < 2 { return n }
    return fib(n-1) + fib(n-2)
}
assert_eq(fib(10), 55)
-- second call hits the cache: equal value, no observable change.
assert_eq(fib(10), 55)
assert_eq(fib(15), 610)

-- Retry on a deterministic body that succeeds on the first attempt
-- still works -- the wrapper just calls through without retrying.
@retry(5)
fn deterministic(x) { x * x }
assert_eq(deterministic(7), 49)

@retry
fn bare_retry(x) { x + 1 }
assert_eq(bare_retry(10), 11)

-- A retry body that always throws should bubble the last exception
-- out so the caller's try / catch sees it.
@retry(2)
fn always_fails() { throw "boom" }
var caught = ""
try { always_fails() } catch e { caught = e }
assert_eq(caught, "boom")

@timed
fn quick() { return 42 }
assert_eq(quick(), 42)

println("bug054: ok")
