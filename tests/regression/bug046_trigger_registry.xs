-- skip-emit: wasm (TODO: runtime trigger registry not lowered by --emit wasm)
-- bug046: top-level fns with trigger / discovery decorators register
-- one entry each in the runtime trigger registry. @once doesn't get
-- its own entry: it's recorded as a flag on the trigger it composes
-- with. interp registers in hoist_functions; vm registers via emitted
-- __register_decorator calls; jit reuses the vm path.

@bench fn bench_a() { let _ = 1 }
@every(1s) fn tick() { let _ = 1 }
@on_start fn boot() { let _ = 1 }
@once @every(5s) fn one_shot() { let _ = 1 }

assert_eq(__trigger_registry_size(), 4)
assert_eq(__trigger_registry_name(0), "bench")
assert_eq(__trigger_registry_name(1), "every")
assert_eq(__trigger_registry_name(2), "on_start")
assert_eq(__trigger_registry_name(3), "every")

println("bug046: ok")
exit(0)
