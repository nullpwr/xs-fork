-- bug043: a sweep across documented features turned up four
-- additional silent divergences between --interp and --vm/--jit.
-- Each one was a "feature claimed to work" case where the VM compiler
-- or runtime took a different path than the tree-walker.
--
-- 1. Try operator (`x?`) was compiled as OP_NOT in NODE_UNARY because
--    the dispatch only recognised `-`, `~`, `!`. The slow path was
--    also keyed off the older XS_MAP-with-_tag/_val enum encoding;
--    the current XS_ENUM_VAL representation never matched.
--
-- 2. `let {a, b} = m` (map destructure) fell through to the simple
--    binding path because NODE_PAT_MAP wasn't in the destructure
--    list. The whole map ended up bound to a synthetic <anon> slot,
--    a and b stayed unbound.
--
-- 3. `[(x,y) for x in xs for y in ys]` ran the clauses sequentially
--    instead of nesting them. The y loop saw x stuck at its last
--    value and the x loop saw y as null. Same bug in NODE_MAP_COMP.
--
-- 4. Map iteration walked bucket order instead of insertion order
--    on `for k in m`, `m.keys()`, `m.values()`, `m.entries()`.
--    LANGUAGE.md promises insertion order; the VM was leaking hash
--    layout to user code.
--
-- 5. Trait method dispatch through a fn parameter resolved the wrong
--    impl when the receiver's underlying instance map happened to
--    land in a slot freed by an earlier short-lived instance of a
--    different type. The IC keyed off the instance pointer alone
--    so a heap-slot collision read the previous type's cached
--    method back. Fold __type into the cache tag.

-- 1. try operator
enum Result { Ok(v), Err(e) }
fn try_double(x) {
    let v = x?
    return Result::Ok(v * 2)
}
println(try_double(Result::Ok(5)))
println(try_double(Result::Err("nope")))

-- 2. map destructure
let m = {x: 10, y: 20, z: 30}
let {x, y, z} = m
assert_eq(x, 10)
assert_eq(y, 20)
assert_eq(z, 30)

-- map destructure with rename
let n = {a: 1, b: 2}
let {a: aa, b: bb} = n
assert_eq(aa, 1)
assert_eq(bb, 2)

-- 3. list comprehension nested clauses
let pairs = [(x, y) for x in 1..=2 for y in 1..=2]
assert_eq(pairs, [(1,1), (1,2), (2,1), (2,2)])

let trips = [(a,b,c) for a in 1..=2 for b in 1..=2 for c in 1..=2]
assert_eq(trips.len(), 8)

-- with filter
let evens = [(x, y) for x in 1..=3 for y in 1..=3 if (x + y) % 2 == 0]
assert_eq(evens.len(), 5)

-- map comprehension
let sq = #{x: x * x for x in 1..=4}
assert_eq(sq["1"], 1)
assert_eq(sq["4"], 16)

-- 4. map iteration
let mm = {b: 1, a: 2, c: 3}
var ks = []
for k in mm { ks.push(k) }
assert_eq(ks, ["b", "a", "c"])

assert_eq(mm.keys(),    ["b", "a", "c"])
assert_eq(mm.values(),  [1, 2, 3])
assert_eq(mm.entries(), [("b", 1), ("a", 2), ("c", 3)])

-- 5. trait dispatch through fn param
trait Greet { fn hi(self) -> str }
struct A {}
impl Greet for A { fn hi(self) { return "A!" } }
struct B {}
impl Greet for B { fn hi(self) { return "B!" } }
fn greet(g) { return g.hi() }
assert_eq(greet(A()), "A!")
assert_eq(greet(B()), "B!")
assert_eq(greet(A()), "A!")
assert_eq(greet(B()), "B!")

println("bug043: ok")
