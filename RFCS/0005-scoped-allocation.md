---
rfc: 0005
title: Scoped allocation, a no-GC subset
author: xs-lang0
status: draft
created: 2026-05-08
---

# 0005: Scoped allocation, a no-GC subset

## Summary

Add a `@scoped` annotation that pins data to a stack frame, freed
deterministically at scope exit. No collection cycle, no write
barrier, no allocator pressure on the GC. Opt-in, lexically bounded,
escape-checked at compile time. Designed to make XS workable for
real-time audio, low-latency networking, and tight loops where GC
pauses are unacceptable.

## Motivation

The book's mobile chapter says the quiet part:

> Real-time audio / video where pause times must be bounded ... keep
> using Rust.

That is honest but ungenerous. The cases where the GC actually hurts
are narrow: tight loops in hot code paths, latency-critical audio
callbacks, embedded targets with no spare RAM. Most code in those
programs is fine on the GC; only the inner loops need
deterministic timing.

This RFC adds an opt-in lifetime-bounded allocation form so a 50-line
audio callback can be GC-free without rewriting the rest of the
program in another language.

It composes with RFC 0004 (concurrent GC): code that opts into
`@scoped` will not produce GC garbage, so the concurrent collector's
write barriers do not fire there. Together they cover both ends of
the latency spectrum.

## Guide-level explanation

```xs
@scoped
fn process_audio(input: [f32], output: [f32]) {
    let buf: scoped [f32; 256] = scoped.new_array(256, 0.0)
    for i in 0..256 {
        buf[i] = filter(input[i])
    }
    output.copy_from(buf)
}    -- buf freed here
```

Two new pieces:

- `@scoped` on a function: this function may not allocate to the GC
  heap. The compiler errors on any non-scoped allocation.
- `scoped` modifier on a type: the value lives in a stack-bounded
  arena that is reset on function return.

Inside a `@scoped` function:

- `scoped.new_array`, `scoped.new_map`, `scoped.box(value)`: arena
  allocations. Returned values carry the `scoped` modifier.
- Closures that capture *any* scoped value cannot escape the
  function. The checker errors with `T0103: scoped value escapes
  via closure`.
- Calling a non-`@scoped` function is allowed if you do not pass it
  any scoped values.

Outside a `@scoped` function: nothing changes. You can call a
`@scoped` function from regular code; you just cannot return scoped
values from it.

## Reference-level explanation

### The `scoped` modifier

A type `scoped T` is a value with a *region label*. Region labels
are not first-class types; they are a piece of metadata the type
checker uses to enforce containment. They erase before bytecode.

Allowed values for the modifier in this RFC:

- `scoped` (no explicit label): the innermost enclosing `@scoped`
  scope.
- (Future) named regions: `scoped'a`, useful for cross-call sharing.
  Not in this RFC.

The compiler synthesizes a fresh region for every `@scoped` function
entry. All `scoped.*` allocations inside that function attach to it.

### The `@scoped` function annotation

A `@scoped` function has these constraints:

1. Cannot call a non-`@scoped` function with a `scoped` argument
   (escape).
2. Cannot return a `scoped` value (escape).
3. Cannot store a `scoped` value into a non-scoped data structure
   (escape).
4. Cannot allocate to the GC heap. (Catches `[1, 2, 3]` literals,
   `#{}` literals, `new ClassName()`, etc. Errors at the literal
   site with a suggestion to use `scoped.new_array(...)`.)

Constraint #4 is enforced even for *transitively* called functions:
if `f` is `@scoped`, every function `f` calls must also be `@scoped`
or marked `@gc_free` (a weaker promise, see below).

### `@gc_free`

A function marked `@gc_free` is permitted to be called from `@scoped`
context but does not get a region of its own. It is purely a promise:
"I do not allocate." Common examples: `math.sin`, `string.len`,
arithmetic helpers.

Stdlib gets `@gc_free` annotations on the obvious ones; user code can
add the annotation explicitly.

### The arena

Each `@scoped` function entry pushes a *region frame* onto a
per-thread region stack. The frame holds a bump pointer into a
preallocated arena (8 KB initial, doubled on overflow to a per-thread
maximum of 256 KB). On function exit the frame pops; the arena's
high-water mark reverts.

If allocation overflows the per-thread max, the runtime panics with
`scoped overflow`. Tunable via `XS_SCOPED_MAX_PER_THREAD` (default
256 KB).

Allocations are not zeroed by default; `scoped.new_array(n, x)`
fills with `x`. There is a `scoped.new_uninit_array(n)` for
performance-critical code; use of uninit values is undefined
behaviour and the compiler warns on apparent uses.

### Escape analysis

Implemented in the existing sema pass. The checker tracks, for every
value, the region label it carries. Operations that would let a
scoped value outlive its region produce a T0103 error:

```text
error[T0103]: scoped value escapes its region
  --> src/audio.xs:14:12
   |
14 |     return buf
   |            ^^^ this value is scoped to function `process_audio`
```

Existing escape-analysis infrastructure (used today for closure
upvalue lifetimes) is the foundation; the region-label tracking is a
small extension.

### Interaction with closures

A closure that captures a `scoped` value is itself scoped: its type
is `scoped fn(...) -> ...`. It cannot leave the region.

```xs
@scoped
fn audio_loop(input) {
    let state: scoped [f32; 4] = scoped.new_array(4, 0.0)

    let step = |x| {
        state[0] = state[0] * 0.9 + x       -- captures `state`
    }

    for sample in input { step(sample) }     -- ok, both same region

    return step                              -- error: scoped escape
}
```

### Interaction with `actor`s and channels

Sending a `scoped` value over a channel or to an actor is an escape
error. The diagnostic suggests either copying the value
(`.to_owned()`, which allocates on the GC heap) or restructuring the
caller to consume the value within the region.

### JIT and bytecode

The JIT and VM both emit a single bump-pointer increment for
`scoped.new_*`. The frame push and pop are one each at function entry
and exit. No write barrier (the concurrent collector ignores the
arena pages; they are not part of the GC heap).

A region overflow path falls through to the slow allocator path
(arena chunk-list expansion), which is one branch per allocation.

## Drawbacks

- Two new annotations on the surface (`@scoped`, `@gc_free`). Adds to
  the things-to-learn list.
- Stdlib retrofit. Every "obvious" pure function needs `@gc_free`
  before it can be called from `@scoped` code; this is hundreds of
  functions. Mechanical work but real.
- Escape-analysis errors are subtle. We will need a dedicated chapter
  in the book and a thorough `xs --explain T0103` entry.
- Composes with concurrent GC, but does not eliminate the need for
  it. Programs will still have a GC heap; this RFC just gives the
  hot path a way out.

## Rationale and alternatives

**Region inference (no annotations).** Tempting but undecidable in
the general case. Cyclone tried this; the inference is fragile. We
follow the modern Rust precedent of explicit annotations at the
function boundary.

**Linear types instead.** Far more invasive; reshapes the type
system. Out of scope.

**`unsafe` arena pointer like Rust's `Bump`.** Possible as a
library, but it cannot enforce the escape constraint at the type
level, so it gives up the property that matters most.

**Just write that part in C and FFI.** This is the existing
workaround. The whole point of the RFC is to make XS competitive on
those workloads without forcing the user to leave the language.

## Prior art

- Cyclone's region system: closest in spirit. Their main lesson:
  inference is hard, annotations are the realistic shape.
- Rust's lifetimes: more general (lifetimes are first-class types),
  more painful to use. We take only the lexical-region subset.
- Java's `Arena` from the foreign-memory API: runtime-level only;
  no compile-time escape check. We do better.
- D's `@nogc` annotation: very close to what we propose for
  `@gc_free`. The full scoped-region story is what D lacks.

## Unresolved questions

- Should `@scoped` functions be allowed to recurse? Each recursive
  call would push another region frame, fine for arena depth but
  potentially confusing for users. Inclined: yes, allow, document
  carefully.
- Polymorphism over regions: should we support `for<'r> fn(scoped'r
  T) -> ...`? Useful for higher-order combinators in scoped code.
  Probably yes, but follow-up RFC. Today the unnamed `scoped` is the
  only label.
- Arena per-thread vs per-actor. We default to per-thread; per-actor
  would be cleaner for the actor concurrency model but has subtle
  ownership issues when an actor migrates threads (which it can
  today, on supervisor restart).

## Future possibilities

- Named regions for cross-call lifetime sharing.
- `scoped` standard library: `scoped.collections`, `scoped.io`
  scratch buffers, `scoped.alloca` for sizing knowable at runtime.
- Region-polymorphic functions for true zero-allocation generic
  code.

## Impact on stability

Pure addition. Existing programs continue to work; no semantics
change.

The `@scoped` annotation, the `scoped` type modifier, and
`scoped.new_*` are Tier 2 (`@unstable`) for at least two minor
cycles. Promotion to Tier 1 is gated on:

- Stable diagnostic codes (T0103 onward) and matching `--explain`
  entries.
- Stdlib audit: every `@gc_free` annotation has been verified.
- The book has a dedicated chapter on scoped programming with
  worked examples (audio callback, low-latency RPC, embedded
  control loop).
- At least one real-world adopter has shipped a non-trivial program
  using `@scoped` and reported back.
