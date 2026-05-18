-- bug007: indexing a non-collection (int, float, bool, null) used to
-- silently return null. Fix: OP_INDEX_GET and NODE_INDEX both call
-- xs_runtime_error for these tags. The actual error case lives in
-- tests/negative/type_mismatch_add.xs; this file asserts that valid
-- indexing still works.
let arr = [10, 20, 30]
assert_eq(arr[0], 10)
assert_eq(arr[2], 30)
assert_eq(arr[-1], 30)

let s = "abc"
assert_eq(s[0], "a")
assert_eq(s[1], "b")

let m = #{ "k": 99 }
assert_eq(m["k"], 99)

println("bug007: ok")
