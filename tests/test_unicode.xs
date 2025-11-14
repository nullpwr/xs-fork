-- unicode and string method tests

-- byte_len returns byte count
assert_eq("hello".byte_len(), 5)
assert_eq("".byte_len(), 0)

-- char_len returns unicode codepoint count
assert_eq("hello".char_len(), 5)
assert_eq("".char_len(), 0)

-- ascii strings: byte_len == char_len
let ascii = "abcdef"
assert_eq(ascii.byte_len(), ascii.char_len())

-- basic string methods still work
assert_eq("Hello".upper(), "HELLO")
assert_eq("Hello".lower(), "hello")
assert_eq("  hi  ".trim(), "hi")
assert_eq("hello world".starts_with("hello"), true)
assert_eq("hello world".ends_with("world"), true)
assert_eq("hello world".contains("lo wo"), true)

-- split and join
let parts = "a,b,c".split(",")
assert_eq(parts.len(), 3)
assert_eq(parts[0], "a")
assert_eq(parts[2], "c")
let joined = ",".join(parts)
assert_eq(joined, "a,b,c")

-- replace
assert_eq("foo bar foo".replace("foo", "baz"), "baz bar baz")

-- repeat
assert_eq("ab".repeat(3), "ababab")

-- char_at
assert_eq("hello".char_at(0), "h")
assert_eq("hello".char_at(4), "o")

-- find / index_of
assert_eq("hello".find("ll"), 2)
assert_eq("hello".find("xyz"), -1)

-- slice
assert_eq("hello".slice(1, 3), "el")

-- pad_left, pad_right
assert_eq("42".pad_left(5, "0"), "00042")
assert_eq("hi".pad_right(5, "."), "hi...")

-- reverse
assert_eq("abc".reverse(), "cba")

-- to_int, to_float
assert_eq("42".to_int(), 42)
assert_eq("3.14".to_float(), 3.14)

print("test_unicode: all passed")
