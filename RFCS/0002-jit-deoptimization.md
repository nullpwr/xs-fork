---
rfc: 0002
title: JIT deoptimization path
author: xs-lang0
status: draft
created: 2026-04-18
---

# 0002: JIT deoptimization path

## Summary

Add a deopt path so the tier-2 JIT can speculate on shapes (small
integers, monomorphic call sites, struct layouts) and fall back to
the bytecode VM when speculation fails. Today the JIT only compiles
patterns it can prove conservative; this leaves performance on the
table for hot paths that are *almost* monomorphic.

## Motivation

The book chapter on the VM/JIT says it directly:

> Not implemented. The lowerer is conservative; it rejects code
> patterns whose runtime shapes it can't prove.

Two consequences:

1. Mixed-shape arithmetic (`x + y` where `y` is float 99% of the time
   but occasionally a string) refuses to JIT at all. Whole hot loop
   stays in the VM.
2. Method dispatch at a call site that is monomorphic in practice
   cannot be inlined; the polymorphic path is the only one we can
   prove, so the polymorphic path is what we emit.

The numbers, from `benchmarks/`:

- `bench_mandelbrot.xs`: 14× faster on Node, 9× on CPython, 1.1× on
  XS-JIT. The float arithmetic is JITted but every iteration goes
  through the boxed-Value path.
- `bench_struct_dispatch.xs`: a callsite that is 99.9% one
  implementation runs at 38% of the throughput of the same code
  written without traits.

Without speculation, the JIT's performance ceiling is too low.

## Guide-level explanation

The user-facing change is invisible in correct code. Things that
previously did not JIT will now JIT, and the same program produces
the same answer. The visible change is a new diagnostic event:

```text
[jit] deopt at fn 'sum' line 17 site 'add': expected int, got string
```

These events are off by default; enable with `XS_JIT_TRACE_DEOPT=1` or
`xs --jit-trace-deopt`. Useful for debugging an unexpectedly slow hot
path.

`@no_speculate` turns off speculation for a single function, leaving
the conservative compiler in charge:

```xs
@no_speculate
fn careful_arith(a, b) { a + b }
```

This is rarely needed; the runtime adapts. It exists as an escape
valve.

## Reference-level explanation

### Speculation points

The JIT lowerer adds a *guard* before each speculated operation. The
guard checks the actual runtime shape against the assumed shape. On
mismatch, the guard transfers control to the *deopt stub*.

Initial speculations:

| site                      | speculation                          | guard      |
|---------------------------|--------------------------------------|------------|
| `LOAD_GLOBAL`             | monomorphic IC hits last seen value  | pointer eq |
| arithmetic op             | both operands are SMI                | tag check  |
| arithmetic op             | both operands are float              | tag check  |
| method call on struct     | struct shape matches last seen layout| shape id   |
| `LOAD_FIELD`              | struct shape matches                 | shape id   |
| array index               | known small int, in bounds           | range check |

Each guard records its source bytecode PC. Failure means: jump to
deopt, rebuild VM frame at that PC, resume in the VM.

### Deopt stub

The deopt stub is one block of native code per JIT'd proto. Its
inputs:

- The PC of the guard that fired.
- The set of values currently live in registers and on the JIT stack.

Its job:

1. Spill all live registers to a small per-thread `DeoptScratch`
   buffer.
2. Rebuild a VM frame with the right bytecode PC and the right local
   slots. The mapping (JIT live-set to VM locals) is stored in a
   side-table per guard.
3. Increment the proto's `deopt_count`.
4. Tail-call into the VM dispatch loop.

If `deopt_count` crosses a threshold (currently planned: 8 per proto
in 1000 calls), the proto is *un-JITted*: its `jit_entry` is reset to
the bytecode dispatcher, future calls go through the VM only, and a
flag prevents re-JIT at that speculation site.

### Side-tables

Each guard owns one *deopt descriptor*:

```c
struct DeoptDesc {
    uint32_t  bytecode_pc;        // where to resume in the VM
    uint32_t  guard_kind;          // for diagnostics + adaptive un-JIT
    uint16_t  live_count;
    uint16_t  spill_offset;        // byte offset into DeoptScratch
    LiveSlot  slots[live_count];   // (jit_reg_or_stack_slot, vm_local) pairs
};
```

Stored adjacent to the codegen output. The JIT writer emits one when
a guard is placed; total size scales with the number of guards, which
empirically is around 1.2 per emitted bytecode op.

### Profile-driven re-speculation

A proto un-JITted because of one bad speculation does not stay
permanently un-JITted. After 10K more invocations, the runtime
re-considers the proto and recompiles with the failed speculations
disabled. The bad guard's site is recorded in the proto's
`deopt_blacklist`; subsequent compiles see the site as off-limits but
still apply other speculations.

### Concurrency

Deopt happens on the same thread as the running code; it does not
need to coordinate with other threads. Un-JITting a proto (resetting
`jit_entry`) is a single pointer write, ordered with a release fence.
Other threads observe either the old or the new entry; both are
correct.

## Drawbacks

- The deopt stub increases code-buffer pressure. Estimated: 1.5x to
  2x the size of the speculative path. Mitigation: per-RFC-future
  multi-region code buffer (already on the roadmap).
- Side-tables eat memory. ~24 bytes per guard, ~1.2 guards per
  bytecode op, ~30 bytes per bytecode op typical. So ~30 bytes of
  metadata per bytecode op JITted. For a 50-op function, ~1.5 KB.
- More complex correctness story. We need to round-trip the same
  side-effects through the VM that the JIT stopped in the middle of
  performing. Most arithmetic ops are pure, but allocation guards
  need to be placed *before* the allocation; we cannot deopt
  mid-allocation.
- Test surface grows substantially. Every speculation needs at least
  one test for the success path, one for the deopt-and-resume path,
  one for the un-JIT path.

## Rationale and alternatives

**Stay conservative forever.** Costs us 2-10x on hot loops with
arithmetic. The roadmap promises competitiveness with V8/CPython on
those, which is impossible without speculation.

**Speculation without deopt (just bail to interpreter slowly).** Does
not work: the speculative code generates tagged values in registers
that the interpreter cannot consume. We need the live-set rebuild.

**Tracing JIT instead of method JIT.** Far more invasive; would
replace the existing pipeline. Tracing has its own slow paths
(trace explosion, indirect-branch handling). The method-with-deopt
shape is closer to what we have already and lets us iterate.

## Prior art

- V8 has had this since Crankshaft; TurboFan refined it. Their model
  of "lazy deopt" (the deopt happens when the activation is next
  resumed rather than synchronously) is more powerful than what we
  propose here, but eager deopt is enough for our shapes.
- LuaJIT's tracing JIT does deopt via trace-exit guards. Different
  shape, same idea.
- HotSpot's C2 has a richer deopt system with multiple recompile
  reasons; we adopt only the basic categories.

## Unresolved questions

- Should `@no_speculate` also disable IC-based monomorphic speculation
  on `LOAD_GLOBAL`, or only the new arithmetic / shape speculations?
  Leaning toward the latter for back-compat.
- Threshold tuning. 8/1000 is a guess; will tune empirically before
  Tier 1.
- How to surface deopt info in the profiler. Options: a separate
  flame layer, an annotation on the existing flamegraph, a dedicated
  `xs --jit-stats` report. Probably the last.

## Future possibilities

- Speculative inlining (inline the monomorphic call target into the
  caller, deopt if the IC site sees a new target).
- Type-feedback-driven specialization (compile two versions of the
  proto for two common shape pairs).
- OSR (on-stack replacement): JIT a long-running loop without
  needing the function to exit first.

## Impact on stability

The guard / deopt machinery is internal and not part of any tier.
The new flags (`--jit-trace-deopt`, `@no_speculate`) are Tier 2
(opt-in) for one minor cycle, then Tier 1.

Behavioural promise stays the same: a JIT'd proto and a VM-run proto
produce identical observable results. The deopt path strengthens this:
even mid-JIT, the deopt resumes in the VM and the program's observable
behaviour is identical to a pure-VM run.
