-- skip-emit: c, js, wasm (TODO: tagged blocks (__block yield) not lowered by --emit transpilers)
-- bug040: tagged blocks worked on --interp but were no-ops on --vm
-- and --jit. The interp's NODE_YIELD case checked at runtime whether
-- __block was in scope (env_get) and called it; the VM compiler
-- always emitted OP_YIELD, treating tag-yield as generator-yield.
-- The block body never executed and a {done: false, value: null}
-- record was returned instead of the block's return value.
--
-- Fix: at compile time the VM resolves __block as a local (or upvalue
-- for nested fns inside a tag) and emits LOAD + CALL instead of
-- OP_YIELD. Mirrors the runtime check the interpreter already did.

tag twice() {
    yield;
    yield;
}

var hits = 0
twice() {
    hits = hits + 1
}
assert_eq(hits, 2)

tag retry(n) {
    var attempts = 0
    loop {
        try {
            let result = yield;
            return result
        } catch e {
            attempts = attempts + 1
            if attempts >= n {
                throw "failed after {n} attempts: {e}"
            }
        }
    }
}

var counter = 0
retry(3) {
    counter = counter + 1
    if counter < 3 {
        throw "not yet"
    }
}
assert_eq(counter, 3)

tag with_default(fallback) {
    let val = yield;
    if val == null {
        return fallback
    }
    return val
}

let v1 = with_default(42) { null }
let v2 = with_default(42) { 99 }
assert_eq(v1, 42)
assert_eq(v2, 99)

println("bug040: ok")
