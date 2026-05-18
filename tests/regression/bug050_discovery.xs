-- skip-emit: wasm (TODO: @bench/@example discovery registry not lowered by --emit wasm)
-- bug050: @bench and @example are tracked in the runtime trigger
-- registry so xs bench / xs doc can find them without re-walking
-- the AST. they don't fire from the run loop themselves; the cli
-- subcommand is the consumer.

@bench fn bench_quick() { let _ = 1 }
@example fn example_quick() { let _ = 1 }

assert_eq(__trigger_registry_size(), 2)
assert_eq(__trigger_registry_name(0), "bench")
assert_eq(__trigger_registry_name(1), "example")

println("bug050: ok")
exit(0)
