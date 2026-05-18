-- skip-emit: wasm (TODO: wasm transpiler doesn't raise the typed-arith runtime error path)
-- bug012: arithmetic ops on incompatible tags used to silently return
-- null. Fix: OP_ADD/SUB/MUL now raise xs_runtime_error when neither
-- operand is numeric, string, or array.
--
-- The error cases live in tests/negative; this fixture asserts that the
-- compatible cases are unchanged.
assert_eq(1 + 2, 3)
assert_eq(1.5 + 1.5, 3.0)
assert_eq("a" + "b", "ab")
assert_eq([1] + [2], [1, 2])
assert_eq(3 - 1, 2)
assert_eq(2 * 3, 6)
assert_eq("ab" * 2, "abab")
println("bug012: ok")
