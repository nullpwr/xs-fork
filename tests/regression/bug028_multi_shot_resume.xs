-- skip-emit: c (TODO: c transpiler effect machinery is single-shot only)
-- bug028: a single perform/handle pair where the body performs
-- multiple times and each arm mutation has to persist past resume.
-- The original VM path captured the entire stack [0, sp_off) on
-- perform and replayed it on resume, which wiped the handler frame's
-- own outer-var slot. The snapshot now spans only the suspended
-- body slice [te->stack_top, sp_off), so closure mutations the arm
-- body performs (counter += v) survive the resume.

effect E { fn op(x) }

var counter = 0

let r = handle {
    perform E.op(1)
    perform E.op(2)
    perform E.op(3)
    "done"
} {
    E.op(v) => {
        counter = counter + v
        resume null
    }
}

assert_eq(counter, 6)
assert_eq(r, "done")
println("ok")
