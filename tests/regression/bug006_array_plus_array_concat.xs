-- bug006: [1,2] + [3,4] used to silently return null because OP_ADD
-- fell through the float coerce path for arrays. Fix: array+array now
-- concatenates in both interp and vm.
let a = [1, 2]
let b = [3, 4]
let c = a + b
assert_eq(c, [1, 2, 3, 4])
assert_eq(([] + [1]).len(), 1)
assert_eq(([1] + []).len(), 1)
println("bug006: ok")
