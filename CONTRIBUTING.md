# Contributing to XS

Short on ceremony, long on what actually needs to be true before a PR lands.

## Build

```
make            # default: -O2, feature flags all on
make debug      # -g -O0 with AddressSanitizer + UBSan
make release    # -O3, LTO, stripped
```

You need gcc or clang and GNU make. No other dependencies.

## Run the tests

```
make test        # all 7 layers via tests/run-all.sh
make test-asan   # the layer-2 behavioural suite under AddressSanitizer + UBSan
make test-unit   # just the C-level unit tests (tests/unit/*_test.c)
```

The 7 layers are unit, end-to-end behavioural (`tests/run.sh`),
negative, property, golden, regression, and conformance. The
behavioural layer runs each `test_*.xs` through `--interp`, `--vm`,
and `--jit`, then diffs the stdout: backend divergence fails the test
even if each backend passes on its own. This is how silent VM/JIT
pattern-match bugs used to slip through, and why the runner was
changed.

The adversarial suite (deep recursion, huge strings, pathological
patterns, stdin pipe bugs, unhandled-throw exit codes) rides along
with layer 2. To trigger it on its own:

```
bash tests/adversarial/run.sh
```

Before you open a PR, `make test-asan` should be clean. CI will run it
anyway, but catching sanitizer reports locally is faster.

## Fuzz the parser

```
make fuzz-parser                    # needs clang with -fsanitize=fuzzer
./fuzz_parser -max_len=65536 tests/
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

## Where to look

- **VM and bytecode**: `src/vm/` (compiler.c, vm.c, bytecode.h).
- **JIT driver**: `src/jit/jit.c` (mmap/VirtualAlloc the code buffer,
  run the tier-2 pipeline, cache `proto->jit_entry`).
- **JIT pipeline** (register-allocating, x86-64 and aarch64):
  `src/jit/ra_ir.h` defines the small IR; `ra_lower.c` turns bytecode
  into it (plus self-recursive inliner, compare/branch fusion, and
  refcount-pair elimination peepholes); `ra_live.c` does per-block
  liveness; `ra_alloc.c` does linear-scan over three callee-saved
  regs; `ra_codegen.c` emits x86-64 and `ra_codegen_arm64.c` emits
  AArch64. Widening `op_supported` in `ra_lower.c` is the fastest way
  to bring more protos into native code. Unsupported-proto fallback
  goes straight to the bytecode VM -- there is no template-JIT middle
  tier.
- **Sema / type checker**: `src/semantic/`, `src/types/`.
- **Transpilers**: `src/transpiler/`.
- **Plugins**: `src/plugins/`.

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
