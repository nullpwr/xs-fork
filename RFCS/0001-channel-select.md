---
rfc: 0001
title: First-class select on channels
author: xs-lang0
status: draft
created: 2026-04-12
---

# 0001: First-class `select` on channels

## Summary

Add a `select` block that waits on multiple channel operations and runs
the arm of whichever fires first. Replaces the current `try_recv`-in-a-
loop idiom with something the runtime can park efficiently.

## Motivation

The migrating-from-Go chapter already calls this out as the only thing
Go has that we don't:

```xs
loop {
    if let Some(x) = ch_a.try_recv() { handle_a(x); break }
    if let Some(x) = ch_b.try_recv() { handle_b(x); break }
    time.sleep_ms(1)
}
```

Three problems with that:

1. The 1ms sleep is a guess. Too short and you burn CPU; too long and
   latency on rare channels suffers.
2. The runtime cannot park the thread, so a fan-in over twenty channels
   keeps an OS thread spinning.
3. Sends do not have a `try_send` variant that integrates with the same
   poll loop, so producers must use a different pattern.

`channel.recv` is currently the second-most common blocking call in
real XS programs (behind `http.get`). A primitive that handles n-way
wait is worth the surface.

## Guide-level explanation

```xs
select {
    x = ch_a.recv() -> println("a: {x}"),
    y = ch_b.recv() -> println("b: {y}"),
    ch_out.send(payload) -> println("sent"),
    timeout(500ms) -> println("nothing in 500ms"),
}
```

Semantics:

- All arms are evaluated for readiness simultaneously.
- If multiple arms are ready, one is picked at random (avoids
  starvation under load).
- If none are ready, the thread parks. Each channel registers the
  waiting select; the first op that becomes ready wakes the thread and
  cancels the others.
- `timeout(d)` is a built-in arm; runs after `d` if no other arm fires.
- `default -> ...` runs immediately if no other arm is ready
  (non-blocking variant).

`select` is an expression: each arm produces a value, and the block
yields whichever arm's body ran.

```xs
let result = select {
    msg = ch.recv() -> Process(msg),
    timeout(1s) -> Timeout,
}
```

## Reference-level explanation

### Grammar

```
SelectExpr := "select" "{" SelectArm ("," SelectArm)* ","? "}"
SelectArm  := Pattern "=" ChanRecv "->" Expr
            | ChanSend "->" Expr
            | "timeout" "(" Expr ")" "->" Expr
            | "default" "->" Expr
ChanRecv   := Expr "." "recv" "(" ")"
ChanSend   := Expr "." "send" "(" Expr ")"
```

`Pattern` reuses the existing pattern grammar so destructuring works:

```xs
select {
    Ok(v) = result_ch.recv() -> use(v),
    Err(e) = result_ch.recv() -> fail(e),
}
```

(Both arms watch the same channel; whichever variant arrives is bound.)

### Runtime

Each channel keeps a list of *waiters*: `(thread_id, slot)` pairs. A
`select` block:

1. Builds a `Select` record with one slot per arm.
2. Tries every arm once in random order. If any succeeds, runs that
   arm's body and returns.
3. If none succeeded, registers itself as a waiter on every involved
   channel (sends register on the receive list and vice versa), then
   parks via the runtime's existing `thread.park()`.
4. On wake, the channel that fired sets the winning slot index and
   cancels the registration on the others.
5. The `Select` record drives the chosen arm.

Random choice on multi-ready: use `rand.range(0, n_ready)` over the
indices that came back ready. This matches Go's behaviour and avoids
priority inversion.

### Closed channels

A closed channel is always ready: `recv` returns `null`, `send` panics.
A `select` arm against a closed channel therefore fires immediately.
Idiom for "drain until closed":

```xs
loop {
    select {
        x = ch.recv() -> {
            if x == null { break }
            process(x)
        }
    }
}
```

### Cancellation

A `select` integrates with the existing nursery cancellation token:
parking inside a `select` registers with the active nursery, so
`nursery.cancel()` wakes the parked thread and propagates a
`Cancelled` panic.

### `select` inside `async`

Permitted. The async runtime treats `select` as a single suspension
point; arms can be a mix of channel ops and `await fut`. (Implementation
note: `await`-arms will share the nursery's wake token; channel arms
share the channel-waiter machinery. Both use the same parking primitive.)

## Drawbacks

- Random arm selection is hard to reason about under contention.
  Documented, but worth a callout in the docs.
- The implementation needs a per-thread "current select" pointer so
  channel operations know they are in a tentative-wait state. Adds one
  pointer per `Thread` record.
- No fairness guarantee; a hot channel can starve a cold one if the
  user does not drain consistently. Same as Go.

## Rationale and alternatives

**Channel.poll(\[\]) function.** Considered. Loses syntactic richness
(no per-arm bodies, no sends, no timeouts in the same surface). Fine as
a low-level primitive, bad as the user-facing API.

**`with_timeout(ch.recv(), 500ms)` only.** Solves the timeout case but
not the n-way wait. Does not subsume `select`.

**Reactive streams instead.** Out of scope; XS has signals for that. The
channel `select` is for imperative concurrency.

## Prior art

- Go's `select` is the obvious reference. We adopt the random-on-tie
  rule and the `default` clause.
- Erlang/Elixir's `receive` blocks are similar but always over a single
  mailbox. Less general.
- Rust's `crossbeam_channel::select!` macro is the closest fit
  semantically; the macro form is awkward, hence we make this a
  language construct rather than a library macro.

## Unresolved questions

- Should `select` work on `signal` (reactive) values too? Tempting, but
  signals fire on every change so the readiness semantics are
  different. Punting to a later RFC.
- Should we expose `Select.handle` as a value (Erlang `Ref`-style)
  for cancellation from another thread? Probably yes, but not in this
  RFC; the nursery integration covers the common cases.

## Future possibilities

- `for x in select { ... }` for n-way iteration patterns. Mostly
  sugar.
- A `priority(select)` form where the first ready arm wins instead
  of random.
- Compile-time exhaustiveness check on closed-channel arms.

## Impact on stability

Tier 1 addition. Pure addition: no existing program changes meaning.
Lands behind `@unstable` for one minor cycle, then promotes.
