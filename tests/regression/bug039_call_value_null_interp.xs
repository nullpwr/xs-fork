-- bug039: when a script runs from a file, main routes through
-- vm_run, and vm_dispatch calls native functions with NULL for the
-- Interp* (vm.c OP_CALL / OP_METHOD_CALL pass NULL because the
-- interp isn't authoritative in VM mode). Some natives turn around
-- and re-enter the runtime via call_value to invoke a user closure
-- -- http.serve dispatching to a route handler is the canonical
-- example, but xs.filter(fn(x) {...}) / xs.sort(comparator) /
-- signal.subscribe(handler) all hit the same path.
--
-- The existing call_value started by recording a trace event keyed
-- off i->current_span.line. With i == NULL that dereferences into
-- 0x700 and segfaults the server before the first request is even
-- replied to. `xs -e 'http.serve(...)'` worked because the -e path
-- builds an Interp* and runs through interp_run, never tripping
-- the guard.
--
-- Fix: top of call_value, when i == NULL, skip the trace + frame
-- bookkeeping and dispatch by callee kind. Native gets called with
-- NULL like the OP_CALL slow path already does; XS_CLOSURE routes
-- through vm_invoke_public on the live thread VM; anything else
-- returns null cleanly.

-- 1. native -> closure via array.filter
let xs = [3, 1, 4, 1, 5, 9, 2, 6]
let evens = xs.filter(fn(x) { return x % 2 == 0 })
assert_eq(evens, [4, 2, 6])

-- 2. native -> closure via array.map
let squared = xs.map(fn(x) { return x * x })
assert_eq(squared.len(), 8)
assert_eq(squared[0], 9)
assert_eq(squared[7], 36)

-- 3. native -> closure with two-arg comparator
let sorted = xs.sort(fn(a, b) { return b - a })
assert_eq(sorted, [9, 6, 5, 4, 3, 2, 1, 1])

-- 4. nested closure callback (filter of mapped values).
--    Note: the sort above mutates `xs` in place, so this runs against
--    the sorted array [9, 6, 5, 4, 3, 2, 1, 1].
let big_squares = xs.map(fn(x) { return x * x }).filter(fn(v) { return v > 10 })
assert_eq(big_squares, [81, 36, 25, 16])

-- 5. closure that itself calls into a native -- exercise the
--    re-entrancy path beyond a single call_value bounce
let lengths = ["one", "two", "three", "four"].map(fn(s) { return s.len() })
assert_eq(lengths, [3, 3, 5, 4])

println("bug039: ok")
