-- skip-emit: wasm (TODO: wasm transpiler has no XS_MAP branch in value_equal)
-- bug008: value_equal had no XS_MAP branch, so #{a:1} == #{a:1} always
-- returned false. Fix: structural map equality (same keys, equal values).
assert_eq(#{a:1} == #{a:1}, true)
assert_eq(#{a:1} == #{a:2}, false)
assert_eq(#{a:1, b:2} == #{b:2, a:1}, true)
assert_eq(#{} == #{}, true)
assert_eq(#{a:1} == #{a:1, b:2}, false)
println("bug008: ok")
