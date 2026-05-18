-- skip-emit: c, wasm (TODO: c+wasm transpilers don't reproduce the jit native dispatch path)
-- bug030: when a JIT-compiled outer proto issued OP_METHOD_CALL on
-- an instance whose method was a user-defined closure, the IR_METHOD_CALL
-- codegen called vm_step_jit and immediately popped vm->sp expecting the
-- result -- but the closure-method dispatch had pushed a new frame
-- mid-op, so the inner method body had not yet run. The "popped result"
-- was actually the last call argument re-purposed as an inner local,
-- and the inner frame stayed on top of vm->frames. The next OP_LOAD_GLOBAL
-- in the JITed outer code missed its inline cache, fell through to
-- vm_load_global_ic, which read frame->closure_val->cl->proto from the
-- still-live inner frame and walked off the inner proto's chk->consts
-- with the outer's const index.
--
-- Fix: IR_METHOD_CALL now stashes baseline frame_count before the
-- step, and after vm_step_jit returns drives any new frames to
-- completion via tier2_run_until before pulling the result -- mirrors
-- IR_CALL's slow path. The minimal repro is class with method, then
-- method call, then any LOAD_GLOBAL.
class C {
    fn noop(self) { }
    fn read(self) { return 1 }
    fn inc(self) { self.x = (self.x ?? 0) + 1 }
}
let c = C()
println("before")
c.noop()
let g1 = println
assert_eq(c.read(), 1)
c.inc()
c.inc()
assert_eq(c.x, 2)
println("after")
println("bug030: ok")
