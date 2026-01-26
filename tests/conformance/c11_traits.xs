-- Traits: declaration, impl for a struct, dynamic dispatch through
-- trait-bound generic receivers. Methods defined in the trait body
-- are the defaults; an impl may override them.

trait Shape {
    fn area(self) -> float
    fn name(self) -> str { "shape" }
}

struct Circle { r: float }
struct Square { s: float }

impl Shape for Circle {
    fn area(self) -> float { 3.14 * self.r * self.r }
    fn name(self) -> str { "circle" }
}

impl Shape for Square {
    fn area(self) -> float { self.s * self.s }
    -- uses default name()
}

let c = Circle { r: 2.0 }
let q = Square { s: 3.0 }

assert_eq(c.area(), 12.56)
assert_eq(q.area(), 9.0)
assert_eq(c.name(), "circle")
assert_eq(q.name(), "shape")

-- dispatch through heterogeneous collection
let shapes = [c, q]
var sum = 0.0
for s in shapes { sum = sum + s.area() }
assert_eq(sum, 21.56)

println("CONFORMANCE OK")
