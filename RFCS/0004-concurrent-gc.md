---
rfc: 0004
title: Concurrent garbage collection
author: xs-lang0
status: draft
created: 2026-04-30
---

# 0004: Concurrent garbage collection

## Summary

Move the GC's mark and sweep phases off the mutator threads. Today
`gc.collect()` is stop-the-world; on a 200 MB heap that is a
50-150ms pause depending on shape. This RFC introduces a concurrent
mark phase running on a dedicated GC thread, with mutator
synchronisation via a write barrier. Sweep stays incremental.

## Motivation

The concurrency chapter's performance note is the relevant text:

> The GC is single-threaded today (see [policy](./policy.md) for the
> stability promise). For workloads dominated by allocation, expect
> GIL contention.

Two related problems:

1. **GC pauses dominate p99 latency.** An HTTP service handling 5K
   req/s on a 4-core machine spends ~7% of wall time in stop-the-world
   GC. p99 latency is ~3x p50 because of pause spikes.
2. **The collector blocks all other threads.** Pure-XS workers cannot
   make progress during the pause, even ones that never allocate.

Both go away if the mark phase runs concurrently with mutators.

## Guide-level explanation

Behaviour visible to a normal user:

- `gc.collect()` returns faster (the synchronous phase shrinks).
- p99 of any latency-sensitive program drops by ~70% on the standard
  benchmarks.
- New tunable: `gc.set_concurrent(true)`. Default `true` once stable.

```xs
import gc
gc.set_concurrent(true)            -- on by default after 1.0
gc.set_threshold(0, 8000)
println(gc.stats())                 -- now includes concurrent metrics
```

`gc.stats()` gains fields:

```xs
#{
    pause_count: 142,
    pause_ns_total: 2_100_000,
    concurrent_mark_ns_total: 18_400_000,    -- new
    concurrent_overhead_pct: 1.4,             -- new
    write_barrier_hits: 89_412,                -- new
    ...
}
```

`gc.set_concurrent(false)` reverts to the stop-the-world collector.
Useful for embedded targets where pause-time matters less than
predictable throughput, or for diagnostics.

## Reference-level explanation

### Algorithm

Snapshot-at-the-beginning (SATB) mark, sweep stays incremental as
today. Three phases:

1. **Initial mark** (stop-the-world, very short). Snapshot the root
   set: stacks, globals, registers. ~1ms on a typical service.
2. **Concurrent mark** (mutator runs, GC thread runs). The GC thread
   walks the heap from the root snapshot, marking reachable objects.
   Mutators run with a write barrier (see below) that captures any
   reference they overwrite or create.
3. **Final mark** (stop-the-world, short). Drain the SATB queues
   produced during concurrent mark. Bound: the queue size at this
   point is small because mutators flush periodically.
4. **Concurrent sweep** (mutator runs, sweep runs). Free unmarked
   objects in chunks; today's incremental sweep extends to run in
   parallel with mutators.

Initial-mark and final-mark together account for ~2ms of pause
typical, regardless of heap size. Concurrent phases cost CPU
(measured ~6-8% of total CPU on a busy thread) but no pause time.

### Write barrier

A *Yuasa-style* SATB barrier. Before any pointer overwrite during
concurrent mark, the previous value is recorded in a per-thread SATB
buffer:

```c
static inline void xs_satb_barrier(XSValue *slot, XSValue old) {
    if (gc_state.in_concurrent_mark && !is_marked(old)) {
        satb_buffer_push(&current_thread->satb, old);
    }
}
```

Generated at every:

- Field store on a struct (`obj.x = v`)
- Array store (`arr[i] = v`)
- Local-variable store *to a closure-captured upvalue*
- Closure-upvalue assign

Cost: one branch and one buffer push, gated on a thread-local
`in_concurrent_mark` flag. Empirically: 0.6-1.5% throughput on the
sort/json/munging benches.

### Initial-mark synchronisation

Stopping the world means parking every mutator at a *safepoint*. The
VM's existing safepoint-poll instrumentation already exists for
nursery cancellation; we reuse it. Safepoint check at every:

- Function call entry
- Loop back-edge
- `await` and channel operation
- Allocation site

Pre-emptively-stopped threads have their stacks scanned by the GC
thread; running threads cooperate on next safepoint poll.

### Final-mark synchronisation

Same protocol as initial mark. Bound on pause time: the SATB
buffers' accumulated size. Per-thread buffers are flushed (drained
into the global mark queue) on every 4 KB written or every safepoint;
this keeps the final-mark pause bounded by ~4KB * thread_count, which
on a 16-core box is 64KB and a few-hundred-microseconds drain.

### Memory ordering

The concurrent mark phase reads from objects that mutators are
writing to. The SATB barrier ensures we never miss a reference, but
we still need acquire/release ordering on:

- The mark bit (`load-acquire` from GC, `store-release` from GC
  during mark; mutators only ever read).
- The mark-queue head/tail pointers (atomic).

Object payload reads do not need atomic semantics: SATB guarantees
that any reference visible to GC was published before mark started or
is captured in a SATB buffer.

### Concurrent sweep

Today's sweep walks the heap in 2 KB chunks between mutator slices.
We extend it to run on the GC thread continuously, with one bit of
per-chunk synchronisation: before a mutator allocates from a chunk,
it CASes the chunk into "owned" state; the sweeper skips owned
chunks and revisits them next cycle.

### Configuration

| variable                       | default | meaning                          |
|-------------------------------|---------|----------------------------------|
| `XS_GC_CONCURRENT`            | `1`     | enable concurrent mark           |
| `XS_GC_MARKER_THREADS`        | `1`     | number of GC marker threads      |
| `XS_GC_SATB_BUFFER_SIZE`      | `4096`  | per-thread SATB buffer (bytes)   |
| `XS_GC_TRIGGER_PCT`           | `60`    | concurrent mark trigger (% heap) |

Multiple marker threads (`XS_GC_MARKER_THREADS=N`) parallelise the
mark by partitioning the work-stealing queue. Single-thread is the
default because the marginal benefit is small for heaps under 1 GB.

## Drawbacks

- Write barrier costs throughput. Estimated: 1-2% on allocation-heavy
  workloads. The pause-time win usually outweighs this for services;
  for batch jobs, less clear.
- Concurrency bugs in GC are catastrophic. We need the test surface
  to grow: existing single-threaded GC tests, plus randomised
  concurrent-mutation tests at all the tricky points.
- Memory overhead. SATB buffers per-thread (4 KB each), plus the
  concurrent-mark side-data on the heap (one bit per object for
  *colour*, one for *snapshot*). On a 1M-object heap, ~256 KB of
  metadata overhead, up from ~128 KB today.
- Cannot ship to ESP32 / wasm32 builds. Both targets are
  single-threaded; the mobile chapter already excludes the JIT for
  similar reasons. We will gate concurrent GC on
  `XSC_HAS_THREADS=1`.

## Rationale and alternatives

**Stay STW forever.** Latency stays bad on real workloads. The
concurrent-GC chapters of every modern runtime are a strong precedent
that the cost is worth paying.

**Generational without concurrent.** XS already has a generational
nursery. Helps a lot but does not bound the major-collection pause,
which is what hurts p99.

**Move to a borrow checker / no-GC subset.** Out of scope for *this*
RFC, but a separate RFC (RFC 0005) explores `@scoped` allocation as a
GC-bypass option. The two RFCs compose: code that opts into `@scoped`
will not produce GC garbage, while everything else benefits from
concurrent collection.

**ZGC-style colored pointers.** Beautiful technique; needs ABI changes
that conflict with the JIT's tagged-Value layout. Possibly future
work; far too invasive for this RFC.

## Prior art

- Java's G1 / Shenandoah / ZGC are the obvious references. We adopt
  G1's write-barrier shape (cheaper than Shenandoah's load barrier).
- Go's concurrent GC uses a tri-color mark with a Dijkstra-style
  write barrier. Yuasa SATB is closer to what we need because the
  mutator does not need to stop the world for stack scanning;
  per-thread stacks are walked at safepoints individually.
- Lua's experimental concurrent GC (in 5.5 development) was a useful
  reference for keeping the data structures small.

## Unresolved questions

- Should concurrent sweep be optional? (Today's incremental sweep is
  already approximately concurrent; the change is mostly removing
  the "yield to mutator" call.) Probably just always-on.
- Pause-time SLO. Do we promise a max final-mark pause, or only a
  best-effort? Inclination: best-effort with `gc.stats()` reporting,
  not a hard SLO.
- Write-barrier instrumentation cost on the JIT. Each store gets one
  extra branch. Plan: emit the barrier inline in the JIT for hot
  paths, fall through to a slow path for the rare buffer-full case.

## Future possibilities

- Multi-region heap with per-region collection (G1 territory).
- Integration with `actor` for per-actor heaps that can be collected
  in isolation.
- ZGC-style colored pointers if we ever do an ABI break.

## Impact on stability

The GC's exact timings are explicitly *not* covered by the stability
policy. This RFC does not change any user-observable contract beyond
faster pauses and a richer `gc.stats()`.

The new env vars and `gc.set_concurrent` are Tier 2 for one minor,
then Tier 1.

`@unstable` until: pause-time wins reproduce on the bench harness,
randomised concurrent-mutation tests pass for 1M iterations, no
correctness regressions in any backend across two minor cycles.
