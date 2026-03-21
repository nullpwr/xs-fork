-- derive_eq: a procedural macro that synthesises a structural-equality
-- closure for a struct. invoked as `eq_for!(SampleInstance)` -- the
-- macro inspects the field shape via reflection and returns a closure
-- comparing field-by-field.

import reflect

@[macro]
fn eq_for(prototype) {
    let fields = reflect.fields(prototype)
    return fn(a, b) {
        let af = reflect.fields(a)
        let bf = reflect.fields(b)
        if len(af) != len(bf) { return false }
        if len(af) != len(fields) { return false }
        for i in 0..len(af) {
            if af[i].name != bf[i].name { return false }
            if af[i].value != bf[i].value { return false }
        }
        return true
    }
}

struct Point { x, y }
let prototype = Point{x: 0, y: 0}
let p1 = Point{x: 1, y: 2}
let p2 = Point{x: 1, y: 2}
let p3 = Point{x: 1, y: 3}

let eq = eq_for(prototype)
println(eq(p1, p2))   -- true
println(eq(p1, p3))   -- false
