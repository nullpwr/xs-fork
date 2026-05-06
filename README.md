# XS

A general-purpose programming language. Gradual typing, multiple execution backends, plugin system. Written in C. Builds on Linux, macOS, and Windows with zero dependencies.

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
make test       # runs tests/run-all.sh (7 layers: unit, e2e, negative, property, golden, regression, conformance)
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
xs --jit file.xs        # JIT backend (x86-64 and aarch64; see STATUS.md)
xs --emit js file.xs    # transpile to JavaScript
xs --emit c file.xs     # transpile to C
xs --check file.xs      # static type check without running
xs --strict file.xs     # require type annotations everywhere
```

The tree-walk interpreter is the default for fast startup. Pass
`--vm` for compute-heavy or long-running code. See [STATUS.md](STATUS.md)
for the backend matrix.

## What's in the box

**Language features:**
- Gradual typing with `--check` and `--strict`
- Reactive bindings (`bind`) that auto-update when dependencies change
- Gradual contracts (`where` clauses) for runtime-checked type conditions
- Structs, traits, enums, classes with inheritance
- Pattern matching with destructuring and guards
- Closures, generators (`fn*`/`yield`), arrow lambdas
- Function overloading (dispatch by argument count)
- Tagged blocks (`tag`) for user-defined control structures
- Inline C blocks for performance-critical code (`inline c { ... }`)
- First-class `Duration` type with native suffixes (`5s`, `200ms`, `1ns`, `2m30s`)
- Decorators on fn declarations: `@on_start`, `@on_exit`, `@on_signal`, `@on_panic`, `@every(d)`, `@cron(...)`, `@delayed(d)`, `@watch(path)`, `@bench`, `@example`, `@export(name)`, `@once`
- Temporal primitives: `every`, `after`, `timeout`, `debounce` for scheduling
- Algebraic effects (`effect`/`perform`/`handle`/`resume`)
- All the concurrency: spawn, async/await, actors, channels, nurseries
- First-class regex (`/pattern/` literals, `.test()`, `.match()`, `.replace()`)
- List/map comprehensions, spread, pipe operator
- try/catch/finally, defer, throw

**Backends:**
- Tree-walk interpreter (default)
- Bytecode VM (`--vm`, full feature parity, faster for compute-heavy code)
- JIT compiler (`--jit`, x86-64 and aarch64): register-allocating specialiser; protos outside the supported opcode set drop back to the VM
- Transpilers: JavaScript, C, WebAssembly

**Tooling:**
- REPL with syntax highlighting
- LSP server (`xs lsp`)
- Formatter (`xs fmt`), linter (`xs lint`)
- Test runner (`xs test`), benchmarks (`xs bench`)
- Profiler (`xs profile`), coverage (`xs coverage`)
- Package manager (`xs install`, `xs remove`)
- Doc generator (`xs doc`)

**Standard library** (37 built-in modules, all registered at startup):
math, time, io, string, path, base64, hash, uuid, collections, process, random, os, json, log, fmt, test, csv, url, re, msgpack, Promise, async, net, crypto, thread, buf, encode, db, cli, ffi, reflect, gc, reactive, toml, http, fs, tracing

**Plugin system:**
Plugins are XS scripts with direct access to the lexer, parser, and runtime. Add keywords, inject globals, hook evaluation, override syntax, intercept imports -- written in XS, not C.

**Networking:**
HTTP / HTTPS client and HTTP/1.1 server. TLS via the bundled BearSSL
tree, no external dependency. Server has per-server body / header /
connection caps and idle culling.

**Mobile and embedded:**
`make ios`, `make android`, `make esp32`. Examples in
`examples/embedded/`.

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
src/             compiler and runtime, one subdirectory per subsystem
src/tls/         bundled BearSSL for HTTPS
tests/           behavioural tests (test_*.xs) and shell drivers (test_cli.sh, test_lint.sh, test_errors.sh, test_transpiler.sh)
tests/*/         adversarial, conformance, golden, regression, negative, property, fuzz layers
examples/        example programs (examples/plugins/ for plugin demos)
benchmarks/      benchmark programs
editors/vscode/  VS Code extension
Makefile         build system
xs.toml          project config
```

## Benchmarks

Wall-clock numbers from a single Linux x86_64 machine, O2 release build,
warm caches. Treat as indicative, not authoritative.

| Program          | Python 3 | Node.js  | xs (interp) | xs (--vm) |
|------------------|----------|----------|-------------|-----------|
| Hello world (startup) | ~15 ms | ~54 ms | **~4 ms** | ~4 ms |
| `fib(25)` recursion   | ~21 ms | ~54 ms | ~101 ms | **~20 ms** |
| `fib(30)` recursion   | ~160 ms | ~60 ms | ~2.2 s  | **~170 ms** |

A tight numeric loop shows where `--jit` pays off relative to `--vm`:

| Program              | `--vm`  | `--jit` | gcc -O2 | node   |
|----------------------|---------|---------|---------|--------|
| `fib(30)`            |  210 ms |   20 ms |   <1 ms | 110 ms |
| `fib(35)`            | 2320 ms |  520 ms |   80 ms | 210 ms |
| 10M-iter `while` sum |  640 ms |  110 ms |   20 ms | 110 ms |
| 1M-iter `while` sum  |   60 ms |   10 ms |   <1 ms | 110 ms |

Startup is around 2.3 ms on this box; `--vm` is in the same ballpark
as CPython on loops and a few times slower on hot recursion; `--jit`
closes the gap. Use `--vm` for everyday work, `--jit` when you've
actually profiled a hotspot.

Reproduce with `benchmarks/bench_fibonacci.xs`, `benchmarks/bench_sort.xs`,
`benchmarks/bench_strings.xs`, or `xs bench`.

## Docs

- [LANGUAGE.md](LANGUAGE.md) -- full language reference
- [COMMANDS.md](COMMANDS.md) -- every CLI command, flag, and subcommand
- [PLUGINS.md](PLUGINS.md) -- plugin system guide with working examples
- [STATUS.md](STATUS.md) -- what works, what's partial, what's planned
- [CONTRIBUTING.md](CONTRIBUTING.md) -- how to contribute

## License

Apache 2.0
