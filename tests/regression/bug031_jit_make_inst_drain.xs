-- skip-emit: c, wasm (TODO: c+wasm transpilers don't reproduce the jit native dispatch path)
-- bug031: OP_MAKE_INST's closure-init branch pushes a new call frame
-- mid-op (PUSH(inst); PUSH(inst); push args; call_frame_push). The
-- previous JIT lowering treated MAKE_INST as unsupported -- protos
-- containing it bailed wholesale -- because the regular IR_VM_STEP
-- post-pop would grab the last ctor arg instead of the instance,
-- and the inner init frame would stay live across the rest of the
-- caller's emitted code.
--
-- Fix mirrors the IR_METHOD_CALL drain landed in bug030: a new
-- IR_VM_STEP_DRAIN op stashes baseline frame_count before
-- vm_step_jit, post-step compares against the new count, and runs
-- tier2_run_until on any pushed inner frame before pulling the
-- result. OP_MAKE_INST lowers through it; OP_MAKE_CLASS / OP_INHERIT
-- still use plain IR_VM_STEP because they never push a frame.
class Counter {
    count = 0
    fn init(self, start) { self.count = start }
    fn inc(self)  { self.count = self.count + 1 }
    fn dec(self)  { self.count = self.count - 1 }
    fn value(self) { return self.count }
}

let c = Counter(10)
c.inc()
c.inc()
c.dec()
assert_eq(c.value(), 11)
assert_eq(type(c.value), "fn")

-- many MAKE_INST sites in one outer proto -- exercises the JIT's
-- frame-drain across multiple consecutive instantiations so a stale
-- inner frame can't survive into the next call.
class P {
    x = 0
    y = 0
    fn init(self, x, y) { self.x = x; self.y = y }
    fn sum(self) { return self.x + self.y }
}
var total = 0
for i in 1..=5 {
    let p = P(i, i * 2)
    total = total + p.sum()
}
assert_eq(total, 45)

println("bug031: ok")
