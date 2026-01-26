-- Effect handlers: an effect perform inside a handle block routes to
-- the nearest handler. The handler may resume with a value or return
-- without resuming.

effect Log {
    fn write(s: str)
}

fn do_work() {
    perform Log.write("start")
    perform Log.write("end")
    42
}

-- handler collects the written messages
var lines = []
let result = handle do_work() with Log {
    fn write(s) { lines.push(s); resume null }
}

assert_eq(result, 42)
assert_eq(lines.len(), 2)
assert_eq(lines[0], "start")
assert_eq(lines[1], "end")

-- handler that short-circuits without resuming
effect Abort {
    fn bail(v: int) -> int
}

fn compute() {
    let a = 10
    let b = perform Abort.bail(a)
    a + b -- never reached
}

let r = handle compute() with Abort {
    fn bail(v) { v * 2 }
}
assert_eq(r, 20)

println("CONFORMANCE OK")
