-- skip-emit: wasm (TODO: wasm transpiler doesn't lower multi-arm effect handle routing)
-- bug027: multi-arm `handle` used to send every effect to arm[0], so
-- a handler that wanted to route Log/Metric/Audit by name would just
-- run the first arm three times and silently drop the rest. The
-- compiler now dispatches by the effect's full name pushed at perform
-- time, with the last arm acting as a catch-all.

effect Log { fn say(msg) }
effect Metric { fn count(name) }

fn run() {
    perform Log.say("hello")
    perform Metric.count("hits")
    perform Log.say("world")
    return "done"
}

var log = []
var metrics = []

let r = handle run() {
    Log.say(m) => {
        log = log + [m]
        resume null
    }
    Metric.count(n) => {
        metrics = metrics + [n]
        resume null
    }
}

assert_eq(r, "done")
assert_eq(log, ["hello", "world"])
assert_eq(metrics, ["hits"])
println("ok")
