-- bug035: `s == s` and `s != s` (and the fused `if s == "lit" {...}`
-- shape that becomes IR_CMP_BR after the lowerer's peephole) used
-- to fall through every fast path in IR_EQ / IR_NE / IR_CMP_BR and
-- land in vm_step_jit's slow path. bench_strings dispatched 500 EQ
-- ops via that route.
--
-- Two new helpers, two new fast paths:
--   vm_str_eq_fast(a, b, invert)         -> TRUE/FALSE singleton
--   vm_str_eq_branch(a, b, take_when_eq) -> 0/1 branch decision
-- IR_EQ / IR_NE call the singleton helper for the unfused case;
-- IR_CMP_BR with kind == EQ/NE calls the branch helper directly
-- and skips materialising the singleton entirely.
--
-- Mixed-type comparisons (str vs int, etc.) still flow to the
-- slow path so the existing dunder dispatch keeps working.

-- Plain `==` / `!=` (unfused) -- exercised when the result is
-- assigned or passed somewhere instead of feeding into `if`.
let words = ["hello", "world", "foo", "hello", "bar"]
var matches = []
for w in words {
    matches.push(w == "hello")
}
assert_eq(matches, [true, false, false, true, false])

var anti = []
for w in words {
    anti.push(w != "hello")
}
assert_eq(anti, [false, true, true, false, true])

-- Fused `if s == "lit"` and `if s != "lit"` (-> IR_CMP_BR).
var hits = 0
var misses = 0
for w in words {
    if w == "hello" { hits = hits + 1 }
    if w != "hello" { misses = misses + 1 }
}
assert_eq(hits, 2)
assert_eq(misses, 3)

-- Empty strings, repeated comparisons.
assert_eq("" == "", true)
assert_eq("" != "", false)
assert_eq("a" == "ab", false)
assert_eq("ab" == "a", false)

-- Mixed-type equality still works through the slow path.
assert_eq("1" == 1, false)
assert_eq("1" != 1, true)

println("bug035: ok")
