-- skip-emit: js, wasm (TODO: js+wasm transpilers don't merge multi-arity fn decls into an overload dispatcher)
-- bug020: VM compiler emitted plain STORE_GLOBAL for every fn decl,
-- so a second fn with the same name shadowed the first. The interp
-- merged into XS_OVERLOAD; the VM did not. Fix: OP_STORE_GLOBAL now
-- builds the same overload wrapper, and OP_CALL picks a candidate by
-- arity before invoking.
fn calc(x) = x * 2
fn calc(x, y) = x + y
fn calc(x, y, z) = x + y + z

assert_eq(calc(5), 10)
assert_eq(calc(3, 4), 7)
assert_eq(calc(1, 2, 3), 6)

fn greet() { return "hi" }
fn greet(name) { return "hi {name}" }
assert_eq(greet(), "hi")
assert_eq(greet("alice"), "hi alice")

println("bug020: ok")
