-- bug047: lifecycle decorators fire in the documented order. on_start
-- runs once after all top-level statements settle (so vars are visible
-- to it), and on_exit runs once after on_start before the process
-- leaves the run loop. assertion lives inside on_exit so that all
-- fires are complete by the time it runs.

var booted = false
var exited = false

@on_start fn boot() { booted = true }

@on_exit fn check() {
    assert(booted, "on_start should fire before on_exit")
    exited = true
}

println("bug047: ok")
