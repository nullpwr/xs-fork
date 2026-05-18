-- bug019: xs_runtime_error used to print to stderr and continue with
-- a null sentinel. try/catch could not catch divide-by-zero, type
-- mismatches, indexing errors, etc. Fix: xs_runtime_error now installs
-- a CF_THROW with a structured error map; the diagnostic render is
-- suppressed when an enclosing try will catch; the global error
-- counter only ticks for uncaught errors.
var caught = false
try {
    let _ = 1 / 0
} catch e {
    caught = (e.kind == "division by zero")
}
assert(caught, "divide-by-zero should be catchable")

var caught_index = false
try {
    let _ = (42)[0]
} catch e {
    caught_index = true
}
assert(caught_index, "indexing a non-collection should be catchable")

var caught_type = false
try {
    let _ = "x" - "y"
} catch e {
    caught_type = true
}
assert(caught_type, "type mismatch on - should be catchable")

println("bug019: ok")
