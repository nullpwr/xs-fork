# Performance

XS ships three execution backends, switched at the command line:

| flag        | backend            | startup | hot loop | notes                   |
|-------------|--------------------|---------|----------|-------------------------|
| `--interp`  | tree-walking       | ~3 ms   | slow     | needed for some plugins |
| `--vm`      | bytecode VM        | ~5 ms   | medium   | the default             |
| `--jit`     | tier-2 native JIT  | ~5 ms   | fast     | x86-64 + arm64          |

The default is the VM. JIT kicks in adaptively for hot protos
(threshold scales with bytecode length: 25 calls for tiny functions,
200 for big ones).

## Where XS is fast today

From the cross-runtime benchmark suite (`make bench`):

- **Sort, json round-trip, string munging**: faster than CPython,
  often by 2-5×.
- **SHA-256 hashing**: faster than CPython, comparable to Node.
- **Startup**: under 5 ms. CPython is ~15 ms, Node ~50 ms.

## Where XS is slow today

- **Recursive integer math** (fib, ackermann). The function-call
  overhead and the absence of speculative inlining show.
- **Tight float loops** (mandelbrot, raytrace inner loops). The JIT
  doesn't yet unbox doubles; every operation goes through the
  tagged-Value path.
- **Hot allocation paths**. Each new `[..]` or `#{...}` walks the
  generational allocator. Reusing arrays helps.

## Tuning knobs

Environment:

| variable                    | default | purpose                       |
|-----------------------------|---------|-------------------------------|
| `XS_JIT_CODE_SIZE_MB`       | 4       | JIT code buffer size          |
| `XS_LIMITS_INSTRUCTIONS`    | unset   | hard cap on bytecode op count |
| `XS_LIMITS_MEMORY_MB`       | unset   | RSS cap                       |
| `XS_LIMITS_WALL_SECONDS`    | unset   | wall-clock cap                |

CLI:

```sh
xs --vm   file.xs       # explicit VM, no JIT
xs --jit  file.xs       # force JIT consideration on every proto
xs --strict file.xs     # type checking on, may error
xs --check  file.xs     # type-check only, don't run
```

## Profiling

```sh
xs --profile file.xs > profile.json
xs --profile-flamegraph file.xs > flame.svg
```

The profiler samples at 1 ms via `SIGPROF` (or `CreateTimerQueueTimer`
on Windows). Output is folded-stack format, compatible with FlameGraph
tools.

## Benchmark suite

```sh
make bench                         # one run, prints a markdown table
make bench-compare                 # diff against committed baseline
TOLERANCE=0.10 make bench-compare  # tighter regression gate
```

Adding a bench: drop a `bench_xx.xs` plus matching `bench_xx.{py,js,go}`
into `benchmarks/`, register the entry in `run.sh`. The CI gate uses
`benchmarks/baseline.json` as the reference.

## Optimisation tips

- **Avoid allocating in hot loops**. Pre-create the array, reuse it.
- **Type the hot path**. `xs --check` is honest about where it fell
  back to dynamic dispatch; annotations let the JIT specialise.
- **Use the VM, not the interpreter**. The interpreter exists for
  plugin debugging; the VM is meant for production.
- **Watch the GC**. `gc.stats()` shows allocation pressure;
  `gc.set_threshold(0, 5000)` raises the gen-0 trigger if you want
  fewer collections at the cost of more peak RAM.
