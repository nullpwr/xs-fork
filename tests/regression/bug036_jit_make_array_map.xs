-- skip-emit: wasm (TODO: wasm transpiler doesn't reproduce the jit native dispatch path)
-- bug036: OP_MAKE_ARRAY / OP_MAKE_TUPLE / OP_MAKE_MAP used to flow
-- through vm_step_jit's slow dispatch on every literal. bench_json
-- alone fired ~1000 of these per run (500 arrays + 500 maps in the
-- record-shape loop), each paying the per-instruction dispatch tax
-- on top of the actual allocation.
--
-- Three new helpers, one shared codegen path for arrays and tuples:
--   vm_make_array_fast(vm, n, is_tuple) -> +1 array/tuple
--   vm_make_map_fast(vm, npairs)        -> +1 map
-- The codegen pushes the n element vregs to vm->sp in order then
-- calls the helper; the helper drains those slots, decrefs each,
-- and returns the freshly built collection. No vm_step_jit, no
-- bytecode fetch, no opcode switch.
--
-- Mixed values (ints + strings + floats + nested), empty
-- collections, and tuple-vs-array still behave identically to
-- the interp/vm path.

-- Plain arrays.
let xs = [1, 2, 3, 4, 5]
assert_eq(xs.len(), 5)
assert_eq(xs[0], 1)
assert_eq(xs[4], 5)

-- Heterogeneous element types.
let mixed = [1, "two", 3.5, true, null]
assert_eq(mixed.len(), 5)
assert_eq(mixed[1], "two")
assert_eq(mixed[2], 3.5)

-- Empty array.
let e = []
assert_eq(e.len(), 0)

-- Tuples (same codegen path, is_tuple flag).
let t = (1, 2, 3)
assert_eq(t.0, 1)
assert_eq(t.2, 3)

-- Nested arrays / tuples.
let n = [[1, 2], [3, 4], [5, 6]]
assert_eq(n.len(), 3)
assert_eq(n[1][0], 3)
assert_eq(n[2][1], 6)

let nt = [(1, "a"), (2, "b"), (3, "c")]
assert_eq(nt[0].0, 1)
assert_eq(nt[2].1, "c")

-- Tight-loop array construction (the bench_json shape).
var rows = []
for i in 0..100 {
    rows.push([i, i * 2, i * 3])
}
assert_eq(rows.len(), 100)
assert_eq(rows[50][0], 50)
assert_eq(rows[50][1], 100)
assert_eq(rows[50][2], 150)

-- Maps with string keys.
let m = {"a": 1, "b": 2, "c": 3}
assert_eq(m["a"], 1)
assert_eq(m["b"], 2)
assert_eq(m["c"], 3)

-- Empty map.
let em = {}
assert_eq(em.len(), 0)

-- Mixed-value maps.
let mv = {"x": 1, "y": "two", "z": [1, 2, 3]}
assert_eq(mv["x"], 1)
assert_eq(mv["y"], "two")
assert_eq(mv["z"][2], 3)

-- Tight-loop map construction.
var mrows = []
for i in 0..50 {
    mrows.push({"id": i, "val": i * 10})
}
assert_eq(mrows.len(), 50)
assert_eq(mrows[20]["id"], 20)
assert_eq(mrows[20]["val"], 200)

-- Map values can themselves be arrays / maps.
let nested = {"list": [1, 2, 3], "obj": {"inner": 42}}
assert_eq(nested["list"][1], 2)
assert_eq(nested["obj"]["inner"], 42)

println("bug036: ok")
