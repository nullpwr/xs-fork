# The xs command

`xs` is the only binary you need. It's the runtime, the package
manager, the test runner, the formatter, the language server, the
debug adapter, and the doc generator. Subcommands handle the
different jobs; flags configure them.

```sh
xs                       # REPL
xs file.xs               # run a script
xs run app.xsc           # run a compiled bundle
xs build src/main.xs     # compile to bytecode
xs test                  # discover and run tests
xs check src/lib.xs      # type-check without running
xs fmt src/              # format in place
xs doc src/lib.xs        # generate API markdown
xs lsp                   # speak LSP on stdin/stdout
xs install               # resolve and fetch xs.toml deps
```

The full list lives in `COMMANDS.md` at the repo root; this chapter
covers the day-to-day shape and the parts you reach for most.

## Flag placement

Flags can appear before *or* after the filename or subcommand. These
all do the same thing:

```sh
xs --check --vm src/lib.xs
xs src/lib.xs --check --vm
xs --vm src/lib.xs --check
```

The lone exception is script arguments. Anything after the script
filename that the script wants to consume should follow the
filename and the parser stops looking for `xs` flags after the
first positional that is a `.xs` / `.xsc` file.

```sh
xs server.xs --port 8080      -- --port goes to the script via argv
```

If you need to pass a flag that `xs` would otherwise consume, use
`--` to mark the boundary:

```sh
xs script.xs -- --check       -- the script gets `--check`
```

## Picking a backend

Three execution backends, one flag:

| invocation | backend                    | when                          |
|------------|----------------------------|-------------------------------|
| `xs`       | tree-walker                | REPL, plugin debugging        |
| `xs --vm`  | bytecode VM                | the production default        |
| `xs --jit` | adaptive JIT (VM + native) | hot loops, long-running jobs  |

The VM is the baseline. Running with `--jit` is identical
behaviourally; the JIT just trades a few bytes of startup for native
speed on hot protos. Force the interpreter only when a plugin needs
AST-level eval hooks.

## Common workflows

### Iterating on a script

```sh
xs --watch script.xs        -- re-runs on file change
```

Polls every 300ms; on save, re-parses with a fresh interpreter. Pair
with a side terminal running `xs test --watch tests/` for instant
feedback on a TDD loop.

### Type-checking without running

```sh
xs --check src/lib.xs       -- exit 0 if clean, 1 if errors
xs --strict src/lib.xs      -- the same, plus annotation requirement
```

Useful in CI before a longer build. Fast (~100ms typical for a
medium file).

### Compiling to a bundle

```sh
xs build src/main.xs -o app.xsc
xs run app.xsc
```

`.xsc` files are versioned bytecode. Older runtimes refuse newer
bundles; the bundle reports its required version on mismatch. They
load ~5x faster than parsing source.

For a single-file deployment, fold the bundle into a runtime:

```sh
xs --embed-program app.xsc xs -o app
./app                        -- standalone binary
```

See [Single-binary deployment](./deployment.md) for the rest of that
story.

### Profiling

```sh
xs --profile script.xs > profile.json
xs --profile-flamegraph script.xs > flame.svg
```

The profiler samples at 1ms via `SIGPROF` (POSIX) or
`CreateTimerQueueTimer` (Windows). Output is folded-stack format,
compatible with FlameGraph tools.

For sampling at a fraction (production, low overhead):

```sh
xs --trace-sample 0.01 server.xs    -- sample 1% of calls
```

### Debugging

```sh
xs --debug app.xs                  -- start with a DAP server attached
```

Connect from VS Code, Neovim, or any DAP client. Supports
breakpoints (with conditions), step in / next / step out, variable
inspection, expression evaluation, full call stack. The bundled VS
Code extension wires this up automatically.

For time-travel:

```sh
xs --record trace.xst app.xs
xs replay trace.xst                -- step through the recording
```

Replay commands: `n` step forward, `p` step backward, `c` continue
to end, `g 1234` jump to event N, `q` quit.

### Linting and formatting

```sh
xs lint src/                       -- complain about unused, shadowed, dead code
xs lint src/ --fix                 -- auto-fix where possible
xs fmt src/main.xs                 -- format in place
xs fmt src/ --check                -- exit 1 if anything changed
```

`xs fmt` is canonical: 4-space indent, consistent operator spacing,
predictable brace placement. CI hooks should pin to `xs fmt --check`.

### Generating API docs

```sh
xs doc src/lib.xs                  -- markdown to stdout
xs doc src/lib.xs > api.md
xs doc .                            -- whole project
```

Pulls function signatures, struct fields, enum variants, traits, type
aliases. Uses doc comments (`-- ...` immediately above a declaration)
verbatim.

## Environment variables

| variable                    | effect                               |
|-----------------------------|--------------------------------------|
| `XS_MAX_DEPTH`              | interpreter call-depth cap (default 500) |
| `XS_JIT_CODE_SIZE_MB`       | JIT code buffer (default 4)          |
| `XS_LIMITS_INSTRUCTIONS`    | hard cap on bytecode op count        |
| `XS_LIMITS_MEMORY_MB`       | RSS cap                              |
| `XS_LIMITS_WALL_SECONDS`    | wall-clock cap                       |
| `XS_GC_TRIGGER_PCT`         | gen-0 collection threshold (% heap)  |
| `XS_LIB_PATH`               | colon-separated package search paths |
| `XS_EXPERIMENTAL`           | unlock `experimental.*` modules      |

The resource limits are the same machinery the embedding API uses;
see [Embedding XS in C](./embedding.md).

## Error codes

Every diagnostic has a stable code (`L0xxx` lex, `P0xxx` parse,
`T0xxx` type, `S0xxx` semantic). To see the full explanation:

```sh
xs --explain T0008
```

Comes back with a description, a minimal failing example, and the
standard fix. Useful when the inline diagnostic gives just a one-line
hint and you want the depth.

## Build feature flags

If you build XS yourself, every feature is opt-out via Make flags.
Disable what you don't need to shrink the binary:

```sh
make release XSC_ENABLE_JIT=0          -- no JIT, ~200 KB smaller
make release XSC_ENABLE_TRANSPILER=0   -- no js/c/wasm emit
make release XSC_ENABLE_DAP=0           -- no debug adapter
```

A bytecode-only build with no JIT, no transpilers, no DAP, no
profiler weighs in around 850 KB. The full-feature release build is
~2 MB.

`XSC_ENABLE_PLUGINS=0` is the most aggressive: it removes the plugin
loader entirely. Useful for security-sensitive embedding hosts.

## REPL

`xs` with no arguments starts the REPL. Multi-line input continues
on lines ending with `{`, `(`, `[`, or `\`. History persists in
`~/.xs_history`.

REPL meta-commands all start with `:`:

| command            | does                                         |
|--------------------|----------------------------------------------|
| `:help`            | list commands                                |
| `:env`             | list bindings with inferred types            |
| `:type <expr>`     | show inferred type                           |
| `:ast <expr>`      | dump AST                                     |
| `:time <expr>`     | evaluate and show wall time                  |
| `:doc <name>`      | inline docs                                  |
| `:load <file>`     | source-load a file into the session          |
| `:save <file>`     | save history to file                         |
| `:reset`           | clear bindings, fresh interpreter            |
| `:test [pattern]`  | run tests matching pattern                   |
| `:tour`            | print the embedded language tour             |
| `:q` / `:exit`     | leave                                        |

Errors do not kill the session; the broken expression rolls back and
you keep your bindings.
