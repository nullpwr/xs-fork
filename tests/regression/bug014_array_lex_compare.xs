-- skip-emit: wasm (TODO: wasm transpiler value_cmp lacks array/tuple lex compare)
-- bug014: value_cmp had no array/tuple branch, so [1] < [2] silently
-- compared by tag and returned false. Fix: lexicographic compare with
-- shorter side ranking less.
assert_eq([1] < [2], true)
assert_eq([1, 2] < [1, 3], true)
assert_eq([1, 2] < [1, 2, 0], true)
assert_eq([2] > [1, 9], true)
assert_eq([1, 2] == [1, 2], true)
assert_eq((1, 2) < (1, 3), true)
println("bug014: ok")
