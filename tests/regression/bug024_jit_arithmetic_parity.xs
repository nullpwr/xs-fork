-- bug024: when --jit was wired into the test architecture (running
-- programs through the C transpiler + system C compiler) it disagreed
-- with VM/interp on five arithmetic operators that the C runtime
-- modeled differently:
--   *  **      pow promoted ints to floats unconditionally
--   *  //      floor division (toward -inf)
--   *  %       truncated remainder (sign follows dividend)
--   *  <=>     never emitted (placeholder XS_NULL)
--   *  &&, ||  never emitted (placeholder XS_NULL)
--   *  is      never emitted
-- Fix: mirror VM/interp semantics in transpiler/c_gen.c and add
-- xs_pow / xs_is helpers to the C runtime.
assert_eq(2 ** 10, 1024)
assert_eq(-7 // 2, -4)
assert_eq(-7 % 2, -1)       -- truncated remainder
assert_eq(5 <=> 3, 1)
assert_eq(3 <=> 5, -1)
assert_eq(5 <=> 5, 0)
assert_eq(true && true, true)
assert_eq(true && false, false)
assert_eq(false || true, true)
assert(42 is int, "int is int")
assert("hi" is str, "str is str")

println("bug024: ok")
