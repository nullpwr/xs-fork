-- skip-emit: wasm (TODO: wasm transpiler doesn't reproduce the map-key string-vs-ident fix)
-- bug009: in the VM, `#{a: 1}` compiled `a` as a global lookup rather
-- than the string key "a". So every bareword-key map literal silently
-- became `#{}`. Most expressions on top of those maps then quietly
-- collapsed to null. Fix: vm/compiler.c special-cases NODE_IDENT keys.
let m = #{ a: 1, b: 2, c: 3 }
assert_eq(m.len(), 3)
assert_eq(m["a"], 1)
assert_eq(m.b, 2)
assert_eq(m.c, 3)

-- spread literal too
let n = #{ ...m, d: 4 }
assert_eq(n.len(), 4)
assert_eq(n.d, 4)

println("bug009: ok")
