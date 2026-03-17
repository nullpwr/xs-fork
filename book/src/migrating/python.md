# Migrating from Python

Most Python idioms have a direct XS equivalent. Pay attention to:

- **Strings are byte-oriented**. `s.len()` counts bytes; for codepoints
  use `s.chars().len()`.
- **`map`/`filter` exist on arrays directly**. No need for
  `list(map(...))`.
- **`{}` is a block, not a dict**. The map literal is `#{...}`.
- **No `self` parameter on instance methods** — XS infers it from the
  context, but you write `self.x` explicitly inside the body.

## Cheat sheet

| Python                      | XS                              |
|-----------------------------|---------------------------------|
| `len(x)`                    | `x.len()`                       |
| `print("a", "b")`           | `println("a", "b")`             |
| `f"hi, {name}"`             | `"hi, {name}"`                  |
| `[x for x in xs if p(x)]`   | `xs.filter(p)`                  |
| `[f(x) for x in xs]`        | `xs.map(f)`                     |
| `dict(a=1, b=2)`            | `#{a: 1, b: 2}`                 |
| `tuple([1, "a"])`           | `(1, "a")`                      |
| `if x is None:`             | `if x == null {`                |
| `x or default`              | `x ?? default`                  |
| `try / except / finally`    | `try / catch / finally`         |
| `raise ValueError("x")`     | `throw "x"`                     |
| `yield x`                   | `yield x` (in `fn*`)            |
| `async def f():`            | `async fn f() {`                |
| `await coro`                | `await coro`                    |
| `with open(...) as f:`      | `with open(...) as f { ... }`   |
| `class Foo: ...`            | `class Foo { ... }` or `struct + impl` |
| `@dataclass`                | `struct Foo { ... }`            |
| `from x import y`           | `import { y } from "x"`         |
| `import json; json.dumps(d)`| `import json; json.stringify(d)`|

## Where XS departs

### Pattern matching by default

```xs
match shape {
    Circle(r)    -> 3.14 * r * r,
    Rect(w, h)   -> w * h,
    Triangle(b, h) -> 0.5 * b * h,
}
```

Python 3.10's `match` is similar but XS treats it as the primary
control-flow construct. Most idiomatic XS replaces `if/elif/else`
chains with `match`.

### Static-ish typing

```xs
fn parse_int(s: str) -> int? {
    let n = s.to_int()
    if n == null { return null }
    return n
}
```

Annotations are optional, but `xs --check` treats them as enforced.
`xs --strict` requires them on every public surface. Pick whichever
gives you the right balance of "Python ease" vs "Rust catch".

### Effects

Python has none. The XS [effects chapter](../effects.md) is the
biggest mental shift; for migration, you can ignore them and write
pure imperative code, but they're worth learning.

## Performance

XS is faster than CPython on most non-recursive workloads (see
`benchmarks/`). For recursive number crunching CPython is faster
because of `sys.setrecursionlimit` games and simpler dispatch. For
the JIT-friendly hot paths (sorting, json, hashing, string munging)
XS wins by 2-5×.

## What's missing vs Python

- **Numpy/Pandas equivalent**: not yet.
- **Decimal**: there's no built-in; `xs-decimal` (third-party) is on
  the roadmap.
- **`asyncio.gather`-style helpers**: use `nursery` blocks, which are
  closer to `anyio`.

## A small port

Python:

```python
import json, sys
from collections import Counter

text = sys.stdin.read()
words = [w.lower() for w in text.split() if w.isalpha()]
top = Counter(words).most_common(10)
print(json.dumps(top))
```

XS:

```xs
import io
import json
import collections

let text = io.stdin_read()
let words = text.split(" ").filter(|w| w.is_alpha()).map(|w| w.lower())
let counts = collections.counter(words)
let top = counts.entries().sort_by(|a, b| b.1 - a.1).slice(0, 10)
println(json.stringify(top))
```
