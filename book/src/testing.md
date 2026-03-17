# Testing

`xs test` discovers `tests/test_*.xs` files and runs them, with
per-backend execution (interpreter / VM / JIT) and per-platform CI
matrices.

## Writing a test

```xs
import test

test.describe("strings", || {
    test.it("uppers ASCII", || test.eq("hi".upper(), "HI"))
    test.it("uppers Unicode", || test.eq("é".upper(), "É"))

    test.before_each(|| println("setup"))
    test.after_each(|| println("teardown"))
})

test.run()
```

## Asserters

| call                 | passes when                       |
|----------------------|-----------------------------------|
| `test.eq(a, b)`      | `a == b`                          |
| `test.neq(a, b)`     | `a != b`                          |
| `test.truthy(v)`     | `v` is truthy                     |
| `test.falsy(v)`      | `v` is falsy                      |
| `test.throws(f, msg?)` | `f()` throws (with substring `msg`) |

For deep equal and richer asserters, the `xs-assert` package
(`packages/xs-assert/`) adds `deep_eq`, `near`, `includes`, and
context-aware messages.

## Running

```sh
xs test                    # everything in tests/
xs test --grep "string"    # only matching descriptions
xs test --backend vm       # force the VM
xs test --backend interp,vm,jit   # cross-check
```

The CLI runner lives in the `xs` binary. CI's
`tests/run-all.sh` orchestrates the seven layers (unit / e2e /
negative / property / golden / regression / conformance) across
backends. Setting `LAYERS="2 R"` runs only specified layers.

## Property tests

```xs
test.property("addition is commutative",
    test.gen.int(),
    test.gen.int(),
    |a, b| a + b == b + a)
```

`test.gen.*` provides shrinkable generators. The runner finds the
smallest failing case and prints the seed for reproducibility.

## Golden / snapshot tests

```xs
test.snapshot("query plan", explain(query))
```

First run records the output to `tests/__snapshots__/`. Subsequent
runs diff against the snapshot. `--update-snapshots` overwrites.

## Coverage

```sh
xs test --coverage
```

Writes a `coverage.json` plus an HTML report. Line and branch
coverage; integrates with `xs --emit cov-lcov` for tooling that
expects lcov format.

## Fuzzing

```sh
make fuzz                  # exercises the parser with libFuzzer
```

The fuzz corpus lives in `tests/fuzz/`. Reproducers from CI failures
land in `tests/fuzz/crashes/`.
