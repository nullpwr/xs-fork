# Introduction

XS is a single language for everything that runs on a computer. It's
gradually-typed, ships with both struct/trait and class/inheritance
object models, supports every common form of concurrency, and
transpiles to JavaScript, C, and WebAssembly so you can deploy it
anywhere.

The pitch:

- replace **Python** for scripts and small services
- replace **Rust** for systems-flavoured tools
- replace **TypeScript** for browser code
- replace **Go** for HTTP services

All with one language that scales from 5-line scripts to 50K-line
systems. Features compose; the syntax holds together; the runtime is
small enough to embed.

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
`xs doc` or [language reference](./tour.md). For the brand-new tour,
start with [Getting started](./getting_started.md).

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
