-- derive_show: a procedural macro that synthesises a Show closure
-- for a struct, returning a "Name x=1 y=2" formatted string.
-- avoids '{' / '}' in the output string because XS reserves them
-- for the string-interpolation grammar.

import reflect

@[macro]
fn show_for(name) {
    return fn(v) {
        let af = reflect.fields(v)
        var parts = []
        for f in af {
            if f.name != "__type" {
                parts.push(f.name + "=" + str(f.value))
            }
        }
        return name + " " + parts.join(", ")
    }
}

struct Vec3 { x, y, z }
let show = show_for("Vec3")
println(show(Vec3{x: 1, y: 2, z: 3}))
