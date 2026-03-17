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
[Browser via WebAssembly](./browser.md) story instead — running the
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

Limitations: no closures over local variables (yet), no GC
integration, generators not supported. For full XS semantics, embed
the interpreter via `xs_embed.h` instead.

## WASM

The WASM backend emits a `.wasm` module with WASI imports for I/O.
Object model is map-based (every struct is a hash table), so it's
slower than native but portable.

```sh
xs --emit wasm hello.xs        # out.wasm
wasmtime out.wasm
```

For full XS semantics in the browser, ship `xs.wasm` (the
interpreter compiled to WASI) and run XS source through it via the
[browser SDK](./browser.md). The transpiler is for when you need a
self-contained `.wasm` of just your program.
