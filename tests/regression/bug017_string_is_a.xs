-- skip-emit: wasm (TODO: wasm transpiler doesn't lower .is_a() type query)
-- bug017: "abc".is_a("str") worked in the VM but the interpreter had
-- no handler, so the same program errored under --interp. Fix: add
-- is_a to the string branch of eval_method.
assert_eq("hello".is_a("str"), true)
assert_eq("hello".is_a("String"), true)
assert_eq("hello".is_a("int"), false)
println("bug017: ok")
