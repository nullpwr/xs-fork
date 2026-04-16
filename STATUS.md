# XS Status

What works, what's partial, and what's planned. For the current
release number, check `git tag` or `xs --version`.

## Bytecode VM

The bytecode VM backend is opt-in via `--vm`. It passes the full test
suite and is ~4-9x faster than the tree-walk interpreter on compute-
heavy code. Programs that register `plugin.runtime.after_eval` hooks
auto-fall back to the interpreter. The interpreter is still the default
for bare `xs file.xs`; see the Known Footguns section for why.

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
| Universal literals (duration, color, date, size, angle) | works |
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
| Closures capturing mutable state | partial: works for single-scope files |
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

## WebAssembly Transpiler

`xs --emit wasm file.xs`: early stage.

| Feature | Status |
|---------|--------|
| Basic arithmetic, function calls | works |
| Everything else | not yet |

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
| Package manager (`xs install/remove/update`) | basic: hosted registry endpoint is live, client HTTP wiring still pending |

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

These are the sharp edges you are most likely to hit. They are here on
purpose: the more users trip over silently, the more trust the project
burns. Fix one, and this list gets shorter.

- **Interpreter is the default, VM is faster.** `xs file.xs` uses the
  tree-walk interpreter, which is ~10x slower on recursion than the
  bytecode VM and more conservative about call depth (500 frames,
  tunable via `XS_MAX_DEPTH`). For anything compute-heavy or
  long-running, pass `--vm`. Pre-v1.0, we will not flip the default.
- **Effect handlers on the JS target work via generator delegation.**
  The transpiler wraps the handled expression in a `function*()` and
  uses `yield*` to forward through any nested generators, so
  `perform` lowers to a plain `yield` and resumes correctly. Direct
  performs in the `handle` body and performs from helper functions
  both round-trip; the prior parse error is gone. Effects in code
  paths the C target reaches (the runtime preamble doesn't model
  continuations) are still VM-only.
- **WASM backend only runs trivial programs.** Arithmetic and direct
  function calls are fine; anything touching GC, strings, closures,
  async, or effects does not yet work. Do not ship.
- **Cycle collector is opt-in for concurrent mode.** The default GC
  catches reference cycles synchronously (CPython-style trial deletion)
  on a generational schedule. For workloads where the multi-ms free
  walk dominates pause time, set `XS_GC_CONCURRENT=1` to move the
  sweep onto a worker thread; mark stays inline because it's already
  fast. Pause-time SLO documented at the top of `src/core/gc_concurrent.h`.
- **`{` inside double-quoted strings is interpolation.** `"a {x} b"`
  evaluates `x`. To pass a literal `{` (e.g. a JSON blob), use a
  backtick raw string: `` `{"a": 1}` ``. The interpolation grammar
  doesn't have an escape sequence for a single `{`; `{{` collapses
  to one but the inner content is still parsed as an expression.
- **Regex is POSIX-extended, not PCRE.** No `\d`, `\w`, lookaround, or
  backreferences. Use `[0-9]`, `[a-zA-Z_]`, etc.
- **HTTPS server uses BearSSL termination.** Pass a PEM cert + key
  to `http_server_use_tls(server, "cert.pem", "key.pem")` before
  calling `http_server_start`, and the listener attaches a per-
  connection BearSSL engine that handles the handshake + record
  framing transparently. Plain HTTP listeners pay zero cost: the
  conn_recv / conn_send bridge dispatches through the engine only
  when `tls_state` is non-null. SSE and the WebSocket helpers
  (`sse_send_event`, `ws_send_frame`) still take a raw fd and so
  bypass TLS for now; threading them through HTTPConnection is the
  remaining piece.
- **Unicode is byte-oriented.** `.len()`, `.slice()`, indexing all work
  on bytes. Multi-byte UTF-8 sequences round-trip correctly, but
  `.upper()`/`.lower()` are ASCII-only and grapheme-aware operations
  are not implemented.
- **Package registry endpoint is live, the CLI client isn't wired
  yet.** The hosted registry runs at `reg.xslang.org` and is ready to
  accept publishes / lookups, but `xs publish` and `xs search` in v0.7.x
  still write a local tarball and print `no registry configured`
  respectively, because `src/pkg/pkg.c` doesn't make HTTP calls. `xsi`
  can install from local paths today. Wiring the CLI to talk to the
  endpoint is on the v0.9.0 list.
- **JIT is opcode-subsetted.** The register-allocating pipeline
  handles the subset listed in the JIT Compiler section above: basic
  arithmetic, compares, loads/stores, branches, returns, and direct
  calls. Anything outside the subset (generators, capturing writes to
  locals shadowed into inner closures) falls back to the bytecode VM
  for that proto. Tight arithmetic/branch loops get 5-8x over VM;
  call-heavy workloads sit closer to VM parity until call-site fast
  paths land. Both x86-64 and aarch64 are supported.

## Known Limitations

- Struct operator overloading only works when both operands are structs (not mixed struct+int)
- C transpiler closures break when the same variable name is captured in multiple functions in one file
- JIT lowers a fixed opcode subset (see the JIT Compiler section
  above); anything else runs on the bytecode VM for that proto.
- WASM transpiler only handles basic programs
- `xs publish` and `xs search` don't yet talk to `reg.xslang.org`; the registry endpoint is live but the CLI HTTP client wiring is pending
- VM effects use snapshot/restore (single-shot only, no nested effects)
- VM actors use flattened state (not full closure capture like the interpreter)
- Regex uses POSIX extended syntax only (no `\d`, `\w` shorthand, use `[0-9]`, `[a-zA-Z_]`)
- Interpreter call-depth cap is 500 frames (raise with `XS_MAX_DEPTH=N`). Hitting it throws a catchable `StackOverflow` rather than segfaulting; the VM has its own growable stack.
- `match` does not support map patterns yet: destructure tuples, arrays, structs, and enums, but build a struct wrapper if you need to match map-shaped data.
- JS transpiler effect handlers wrap the handled expression in a generator and use `yield*` delegation, so a `perform` lowers to `yield` cleanly when the handle body itself yields. Direct top-level `perform` outside a `handle` still has no surrounding generator and will be a parse error under Node, mirroring the language rule that `perform` only makes sense in a handled context.
- `http` module exposes client methods (`get`, `post`, ...) on a
  shared keep-alive pool, plus `http.serve(port, handler)` for a
  small request loop. The richer async router in
  `src/net/http_server.c` (idle / slow-request culling, graceful
  shutdown, per-server limits) is reachable from C hosts but is
  not yet exposed through the XS-side `http` module surface; that
  rewire is pending.
- TLS server termination uses BearSSL via
  `http_server_use_tls(server, cert_pem, key_pem)`. SSE and WebSocket
  helpers still take a raw fd, so streaming endpoints over HTTPS
  need to be threaded through the engine in a follow-up.
