-- skip-emit: c, wasm (TODO: c+wasm transpilers don't reproduce the jit native dispatch path)
-- bug032: OP_IMPL_METHOD and OP_TRAIT_APPLY were missing from the
-- JIT's op_supported set. Any proto that compiled an `impl` block
-- or an `impl Trait for Class` declaration bailed wholesale.
-- Both are frame-stable so they lower through plain IR_VM_STEP --
-- IMPL_METHOD pops (type, name, closure) and mutates type's method
-- table in place; TRAIT_APPLY pops (class, trait) and merges the
-- trait into the class.
class Counter {
    n = 0
    fn init(self, start) { self.n = start }
    fn value(self)       { return self.n }
}

-- impl block: adds methods to an existing class via OP_IMPL_METHOD
-- per declared method.
impl Counter {
    fn bump(self)         { self.n = self.n + 1 }
    fn bump_by(self, k)   { self.n = self.n + k }
}

let c = Counter(10)
c.bump()
c.bump()
c.bump_by(5)
assert_eq(c.value(), 17)
assert_eq(type(c.bump), "fn")

-- impl Trait for Struct: trait defaults + concrete impl. OP_TRAIT_APPLY
-- folds the trait's default body into the struct's method map; per-
-- impl methods register via OP_IMPL_METHOD on the struct's type.
trait Shape {
    fn area(self) -> float
    fn name(self) -> str { "shape" }
}
struct Square { s: float }
impl Shape for Square {
    fn area(self) -> float { self.s * self.s }
}

let q = Square { s: 3.0 }
assert_eq(q.area(), 9.0)
assert_eq(q.name(), "shape")

println("bug032: ok")
