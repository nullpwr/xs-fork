-- bug011: slice and tuple patterns used to ignore the subject's actual
-- shape. `match (1,2) { [a,b] => ... }` would fire on a tuple, and
-- `match [1,2] { (a,b) => ... }` would fire on an array. Fix:
--   * <array-like> predicate is now strictly XS_ARRAY (not tuple)
--   * compile_tuple_pattern_at emits an <tuple-like> guard up front
let r1 = match (1, 2) { [a, b] => "arr" _ => "tup" }
assert_eq(r1, "tup")

let r2 = match [1, 2] { (a, b) => "tup" _ => "arr" }
assert_eq(r2, "arr")

-- And the matching shape still works
let r3 = match (1, 2) { (a, b) => a + b _ => -1 }
assert_eq(r3, 3)

let r4 = match [1, 2] { [a, b] => a + b _ => -1 }
assert_eq(r4, 3)

println("bug011: ok")
