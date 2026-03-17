# Migrating from Go

XS aims at the same niche Go does — single-binary deployment, real
threads, networking out of the box — with more language features and
a smaller runtime.

## What's the same

- **Single static binary**. `make release` gives you ~2 MB with no
  shared library deps.
- **Channels and goroutines**. `spawn { ... }` is `go func()` and
  `channel(N)` is `make(chan T, N)`.
- **Stdlib networking**. `import http` is the equivalent of `net/http`.

## What's different

- **Generics work without `[T any]` ceremony**. The type checker
  infers and constrains where it can.
- **Errors are values *and* exceptions**. You can `return Err(e)` or
  `throw e`. Both are idiomatic; pick per call site.
- **Pattern matching beats type switches**. `match` over enums is
  exhaustive at compile time.
- **No `defer`-on-return funkiness**. `defer expr` runs at scope exit,
  same as Go.

## Cheat sheet

| Go                                | XS                              |
|-----------------------------------|---------------------------------|
| `func add(a, b int) int { ... }`  | `fn add(a: int, b: int) -> int { ... }` |
| `if err != nil { return err }`    | `try { ... } catch e { return e }` |
| `go f(x)`                         | `spawn { f(x) }`                |
| `ch := make(chan T, 16)`          | `let ch = channel(16)`          |
| `ch <- x`                         | `ch.send(x)`                    |
| `x := <-ch`                       | `let x = ch.recv()`             |
| `select { case ... }`             | not built-in; use a `match` over try_recv on each channel |
| `defer f.Close()`                 | `defer f.close()`               |
| `type Foo struct { ... }`         | `struct Foo { ... }`            |
| `type Foo interface { ... }`      | `trait Foo { ... }`             |
| `time.Sleep(500 * time.Millisecond)` | `time.sleep_ms(500)`         |
| `json.Marshal(v)`                 | `json.stringify(v)`             |
| `fmt.Errorf("x: %d", n)`          | `"x: {n}"` then `throw`         |

## Concurrency feels familiar

```xs
fn worker(id, jobs, results) {
    for job in jobs {
        results.send(process(job))
    }
}

let jobs = channel(100)
let results = channel(100)
for i in 0..5 { spawn worker(i, jobs, results) }
for j in tasks { jobs.send(j) }
```

For request-scoped fan-out, prefer a `nursery` block — it gives you
the structured-concurrency property Go's `errgroup.Group` chases.

## What's missing vs Go

- **`select`** on channels — currently you poll with `try_recv` in a
  loop. An RFC for first-class `select` is open.
- **Mature stdlib breadth**: `crypto/x509` analogues exist (BearSSL is
  bundled) but the API surface is narrower than Go's.
- **`go test` parallelism**: `xs test` runs sequentially per file
  today.

## What you gain

- Algebraic effects (see [chapter](../effects.md)) replace the noisy
  error-handling style Go is known for, when you want.
- Universal literals: `500ms`, `10MB`, `45deg` are typed values, not
  comments on integers.
- Plugins for compile-time codegen — Go's `go generate` is text-level;
  XS plugins operate on the AST.
