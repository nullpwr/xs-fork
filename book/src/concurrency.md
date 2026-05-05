# Concurrency

XS gives you four shapes of concurrency, all in the same language:

| feature       | shape              | use when                                    |
|---------------|--------------------|--------------------------------------------|
| `spawn`       | OS threads         | real parallelism for CPU-bound work        |
| `async`/`await` | cooperative coroutines | I/O-bound, many in flight             |
| `channel`     | typed message queue | pipelines, fan-out/fan-in                 |
| `actor`       | isolated state with mailbox | actor-model service workers       |
| `nursery`     | structured concurrency block | a parent that owns children's lifetimes |

## Spawn: real threads

```xs
let h = spawn {
    println("hi from thread")
    expensive_compute()
}

h.join()                       -- block until the thread returns
```

`spawn` creates a real OS thread with its own native stack. The
runtime's GIL is dropped on blocking I/O calls (sleep, socket read,
mutex acquire) so multiple spawned threads get real parallelism on
syscalls.

CPU-bound parallelism on multi-core boxes: yes for parts of the
program that don't touch shared XS values, yes for any path that
spends most of its time in C extensions or syscalls. Pure-XS
hot loops contend on the GIL today.

## Async / await

```xs
import http
import json

async fn fetch_user(id) {
    let resp = await http.get("/users/{id}")
    return json.parse(resp.body)
}

let user = await fetch_user(42)
```

`await` suspends the current coroutine until the awaited value is
ready, releasing the runtime to run other coroutines. The scheduler
is cooperative: a coroutine that never `await`s never yields.

## Channels

```xs
let ch = channel(64)         -- bounded, capacity 64

spawn { for x in source { ch.send(x) } }

while not ch.is_empty() {
    process(ch.recv())
}
```

Channels are blocking by default; pair with `try_send` / `try_recv`
for non-blocking variants. They're the canonical way to share values
across threads, with no shared mutable state and no locks.

## Nurseries: structured concurrency

```xs
nursery {
    spawn { fetch_user(42) }
    spawn { fetch_orders(42) }
    spawn { fetch_billing(42) }
}                              -- exits when all three finish
```

The block returns *after* every spawned task finishes (or one of them
throws, in which case the others are cancelled and the throw
propagates). No leaked goroutines, no orphaned threads.

## Actors

```xs
actor Counter {
    var count = 0
    fn inc()  { count = count + 1 }
    fn read() -> int { count }
}

let c = Counter.spawn()
c.inc()
c.inc()
println(c.read())               -- 2
```

Actors own their state; the only way in is through their methods,
which are serialised via a per-actor message queue. Crash isolation
is at the actor boundary; a panic kills only that actor.

## Choosing

- One-off background work, no result needed → `spawn { ... }`.
- One-off background work, result awaited → `let h = spawn { ... }; h.join()`.
- Lots of overlapping I/O → `async`/`await`.
- Long-lived stream of items → `channel`.
- Mutable state shared between many tasks → `actor`.
- A request that spawns several pieces of work and waits on all →
  `nursery`.

## Cancellation

`spawn` returns a handle with `.cancel()` that signals the thread.
Cooperative; the thread checks at safe points (every `await`, every
channel op, every loop back-edge in tight loops). Force-killing a
thread mid-instruction is unsafe and we don't expose it.

`nursery` blocks cancel their children when leaving the block by
exception; explicit `nursery_handle.cancel()` does the same.

## Performance notes

- The GC is single-threaded today (see [policy](./policy.md) for the
  stability promise). For workloads dominated by allocation, expect
  GIL contention.
- `channel(0)` (unbuffered) synchronises send/recv pairs; `channel(N)`
  decouples producers and consumers up to `N` items in flight.
- `spawn` creates a fresh OS thread; if you spawn thousands you'll
  exhaust the system. For high-fan-out work prefer async or actors.
- Effect handlers (see [Effects](./effects.md)) compose with
  concurrency: a `perform` inside a `spawn` is handled by the
  *spawning* thread's installed handlers.
