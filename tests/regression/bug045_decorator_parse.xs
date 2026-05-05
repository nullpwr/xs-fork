-- bug045: the new trigger / discovery / api / modifier decorators
-- parse cleanly and attach to the function declaration. The runtime
-- doesn't fire any of them yet (later chunks land each handler), but
-- the parser must accept the syntax and reject obvious misuse.

@on_start fn boot() { let _ = 1 }
@on_exit  fn shutdown() { let _ = 1 }
@on_panic fn panicker() { let _ = 1 }
@on_signal("INT") fn graceful() { let _ = 1 }

@every(1s)            fn tick() { let _ = 1 }
@cron("0 * * * *")    fn hourly() { let _ = 1 }
@delayed(500ms)       fn later() { let _ = 1 }

@watch("./config.toml") fn reload() { let _ = 1 }

@bench   fn bench_a() { let _ = 1 }
@example fn example_a() { let _ = 1 }

@export("publicName") fn local_name() { let _ = 1 }

@once @every(5s) fn once_only() { let _ = 1 }

println("bug045: ok")
