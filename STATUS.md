# XS Status

What works, what's partial, and what's planned. For the current
release number, check `git tag` or `xs --version`.

## Bytecode VM

The bytecode VM is the default backend. Bare `xs file.xs` runs on the
VM; pass `--interp` to force the tree-walker, `--jit` to use the
tier-2 JIT. The VM passes the full test suite and is ~4-9x faster
than the tree-walk interpreter on compute-heavy code. Programs that
register `plugin.runtime.after_eval` hooks auto-fall back to the
interpreter.

## Tree-Walk Interpreter

Reserved for debugging and for the handful of plugins that rely on
AST-level runtime hooks. Pass `--interp` to force it.

| Feature | Status |
|---------|--------|
| Variables (let, var, const) | works |
| All data types (int, float, str, bool, null, array, map, tuple, range, re) | works |
| Arithmetic, bitwise, logical operators | works |
| String interpolation, escapes, methods | works |
| Control flow (if/elif/else, for, while, loop, match, break, continue) | works |
| Pattern matching with destructuring, guards, nested patterns | works |
| Functions, closures, default params, variadic, arrow lambdas | works |
| Function overloading (dispatch by arity) | works |
| Tagged blocks (user-defined control structures) | works |
| Reactive bindings (bind) | works |
| Gradual contracts (where clauses) | works |
| Adapt functions (multi-target) | works |
| Inline C blocks (for C transpiler) | works |
| Generators (fn*/yield) | works |
| Structs, impl, traits | works |
| Enums with associated data | works |
| Classes with inheritance | works |
| Variance markers (`<+T>`, `<-T>`) on fn / struct / enum | works |
| Higher-rank `forall<T>` types | works |
| `@scoped` annotations + escape analysis | works |
| `@[macro]` procedural-macro markers | works |
| Algebraic effects (effect/perform/handle/resume) | works |
| Concurrency (spawn, async/await, channels, actors, nurseries) | works |
| Error handling (try/catch/finally, throw, defer) | works |
| Modules and imports | works |
| List/map comprehensions | works |
| Pipe operator | works |
| Gradual typing (--check, --strict) | works |
| Plugin system | works |
| Standard library (37 modules) | works |
| HTTPS client via embedded BearSSL | works |
| Generational refcount GC + concurrent cycle collector | works |
| First-class `Duration` type (`5s`, `100ms`, `1ns`, `2m30s`) | works |
| Temporal primitives (every, after, timeout, debounce) | works |
| Multi-line strings (triple-quote) | works |
| `do` expressions | works |
| `with` resource management | works |
| Named arguments | works |
| Enum methods via impl | works |

Full test run: `tests/test_*.xs` + adversarial suite + examples + CLI drivers all pass on Linux, macOS, and MinGW Windows.

## Bytecode VM (feature matrix)

Use `--vm` flag. Full feature parity with the interpreter (except reactive bindings, which evaluate once).

| Feature | Status |
|---------|--------|
| Arithmetic, variables, functions | works |
| Closures and upvalues | works |
| Control flow (if, while, for, loop, match) | works |
| Labeled break/continue | works |
| Arrays, maps, tuples, ranges | works |
| String interpolation | works |
| Pattern matching (literals, guards, tuples, enums, structs) | works |
| Functions with default params, variadic | works |
| Structs with impl methods, spread | works |
| Classes with inheritance and super | works |
| Traits | works |
| Enums with data and matching | works |
| Concurrency (spawn, channels, actors, async/await, nursery) | works |
| Algebraic effects (perform/handle/resume) | works |
| Error handling (try/catch/finally, throw, defer) | works |
| Modules and imports | works |
| List/map comprehensions | works |
| Pipe operator | works |
| Plugin system (`load`, global.set, add_method) | works |
| All string methods (80+) | works |
| All array methods (50+) | works |
| All map methods (20+) | works |
| Number methods (is_even, digits, to_hex, etc.) | works |
| Result/Option methods (unwrap, is_ok, etc.) | works |
| Optional chaining (?.) | works |
| Range indexing (arr[1..3]) | works |
| All builtins matching interpreter | works |
| Growable stack and frames (no fixed limits) | works |

The VM test (`test_vm.xs`) runs through `--vm` automatically from `tests/run.sh`. Every other test also runs under both backends and the two outputs are diffed; a divergence fails the test even if each backend passes on its own. Use `xs build file.xs` to compile, `xs run file.xsc` to execute.

## JIT Compiler

Opt-in via `--jit`. Single register-allocating tier, x86-64 and
aarch64. Bytecode is lowered to a small linear IR (`src/jit/ra_ir.h`),
basic blocks are split, per-block liveness is computed
(`src/jit/ra_live.c`), a linear-scan allocator (`src/jit/ra_alloc.c`)
maps virtual registers onto three callee-saved regs, and a per-arch
code generator emits native code with SMI fast paths for arithmetic
and compares, an XMM fast path for boxed `XS_FLOAT` binops, an inlined
monomorphic IC for `LOAD_GLOBAL`, inline closure-upvalue access, a
fused compare-and-branch peephole, and a refcount-pair elimination
pass that drops redundant incref/decref around dead produced values.
Recursive calls stay in native code through a small dispatcher
(`tier2_run_until`) that re-enters compiled protos via the
`XSProto.jit_entry` cache.

Supported opcodes (from `op_supported` in `src/jit/ra_lower.c`): the
full bytecode set except generators and `OP_STORE_GLOBAL`-writes of
locals captured by inner closures (shadow-model guard). Anything that
falls outside the subset drops the whole proto back to the bytecode
VM; no template-JIT middle tier.

| Feature | Status |
|---------|--------|
| Register-allocating JIT (x86-64) | works |
| Register-allocating JIT (aarch64) | works |
| SMI fast paths for arithmetic and compares | works |
| XMM fast path for boxed floats | works |
| Inlined monomorphic IC for LOAD_GLOBAL | works |
| Closure upvalue ops in native | works |
| Recursive call re-entry via `jit_entry` | works |
| Refcount-pair peephole | works |
| Control-flow ops (THROW, TAIL_CALL, AWAIT, YIELD, SPAWN, EFFECT_*, DEFER_*) | deopt trampoline |

Observed numbers on a Linux x86-64 box:

| Workload              | `--vm`  | `--jit` | gcc -O2 | node  |
|-----------------------|---------|---------|---------|-------|
| fib(30)               |  210 ms |   20 ms |   <1 ms | 110 ms |
| fib(35)               | 2320 ms |  520 ms |   80 ms | 210 ms |
| 10M-iter `while` sum  |  640 ms |  110 ms |   20 ms | 110 ms |
| 1M-iter `while` sum   |   60 ms |   10 ms |   <1 ms | 110 ms |

The JIT is 5-8x faster than `--vm`, beats Node on every loop, and
matches or beats Node on short recursion; the V8 gap only opens up on
heavy recursion where cross-call inlining pays off. `bash tests/run.sh`
runs every test through `--jit` alongside `--interp` and `--vm` and
diffs the three outputs.

## C Transpiler

`xs --emit c file.xs` generates standalone C that compiles with gcc/clang.

| Feature | Status |
|---------|--------|
| Variables, arithmetic, control flow | works |
| Functions, default params, expression bodies | works |
| Strings, interpolation, string methods | works |
| Arrays, maps, array methods (map/filter/reduce) | works |
| Structs with impl methods | works |
| Enums with constructors and matching | works |
| Pattern matching with guards | works |
| Channels, actors, spawn, nursery | works |
| Async/await (sequential) | works |
| Closures capturing mutable state | works |
| Generators | not yet |
| Algebraic effects | not yet |
| Plugins | not supported (requires runtime) |

## JavaScript Transpiler

`xs --emit js file.xs` generates Node.js-compatible JavaScript.

| Feature | Status |
|---------|--------|
| Variables, functions, control flow | works |
| Closures, arrow lambdas | works |
| Arrays, maps | works |
| Concurrency | partial |
| Algebraic effects (perform/handle) | works: handle body lowers to `function*()` and `yield*` delegates through nested performs |
| Everything else | rough |

## WebAssembly

The path to running xs in a browser is the runtime build, not the
AOT transpiler.

| Build | What it gives you | Where |
|-------|-------------------|-------|
| `make wasm` | full runtime as `xs.wasm` (~1.4 MB), wasi target. Same feature set as the native binary. | release artefact |
| `make wasm-browser` | stripped runtime as `xs-browser.wasm` (~600 kB after wasm-opt). TLS / x509 / RSA / EC / http_server / pkg / lint / doc / coverage are dropped, hashes / mac / kdf / symcipher kept. | release artefact, hosted at `static.xslang.org/xs.wasm` |

The browser SDK at `static.xslang.org/xs.js` wraps `xs-browser.wasm`
with a virtual filesystem, captured stdout/stderr, and a `loadXS()` /
`xs.run()` / `xs.exec()` API. Releases publish both artefacts, and the
static repo's daily sync workflow picks up the browser build.

`xs --emit wasm` only handles arithmetic and direct calls. The
runtime build covers the browser case; `--emit c` covers AOT.

## Tooling

| Tool | Status |
|------|--------|
| REPL with syntax highlighting | works |
| LSP server (hover, completion, diagnostics, definition, references, rename, formatting, signature help) | works |
| DAP debugger (breakpoints, stepping, variable inspection, evaluate) | works |
| VSCode extension | works: available on marketplace |
| Formatter (`xs fmt`) | works |
| Linter (`xs lint`) | works |
| Test runner (`xs test`) | works |
| Benchmarks (`xs bench`) | works |
| Execution tracer (`xs --record`, `xs replay`) | works |
| Profiler (`xs profile`) | works |
| Coverage (`xs coverage`) | works |
| Doc generator (`xs doc`) | works |
| Package manager (`xs install/remove/update`) | works: git + local + reg.xslang.org via `xs install <name>`, `xs search`, `xs publish` (with `XS_REGISTRY_TOKEN`) |

## Platform Support

CI runs the full 7-layer suite on every commit across each of these.
libFuzzer runs the parser fuzz harness on a short budget, and the
WASM job cross-compiles with wasi-sdk and runs the conformance and
regression layers under wasmtime.

| Platform | Status |
|----------|--------|
| Linux (x86-64) | builds and tests pass; ASan + UBSan also clean |
| macOS (aarch64) | builds and tests pass |
| Windows (MinGW, x86-64) | builds and tests pass, statically linked (`-static` in Makefile) |
| WASM (wasi-sdk 25) | conformance + regression layers pass under wasmtime 25 |
| iOS (arm64 device + x86_64 sim) | `make ios` produces `xs-ios.a` static archive (no JIT, App Store policy) |
| Android (arm64-v8a, armeabi-v7a, x86_64) | `make android` via NDK r25+ produces `libxs.so` per ABI |
| ESP32 (xtensa) | `make esp32` produces `libxs.a` for an ESP-IDF component (VM-only build) |
| Raspberry Pi (aarch64 Linux) | `make release CC=aarch64-linux-gnu-gcc` full feature set including JIT |

## Standard Library

37 modules are registered at interpreter startup (`stdlib_register` in `src/runtime/builtins.c`):
`math`, `time`, `io`, `string`, `path`, `base64`, `hash`, `uuid`, `collections`, `process`,
`random`, `os`, `json`, `log`, `fmt`, `test`, `csv`, `url`, `re`, `msgpack`, `Promise`,
`async`, `net`, `crypto`, `thread`, `buf`, `encode`, `db`, `cli`, `ffi`, `reflect`, `gc`,
`reactive`, `toml`, `http`, `fs`, `tracing`.

## Known Footguns

- Interpreter has a 500-frame call-depth cap (`XS_MAX_DEPTH` overrides).
  Hitting it throws `StackOverflow`; switch to the VM if you're
  recursing deep.
- `{` inside `"..."` is interpolation. For literal braces use a
  backtick raw string: `` `{"a": 1}` ``. `{{` collapses to one `{`
  but the contents are still parsed as an expression.
- Regex is POSIX extended. No `\d`, `\w`, lookaround, backrefs.
- `.upper()` / `.lower()` are ASCII-only. Strings are byte-indexed;
  multi-byte UTF-8 round-trips through everything that doesn't need
  case mapping.
- `--emit wasm` only handles arithmetic and direct calls. Use
  `--emit c` for AOT, or the runtime build (`xs-browser.wasm`) for
  the browser.
- Effects on the JS target lower through generator delegation
  (`yield*`). `perform` outside a `handle` is a parse error.
  Effects on the C target are not implemented.
- `XS_GC_CONCURRENT=1` moves GC sweep onto a worker thread; off by
  default.
- JIT bails on actor methods (and any proto that builds one). The
  fallback to VM is automatic. x86-64 and aarch64 only.
- JIT and VM disagree on a few `--jit` corner cases listed in the
  test suite as documented carryovers; pass `--vm` if you hit a
  silent miscompile.

## Known Limitations

- VM effects: multi-shot resume captures the live stack at perform
  time. Mutable heap state (arrays, maps, closures) is shared by
  reference between resumes, not deep-cloned. Matches the "values
  capture, references share" rule in `LANGUAGE.md`.
- HTTPS server uses BearSSL termination. Call
  `http_server_use_tls(server, cert_pem, key_pem)` before
  `http_server_start`. SSE / WebSocket helpers go through the same
  bridge.
- `http.serve(port, ...)` takes either a single handler or a
  router map with `routes`, `middleware`, `not_found`. Patterns
  support `:name` captures and trailing `*`.
- Registry CLI (`xs install`, `publish`, `search`, `whoami`,
  `login`, `logout`) talks to `https://reg.xslang.org`. Windows
  goes through a raw-socket + BearSSL client to avoid MinGW pulling
  in the full async stack.
- HTTPS client throws `HttpError` on connect / TLS / parse failure
  rather than returning a sentinel. Wrap in `try/catch` if you want
  to recover.
