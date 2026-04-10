# Introduction

XS is one language for the whole stack. It runs as a script, builds
to a single static binary, transpiles to JavaScript, C, or WASM, and
embeds in C as a ~2 MB library. Same syntax, same stdlib, same
debugger across all of them.

The pitch:

- write **scripts** the way you would in Python
- write **systems tools** the way you would in Rust, minus the borrow checker
- write **browser code** the way you would in TypeScript
- write **HTTP services** the way you would in Go

One language that scales from a 5-line script to a 50K-line system.
Types are optional but enforced when you write them; concurrency
covers spawn, async/await, channels, actors, and nurseries; effects
sit alongside try/catch when you want them. The runtime is small
enough to drop into a `<script>` tag or an ESP32.

```xs
fn fizzbuzz(n: int) {
    for i in 1..=n {
        match (i % 3, i % 5) {
            (0, 0) -> println("FizzBuzz"),
            (0, _) -> println("Fizz"),
            (_, 0) -> println("Buzz"),
            (_, _) -> println(i),
        }
    }
}

fizzbuzz(15)
```

## What you'll find here

This book is the long-form reference. For the API surface, see
`xs doc` or the [language tour](./tour.md). For installation and a
first program, start with [Getting started](./getting_started.md).
For the day-to-day shape of the toolchain, see
[The xs command](./cli.md).

## What this book isn't

- It is *not* the language specification. The grammar lives in
  `xsypy/src/core/parser.c` and the semantic rules in `semantic/`.
- It is *not* a marketing brochure. We'll be honest about rough edges
  (the JIT does not deoptimise, the GC isn't concurrent yet, the WASM
  backend has a map-backed object model). See
  [policy](./policy.md) for what we promise to keep stable.
- It is *not* exhaustive. Some niche stdlib modules (`db`, `ffi`,
  `reactive`) get one section each rather than a chapter.

## Conventions

- Code that runs as-is is in `xs` blocks; you can paste it into a
  `.xs` file and run with `xs <file>`.
- Output appears in `text` blocks under the running snippet.
- "Tier 1" and "Tier 2" refer to the [stability tiers](./policy.md);
  Tier 1 is what you can rely on, Tier 2 is `@unstable` opt-in.
