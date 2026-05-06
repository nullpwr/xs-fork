# Transpiling to JS / C / WASM

`xs --emit <target>` writes equivalent code in another language.
Useful for shipping XS to runtimes you can't run the interpreter in.

| target     | what you get                                    |
|------------|-------------------------------------------------|
| `js`       | a single JS file with no runtime dependency     |
| `c`        | C99 source compileable with any C compiler      |
| `wasm`     | a `.wasm` binary using WASI imports             |
| `ast`      | the parsed AST as JSON (debugging)              |
| `bytecode` | the compiled `.xsc` bytecode                    |
| `ir`       | tier-2 IR (debugging the JIT)                   |

```sh
xs --emit js   hello.xs > hello.js
xs --emit c    hello.xs > hello.c
xs --emit wasm hello.xs           # writes out.wasm
```

## JavaScript

The JS target produces idiomatic ES2020. Closures map to functions,
arrays to arrays, maps to `Map`. The bundle includes a small runtime
for things JS doesn't have (XS-style pattern matching, effect
handlers).

```sh
xs --emit js mathlib.xs > mathlib.js
node mathlib.js
```

For browsers, pair with the
[Browser via WebAssembly](./browser.md) story instead; running the
wasm interpreter is closer to native XS semantics and the runtime is
smaller.

## C

The C target produces standalone C99 (no XS runtime needed). Useful
for embedding XS-derived logic into a C codebase, or for Ahead-Of-
Time builds.

```sh
xs --emit c calculator.xs > calc.c
gcc -O2 calc.c -o calc
./calc
```

What works: structs / enums / classes, pattern matching, closures
that don't capture mutable upvalues, try / catch / throw / defer,
arithmetic, the bytecode-VM-equivalent surface for non-effecting
code.

What doesn't:

- Effects (`perform` / `resume`): emits a runtime error. Implementing
  proper resume requires delimited continuations on top of longjmp;
  tracked for v1.3.
- Closures that capture mutable locals: the C target boxes immutable
  upvalues but doesn't track mutation through a captured `var` yet.
- Generators (`fn*`): not lowered. Use the interp / VM / JS targets.

For programs that need the full XS surface, embed the interpreter
via `xs_embed.h` instead of going through `--emit c`.

## WASM

The WASM backend emits a `.wasm` module with WASI imports for I/O.
Object model is map-based (every struct is a hash table), so it's
slower than native but portable.

```sh
xs --emit wasm hello.xs        # out.wasm
wasmtime out.wasm
```

What works: arithmetic and direct calls, full match expressions
(including guards), try / catch, struct field access with proper
nominal typing, large-float printing, tuple swap, divide-by-zero,
the format methods.

Known gaps in the AOT path:

- Effects: single-shot resume only (multi-shot bails to the
  interp's eff_stack semantics, which the AOT can't model).
- Spawn / channels / nurseries: lower to single-thread sequencing
  (WASI doesn't grant real threads to a freestanding module).
- IEEE 754 display precision: no full grisu / dragon4 in the
  generated module, so a handful of float prints differ from interp
  by the last digit.

For full XS semantics in the browser, ship `xs.wasm` (the
interpreter compiled to WASI) and run XS source through it via the
[browser SDK](./browser.md). The transpiler is for when you need a
self-contained `.wasm` of just your program.
