# The language tour

A quick read through every major feature.

## Bindings

```xs
let x = 42                  -- immutable
var y = "hello"             -- reassignable
const PI = 3.14159          -- compile-time constant
```

Type annotations are optional but enforced when present:

```xs
let x: int = 42
let names: [str] = ["a", "b", "c"]
```

`xs --strict` requires annotations on every public surface.

## Functions

```xs
fn add(a, b) { return a + b }
fn add_typed(a: int, b: int) -> int { a + b }     -- last expr is the value
fn greet(name, greeting = "hi") { println("{greeting}, {name}") }
fn sum(...xs) { xs.fold(0, |acc, x| acc + x) }
```

Closures use `|args| body`:

```xs
let inc = |x| x + 1
let names = ["alice", "bob"].map(|n| n.upper())
```

## Control flow

```xs
if score > 90 { "A" }
elif score > 80 { "B" }
else { "C" }

for i in 0..10 { ... }       -- exclusive
for i in 0..=10 { ... }      -- inclusive
while ready { tick() }
loop { if done { break } }   -- infinite

label: for x in xs {
    if x == target { break label }
}
```

## Pattern matching

```xs
match value {
    0 -> "zero",
    1..=9 -> "single digit",
    n when n % 2 == 0 -> "even {n}",
    [head, ...tail] -> "list of {tail.len() + 1}",
    Point { x, y } -> "({x}, {y})",
    _ -> "other",
}
```

Patterns are exhaustive: the compiler errors if you miss a case
on a closed `enum`.

## Data: arrays, maps, tuples

```xs
let arr = [1, 2, 3]
let m   = #{name: "ada", age: 30}
let t   = (1, "two", 3.0)
```

Arrays and maps are heap-allocated, refcounted, and grow as needed.
Tuples are fixed-size and stack-allocated when small enough.

## Structs, enums, traits

```xs
struct Point { x: float, y: float }

enum Shape {
    Circle(float),
    Rect(Point, Point),
}

trait Area {
    fn area(self) -> float
}

impl Area for Shape {
    fn area(self) -> float {
        match self {
            Circle(r) -> 3.14159 * r * r,
            Rect(a, b) -> (b.x - a.x) * (b.y - a.y),
        }
    }
}
```

`class` and inheritance are also available for OO-style code.

## Error handling

XS supports both `try`/`catch` *and* algebraic effects:

```xs
import http
import json

try {
    let body = http.get("https://example.com").body
    println(json.parse(body))
} catch e {
    eprintln("fetch failed: {e}")
}
```

[Effects](./effects.md) are a more expressive cousin: you `perform`
an effect, the surrounding `handle` block decides what to do.

## Concurrency

```xs
spawn { do_background_work() }            -- real OS thread

let answer = await fetch("/api/v1")       -- async/await

let ch = channel(64)                       -- typed channel
ch.send(item)
ch.recv()

nursery {                                  -- structured concurrency
    spawn { fetch_a() }
    spawn { fetch_b() }
}                                          -- waits for both
```

See [Concurrency](./concurrency.md) for the full menu.

## Pipes and null-coalescing

```xs
result = data |> normalise |> validate |> save
let name = user.name ?? "anonymous"
```

## Duration

```xs
let dur = 500ms                            -- 500 ms
let frame = 100us                          -- microseconds
let tick  = 1ns                            -- nanoseconds
let warmup = 2m30s                         -- compound
```

Numbers with `ns` / `us` / `ms` / `s` / `m` / `h` / `d` suffixes are first-class `Duration` values backed by an int64 nanosecond count. Arithmetic, comparisons, and the `.ns` / `.s` / `.ms` accessors all work; `println(2s + 500ms)` prints `2.5s`.

## Decorators

```xs
@on_start fn boot()        { ... }
@every(1s) fn tick()       { ... }
@cron("0 * * * *") fn hourly() { ... }
@delayed(500ms) fn warmup() { ... }
@watch("./config") fn reload() { ... }
@on_signal("INT") fn graceful() { ... }
@export("publicName") fn local_name() { ... }
@once @every(5s) fn one_shot() { ... }
```

Decorators answer "what triggers this function?" The runtime keeps
the process alive while any persistent trigger (`@every`, `@cron`,
`@watch`, `@on_signal`) is registered. `@once` collapses repeating
triggers to a single fire.

## Modules

```xs
import json                                -- stdlib
import time                                -- stdlib
import { hash, sign } from "crypto"        -- named imports
use ./helpers                              -- local file
use plugin "myplugin"                      -- compile-time plugin
```

## Testing

```xs
import test
test.describe("math", || {
    test.it("adds", || test.eq(1 + 1, 2))
})
test.run()
```

## Where to next?

- Each feature here has its own chapter with the deep version.
- [Concurrency](./concurrency.md) and [Effects](./effects.md) are the
  most distinctive.
- The [Plugin system](./plugins.md) lets you change the parser at
  compile time; if that idea worries you, that chapter is for you.
- For the toolchain shape (build, test, format, debug, the REPL),
  see [The xs command](./cli.md).
- For the package layout, dependencies, and publishing flow, see
  [Packages and the registry](./packages.md).
- If you're moving from another language, jump to
  [Migrating from Python](./migrating/python.md) (or Node, Go, Rust).
