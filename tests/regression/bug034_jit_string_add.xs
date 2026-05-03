-- bug034: String + string under OP_ADD used to fall through every
-- fast path (SMI rejects non-SMI, float rejects non-FLOAT) and
-- hit vm_step_jit's slow path -- ~50ns of dispatch per concat on
-- top of the actual malloc+strcpy. Hash and string benchmarks
-- dispatched ~1000 string-ADDs each.
--
-- The IR_ADD codegen now has a third fast path between float and
-- the slow fallback: tag-checks both operands for XS_STR and calls
-- vm_concat_fast directly (the same helper bug033 added for
-- OP_CONCAT). Mixed-type ADDs (string + int, float + int, etc.)
-- still flow to the slow path so the existing dunder dispatch
-- catches them.
let parts = ["a", "b", "c", "d", "e"]
var s = ""
for p in parts { s = s + p }
assert_eq(s, "abcde")

-- repeated tight-loop concat (the shape that dominated bench_hash)
var t = ""
for i in 1..=100 { t = t + "x" }
assert_eq(t.len(), 100)

-- String-only chain folds left-to-right.
let g = "hi" + " " + "world"
assert_eq(g, "hi world")

-- Mixed-type concat still works (slow-path dunder handles it).
let m = "n=" + 42
assert_eq(m, "n=42")

println("bug034: ok")
