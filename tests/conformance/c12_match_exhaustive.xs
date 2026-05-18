-- Match expressions must evaluate every arm's pattern in order and
-- only run the body of the first that matches. Bindings from one arm
-- must not leak into another.

fn classify(v) {
    match v {
        0        => "zero"
        n if n < 0 => "negative"
        n        => "positive"
    }
}

assert_eq(classify(0), "zero")
assert_eq(classify(-3), "negative")
assert_eq(classify(5), "positive")

-- OR patterns
fn is_vowel(c) {
    match c {
        "a" | "e" | "i" | "o" | "u" => true
        _ => false
    }
}
assert_eq(is_vowel("a"), true)
assert_eq(is_vowel("z"), false)

-- nested destructuring
fn sum_pair(p) {
    match p {
        (a, b) => a + b
    }
}
assert_eq(sum_pair((4, 5)), 9)

-- struct patterns with rest
struct Point { x: int, y: int, label: str }
let p = Point { x: 1, y: 2, label: "home" }
let summed = match p {
    Point { x, y, .. } => x + y
}
assert_eq(summed, 3)

-- slice with rest binding
let nums = [1, 2, 3, 4, 5]
let tail_len = match nums {
    [first, ..rest] => rest.len()
    _ => -1
}
assert_eq(tail_len, 4)

-- Enum constructors and matches: the with-arg form (`Some(42)`) and
-- the zero-arg form (`None`) need to lower the same way at every
-- backend so the match arms can dispatch on them. The WASM path used
-- to take different lowerings for the two forms and the with-arg
-- branch silently produced no output.
enum Maybe { None, Some(int) }
fn unwrap(m) {
    match m {
        Maybe::Some(x) => x
        Maybe::None    => -1
    }
}
assert_eq(unwrap(Maybe::Some(42)), 42)
assert_eq(unwrap(Maybe::None), -1)

println("CONFORMANCE OK")
