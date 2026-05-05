-- test regex engine via the re module

import re

-- basic match
assert_eq(re.test("hello", "hello world"), true)
assert_eq(re.test("xyz", "hello world"), false)

-- dot matches any
assert_eq(re.test("h.llo", "hello"), true)
assert_eq(re.test("h.llo", "hxllo"), true)

-- anchors
assert_eq(re.test("^hello", "hello world"), true)
assert_eq(re.test("^world", "hello world"), false)
assert_eq(re.test("world$", "hello world"), true)
assert_eq(re.test("hello$", "hello world"), false)

-- quantifiers
assert_eq(re.test("ab*c", "ac"), true)
assert_eq(re.test("ab*c", "abc"), true)
assert_eq(re.test("ab*c", "abbc"), true)
assert_eq(re.test("ab+c", "ac"), false)
assert_eq(re.test("ab+c", "abc"), true)
assert_eq(re.test("ab?c", "ac"), true)
assert_eq(re.test("ab?c", "abc"), true)

-- character classes
assert_eq(re.test("[abc]", "b"), true)
assert_eq(re.test("[abc]", "d"), false)
assert_eq(re.test("[a-z]", "m"), true)
assert_eq(re.test("[0-9]+", "123"), true)

-- alternation
assert_eq(re.test("cat|dog", "cat"), true)
assert_eq(re.test("cat|dog", "dog"), true)
assert_eq(re.test("cat|dog", "fish"), false)

-- find_all
let nums = re.find_all("[0-9]+", "abc 123 def 456 ghi 789")
assert_eq(nums.len(), 3)
assert_eq(nums[0], "123")
assert_eq(nums[1], "456")
assert_eq(nums[2], "789")

-- find_all words
let words = re.find_all("[a-z]+", "hello 42 world 99 foo")
assert_eq(words.len(), 3)
assert_eq(words[0], "hello")
assert_eq(words[1], "world")
assert_eq(words[2], "foo")

-- replace
let r1 = re.replace("world", "hello world", "XS")
assert_eq(r1, "hello XS")

-- replace_all
let r2 = re.replace_all("[0-9]+", "a1b2c3", "N")
assert_eq(r2, "aNbNcN")

-- replace with pattern
let r3 = re.replace_all(" +", "too   many    spaces", " ")
assert_eq(r3, "too many spaces")

-- split
let parts = re.split("[,;]+", "a,b;;c,d")
assert_eq(parts.len(), 4)
assert_eq(parts[0], "a")
assert_eq(parts[1], "b")
assert_eq(parts[2], "c")
assert_eq(parts[3], "d")

-- split on whitespace
let ws_parts = re.split("[ ]+", "hello world foo")
assert_eq(ws_parts.len(), 3)
assert_eq(ws_parts[0], "hello")
assert_eq(ws_parts[1], "world")
assert_eq(ws_parts[2], "foo")

-- groups (capture groups)
let m = re.groups("([0-9]+)-([0-9]+)", "tel: 555-1234")
assert_eq(m.len(), 2)
assert_eq(m[0], "555")
assert_eq(m[1], "1234")

-- groups with more captures
let m2 = re.groups("([a-z]+)=([a-z0-9]+)", "key=val42")
assert_eq(m2.len(), 2)
assert_eq(m2[0], "key")
assert_eq(m2[1], "val42")

-- is_match alias
assert_eq(re.is_match("[0-9]+", "abc123"), true)
assert_eq(re.is_match("[0-9]+", "abc"), false)

-- match (returns first match text)
let first_num = re.match("[0-9]+", "hello 42 world 99")
assert_eq(first_num, "42")

-- match returns null on no match
let no_match = re.match("[0-9]+", "no numbers here")
assert_eq(no_match, null)

-- test empty pattern
assert_eq(re.test("", "anything"), true)

-- test escaped special chars
assert_eq(re.test("a\\.b", "a.b"), true)
assert_eq(re.test("a\\.b", "axb"), false)

-- complex patterns
assert_eq(re.test("^[a-zA-Z][a-zA-Z0-9_]*$", "valid_name123"), true)
assert_eq(re.test("^[a-zA-Z][a-zA-Z0-9_]*$", "123invalid"), false)

-- email-like pattern
assert_eq(re.test("[a-zA-Z0-9.]+@[a-zA-Z0-9.]+", "user@example.com"), true)
assert_eq(re.test("[a-zA-Z0-9.]+@[a-zA-Z0-9.]+", "not-an-email"), false)

-- IP address pattern
assert_eq(re.test("[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+", "192.168.1.1"), true)

print("test_regex_engine: all passed")
