# XS

A general-purpose programming language with gradual typing, multiple execution backends, and a plugin system that lets you modify anything. Written in C, builds on Linux, macOS, and Windows with zero dependencies. Started building privately in 2024, made public in early 2026.

```xs
-- types are optional. add them when you want enforcement.
fn fib(n) {
    if n <= 1 { return n }
    return fib(n - 1) + fib(n - 2)
}

fn fib_typed(n: int) -> int {
    if n <= 1 { return n }
    return fib_typed(n - 1) + fib_typed(n - 2)
}

-- closures, pipes, comprehensions
let squares = [x * x for x in 0..10 if x % 2 == 0]
let total = squares |> reduce(fn(a, b) { a + b }, 0)

-- structs with traits
struct Point { x, y }
impl Point {
    fn distance(self) -> f64 {
        return (self.x ** 2 + self.y ** 2) ** 0.5
    }
}

-- pattern matching
fn describe(val) {
    match val {
        0 => "zero"
        x if x < 0 => "negative"
        _ => "positive"
    }
}

-- reactive bindings: auto-update when dependencies change
var price = 10
var qty = 3
bind total = price * qty         -- total is 30
price = 20                       -- total is now 60

-- contracts: gradual runtime checks
fn safe_div(a: int, b: int where b != 0) {
    return a / b
}

-- adapt: multi-target functions
adapt fn render(data: str) -> str {
    when native { return "native:" ++ data }
    when js     { return "js:" ++ data }
}

-- function overloading
fn area(r) = 3.14 * r * r
fn area(w, h) = w * h

-- tagged blocks: custom control structures
tag retry(n) {
    var attempts = 0
    loop {
        try { let r = yield; return r }
        catch e { attempts += 1; if attempts >= n { throw e } }
    }
}
retry(3) { http_get("https://api.example.com") }
```

## Install

Install with a single command:

```bash
curl -fsSL https://xslang.org/install | sh        # linux/macos
irm https://xslang.org/install.ps1 | iex          # windows (powershell)
```

This downloads the XS installer (xsi), which sets up `/usr/local/xs/` (or `C:\xs\` on Windows) with the compiler, VM, and all built-in tools. Requires sudo/admin. After install:

```
/usr/local/xs/
  bin/     xs, xsi (added to system PATH)
  lib/     globally installed packages
  cache/   download cache
  env      shell environment setup
```

Or build from source:

```bash
make            # produces ./xs (or xs.exe on Windows)
make debug      # -g -O0 with AddressSanitizer + UBSan
make release    # -O3 with LTO, stripped
make test       # runs tests/run.sh (all tests/test_*.xs + examples + test_cli.sh)
make install    # install release build to /usr/local/bin/xs
make wasm       # produces xs.wasm via wasi-sdk (needs WASI_SDK env var)
```

Needs gcc or clang and GNU make. No other build or runtime dependencies. HTTPS is handled by the bundled BearSSL tree under src/tls/bearssl/.

## Run

```bash
xs file.xs              # run a script (tree-walk interpreter, default)
xs                      # interactive REPL
xs -e 'println(42)'     # eval one-liner
xs --vm file.xs         # bytecode VM backend (faster for compute, recommended)
xs --jit file.xs        # JIT backend (x86-64, early stage)
xs --emit js file.xs    # transpile to JavaScript
xs --emit c file.xs     # transpile to C
xs --check file.xs      # static type check without running
xs --strict file.xs     # require type annotations everywhere
```

> **Which backend:** the tree-walk interpreter is the default so small
> one-off scripts start instantly and REPL/debug sessions behave
> predictably. For anything compute-heavy or long-running, pass `--vm`:
> the bytecode VM is consistently faster and handles deep recursion
> on its own growable stack. See [STATUS.md](STATUS.md) for the full
> backend matrix.

## What's in the box

**Language features:**
- Gradual typing with `--check` and `--strict`
- Reactive bindings (`bind`) that auto-update when dependencies change
- Gradual contracts (`where` clauses) for runtime-checked type conditions
- Adapt functions (`adapt fn`) with per-target implementations (native/js/wasm)
- Structs, traits, enums, classes with inheritance
- Pattern matching with destructuring and guards
- Closures, generators (`fn*`/`yield`), arrow lambdas
- Function overloading (dispatch by argument count)
- Tagged blocks (`tag`) for user-defined control structures
- Inline C blocks for performance-critical code (`inline c { ... }`)
- Universal literals: durations (`5s`, `200ms`), colors (`#ff6600`), dates, sizes (`10kb`), angles (`90deg`)
- Temporal primitives: `every`, `after`, `timeout`, `debounce` for scheduling
- Algebraic effects (`effect`/`perform`/`handle`/`resume`)
- All the concurrency: spawn, async/await, actors, channels, nurseries
- First-class regex (`/pattern/` literals, `.test()`, `.match()`, `.replace()`)
- List/map comprehensions, spread, pipe operator
- try/catch/finally, defer, throw

**Backends:**
- Tree-walk interpreter (default)
- Bytecode VM (`--vm`, full feature parity, faster for compute-heavy code)
- JIT compiler (x86-64, early stage)
- Transpilers: JavaScript, C, WebAssembly

**Tooling:**
- REPL with syntax highlighting
- LSP server (`xs lsp`)
- Formatter (`xs fmt`), linter (`xs lint`)
- Test runner (`xs test`), benchmarks (`xs bench`)
- Profiler (`xs profile`), coverage (`xs coverage`)
- Package manager (`xs install`, `xs remove`)
- Doc generator (`xs doc`)

**Standard library** (36 built-in modules, all registered at startup):
math, time, io, string, path, base64, hash, uuid, collections, process, random, os, json, log, fmt, test, csv, url, re, msgpack, Promise, async, net, crypto, thread, buf, encode, db, cli, ffi, reflect, gc, reactive, toml, http, fs

**Plugin system:**
Plugins are XS scripts with direct access to the lexer, parser, and runtime. Add keywords, inject globals, hook evaluation, override syntax, intercept imports -- written in XS, not C.

**Networking:**
HTTP/HTTPS client with zero external dependencies (BearSSL embedded for TLS).

## Quick examples

```xs
-- http request
import net
let resp = net.http_get("https://httpbin.org/get")
println(resp["status"])   -- 200

-- file operations
import fs
fs.write("/tmp/hello.txt", "hi from xs")
println(fs.read("/tmp/hello.txt"))

-- actors
actor Counter {
    var count = 0
    fn increment() { count = count + 1 }
    fn get() { count }
}
let c = spawn Counter
c.increment()
c.increment()
println(c.get())  -- 2

-- gradual typing catches mistakes
let nums: [int] = [1, 2, "oops"]  -- runtime error: expected '[int]', got '[mixed]'
```

## Project layout

```
src/            compiler and runtime (348 .c files across 27 subsystems)
src/tls/        bundled BearSSL for HTTPS
tests/          35 test_*.xs files + test_cli.sh, test_lint.sh, test_errors.sh, test_transpiler.sh
examples/       15 .xs examples + examples/plugins/
benchmarks/     benchmark programs
editors/        VS Code extension (editors/vscode/)
Makefile        build system
xs.toml         project config
```

## Benchmarks

Wall-clock numbers from a single Linux x86_64 machine, O2 release build,
warm caches. Treat as indicative, not authoritative.

| Program          | Python 3 | Node.js  | xs (interp) | xs (--vm) |
|------------------|----------|----------|-------------|-----------|
| Hello world (startup) | ~15 ms | ~54 ms | **~4 ms** | ~4 ms |
| `fib(25)` recursion   | ~21 ms | ~54 ms | ~101 ms | **~20 ms** |
| `fib(30)` recursion   | ~160 ms | ~60 ms | ~2.2 s  | **~170 ms** |

Startup time is the flagship number: `xs file.xs` spins up about **10x
faster than Node** and **3-4x faster than Python**, which makes it
practical for small CLI tools. For compute, the VM backend is roughly
on par with CPython; the tree-walk interpreter is ~10x slower on hot
recursion and should not be used for compute-heavy work.

Reproduce with `benchmarks/bench_fibonacci.xs`, `benchmarks/bench_sort.xs`,
`benchmarks/bench_strings.xs` and `xs bench` (runs each 10 times and
reports min/max/avg).

## Docs

- [LANGUAGE.md](LANGUAGE.md) -- full language reference
- [COMMANDS.md](COMMANDS.md) -- every CLI command, flag, and subcommand
- [PLUGINS.md](PLUGINS.md) -- plugin system guide with working examples
- [STATUS.md](STATUS.md) -- what works, what's partial, what's planned
- [CONTRIBUTING.md](CONTRIBUTING.md) -- how to contribute

## License

Apache 2.0
