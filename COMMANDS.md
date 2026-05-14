# XS Command Reference

Reference for the `xs` command-line tool. Most of these work, some are rough around the edges.

> **Convention:** Optional arguments are shown in `[brackets]`, required
> arguments in `<angle brackets>`. Flags can appear before or after positional
> arguments unless noted otherwise.

---

## Running Scripts

### `xs <file.xs> [args...]`

Run an XS script on the bytecode VM (the default backend).

```bash
xs hello.xs
xs server.xs --port 8080
```

Arguments after the filename are available in the script via `os.args`
after `import os`. `os.args` does not include the interpreter path or
the script filename; only the arguments that follow.

### `xs run <file.xs|file.xsc> [args...]`

Run a source file or compiled bytecode:

- `xs run <file.xs>`: same as `xs <file.xs>`, runs on the default VM backend
- `xs run <file.xsc>`: run a compiled bytecode file produced by `xs build`

```bash
xs run hello.xs
xs run app.xsc
```

### `xs -e <code>` / `xs --eval <code>`

Evaluate an inline XS expression without a file.

```bash
xs -e 'println("hello")'
xs --eval 'println(2 ** 10)'
```

Useful for quick one-liners and shell scripting.

### `xs --vm <file.xs>`

Run a script with the bytecode VM backend instead of the tree-walker.

```bash
xs --vm fib.xs
```

The VM compiles the AST to bytecode first, then executes it. Has full feature
parity with the interpreter: all language features, builtins, and methods work.
Faster for compute-heavy code (loops, recursion, tight numeric work).

### `xs --backend <interp|vm|jit> <file.xs>`

Select execution backend explicitly.

```bash
xs --backend vm program.xs
xs --backend interp program.xs
xs --backend jit program.xs
```

- `vm`: bytecode VM (default; same as bare `xs file.xs`)
- `interp`: tree-walker interpreter (use for plugin debugging or AST-level runtime hooks)
- `jit`: JIT compilation on x86-64 and aarch64. A register-allocating
  specialiser lowers bytecode through a small linear IR, runs liveness
  and linear-scan allocation over three callee-saved regs, and emits
  native code with SMI fast paths for integer arithmetic/compares, an
  XMM fast path for boxed floats, an inlined monomorphic inline cache
  for `LOAD_GLOBAL`, inline closure-upvalue access, a fused compare-
  and-branch peephole, and a refcount-pair elimination pass. Protos
  whose bytecode steps outside the supported subset drop to the
  bytecode VM; there is no template-JIT middle tier. Requires
  `XSC_ENABLE_JIT` at build time.

---

## Self-Management

### `xs upgrade`

Replace the current binary with the latest release from github.com/xs-lang0/xs.

```bash
xs upgrade         # prompt before replacing
xs upgrade --yes   # skip confirmation
```

Compares the current version against the GitHub releases API. If already up to date, exits 0 without touching anything. Verifies the SHA-256 of the downloaded binary against the sibling `.sha256` file before swapping.

Not supported on Windows in this build - re-run the install script instead:

```powershell
irm xslang.org/install.ps1 | iex
```

### `xs uninstall`

Remove the xs binary from the system.

```bash
xs uninstall                 # remove only the binary (prompts)
xs uninstall --with-data     # also remove ~/.xs and ~/.xs_cache
xs uninstall --yes           # skip confirmation
```

Prompts for confirmation by default. Use `--yes` or `-y` to skip.

---

## Interactive REPL

### `xs` (no arguments) / `xs repl`

Start the interactive Read-Eval-Print Loop. Bindings persist across lines
for the duration of the session.

```bash
xs                       # launch REPL
xs repl                  # same
printf '1 + 1\n:q\n' | xs repl  # pipe input, exit
```

Expression results print automatically, prefixed with `=> `. Declarations
(let, fn, struct, etc.) are registered silently.

```
xs 1.2.10
type :help for commands, :quit to exit (or Ctrl-D)
>> let x = 10
>> x * 3
=> 30
>> fn square(n) { n * n }
>> square(7)
=> 49
```

**Multi-line input**: keep typing after an unclosed `(`, `[`, or `{`.
The prompt switches to `.. ` until delimiters balance.

```
>> fn add(a, b) {
..   a + b
.. }
>> add(3, 4)
=> 7
```

**Meta-commands:**

| Command | Description |
|---------|-------------|
| `:help`, `:h` | Show this list |
| `:quit`, `:q` | Exit the REPL |
| `:env` | List all bindings in scope with their kind and type |
| `:clear` | Reset to a fresh interpreter, dropping all bindings |
| `:t <expr>` | Show the runtime type of an expression |

**Error recovery**: parse errors and runtime errors print a diagnostic and
re-prompt. The session does not exit.

---

## Static Analysis

### `xs --check <file.xs>` / `xs check <file.xs>`

Run semantic analysis (type checking, symbol resolution, exhaustiveness checks)
without executing the script. Returns exit code 0 if clean, 1 if errors found.

```bash
xs --check mylib.xs
xs check mylib.xs
echo $?  # 0 = no errors
```

This catches type mismatches, undefined variables, non-exhaustive matches, and
other semantic issues that can't be detected by parsing alone. Hindley-Milner
type inference is wired in, so it can catch type errors even in unannotated code.

### `xs --lenient <file.xs>`

Run with lenient mode: semantic errors are downgraded to warnings instead of
blocking execution.

```bash
xs --lenient script.xs
```

Useful during development when you want to run partially-typed code.

### `xs --optimize <file.xs>`

Run the AST optimizer before execution. Performs constant folding and other
optimizations.

```bash
xs --optimize heavy_compute.xs
```

## Code Quality Tools

### `xs lint [file|dir] [--fix]`

Lint source files for style and correctness issues.

```bash
xs lint src/main.xs
xs lint .                 # lint current directory
xs lint src/ --fix        # auto-fix where possible
```

**Checks performed:**

- Unused variables
- Naming conventions (snake_case for variables/functions)
- Unreachable code after return/break/continue
- Empty blocks
- Deeply nested code (>5 levels)
- Shadowed variables

### `xs fmt <file.xs> [--check]`

Format source files in canonical XS style.

```bash
xs fmt main.xs            # format in-place
xs fmt main.xs --check    # check without modifying (exit code 1 if changes needed)
```

**Style rules:** 4-space indent, canonical spacing around operators, consistent
brace placement.

The `--check` flag is useful in CI pipelines: it reports whether formatting
changes are needed without modifying files.

### `xs doc <file.xs|dir>`

Generate documentation from source files.

```bash
xs doc src/lib.xs         # generate docs for a file
xs doc .                  # generate docs for all files
xs doc src/lib.xs > api.md
```

Extracts function signatures, struct fields, enum variants, traits, and type
aliases. Outputs Markdown to stdout.

---

## Testing & Benchmarking

### `xs test [pattern]`

Run test files. Scans for files matching `test_*.xs` or `*_test.xs`.

```bash
xs test                   # run all tests
xs test math              # run tests matching "math"
```

**Output:**

```
Running tests...
  PASS  test_math.xs (0.012s)
  FAIL  test_parser.xs (0.005s)

Results: 1 passed, 1 failed, 2 total (0.017s)
```

Tests use `assert(condition, message?)` and `assert_eq(a, b)` for assertions.
A test file fails if any assertion panics.

### `xs bench [pattern]`

Run benchmark files. Scans for `bench_*.xs` or `*_bench.xs`.

```bash
xs bench                  # run all benchmarks
xs bench sort             # run benchmarks matching "sort"
```

Each benchmark is run 10 times. Reports min/max/avg execution time.

---

## Profiling & Coverage

### `xs profile <file.xs>` / `xs --profile <file.xs>`

Run a script with the sampling profiler enabled.

```bash
xs profile compute.xs
```

**Output:**

```
=== XS Profiler Report ===
Duration: 1.234s
Samples: 1234

  %time  samples  function       line
  45.2%      558  fibonacci         5
  23.1%      285  main             12
```

### `xs --trace-sample <rate> <file.xs>`

Production sampling: only profile a fraction of executions.

```bash
xs --trace-sample 0.01 server.xs  # sample 1% of calls
```

The rate is a float between 0.0 and 1.0.

### `xs coverage <file.xs>` / `xs --coverage <file.xs>`

Run a script with line and branch coverage tracking.

```bash
xs coverage mylib.xs
```

**Output:**

```
=== XS Coverage Report ===
File: mylib.xs
Lines:    45/60  (75.0%)
Branches: 12/16  (75.0%)

Uncovered lines: 23, 24, 42
```

---

## Transpilation & IR Dumps

### `xs --emit <target> <file.xs>`

Dump intermediate representations or transpile to other languages.

```bash
xs --emit ast script.xs       # print AST tree
xs --emit bytecode script.xs  # print VM bytecode
xs --emit ir script.xs        # print SSA IR (three-address form)
xs --emit js script.xs        # transpile to JavaScript
xs --emit c script.xs         # transpile to C
xs --emit wasm script.xs      # transpile to WebAssembly
```

### `xs transpile --target <js|c|wasm32|wasi> <file.xs>`

Explicit transpile command with target selection.

```bash
xs transpile --target js app.xs > app.js
xs transpile --target c lib.xs > lib.c
xs transpile --target wasm32 module.xs
xs transpile --target wasi server.xs
```

**Targets:**

- `js`: ES2020+ JavaScript
- `c`: C11 source with `xs_val` runtime (supports `inline c { ... }` blocks)
- `wasm32`: WebAssembly binary (`.wasm`)
- `wasi`: WASI-compatible WebAssembly

The WASM backend is the least mature of the three -- it handles basic arithmetic and function calls but doesn't cover the full language yet.

### `xs build <file.xs> [-o out.xsc]`

Compile a source file to bytecode without executing it. Writes a `.xsc` file:
defaults to the same name with the `.xsc` extension, or a custom path via `-o`.

```bash
xs build app.xs                    # produces app.xsc
xs build app.xs -o dist/app.xsc   # write to a specific path
xs run app.xsc                     # run the compiled output
```

---

## Debugging & Tracing

### `xs --debug <file.xs>`

Run with the Debug Adapter Protocol (DAP) server. Connect from VS Code, Neovim,
or any DAP client.

```bash
xs --debug app.xs
```

Supports: breakpoints, step/next, variable inspection, call stack.

### `xs dap`

Start the DAP server directly (for editor integration).

```bash
xs dap
```

Supports: breakpoints (including conditional breakpoints), step in / next / step
out, variable inspection, evaluate expressions, call stack. Supports
`stopOnEntry` in the launch configuration to pause at the first line
automatically. The VS Code extension wires this up automatically: no manual
setup needed.

### `xs --record <file.xst> <file.xs>`

Record an execution trace for time-travel debugging.

```bash
xs --record trace.xst program.xs
```

Records every function call, return, variable store, and I/O operation to a `.xst`
trace file. Use `--trace-deep` to serialize complex values (arrays, maps, structs)
as JSON in the trace output.

### `xs replay <trace.xst>` / `xs --replay <trace.xst>`

Replay a recorded execution trace interactively.

```bash
xs replay trace.xst
```

**Replay commands:**

- `n`: step forward
- `p`: step backward
- `c`: continue to end
- `g <n>`: goto event number
- `q`: quit

---

## Error Diagnostics

### `xs --explain <code>` / `xs explain <code>`

Show a detailed explanation of an error code. Error codes appear in diagnostic
output (e.g., `error[T0001]`).

```bash
xs --explain T0001
xs explain S0003
```

**Error code prefixes:**

| Prefix | Category |
|--------|----------|
| `L0xxx` | Lexer errors (invalid tokens, unterminated strings) |
| `P0xxx` | Parser errors (syntax errors, unexpected tokens) |
| `T0xxx` | Type errors (type mismatches, undefined types) |
| `S0xxx` | Semantic errors (unused variables, shadowing, unreachable code) |

---

## File Watching

### `xs --watch <file.xs>`

Run a script and re-execute automatically when the file changes.

```bash
xs --watch server.xs
```

Uses filesystem polling (300ms interval). On file change, the script is
re-parsed and re-run with a fresh interpreter.

---

## Package Management

### `xs new <name>`

Scaffold a new XS project in a new directory.

```bash
xs new myapp
```

Creates:

```
myapp/
|-- xs.toml                 # package manifest
|-- src/main.xs             # hello world entry point
|-- .gitignore
```

### `xs init`

Initialize an XS project in the current directory. Writes `xs.toml`,
writes `src/main.xs` if absent, and adds a `.gitignore` if none exists.
Fails if `xs.toml` already exists.

```bash
xs init
```

### `xs add <name>`

Add a dependency. Accepts a bare package name or a `user/repo` shortform.
Updates `xs.toml` and installs to `xs_lib/`.

```bash
xs add http-client
xs add user/repo
```

### `xs install [pkg]`

Install dependencies.

```bash
xs install              # install all from xs.toml
xs install http-client  # install a specific package
```

Packages are installed to `.xs_lib/` in the project directory. Global packages
(installed via `xsi get -g`) go to `/usr/local/xs/lib/`.

### `xs remove <pkg>`

Remove an installed package.

```bash
xs remove http-client
```

### `xs update [pkg]`

Update dependencies to latest compatible versions.

```bash
xs update               # update all
xs update http-client   # update specific package
```

### `xs publish`

Publish the current package to the XS registry. Reads the API token
from `$XS_REGISTRY_TOKEN`, falling back to `~/.xs/credentials` (set
via `xs login`). Without a token the tarball is built locally and the
path is printed so you can sideload it.

```bash
xs login           # one-time, stores ~/.xs/credentials
xs publish         # builds tarball, POSTs to reg.xslang.org
```

### `xs search <query>`

Search the package registry. Hits `reg.xslang.org/api/search`, prints
the top 20 results.

```bash
xs search json
```

### `xs login`

Store a registry token at `~/.xs/credentials` (chmod 600). Reads
`$XS_REGISTRY_TOKEN` if set, otherwise prompts on stdin. Get the token
from the registry account page.

### `xs logout`

Delete `~/.xs/credentials`.

### `xs whoami`

Print the username and email associated with the stored token.

### `xs list`

List packages installed in the current project (`xs_lib/`).

```bash
xs list
```

### `xs pkg <subcommand>`

Alternative package management interface. Implemented subcommands:
`install`, `remove`, `update`, `list`, `add`, `search`, `publish`.

```bash
xs pkg list             # list installed packages
xs pkg install foo      # install a package
xs pkg add user/repo    # add a dependency
```

---

## IDE Integration

### `xs lsp [-s <lsp.xs>]`

Start the Language Server Protocol server on stdin/stdout.

```bash
xs lsp
xs lsp -s /path/to/lsp.xs   # use a custom LSP implementation
```

The `-s`/`--source` flag lets you point at a specific `lsp.xs` script. The VS
Code extension bundles its own `lsp.xs` and passes it via `-s` automatically.

**Supported features:**

- Diagnostics (parse errors, type errors, semantic errors)
- Hover (identifier info, inferred types)
- Completion: keywords, builtins, and dot completion for types and modules
- Signature help
- Go to definition
- Find references
- Document symbols
- Formatting
- Rename
- Code actions

Configure in your editor by pointing the LSP client to `xs lsp`.

---

## Plugin System

### `xs --plugin <path>`

Load a native plugin (`.so`/`.dll`/`.dylib`) before running the script.

```bash
xs --plugin ./my_plugin.so app.xs
```

Plugins must export `xs_plugin_init(Interp*, int api_version)`.

### `xs --sandbox`

Run plugins in sandboxed mode, restricting I/O and network access.

```bash
xs --sandbox --plugin untrusted.so app.xs
```

---

## Global Flags

These flags work with any subcommand or when running scripts directly.

| Flag | Description |
|------|-------------|
| `--help` / `-h` | Show help. Works globally and per-subcommand. |
| `--version` / `-V` | Show version string. |
| `--no-color` | Disable ANSI color output. |
| `--check` | Run semantic analysis only, do not execute. |
| `--lenient` | Downgrade semantic errors to warnings. |
| `--strict` | Require type annotations on every binding and function. |
| `--optimize` | Run AST optimizer before execution. |
| `--watch` | Re-run on file change. |
| `--vm` | Use bytecode VM backend. |
| `--jit` | Use JIT compilation backend. |
| `--backend <name>` | Select backend: `interp`, `vm`, or `jit`. |
| `--lsp` | Start the LSP server (same as `xs lsp`). |
| `--dap` | Start the DAP server (same as `xs dap`). |
| `--emit <target>` | Dump IR or transpile: `ast`, `bytecode`, `ir`, `js`, `c`, `wasm`. |
| `-e` / `--eval <code>` | Evaluate inline code. |
| `--explain <code>` | Explain an error code. |
| `--record <file>` | Record execution trace to `.xst` file. |
| `--replay <file>` | Replay a `.xst` trace file. |
| `--debug <file>` | Run with the DAP debug server attached. |
| `--profile` | Enable sampling profiler. |
| `--coverage` | Enable line coverage tracking. |
| `--trace` | Enable execution tracer (provenance recording). |
| `--trace-sample <rate>` | Set profiler sampling rate (0.0-1.0). |
| `--trace-deep` | Serialize complex values as JSON in execution traces. |
| `--gc-debug` | Enable GC debug prints. |
| `--plugin <path>` | Load native plugin before execution. |
| `--sandbox` | Sandbox plugin execution. |

**Environment variables that affect runtime:**

| Variable | Effect |
|----------|--------|
| `XS_MAX_DEPTH` | Interpreter call-depth cap (default 500). On overflow the interpreter throws a catchable `StackOverflow`. The VM has its own growable stack and ignores this. |
| `XS_JIT_CMPBR` | Compare+branch fusion peephole; defaults on. Set to `0`, `n`, or `N` to disable. |
| `XS_JIT_INLINE` | Self-recursive inliner; defaults on. Same disable spelling as above. |
| `XS_JIT_REFOPT` | Refcount-pair elimination peephole; defaults on. |

**Flag placement:** Global flags like `--no-color`, `--vm`, and `--check` can
appear anywhere in the argument list: before or after the filename or
subcommand. Flags can be stacked freely. For example, all of these work:

```bash
xs --no-color --check mylib.xs
xs mylib.xs --no-color --check
xs --check --vm mylib.xs         -- type-check then run on the VM
```

**Per-subcommand help:** Every subcommand supports `--help`:

```bash
xs test --help
xs lint --help
xs transpile -h
xs fmt --help
```

## Building from Source

### Makefile Targets

```bash
make                # default build (-O2)
make debug          # build with AddressSanitizer + UBSan (-g -O0)
make release        # optimized build (-O3, LTO, stripped)
make test           # run smoke tests
make clean          # remove build artifacts
make install        # install to /usr/local/bin/xs (builds release first)
```

### Feature Flags

All features are **enabled by default**. Disable specific features by setting
flags to 0:

```bash
make XSC_ENABLE_JIT=0       # build without JIT
make XSC_ENABLE_PLUGINS=0   # build without plugin support
```

| Flag | Feature | Enables |
|------|---------|---------|
| `XSC_ENABLE_VM` | Bytecode VM | `--vm`, `--emit bytecode` |
| `XSC_ENABLE_JIT` | JIT compilation | `--jit`, `--backend jit` |
| `XSC_ENABLE_TRANSPILER` | Transpilers | `--emit js\|c\|wasm`, `xs transpile` |
| `XSC_ENABLE_TRACER` | Execution tracing | `--record`, `xs replay` |
| `XSC_ENABLE_LSP` | Language Server | `xs lsp` |
| `XSC_ENABLE_DAP` | Debug Adapter | `--debug`, `xs dap` |
| `XSC_ENABLE_PROFILER` | Sampling profiler | `xs profile`, `--profile` |
| `XSC_ENABLE_COVERAGE` | Coverage tracking | `xs coverage`, `--coverage` |
| `XSC_ENABLE_DOC` | Doc generator | `xs doc` |
| `XSC_ENABLE_FMT` | Code formatter | `xs fmt` |
| `XSC_ENABLE_PKG` | Package manager | `xs install\|remove\|update\|publish` |
| `XSC_ENABLE_EFFECTS` | Algebraic effects | `effect`, `perform`, `handle` |
| `XSC_ENABLE_PLUGINS` | Plugin loading | `--plugin`, `--sandbox` |
| `XSC_ENABLE_SANDBOX` | Plugin sandboxing | `--sandbox` |

If a feature is disabled at compile time, the corresponding CLI command will print an error saying the feature isn't available in this build.
