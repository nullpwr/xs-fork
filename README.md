# XS

A programming language. Anywhere, anytime, by anyone.

One statically-linked binary contains the compiler, the language server, the debugger, the formatter, the linter, the test runner, the profiler, and the package manager. Source compiles to native machine code, JavaScript, or WebAssembly, and runs unchanged on Linux, macOS, Windows, WASI, iOS, Android, ESP32, and Raspberry Pi.

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

-- duration as a real type
let warmup = 30s
let frame  = 16ms
println(warmup + frame)          -- 30.016s

-- function overloading
fn area(r) = 3.14 * r * r
fn area(w, h) = w * h
```

## Install

```bash
curl -fsSL https://xslang.org/install | sh        # linux/macos
irm https://xslang.org/install.ps1 | iex          # windows (powershell)
```

The installer downloads `xsi`, which sets up `/usr/local/xs/` (or `C:\xs\` on Windows) with the compiler, the VM, and every built-in tool. Requires sudo / admin.

```
/usr/local/xs/
  bin/     xs, xsi (added to system PATH)
  lib/     globally installed packages
  cache/   download cache
  env      shell environment setup
```

Both installers verify the GitHub release against its published SHA-256 sums before running anything. Static binaries with checksums also live at [xslang.org/downloads](https://xslang.org/downloads).

## Build from source

```bash
make            # produces ./xs (or xs.exe on Windows)
make debug      # -g -O0 with AddressSanitizer + UBSan
make release    # -O3 with LTO, stripped
make test       # tests/run-all.sh: 7 layers
make install    # install release build to /usr/local/bin/xs
make wasm       # produces xs.wasm via wasi-sdk (needs WASI_SDK env var)
```

Needs gcc or clang and GNU make. No other build or runtime dependencies. HTTPS goes through the bundled BearSSL tree under `src/tls/bearssl/`.

## Run

```bash
xs file.xs              # bytecode VM (default)
xs                      # interactive REPL
xs -e 'println(42)'     # eval one-liner
xs --interp file.xs     # tree-walk interpreter (REPL / plugin debugging)
xs --jit file.xs        # JIT (x86-64 + aarch64; see STATUS.md)
xs --emit js file.xs    # transpile to JavaScript
xs --emit c file.xs     # transpile to C
xs --check file.xs      # static type check without running
xs --strict file.xs     # require type annotations everywhere
```

The bytecode VM is the default. Use `--jit` for hotspots that have actually been profiled; the JIT compiles opcodes that fit its supported set and falls back to the VM for the rest. Use `--interp` for plugin debugging or anything that hooks AST-level evaluation. See [STATUS.md](STATUS.md) for the per-feature backend matrix.

## Backends

| flag           | what it does                                                                          |
| -------------- | ------------------------------------------------------------------------------------- |
| (default)      | bytecode VM                                                                           |
| `--interp`     | tree-walk interpreter, for the REPL and AST-level plugin hooks                        |
| `--jit`        | register-allocating JIT for x86-64 and aarch64; unsupported opcodes fall back to VM   |
| `--emit c`     | self-contained C source                                                               |
| `--emit js`    | JavaScript for Node or the browser                                                    |
| `make wasm`    | runtime build (`xs.wasm`); the same compiler running in any WASI host or the browser  |

Both the interp and the VM run against the same test suite on every commit and their outputs are diff'd; a divergence fails the test even when each backend passes on its own.

## Stdlib

Lazy-loaded on first `import`. 36 built-in modules:

`math`, `time`, `io`, `string`, `path`, `base64`, `hash`, `uuid`, `collections`, `process`, `random`, `os`, `json`, `log`, `fmt`, `test`, `csv`, `url`, `re`, `msgpack`, `Promise`, `async`, `net`, `crypto`, `thread`, `buf`, `encode`, `db`, `cli`, `ffi`, `reflect`, `gc`, `toml`, `http`, `fs`, `tracing`.

HTTP / HTTPS client and HTTP/1.1 server. Per-server body / header / connection caps and idle culling. The HTTPS client parses certificates but does not validate the trust chain; suitable for talking to known endpoints, not general public HTTPS in production. Trust-chain validation is on the roadmap.

## Plugins

Plugins are XS scripts with direct access to the lexer, parser, and runtime. They can add keywords, inject globals, hook evaluation, override syntax, and intercept imports. Written in XS, not C. See [PLUGINS.md](PLUGINS.md).

## Mobile and embedded

`make ios`, `make android`, `make esp32`. The mobile builds drop the JIT (App Store policy and Android NDK constraints); ESP32 is VM-only.

## Concurrency

`spawn` creates real OS threads. The bytecode VM holds a global lock during its dispatch loop, so two pure-compute threads take turns rather than running in parallel. The lock releases around sleep, I/O, channel receive, and the like, so spawn-and-block parallelises the way you would expect. Same model that CPython uses. Removing the lock is on the roadmap; it is not a 1.x change.

`channel`, `actor`, `nursery`, `async` / `await` are all available. See [docs/concurrency](https://xslang.org/docs/guide/concurrency).

## Benchmarks

Linux x86-64, AMD Ryzen 7, best of three runs, each binary cold from disk. Treat as indicative, not authoritative.

| program               | `--interp` | `--vm` | `--jit` | node 20 | python 3.13 |
| --------------------- | ---------: | -----: | ------: | ------: | ----------: |
| hello world (startup) |       3 ms |   3 ms |    3 ms |   57 ms |       15 ms |
| `fib(30)`             |     950 ms | 180 ms |   31 ms |   62 ms |       71 ms |

Reproduce with `bash tests/bench_backends.sh` or `xs bench`.

## Project layout

```
src/             compiler and runtime, one subdirectory per subsystem
src/tls/         bundled BearSSL for HTTPS
tests/           behavioural tests (test_*.xs) and shell drivers
tests/*/         adversarial, conformance, fuzz, golden, lint_samples,
                 negative, plugins, property, regression
benchmarks/      benchmark programs
editors/vscode/  VS Code extension
Makefile         build system
xs.toml          project config
```

## Docs

- [LANGUAGE.md](LANGUAGE.md) — full language reference
- [COMMANDS.md](COMMANDS.md) — every CLI command, flag, and subcommand
- [PLUGINS.md](PLUGINS.md) — plugin system guide with working examples
- [STATUS.md](STATUS.md) — what works, what's partial, what's planned
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [xslang.org](https://xslang.org) — the website, with a browser playground

## License

Apache 2.0.
