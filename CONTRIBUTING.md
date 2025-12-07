# Contributing to XS

Thanks for wanting to help out. This doc is short on ceremony and long on
what you actually need to know.

## Build

```
make            # default: -O2, feature flags all on
make debug      # -g -O0 with AddressSanitizer + UBSan
make release    # -O3, LTO, stripped
```

You need gcc or clang and GNU make. No other dependencies.

## Run the tests

```
make test        # tests/run.sh: 35+ test_*.xs files + examples + CLI
make test-asan   # the same suite under AddressSanitizer + UBSan
make test-diff   # cross-backend diff: interp vs VM vs C vs Node (JS)
```

Adversarial suite (deep recursion, huge strings, pathological patterns,
stdin pipe bugs, unhandled-throw exit codes):

```
bash tests/adversarial/run.sh
```

Before you open a PR, `make test-asan` should be clean. CI will run it
anyway, but catching sanitizer reports locally is faster.

## Fuzz the parser

```
make fuzz-parser                    # needs clang with -fsanitize=fuzzer
./fuzz_parser -max_len=65536 tests/ examples/
```

Run it for a weekend and file anything it finds.

## Adding a test

Drop a file in `tests/` following `test_*.xs`. The runner picks it up
automatically. Tests use `assert(cond, msg?)` and `assert_eq(a, b)`;
a file fails if any assertion fires.

If you're adding a test for a bug you hit, also add a minimal repro in
`tests/adversarial/` so future regressions get caught under sanitizers.

## Code style

- C11. 4-space indent, no tabs.
- Short functions. If you need five levels of nesting, extract.
- Span-track every AST node so error messages stay precise.
- When you add a feature flag in the Makefile, wire it through the
  existing `$(foreach f, ... XSC_ENABLE_$(f))` pattern so debug/release
  stay in sync.
- No em dashes, no filler boilerplate, no commentary that restates the code.

## Good first issues

These are small, self-contained places to start:

- Add a missing method to a stdlib module (see `src/runtime/builtins.c`,
  make\_\<mod\>\_module). Follow the existing signatures.
- Fix a `TODO` in `src/vm/vm.c` or `src/transpiler/*.c`.
- Add an adversarial test case for a footgun listed in STATUS.md.
- Improve a diagnostic message so the hint points the user at the fix.
- Trim a deprecated code path the runtime no longer reaches.

Grep `TODO` and `FIXME` in `src/` for 60+ starting points.

## Opening a PR

1. Fork, branch from `main`.
2. `make test-asan` locally.
3. Open the PR against `main`. The CI pipeline runs the default suite
   on Linux / macOS / Windows plus an ASan + UBSan job; wait for it.
4. If you're changing a backend, run `make test-diff` and paste the
   result in the PR description.
5. Explain the motivation in prose. One-line commit messages that just
   say "fix foo" make reviews slower.

## Scope

Please read STATUS.md before proposing a new feature. The project is
pre-1.0 and the maintainers are deliberately cutting scope. Bugfixes,
stdlib completeness, and backend consistency are always welcome.
Syntax additions need a clear reason.

## That's it

No CLA, no lengthy process. Write good code and don't break stuff.
