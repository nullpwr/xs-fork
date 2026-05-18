-- skip-emit: wasm (TODO: wasm transpiler doesn't lower range method dispatch)
-- bug016: range objects had no method dispatch in the VM, so `.len()`,
-- `.to_array()`, etc. silently returned null. Fix: added a XS_RANGE
-- branch mirroring the interpreter.
let r = 1..5
assert_eq(r.len(), 4)
assert_eq(r.start(), 1)
assert_eq(r.end(), 5)
assert_eq(r.is_empty(), false)
assert_eq(r.contains(3), true)
assert_eq(r.contains(5), false)
assert_eq(r.to_array(), [1, 2, 3, 4])

let ri = 1..=5
assert_eq(ri.len(), 5)
assert_eq(ri.to_array(), [1, 2, 3, 4, 5])
println("bug016: ok")
