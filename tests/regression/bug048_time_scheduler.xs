-- skip-emit: c, js, wasm (TODO: @every/@delayed run-loop scheduling not lowered by --emit transpilers)
-- bug048: @every and @delayed actually fire from the run-loop after
-- the script finishes. tests use very short intervals (50ms) and the
-- counter exits the process so the runner doesn't hang. parity covers
-- interp, vm, jit.

var delayed_fired = false
var ticks = 0

@delayed(20ms) fn boom() { delayed_fired = true }

@every(50ms) fn tick() {
    ticks = ticks + 1
    if ticks >= 3 {
        if !delayed_fired { exit(2) }
        exit(0)
    }
}

println("bug048: ok")
