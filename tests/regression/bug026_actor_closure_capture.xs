-- skip-backend: jit (actor-method closures still segfault the JIT; v0.9 work)
-- bug026: VM actors used to compile with flattened state and dropped
-- captures of outer-scope variables, so a method that referenced an
-- outer `let` either crashed or read garbage. The interpreter handled
-- this through real closure capture; the VM didn't, and STATUS called
-- out the divergence as a known footgun. The compiler now treats
-- actor methods like ordinary nested functions for upvalue purposes,
-- so both backends should agree.

fn build() {
    var log = []
    let prefix = "log: "

    actor Logger {
        fn record(msg) { log = log + [prefix + msg] }
        fn dump() { return log }
    }

    return spawn Logger
}

let l = build()
l.record("hello")
l.record("world")
let messages = l.dump()
assert_eq(messages.len(), 2)
assert_eq(messages[0], "log: hello")
assert_eq(messages[1], "log: world")

-- Two actors capturing the same outer var read each other's writes.
fn pair() {
    var shared = 0
    actor R { fn read() { return shared } }
    actor W { fn write(n) { shared = n } }
    return [spawn R, spawn W]
}

let p = pair()
let r = p[0]
let w = p[1]
assert_eq(r.read(), 0)
w.write(42)
assert_eq(r.read(), 42)
println("ok")
