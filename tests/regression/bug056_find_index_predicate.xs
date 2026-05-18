-- skip-emit: wasm (TODO: wasm transpiler doesn't lower the find_index callback path)
-- bug056: arr.find_index(pred) was equality-checking against the
-- predicate value itself instead of running it as a callback. The
-- method now takes either a value (equality) or a callable (run as
-- predicate). index_of accepts either form too for symmetry.

let arr = [3, 1, 4, 1, 5, 9, 2, 6]

-- value form: first index where item == arg
assert_eq(arr.find_index(5), 4)
assert_eq(arr.find_index(99), -1)

-- predicate form: first index where pred(item) is truthy
assert_eq(arr.find_index(|x| x > 5), 5)
assert_eq(arr.find_index(|x| x > 100), -1)
assert_eq(arr.find_index(|x| x == 4), 2)

-- index_of also accepts both forms
assert_eq(arr.index_of(9), 5)
assert_eq(arr.index_of(|x| x > 5), 5)

-- contains gained the same dual: value or predicate
assert_eq(arr.contains(5), true)
assert_eq(arr.contains(99), false)
assert_eq(arr.contains(|x| x > 5), true)
assert_eq(arr.contains(|x| x > 100), false)

println("bug056: ok")
