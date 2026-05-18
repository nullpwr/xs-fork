# Changelog

## 1.2.15

Four cross-backend divergences found by a strict probe matrix beyond
the conformance suite.

Closures that pass through more than one nested function now forward
captures all the way through. Both transpilers walked only one level
of the lexical chain, so `fn outer() { var x; fn middle() { fn inner()
{ x } } }` returned the wrong value or failed to compile because the
middle frame never saw `x` at all. The C path's free-var collector
now recurses into nested lambda bodies and forwards captured names
through `__env`; same fix for WASM.

WASM's enum encoding is now uniform: every variant lowers to
`[tag, args...]` and pattern matching dispatches on `arr[0]`. Before,
zero-arg constructors and constructor calls took different paths and
the call form trapped silently, which made `Maybe::Some(x)` produce
no output at all.

C transpiler stopped wrapping bigint literals to i64. A literal like
`99999999999999999999` now lowers to `XS_BIGINT("...")` instead of
truncating to whatever fits in `int64_t`. The overflow detection on
arithmetic also moved off raw signed-overflow comparisons (UB at
`-O2`) to `__builtin_add_overflow` / `__builtin_mul_overflow`, so
`10 ** 30` promotes to bigint cleanly under any optimisation level.

The strict probe matrix (every program through every backend, byte-
for-byte against interp, with the C path checked at both `-O0` and
`-O2`) is now clean. All 17 conformance tests + the full regression
corpus stay 17 / 17 / 17 across `--emit c`, `--emit js`, `--emit
wasm`.

## 1.2.14

The C transpiler caught up with JS and WASM: every conformance test
now compiles with gcc and matches interpreter output byte-for-byte.

Generators (`fn*`, `yield`, `g.next()`) lower to an eager array fill,
the same shape the WASM path uses, with a small `__gen_iter` /
`__gen_next` runtime spliced into the program. `async fn` / `await`
/ `spawn` / `nursery` strip to plain functions and direct calls so a
single-threaded compiled program runs the same observable behaviour
as the scheduler-backed runtime. Nested fn declarations get rewritten
to `let f = fn() {...}` so closures over the enclosing locals work,
including mutual recursion through trait-default machinery.

`try` / `catch` lifts to an expression form so its arm value
propagates the way it does under interp; `defer` runs on both throw
and return, not just normal fall-through. Struct match patterns
(`Point { x, y }`) recognise instances by name because the
constructor tags `__type__`; trait default methods get copied onto
every impl that doesn't override. Cross-file `use "./mod.xs"`
inlines the imported file with renamed bindings and exposes the
public surface as a namespace map; `arr.reduce(init, fn)` matches
the interp signature; bigint promotion uses decimal-string add /
mul / pow so `9223372036854775807 + 1` lands on
`9223372036854775808` instead of wrapping. Tolerant `assert_eq`
for chained float arithmetic, the same slack as the native runtime.

Every backend (`--vm`, `--interp`, `--jit`, `--emit c`, `--emit js`,
`--emit wasm`) is now at 17/17 conformance. First time they're all
at parity.

## 1.2.13

The WASM AOT path (`xs --emit wasm`) used to silently produce wrong
output for most non-trivial programs. The whole audit landed: closure
write-through, byte-wise string equality, UTF-8 codepoint `.len`,
struct match by type name, indirect closure calls, in-place map
updates, top-level globals, mutual nested fns, `defer` with closure
capture, receiver-aware method dispatch with trait default methods,
map patterns, higher-order array methods (`.map` / `.filter` /
`.reduce` / `.fold` / `.each` / `.some` / `.every` / `.find` /
`.sort_with` / `.flat_map` / `.group_by` / `.partition` / `.sum` /
`.product` / `.min_by` / `.max_by` / `.count`), codepoint-aware
`.chars` / `.lines` / `.lower` / `.trim` / `.split` / `.replace`
/ `.sort`, deep array equality, `.starts_with` correctness, bigint
with arbitrary precision, tolerant `assert_eq` for chained float
arithmetic, stdlib `import math / json / fs / time`, cross-file
`use "./mod.xs"` with `as` rename and selective destructure,
generators via eager array fill, `async` / `await` / `spawn` /
`nursery` resolved synchronously in a single-threaded module,
algebraic effects (`perform` / `handle` / `resume`) lowered through
try-throw and global handler slots, reactive `bind` lowered to a
one-shot let.

The full 17-test conformance suite now runs end-to-end through
`wasmtime` and matches the interpreter byte-for-byte. The
`make wasm` runtime build remains the production browser path;
`--emit wasm` is finally a viable AOT path for real programs.

## 1.2.12

JS transpiler stopped crashing on six conformance cases. Trait default
methods get copied onto every impl that doesn't override them, so
`square.name()` returning `"shape"` works the same under `--emit js`
as under the interpreter. Struct match patterns (`Point { x, y, .. }`)
recognise instances by name because the constructor tags
`this.__type__`; class declarations get the same field. Cross-file
`use "./mod.xs"` actually emits something now: the imported file
inlines into an IIFE and the result binds to the namespace alias,
the `as` rename, or the selective destructure -- whichever shape the
use site asked for. Field reads on missing keys return `null` instead
of `undefined` so XS-style `m.k == null` checks match the runtime.
`assert_eq` does the same tolerant float compare as the native
runtime, so chained arithmetic across shapes still matches a literal.
Top-level `await` inside an `assert_eq` arg auto-wraps the assertion
IIFE as an awaited async function. The prelude's builtin `range` now
lives on `globalThis` so a user-declared `fn* range` doesn't trip
"Identifier already declared". Effect handlers that don't call
`resume` terminate the handle block with the arm's value instead of
looping forever.

The exhaustiveness analyser stopped warning on `(a, b)` and
`Point { x, y, .. }`; both are catchall for their shape.

## 1.2.11

Cross-file `use` actually does something on the VM now -- it was a
no-op with a "for now" comment. The bytecode compiler hands off to a
new `__use_file` native that compiles the imported file with the VM
compiler and runs it on a child VM, so the resulting closures invoke
through the parent. Module-method dispatch on a struct constructor
inside a loaded module (e.g. `lib.Point { x: 3 }`) constructs an
instance instead of erroring with no-method.

Visibility is one mechanism now: `export { name, name as alias, ... }`
at the top level of a file. `pub` and `@export` are gone; using either
raises P0054 with a one-liner pointing at the new shape. A file with
no `export` list at all falls back to exposing every top-level
binding, so quick scripts and throwaways stay zero-ceremony.

`@deprecated("msg")` actually warns its callers now -- the message
was parsed and parked on the AST but no one read it. Sema emits W0001
at every call site of a deprecated fn.

C transpiler grew per-arm effect handler dispatch, structured runtime
errors, comparator support in `arr.sort_with`, predicate dispatch on
`find` / `index_of`, the `del` tombstone semantics, map-spread, and
explicit refusals for `bind` and the wrapping decorators. JS
transpiler grew the same `del` tombstone, range methods, the bare
builtins prelude, tuple-vs-array shape, and a parallel set of
refusals.

`make` now routes every `.o` / `.d` through `build/obj/` so the source
tree stays clean. `make clean` sweeps any stray root-level artefacts
that previous builds left behind.

## 1.2.10

`xs upgrade` and `xs uninstall` fold the old xsi installer's job
into xs proper. The xsi binary is gone; the install one-liner now
fetches the xs binary directly. `xs repl` (and bare `xs` with no
args) drops into an interactive read-eval loop with persistent
bindings, multi-line input, and `:help :quit :env :clear :t`
meta-commands.

`assert_eq` raises a catchable `AssertionError` instead of calling
exit, so `try { assert_eq(...) } catch e { ... }` works. Pulled
the inline temporal block forms (`every 1s { ... }`, `after`,
`timeout`, `debounce`) since they never actually scheduled the
body; the `@every` / `@after` / `@cron` decorators stay and are
the way to do this. Also fixed `import log` colliding with the
math `log` builtin, `Set.has(x)` for non-string elements,
`arr.sorted()` as a VM method, and `reflect.type_of` for struct
values built via the VM.

VM `del x` now tombstones the slot so subsequent reads throw
(was binding to null silently). Struct match patterns reject
mismatched types instead of binding fields to null on a sibling
shape, so `match shape { Circle { radius } => ... }` no longer
fires on a `Rect`. macOS Apple Clang build under `-Wenum-conversion`
and `-Wenum-compare-conditional` is fixed; POSIX builds include
`<sys/select.h>` explicitly.

## 1.2.9

Patch release that filled in `xs --help` to actually list
`upgrade`, `uninstall`, `publish`, and `search`, and regenerated
the VS Code extension icons (the marketplace icon plus the .xs /
.xsc file icons) in the new sage-on-dark style.

## 1.2.8

The pipe-to-free-function pattern (`nums |> reduce(0, fn)`) was
emitting JS that referenced a top-level `reduce` that doesn't
exist, so Node errored with `ReferenceError: reduce is not
defined` even though the same XS source ran fine on the VM.
Added `__xs_reduce` / `map` / `filter` / `each` / `some` /
`every` / `find` / `count` / `sum` polyfills to the JS prelude
and routed free calls of those names through them. The helpers
forward to the corresponding Array prototype method, and the
reduce / fold helpers sniff which arg is callable so both
`(init, fn)` and `(fn, init)` orderings work. Method-form
(`arr.reduce(...)`) was already correct and is unchanged.

## 1.2.7

`xs fmt` was emitting every NODE_BINOP as `left op right` with no
parentheses, so `(x ?? 1) + 2` round-tripped to `x ?? 1 + 2`.
Those don't parse the same: the second form groups as
`x ?? (1 + 2)` because `??` has prec 3 and `+` has prec 12, so a
non-null x makes the program return `x` instead of `x + 2`.
Silent semantic change. fmt.c now mirrors parser.c's `prec_of()`
and parenthesises a binop child whose precedence is lower than
the parent (left side) or lower-or-equal (right side; XS binops
are all left-associative). Idempotent on already-formatted
output.

The bytecode VM materialises generators eagerly: OP_YIELD pushes
into a per-frame array and the body runs to completion. Infinite
generators (`while true { yield ... }`) used to climb forever
and OOM-kill the process. Capped at 1M values with a
GeneratorOverflow that points at `--interp` (which already runs
generators on a real worker thread with channel handoff).
Lazy yield/resume on the VM is tracked for v1.3.

## 1.2.6

A plugin that uses `plugin.lexer.add_keyword` /
`plugin.parser.on_unknown` / `plugin.parser.on_postfix` /
`plugin.parser.override` / `plugin.lexer.transform` registered the
hooks under `--vm` and `--jit`, but the main program had already
been parsed before the `load` ran, so the new keyword token never
went through `on_unknown` and the constructed AST node was never
spliced into the program. The interp's re-parse loop is the only
place that reflows the source after a plugin registers handlers,
so syntax-extending plugins now force the interp fallback the same
way runtime-hook plugins (`after_eval`, `before_eval`, `ast_hook`)
already did. Plugins that only call `plugin.runtime.global.set` or
`plugin.runtime.add_method` keep running on the VM at full speed.

## 1.2.5

`xs run file.xs foo bar` was swallowing the trailing args -- the
`run` subcommand's `goto run_file` jumped past the line that
populates `os.args`. Fixed in both the `.xs` and `.xsc` branches
so `xs run` matches the bare `xs file.xs ...` form.

Also a documentation cull: removed the book/, RFCS/, packages/,
API.md, and POLICY.md trees. The book carried out-of-date API
references (`os.argv` instead of `os.args`, `fs.read_text` instead
of `fs.read`, `string.split_re` instead of `re.split`, a fictional
`test.describe` / `test.it` API), RFCS hadn't been touched in
months, the in-repo packages/ duplicated the registry copies and
was drifting, API.md repeated content from `src/xs_embed.c`'s doc
comments with a few wrong entries, and POLICY.md described a
sandbox that doesn't exist. The remaining surface (README,
LANGUAGE, COMMANDS, PLUGINS, STATUS, CHANGELOG, CONTRIBUTING) is
the canonical reference.

## 1.2.4

Two paper cuts from the post-1.2.3 bug list.

`--emit wasm` now writes the binary to stdout when stdout isn't a tty,
matching `--emit js` and `--emit c`. So
`xs --emit wasm fib.xs > fib.wasm` Just Works. Interactive runs still
drop a fresh `out.wasm`, with the status line moved to stderr.

`xs test <dir>` no longer prints "0 passed, 0 failed, 0 total" and
exits 0 when the directory has `.xs` files but none match
`test_*.xs` or `*_test.xs`. The runner reports
"No tests found in <dir>" and the expected filename patterns,
matching the per-file "No #[test] functions found" hint that was
already there.

## 1.2.3

Three bugs that came up after 1.2.2 shipped.

The HTTP response status line was always rendering "OK" as the reason
phrase regardless of code -- a 404 came out as `HTTP/1.1 404 OK`. The
format string in `builtins_http.c` was hardcoded; the formatter now
runs through `http_status_text(code)` so the phrase tracks the code.

`@every` / `@cron` / `@watch` / `@on_signal` triggers stopped firing
once a script entered `http.serve` because the accept loop never
yielded back to the trigger event loop. The accept loop now uses a
100ms `select` timeout and pumps `trigger_pump_due` (one pass each
of `pump_signals` + `check_watches` + `fire_due`) every iteration.

The wasm bump allocator was dropping `memory.grow`'s return value and
only ever growing one page per call. Tight recursion like `fib(32)`
under Node WASI walked off the end of linear memory and segfaulted in
the host. The grow path now loops until the new `heap_ptr` fits, traps
explicitly via `unreachable` if `memory.grow` returns -1, and the
initial memory section moved from 2 pages to 16 (1 MB) so the common
case skips the grow entirely. `fib(32)` completes correctly under
both Node WASI and wasmtime.

Plus: `build_plugin_map` is now in `interp.h` and the VM's plugin
loader uses it, so `plugin.lexer` / `plugin.parser` / `plugin.hooks`
/ `plugin.ast` are available under `--vm` and `--jit` (previously
only `--interp` exposed them; `plugin.parser.on_unknown(...)` would
crash on the default backend).

## 1.2.2

Big surface-level pass on the language to push parity across all four
backends and add the long-promised wrapping decorators.

**`__str__` / `value_repr`.** Instances now print through `__str__`
when defined; `value_repr` produces the canonical form that round-trips
through `xs_eval`. `str.format("{}", obj)` and `println(obj)` agree.

**Wrapping decorators.** `@memoize`, `@retry`, `@trace`, `@timed`
work on every backend. The compiler builds a wrapper map carrying
`_wrap_kind` + `_wrap_fn` + decorator args, and `call_value` /
`OP_CALL` recognise it before reaching for the underlying fn.

**VM typed-collection methods + bigint promotion.** `OP_METHOD_CALL`
dispatches on the receiver's tag instead of falling through to a
generic native, picking up `arr.sum`, `arr.fold`, `m.entries`,
`s.chars` etc. directly. Integer arithmetic that overflows promotes
to `bigint` rather than silently wrapping.

**Predicate-aware methods + alias fan-out.** `arr.find(p)`,
`arr.partition(p)`, `arr.first / last / take_while / drop_while`
take callable predicates everywhere. `arr.head` / `arr.tail` /
`arr.init` are aliases for the natural shape on every backend.

**JS transpile foundation + second pass.** Stdlib polyfills for
`json`, `math`, `time`, `random`, `collections`, plus `range`. Format
spec `f"{x:0.2f}"` lowers through `__xs_fmt_float`. Match arm guards,
tuple/enum patterns, trait dispatch (`__traits` map), `arr.partition`
/ `arr.zip` / `arr.chunk` / `arr.window` / `arr.scan`, `Set` wrapper
with insertion order, `str.lines` / `str.indent` / `str.dedent`
shaped exactly like the C runtime.

**C transpile foundation.** `arr.first/head/last/tail/init`,
`arr.sum/product/avg`, `take`/`drop`/`unique`/`enumerate`,
`fold(init, fn)` (alongside `reduce` with the args-swapped ordering),
`str.bytes` / `str.reverse`, `parse_int` routed through
`xs_conv_to_int`. The `xs_to_str` rotating buffer pool grew from 8
to 256 entries so deep nesting (lists of tuples) no longer wraps
back onto the outer buffer. `math.*` / `json.*` / `m.get/set/clone`
dispatch matches the interp.

**Transpile parity sweep.** Closures over let-rebound captures copy
the slot at lambda emission rather than reading the slot post-hoc.
Defer ordering inside try/catch matches the runtime. `arr.group_by`
returns a Map (keeps insertion order; the prior plain-object form
lost numeric-key ordering). `yield*` for delegated iterables.
`{ ..a, x: 1 }` spread preserves order. `/.../` regex literals route
to the embedded Thompson NFA on both targets.

**VM bugs the parity sweep flushed out.** `OP_CALL_KW` raises on an
unknown kwarg. Channel buffered send + close drain in the right order
under contention. Nursery cancellation rethrows in a sibling task that
was inside a defer. BigInt sign on mixed-sign multiply / divmod.
Merge-sort stability tightened with `<=` on the left-half compare.

**JS shadow-fix pass.** The "fast path" for known-shape receivers
(`arr.map` -> `Array#map`, `s.split` -> `String#split`, etc.) now
probes `typeof __o.method === 'function'` first, so a user type
defining `map` / `split` / `replace` / `to_str` / etc. wins over the
host built-in.

**bug055 portability.** The VM-method-error span regression test now
skips on `wasi` (no fork, no `/tmp`) and `windows` (the inline
`NO_COLOR=1 ./xs ...` form cmd.exe doesn't parse), and sets
`NO_COLOR` via `os.setenv` so the child inherits it portably. Added
`os.platform == "wasi"` detection in `builtins_os.c`.

## 1.2.1

Cleared the v1.2 deferred queue plus a friend-feedback batch.

**`--emit c` effects.** Single-shot perform/resume now lower through
setjmp/longjmp + a GCC nested-function dispatcher that closes over
the enclosing scope. Multi-shot resume on the C target still needs
delimited continuations and is tracked for v1.3.

**JS target.** 9/9 of the original tracked conformance corpus passes.
BigInt-aware `__xs_add`/`__xs_arith`/`__xs_pow`/`__xs_eq` now carry
overflow promotion, try-as-expr returns the body's trailing value,
`.to_str` / `.to_int` / `.to_float` map to the obvious JS conversions,
and compile-time float-divide detection means `0.0/0.0` evaluates to
NaN instead of throwing.

**VM merge sort.** Closed the O(n²) cliff that hit any `arr.sort(|a,b| ...)`
once the array got past a few thousand elements.

**Friend-feedback batch.**
- `timeout` is contextual now so `timeout = 30` doesn't shadow the
  block keyword.
- `@on_exit` runs on `os.exit` / `process.exit` instead of being a
  no-op.
- Float repr keeps the `.0` (`println(1.0)` -> `1.0`, not `1`).
- `@test` is an alias for `@example` so the natural decorator name
  works.
- Struct-variant exhaustiveness covers the variant fields, not just
  the enum head.
- `m.delete(k)` actually removes the key.
- HTTP headers are lowercased before lookup so `headers["Content-Type"]`
  and `headers["content-type"]` both work.
- `arr ++ [x]` produces a flat array on VM/JIT (was nesting `[x]`).
- `xs --emit jit-ir` dumps the lowered IR per proto for eligibility
  decisions.
- Lint surfaces `shadowed-builtin` cleanly.
- `jit.h` doc sweep.

**Multi-shot resume + nested perform** now produces the cartesian
product across VM and JIT. Compiler plumbs `handle_local_base` through
`OP_EFFECT_HANDLE` -> `TryEntry` -> `EffectCont` so the snapshot
captures only `[arm_local_base, arm_sp_off)`; outer-arm restore on
`OP_EFFECT_DONE` walks the eff_stack instead of stomping outer locals.
JIT `FRAME_SIZE` updated for the grown `TryEntry`.

**`for v in ch`** blocks until the channel is closed-and-drained on
both VM and interp. The VM was snapshotting the buffered length at
loop start; ITER_LEN now reports `INT64_MAX` for channels and
ITER_GET does a blocking recv. Interp's null check was missing the
`XS_NULL_VAL` singleton path and bound null once before exit.

**Embed C API.** `xs_eval` returns the trailing expression's value
(was always `XS_NULL`). `xs_call` routes through the parser via a
synthesized call expression instead of the segfault-prone direct
call_value path.

**Bug fixes.** `[v; n]` repeat literal works on the VM. Map int-key
round-trip on dynamic assignment. JS yield no longer escapes the
`function*()` wrapper for effect handlers. `xs doc <dir>` walks `.xs`
files and `xs doc <file>` emits `---` doc comments. f-string format
specs (Python-style: alignment, width, precision, type, comma).
`re.*` raises `RegexError` on bad regex; `re.split` rejects empty
pattern. `json.parse` raises on invalid input (use `json.parse_safe`
for the null fallback).

**Quiet JIT fallback.** The `--jit: native code emit failed` and
`native JIT unavailable` stderr lines now require `XS_VERBOSE_JIT=1`;
graceful fallback is silent by default.

**`--backend foo`** now errors instead of silently accepting unknown
backends.

## 1.2.0

Big subtraction + correctness release. Three rarely-used features
came out of the language; a pile of "documented but didn't actually
work" claims got either fixed or owned up to in the docs.

**Cuts.** Easier to learn, easier to type-check, smaller compiler.
- `adapt fn` (multi-target fn declaration form): gone. Anything it
  expressed can be a closure over a target switch.
- `signal()` / `derived()` / `subscribe()` / `effect()` reactive
  library: gone. The `bind` keyword covers the same use cases on
  every backend with less surface.
- `inline c { ... }` blocks: gone. Any program with one immediately
  failed to transpile to JS or WASM, so the portability story was a
  lie. FFI is still available via the `ffi` module.

**Real regex.** The Thompson-NFA engine in src/core/regex.c had been
shipping for releases but the user-facing `re.match` / `re.test` /
`re.find_all` / `re.split` / `re.replace` / `re.replace_all` /
`re.groups` natives were thin wrappers around POSIX ERE with a
hand-rolled `\d -> [0-9]` translator. Re-pointed all of them at the
real engine. `\d`, `\w`, `\s`, `\b`, non-greedy `*?` / `+?` / `??`,
non-capturing `(?:...)`, positive `(?=...)` and negative `(?!...)`
lookaheads all work now.

**JIT.** Stop bailing the entire proto on actor methods, module
construction, enum construction, send, kwarg calls, or import.
`OP_MAKE_ACTOR` / `OP_SEND` / `OP_MAKE_ENUM` / `OP_MAKE_MODULE` /
`OP_END_MODULE` / `OP_CALL_KW` / `OP_IMPORT` / `OP_IMPORT_ITEM`
now lower through `vm_step_jit` so calling code stays in tier-2
across `actor.method()`, `import math` etc. The proto-level
`is_actor_method` / `has_actor` bails are gone with them.
Closure-callback inlining for `arr.sort(|a,b| ...)` is still
deferred -- merge sort already kills the O(n²) cliff that was the
worst part of the perf trap.

**JS target.** Cleared the "rough" reputation:
- `==` / `!=` / `assert_eq` now use a structural `__xs_eq`. JS `===`
  is reference equality, which silently broke any conformance test
  that compared arrays / maps. (this caused 5/9 conformance test
  failures by itself)
- `let _ = expr` no longer emits `const _ = ...`, which would crash
  with `Identifier '_' has already been declared` the moment a
  function had two side-effecting wildcard lets.

**Doc sweep.** README claimed the tree-walker was the default
backend. It isn't, and STATUS.md said the opposite. Fixed the bash
example, the prose, and the backend-list ordering. book/cli.md and
book/performance.md no longer tell people to "use --vm by default";
the right rule is `--jit` for hot loops, default for everything
else, `--interp` for plugin debugging. Acknowledged in
book/effects.md that `--emit c` doesn't run effects yet (tracked for
v1.3) and `--emit wasm` is single-shot only. book/transpilers.md
gained a per-target "what works / what doesn't" section so the gaps
are visible at the surface, not just by reading source.

## 1.1.1

Stdlib modules (`math`, `os`, `fs`, `time`, `string`, `path`, `json`,
`http`, `re`, `crypto`, `db`, `random`, `process`, `net`, `async`,
...) now require an explicit `import name` instead of being
auto-bound on every interp / VM init. Constants (`PI`, `E`, `INF`,
`NAN`), Result / Option constructors (`Ok`, `Err`, `Some`, `None`),
the bare math fns (`sqrt`, `floor`, `sin`, ...), and the collection
helpers (`map`, `filter`, `sorted`, ...) stay top-level.

- `pause <duration>` and `del <name>` now run under the VM compiler
  (they used to print "unhandled node tag" to stderr and silently
  no-op).
- LSP `textDocument/documentSymbol` no longer trips the "selectionRange
  must be contained in fullRange" assertion that VS Code surfaced on
  incomplete source.
- Diagnostic colourizer learned the rest of the keyword set
  (`pause`, `del`, `every`, `after`, `timeout`, `debounce`, `do`,
  `with`, `use`, `plugin`, `actor`, `pure`, `inline`, `unsafe`,
  `tag`), highlights `@decorators` distinctly, and recolours
  `<number><unit>` sequences (`100ms`, `2m30s`, `5d`) as durations
  instead of "number followed by an identifier".
- Editor highlighters (vscode tmLanguage, tree-sitter grammar,
  helix / neovim / zed queries) get a dedicated capture for
  duration literals; the lingering `universal_literal` rule got
  trimmed down to the unit set the lexer actually accepts.
- `@on_panic fn report(err) { ... }` now parses (the parser used to
  reject the parameter even though the runtime calls the handler
  with the exception) and the unhandled-exception value is parked
  separately from `cf.value` so the handler actually sees it.
- Parser bug fix: `import math` followed by `import time` was
  being eaten as one import where the second `import` keyword
  looked like the bracketed-items continuation. Now we only
  consume the second token if a `{` actually follows.
- `examples/` removed; will be rebuilt separately.

## 1.1.0

Decorators land. A function declaration can now carry one trigger, an
optional @once, and any number of metadata markers, and the runtime
walks the resulting registry from a single event loop.

- `@on_start` / `@on_exit` / `@on_signal(name)` / `@on_panic` for
  lifecycle. on_start fires once after the top-level settles;
  on_exit and on_panic fire on the way out; on_signal supports
  `"INT"` and `"TERM"` portably plus `"HUP"` / `"USR1"` / `"USR2"` on
  unix.
- `@every(d)` / `@cron(spec)` / `@delayed(d)` for scheduling.
  Drift-free CLOCK_MONOTONIC ticks. The cron spec is the standard
  five-field POSIX form with `*` / `*/N` / `a-b` / comma lists.
- `@watch(path)` for filesystem changes. Linux uses inotify; other
  unixes fall back to a stat-poll against a cached snapshot.
- `@bench` / `@example` for discovery. They appear in the runtime
  registry so `xs bench` / `xs doc` can find them.
- `@export("name")` aliases the fn under a public name; sema sees
  the alias too so callers don't trip the undefined-name check.
- `@once` collapses with any repeating trigger and quiesces it after
  the first fire.

Time literals (`5s`, `100ms`, `1ns`, `2us`, `1m`, `1h`, `1d`) are
now native: no `use literals duration` pragma. They construct a
first-class `Duration` type backed by an int64 nanosecond count, so
arithmetic and comparisons work and `println` prints `2.5s` rather
than `2500`. The other domain literal families (color, date, size,
angle) were dropped to keep the Duration design clean.

The bytecode VM compiler emits a `__register_decorator` call after
each closure binds, so --vm and --jit see the same trigger registry
the interpreter does. The semantic resolver picks up `@export`
aliases as top-level symbols. Editor extensions (vscode, neovim,
helix, zed, jetbrains via tmlanguage, plus the tree-sitter grammar)
all carry decorator highlighting; the LSP surfaces the decorators
in completion. ~10 new regression tests under `tests/regression/`
cover the surface across interp / vm / jit.

## 1.0.0

First production release. The Tier 1 surface from `POLICY.md` is now
locked: language syntax, stdlib modules without `@unstable`, CLI flags,
and diagnostic shape do not break inside the 1.x line.

Highlights since 0.9:
- Multi-shot algebraic effects: `resume` may fire more than once and
  the second resume sees the captured stack, not the mutations the
  first resume left behind. Nested `perform`/`handle` pairs each push
  their own continuation onto a LIFO stack so an inner handler's
  resume lands on the inner body and the outer handler still has its
  own state to rewind to.
- Multi-arm `handle` finally dispatches by effect name. Previously
  every arm fell through to arm 0; now each arm gets a `DUP`/`EQ`
  check against the stacked effect name and the last arm acts as a
  catch-all so single-arm handlers keep working.
- JIT bails on actor methods (and any proto whose descendants
  contain one). The actor dispatcher passes an implicit `self` and
  state-field locals that the JIT prologue doesn't model, so the
  template VM-step path takes over and bug026 (actor + closure +
  outer var) now passes on both backends.
- `xs login` / `xs logout` / `xs whoami` for the registry. Token is
  stored at `~/.xs/credentials` (chmod 600). `xs publish` reads it
  as a fallback to `XS_REGISTRY_TOKEN`.
- `http.serve(port, router)` accepts a router map with `routes`,
  `middleware`, and `not_found`. Patterns support `:name` captures
  (populating `req.params`) and trailing `*` wildcards. Existing
  single-handler form keeps working.
- Windows registry CLI: `xs install` / `xs publish` / `xs search` /
  `xs whoami` now talk to the registry on Windows via a small raw
  socket + BearSSL HTTP client. Linking `-lws2_32` is already in
  the mingw branch of the Makefile.
- C transpiler: `println(a, b, c)` and `print(a, b, c)` now emit a
  proper sequence with single-space separators instead of dropping
  every argument after the first.
- pkg JSON field grabber rejects matches that occur inside string
  values (the previous parser would fish out the wrong substring
  when a description field happened to contain `tarball_url`), and
  caps decoded values at 8 MB so a malicious registry can't drive
  the CLI to OOM.

## 0.9.0

Multi-shot continuations groundwork; SSE/WS now route through TLS;
JIT lowers generators and shadowed locals; profiler caveats and
documented gaps in STATUS.md.

## 0.8.0

Tier-2 register-allocating JIT, inline caches for `LOAD_GLOBAL`,
soft-limit pointer in the VM struct, regex POSIX wrapper, Unicode
bytes spec, browser SDK at `static.xslang.org/xs.js`.

## 0.7.x

Pre-public cleanup: Python remnants removed, tests consolidated,
gradual typing enforced at runtime, plugin pipeline (load → eval →
parse override), HTTPS via embedded BearSSL, stdlib coverage filled
in (math, string, time, fs, os, io, fmt, json, csv, toml, http,
crypto, collections, re, net, db, ffi, reflect, gc, reactive).
