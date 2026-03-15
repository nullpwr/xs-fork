# XS benchmarks

Cross-runtime benchmark suite comparing xs (interpreter, bytecode VM,
tier-2 JIT) against CPython, Node.js, and Go on a fixed workload set.

## Running

```sh
make bench            # run everything, write results.json
make bench-compare    # run + diff against baseline, fail on regression
```

Environment knobs:

- `REPS=N`       wall-time is best-of-N (default 5)
- `WARMUP=N`    warmup iterations per bench (default 1)
- `TOLERANCE=X` allowed slowdown before `bench-compare` fails
                (default 0.25 = 25%)

## Benches

| file                  | what                                    |
|-----------------------|-----------------------------------------|
| `bench_fib.*`         | recursive fibonacci(30)                 |
| `bench_sort.*`        | quicksort of 1000 ints                  |
| `bench_mandelbrot.*`  | 200x200 mandelbrot, 500 iter cap        |
| `bench_nbody.*`       | 5-body solar system, 1000 steps         |
| `bench_json.*`        | 500-record json encode/parse round-trip |
| `bench_strings.*`     | build, split, and upper-case 500 words  |
| `bench_hash.*`        | sha-256 a 55 KB buffer 50 times         |
| `bench_startup.*`     | process spawn + `println "hi"`          |

Each bench has an `xs`, `py`, `js`, and `go` port that do the same
work. Any runtime missing from PATH is skipped.

## Baseline

`baseline.json` holds the committed reference numbers. Update it with:

```sh
make bench
cp benchmarks/results.json benchmarks/baseline.json
git commit -m "bench: new baseline"
```

CI reads it to fail PRs that regress xs timings beyond `TOLERANCE`.
