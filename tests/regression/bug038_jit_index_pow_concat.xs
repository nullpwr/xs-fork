-- bug038: more dispatch elimination on top of bug037.
--
-- 1. compile-time fold for binary `LIT op LIT` on int / float operands.
--    catches `2 ** 31` recomputed every loop iter in a PRNG and the
--    `1 + 2` shapes inside larger constant expressions. uses
--    __builtin_*_overflow so wrap-around cases still defer to the
--    runtime (where bigint promotion handles them).
--
-- 2. vm_index_get_fast for the three high-traffic shapes:
--      arr[int], tup[int]   -> array element (with negative wrap)
--      map[str]             -> map_get  (channels still slow-path)
--      range[int]           -> arithmetic
--    bench_sort's 23K INDEX_GET dispatches per quicksort run drop
--    to zero.
--
-- 3. array .concat on the JIT method-call fast path. quicksort's
--    `quicksort(left).concat([pivot]).concat(quicksort(right))`
--    fired 1316 .concat dispatches per run; now zero.
--
-- 4. float fast path for IR_NEG codegen (vm_float_neg). const-folding
--    handles `-2.0`; this catches the runtime case `-x` where x is
--    a non-SMI float.

-- ---- 1. constant-folded binary ops ----
let pow1 = 2 ** 31
assert_eq(pow1, 2147483648)
let pow2 = 3 ** 4
assert_eq(pow2, 81)
let prod = 7 * 8 * 9
assert_eq(prod, 504)
let summ = 100 + 200 + 300
assert_eq(summ, 600)
-- float folds
let f1 = 1.5 + 2.5
assert_eq(f1, 4.0)
let f2 = 3.0 * 0.5
assert_eq(f2, 1.5)
-- mixed (no fold; runtime handles via dunder)
var x = 2
let unfolded = x ** 3
assert_eq(unfolded, 8)
-- overflow safely defers to runtime (bigint)
let big = 10 ** 20
assert(big > 0, "big positive")

-- ---- 2. INDEX_GET fast path ----
let arr = [10, 20, 30, 40, 50]
assert_eq(arr[0], 10)
assert_eq(arr[4], 50)
-- negative wrap
assert_eq(arr[-1], 50)
assert_eq(arr[-2], 40)
-- out-of-bounds: arr[i] throws, arr.get(i) returns null
assert_eq(arr.get(100), null)
assert_eq(arr.get(100, "miss"), "miss")

-- map[str]
let m = {"a": 1, "b": 2, "c": 3}
assert_eq(m["a"], 1)
assert_eq(m["c"], 3)
assert_eq(m["missing"], null)

-- range[int]
let r = 10..20
assert_eq(r[0], 10)
assert_eq(r[5], 15)

-- tuple[int]
let t = (100, 200, 300)
assert_eq(t[0], 100)
assert_eq(t[2], 300)

-- tight loop -- the bench_sort shape
let nums = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5]
var total = 0
var i = 0
while i < len(nums) {
    total = total + nums[i]
    i = i + 1
}
assert_eq(total, 44)

-- ---- 3. .concat fast path ----
let xs1 = [1, 2, 3]
let xs2 = [4, 5, 6]
let xs3 = xs1.concat(xs2)
assert_eq(xs3, [1, 2, 3, 4, 5, 6])
-- chained
let chained = [1].concat([2]).concat([3, 4]).concat([])
assert_eq(chained, [1, 2, 3, 4])
-- empty arg
let weird = [1].concat([])
assert_eq(weird, [1])
-- multi-arg concat
let multi = [1].concat([2], [3], [4])
assert_eq(multi, [1, 2, 3, 4])

-- ---- 4. runtime float NEG ----
fn neg_at_runtime(x) { return -x }
assert_eq(neg_at_runtime(2.5), -2.5)
assert_eq(neg_at_runtime(-7.25), 7.25)
-- through a variable so const fold can't help
var f = 4.0
var s = 0.0
for i in 1..10 {
    f = f * 1.1
    s = s + neg_at_runtime(f)
}
assert(s < 0.0, "sum of negatives is negative")

println("bug038: ok")
