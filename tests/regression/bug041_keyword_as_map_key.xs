-- skip-emit: wasm (TODO: wasm transpiler doesn't lower dynamic method-on-map dispatch)
-- bug041: keywords like `handle`, `fn`, `effect`, `type` couldn't
-- appear as bareword map keys or as method names after a dot. The
-- parser saw `{ handle: ... }` and dispatched to parse_handle for
-- the effect-handler grammar, which immediately failed on the colon.
-- Same shape with `obj.handle(...)` because the field-access path
-- only accepted TK_IDENT and the TK_IF..TK_PANIC range, missing the
-- contextual effect keywords (TK_EFFECT, TK_PERFORM, TK_HANDLE,
-- TK_RESUME) and a few other keywords.
--
-- Real users hit this immediately; wrapping a framework whose
-- handler method happens to be named `handle` is common (http
-- servers, signal subscribers, command dispatchers).
--
-- Fix: any keyword token followed by `:` in a map literal lifts
-- into a string-keyed entry. After `.`, any keyword token resolves
-- as a bareword field/method name.

-- map keys
let server = { handle: fn(req) { return "ok " + req }, port: 8080 }
assert_eq(server.handle("/x"), "ok /x")
assert_eq(server.port, 8080)

-- explicit-map prefix #{}
let cfg = #{
    fn: "main",
    type: "worker",
    effect: "log",
    handle: "default",
    perform: "noop",
    resume: false,
}
assert_eq(cfg["fn"], "main")
assert_eq(cfg["type"], "worker")
assert_eq(cfg["effect"], "log")
assert_eq(cfg["handle"], "default")
assert_eq(cfg["perform"], "noop")
assert_eq(cfg["resume"], false)

-- nested + dot access
let app = {
    routes: [],
    handle: fn(path) { return "route:" + path },
}
assert_eq(app.handle("/api"), "route:/api")
assert_eq(app["routes"].len(), 0)

-- keyword as method call
let dispatcher = { perform: fn(action) { return action + "!" } }
assert_eq(dispatcher.perform("save"), "save!")

println("bug041: ok")
