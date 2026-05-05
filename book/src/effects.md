# Algebraic effects

`effect`, `perform`, `handle`, `resume` give you a richer cousin of
exceptions: a callee names something it'd like the world to do for
it, the *caller* decides how to fulfil that, then control optionally
returns to the callee.

```xs
effect Read {
    fn read() -> str
}

fn greet() {
    let name = perform Read.read()
    println("hello, {name}")
}

handle greet() {
    Read.read -> { resume("aria") }
}
```

```text
hello, aria
```

## What you can do with this

- **Dependency injection**, but tracked in the type system. A
  function declares the effects it `perform`s; the caller sees them in
  the signature and must `handle` them.
- **State, swap, undo**. `resume` can be called with a different
  value, called multiple times, or never called at all.
- **Async without colour**. The same `read` effect can be handled
  synchronously in tests and with `await` in production; the function
  body doesn't change.
- **Crash isolation**. `handle` can swallow a `perform` and return a
  default; the caller never sees the failure.

## Anatomy

```xs
effect E { fn op(args) -> Result }       -- declare
perform E.op(args)                        -- raise
handle expr { E.op -> { ...; resume(v) } } -- catch and continue
```

- `perform E.op(...)` looks up the nearest enclosing handler that
  catches `E.op`, jumps there, and waits.
- The handler body has access to `resume(v)`, which jumps back to
  the perform site with `v` as the result.
- If the handler returns *without* calling `resume`, control falls out
  of the surrounding `handle` block. (i.e. like `throw`.)

## Multiple handlers, multiple effects

```xs
effect Log { fn info(msg: str) }
effect Time { fn now() -> int }

fn job() {
    let t = perform Time.now()
    perform Log.info("started at {t}")
    do_work()
}

handle job() {
    Time.now  -> { resume(0) },        -- pretend the clock is frozen
    Log.info  -> |msg| { eprintln("[test] {msg}"); resume(()) },
}
```

## Re-raising

A handler can re-`perform` to delegate up the stack:

```xs
handle job() {
    Log.info -> |msg| {
        if production {
            perform RealLog.info(msg)         -- forward
            resume(())
        } else {
            resume(())                        -- swallow
        }
    },
}
```

## Async via effects

```xs
import http

effect Net {
    fn fetch(url: str) -> str
}

fn page(url) {
    let body = perform Net.fetch(url)
    parse(body)
}

handle page("https://example.com") {
    Net.fetch -> |url| { resume(http.get(url).body) }
}
```

The same `page()` function works in tests with a stubbed `Net.fetch`
that returns canned data; no separate test scaffolding, no mocking
library.

## Cost

Each `perform` is a stack walk to find the nearest matching handler,
followed by a captured-continuation jump. In our current
implementation, that's about 20-50× slower than a normal function
call. For hot loops, prefer the effect at the boundary and keep the
loop free of `perform`s.

## Compared to exceptions

| feature                  | exceptions | effects |
|--------------------------|-----------|---------|
| jump to handler          | yes       | yes     |
| return to call site      | no        | yes (via `resume`) |
| resume with a value      | n/a       | yes     |
| multiple resumes         | n/a       | yes     |
| visible in type          | unchecked | declared |
| compile-time exhaustion  | no        | yes     |

Use `try`/`catch` when you actually want failure semantics; use
effects when the callee is asking the caller to provide something.
