# XS Status

What works, what's partial, and what's planned. Current release: v0.5.0.

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
| Algebraic effects (effect/perform/handle/resume) | works |
| Concurrency (spawn, async/await, channels, actors, nurseries) | works |
| Error handling (try/catch/finally, throw, defer) | works |
| Modules and imports | works |
| List/map comprehensions | works |
| Pipe operator | works |
| Gradual typing (--check, --strict) | works |
| Plugin system | works |
| Standard library (36 modules) | works |
| HTTPS via embedded BearSSL | works |
| Universal literals (duration, color, date, size, angle) | works |
| Temporal primitives (every, after, timeout, debounce) | works |

| Multi-line strings (triple-quote) | works |
| `do` expressions | works |
| `with` resource management | works |
| Named arguments | works |
| Enum methods via impl | works |

Full test run: 40 test_*.xs files + examples sweep + test_cli.sh all pass on Linux, macOS, and MinGW Windows.

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
| Plugin system (use plugin, global.set, add_method) | works |
| All string methods (80+) | works |
| All array methods (50+) | works |
| All map methods (20+) | works |
| Number methods (is_even, digits, to_hex, etc.) | works |
| Result/Option methods (unwrap, is_ok, etc.) | works |
| Optional chaining (?.) | works |
| Range indexing (arr[1..3]) | works |
| All builtins matching interpreter | works |

| Growable stack and frames (no fixed limits) | works |

The VM test (`test_vm.xs`) is run through `--vm` automatically by `tests/run.sh`. Use `xs build file.xs` to compile, `xs run file.xsc` to execute.

## JIT Compiler

x86-64 only. Two tiers, both opt-in via `--jit`:

**Tier 1** is a template/copy-patch JIT over the bytecode dispatch: it
emits a specialised version of the VM inner loop, but every opcode still
fans out to a helper call. This is what you get for anything the newer
tier 2 can't handle.

**Tier 2** is a register-allocating specialiser: bytecode is lowered to
a small linear IR (`src/jit/ra_ir.h`), basic blocks are split, per-block
liveness is computed (`src/jit/ra_live.c`), a linear-scan allocator
(`src/jit/ra_alloc.c`) maps virtual registers onto `rbx`, `r14`, and
`r15`, and `src/jit/ra_codegen.c` emits x86-64 with SMI fast paths for
arithmetic/compares, register-resident locals (no stack chase for
`LOAD_LOCAL` on read-only slots), and inline truthy/decref in the
conditional-branch path. Recursive calls stay in native code through
a small dispatcher (`tier2_run_until`) that re-enters compiled inner
protos via the new `XSProto.jit_entry` cache.

Tier 2 supports a strict subset of opcodes; a proto that steps outside
it transparently falls back to tier 1. The current set (from
`op_supported` in `src/jit/ra_lower.c`):

```
NOP PUSH_CONST PUSH_NULL PUSH_TRUE PUSH_FALSE POP DUP
LOAD_LOCAL STORE_LOCAL LOAD_GLOBAL
ADD SUB MUL LT GT LTE GTE EQ NEQ
JUMP LOOP JUMP_IF_FALSE JUMP_IF_TRUE
RETURN CALL
```

Anything else (string concat, division, map/array ops, MAKE_CLOSURE,
etc.) lands in tier 1 or the interpreter.

| Feature | Status |
|---------|--------|
| Tier-1 template JIT (x86-64) | works |
| Tier-2 IR + linear-scan allocator | works |
| SMI fast paths for arithmetic and compares | works |
| Tier-2 recursive calls via `jit_entry` cache | works |
| Opcodes outside the supported subset | fall back to tier 1 |
| ARM64 backend | stub only (`src/jit/arm64.c`) |

Environment knobs:

- `XS_JIT_TIER2=0` / `n` / `N` disables tier 2 and forces everything
  through tier 1. Defaults on.
- `XS_JIT_TIER2_DUMP=1` writes the emitted tier-2 code buffer to
  `/tmp/tier2.bin` and logs `[tier2] <nbytes>B at <addr> (proto=... )`
  for each successful compile. Useful for `objdump -D -b binary -m
  i386:x86-64 /tmp/tier2.bin`.

Tier 2 was added recently; observed numbers on a Linux x86-64 box:

| Workload | VM | `--jit` tier 1 | `--jit` tier 2 |
|----------|-----|----------------|-----------------|
| 10M-iter `while` sum | 0.395s | 0.231s | 0.064s |
| `bench_fibonacci.xs` | 1.00x | ~0.92x | ~0.92x |

Tight arithmetic/branch loops see a 3-6x win over VM; call-bound
workloads like fib are currently call-dispatch-limited and sit roughly
at parity (there is follow-up work to land call fast paths). All 47
test files pass with tier 2 enabled (`bash tests/run.sh`), and all 7
test layers pass via `bash tests/run-all.sh`.

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
| Algebraic effects (perform/handle) | broken: emits `yield` inside a non-generator arrow, fails to parse in Node |
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
| Package manager (`xs install/remove/update`) | basic: registry not live |

## Platform Support

| Platform | Status |
|----------|--------|
| Linux (x86-64) | fully tested |
| macOS (x86-64, ARM) | builds and tests pass |
| Windows (MinGW) | builds and tests pass, statically linked (`-static` in Makefile) |

## Standard Library

36 modules are registered at interpreter startup (`stdlib_register` in `src/runtime/builtins.c`):
`math`, `time`, `io`, `string`, `path`, `base64`, `hash`, `uuid`, `collections`, `process`,
`random`, `os`, `json`, `log`, `fmt`, `test`, `csv`, `url`, `re`, `msgpack`, `Promise`,
`async`, `net`, `crypto`, `thread`, `buf`, `encode`, `db`, `cli`, `ffi`, `reflect`, `gc`,
`reactive`, `toml`, `http`, `fs`.

## Known Footguns

These are the sharp edges you are most likely to hit. They are here on
purpose: the more users trip over silently, the more trust the project
burns. Fix one, and this list gets shorter.

- **Interpreter is the default, VM is faster.** `xs file.xs` uses the
  tree-walk interpreter, which is ~10x slower on recursion than the
  bytecode VM and more conservative about call depth (500 frames,
  tunable via `XS_MAX_DEPTH`). For anything compute-heavy or
  long-running, pass `--vm`. Pre-v1.0, we will not flip the default.
- **Effect handlers are broken on the JS target.** The transpiler
  wraps the `handle` body in a `function*()` IIFE but the statements
  inside get nested inside a plain arrow `(() => { ... })()`, so the
  emitted `yield { __effect: ... }` sits in a non-generator and the
  output fails to parse under Node (`SyntaxError: Unexpected token
  '{'`). This applies to direct performs in the `handle` body too, not
  just helper-function performs. Do not rely on effects on the JS
  target; use `--vm` or the interpreter until the handle lowering is
  reworked.
- **WASM backend only runs trivial programs.** Arithmetic and direct
  function calls are fine; anything touching GC, strings, closures,
  async, or effects does not yet work. Do not ship.
- **Circular references leak.** The GC is refcount-only; a cycle between
  two arrays or closures never frees. Avoid long-lived mutual references,
  or break the cycle by hand before dropping the last external ref. A
  cycle collector is on the roadmap.
- **Regex is POSIX-extended, not PCRE.** No `\d`, `\w`, lookaround, or
  backreferences. Use `[0-9]`, `[a-zA-Z_]`, etc.
- **`http.serve` is minimal.** There is a working HTTP/1.1 server in
  `src/runtime/builtins.c` (`native_http_serve`) that threads out each
  accepted connection and releases the GIL around socket I/O, so slow
  handlers don't block subsequent connects. The richer async router
  scaffolding in `src/net/http_server.c` is not wired up. Fine for
  demos and internal tools; not a production server.
- **Unicode is byte-oriented.** `.len()`, `.slice()`, indexing all work
  on bytes. Multi-byte UTF-8 sequences round-trip correctly, but
  `.upper()`/`.lower()` are ASCII-only and grapheme-aware operations
  are not implemented.
- **Package registry is a stub.** `xs publish` / `xs search` print
  "no registry configured" unless `[registry]` is set in `xs.toml`.
  `xsi` can install from local paths today; the hosted registry at
  registry.xslang.org is not live yet.
- **JIT is x86-64 only and opcode-subsetted.** Tier 2 (the register-
  allocating pipeline) handles the subset listed in the JIT Compiler
  section above -- basic arithmetic, compares, loads/stores, branches,
  returns, and direct calls. Anything outside the subset lands in
  tier 1 or the interpreter. Tight arithmetic/branch loops get a
  3-6x wall-clock win over VM; call-heavy workloads sit near VM
  parity until the planned call-site fast paths land. ARM64 is a
  stub.

## Known Limitations

- Struct operator overloading only works when both operands are structs (not mixed struct+int)
- C transpiler closures break when the same variable name is captured in multiple functions in one file
- JIT is x86-64 only; ARM64 is a stub. Tier 2 lowers a fixed subset of
  opcodes (see the JIT Compiler section); anything else falls back to
  tier 1 or the interpreter.
- WASM transpiler only handles basic programs
- `xs publish` and `xs search` print "no registry configured" unless `[registry]` is set in `xs.toml`
- VM effects use snapshot/restore (single-shot only, no nested effects)
- VM actors use flattened state (not full closure capture like the interpreter)
- Regex uses POSIX extended syntax only (no `\d`, `\w` shorthand, use `[0-9]`, `[a-zA-Z_]`)
- Interpreter call-depth cap is 500 frames (raise with `XS_MAX_DEPTH=N`). Hitting it throws a catchable `StackOverflow` rather than segfaulting; the VM has its own growable stack.
- `match` does not support map patterns yet: destructure tuples, arrays, structs, and enums, but build a struct wrapper if you need to match map-shaped data.
- JS transpiler's `perform`/`handle` story is currently broken: the emitted JS puts `yield` inside a non-generator arrow IIFE, so even a direct `perform` inside a `handle` fails to parse under Node (SyntaxError). Use `--vm` or the interpreter for effects until the handle lowering is reworked.
- `http` module exposes client methods (`get`, `post`, ...) plus a
  basic `http.serve(port, handler)` defined in `src/runtime/builtins.c`.
  The richer async router in `src/net/http_server.c` is not wired up
  yet; `http.serve` is the only server entry point.
