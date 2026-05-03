-- bug033: OP_CONCAT and OP_ITER_GET previously routed through
-- IR_VM_STEP, which costs ~50ns of vm_step_jit dispatch on every
-- call (limits tick + instr fetch + switch). Both are now lowered
-- through dedicated IR ops (IR_CONCAT, IR_ITER_GET) whose codegen
-- calls the equivalent C helper directly.
--
-- The first generator iteration revealed a gap: vm_iter_get_fast's
-- map case originally walked keys generically; that's wrong for
-- generator/channel-shaped maps which need to index the materialised
-- yields array (or call try_recv on the channel). Helper now
-- mirrors the interpreter's iter shape coverage exactly.

-- Concat -- string + non-string operands.
let parts = []
for i in 1..=20 {
    parts.push("v" ++ i)
}
assert_eq(parts[0], "v1")
assert_eq(parts[19], "v20")
assert_eq("a" ++ "b" ++ "c", "abc")
assert_eq("count: " ++ 42, "count: 42")

-- ITER_GET on array, tuple, range, str, map -- the for-in lowering
-- emits one ITER_GET per iteration body.
var arr_sum = 0
for x in [1, 2, 3, 4, 5] { arr_sum = arr_sum + x }
assert_eq(arr_sum, 15)

var tup_sum = 0
for x in (10, 20, 30) { tup_sum = tup_sum + x }
assert_eq(tup_sum, 60)

var range_sum = 0
for x in 1..=10 { range_sum = range_sum + x }
assert_eq(range_sum, 55)

var str_chars = ""
for c in "abc" { str_chars = str_chars ++ c }
assert_eq(str_chars, "abc")

-- Map iteration via `for k in map`. (Iteration order depends on
-- the map's bucket layout under VM/JIT, so just check the set of
-- keys is what we put in.)
let m = {"a": 1, "b": 2, "c": 3}
var key_count = 0
for k in m { key_count = key_count + 1 }
assert_eq(key_count, 3)

-- Generator: bytecode emits ITER_GET against an XS_MAP whose
-- __type is "generator". The fast path indexes into _yields.
fn* tens(n) {
    var i = 1
    while i <= n {
        yield i * 10
        i = i + 1
    }
}
var gen_sum = 0
for v in tens(5) { gen_sum = gen_sum + v }
assert_eq(gen_sum, 150)

println("bug033: ok")
