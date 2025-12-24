-- Shape tests for match patterns. Each case asserts that the
-- subject is matched by *exactly* the patterns it should be, and
-- rejected by every other shape. Previously this was not held by
-- the VM: match 99 { [] => ... } would spuriously succeed.

fn kind(x) {
    return match x {
        [] => "empty-slice"
        [_] => "singleton-slice"
        [_, _] => "pair-slice"
        [_, ..rest] => "open-slice"
        #{} => "empty-map"
        _ => "other"
    }
}

-- integers never destructure
assert_eq(kind(0), "other")
assert_eq(kind(99), "other")
assert_eq(kind(-1), "other")

-- strings never destructure
assert_eq(kind(""), "other")
assert_eq(kind("hi"), "other")

-- null, booleans
assert_eq(kind(null), "other")
assert_eq(kind(true), "other")

-- arrays
assert_eq(kind([]), "empty-slice")
assert_eq(kind([1]), "singleton-slice")
assert_eq(kind([1, 2]), "pair-slice")
assert_eq(kind([1, 2, 3]), "open-slice")

-- tuples (array-like)
assert_eq(kind((1,)), "singleton-slice")
assert_eq(kind((1, 2)), "pair-slice")

-- maps
assert_eq(kind(#{}), "empty-map")

-- exact length check on closed slice: a 3-element array must NOT
-- match [_, _]. Before the fix the VM accepted it because ITER_LEN
-- + GTE treated the closed form like open.
fn is_pair(x) {
    return match x {
        [_, _] => true
        _ => false
    }
}
assert_eq(is_pair([1, 2]), true)
assert_eq(is_pair([1, 2, 3]), false)
assert_eq(is_pair([1]), false)
assert_eq(is_pair([]), false)
assert_eq(is_pair(99), false)

-- binding: [x] on 99 used to bind x = null and succeed
fn first_or_none(x) {
    return match x {
        [a] => a
        _ => null
    }
}
assert_eq(first_or_none([42]), 42)
assert_eq(first_or_none(42), null)
assert_eq(first_or_none(null), null)
assert_eq(first_or_none([]), null)

println("test_match_shapes: all passed")
