-- String operations must treat strings as UTF-8 byte sequences with
-- codepoint-aware .len / .chars / slicing.

let s = "café"
assert_eq(s.len(), 4)  -- grapheme/codepoint count, not bytes

let emoji = "hi 😀"
assert_eq(emoji.len(), 4)

-- chars iterate by codepoint
var count = 0
for _ in s.chars() { count = count + 1 }
assert_eq(count, 4)

-- upper/lower handle ASCII; case-insensitive compare via eq_ignore_case
assert_eq("hello".upper(), "HELLO")
assert_eq("HELLO".lower(), "hello")

-- concat preserves UTF-8
let joined = "αβ" ++ "γδ"
assert_eq(joined.len(), 4)

-- contains works with multi-byte needle
assert_eq("naïve approach".contains("naïve"), true)
assert_eq("naïve approach".contains("xyz"), false)

println("CONFORMANCE OK")
