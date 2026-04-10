# Gradual typing

Annotations are optional. You can write a quick script with zero
types, or annotate a service with full coverage; XS treats both
codebases as legitimate.

```xs
-- untyped: works
fn greet(name) { println("hi, {name}") }

-- typed: enforced
fn greet_typed(name: str) -> () {
    println("hi, {name}")
}
```

The compiler runs type inference everywhere; annotations just
constrain it.

## Three modes

| invocation        | what runs                                        |
|-------------------|--------------------------------------------------|
| `xs file.xs`      | runs the program; type errors don't block        |
| `xs --check file.xs` | runs the type checker, reports issues, doesn't run |
| `xs --strict file.xs` | requires annotations on every public surface  |

Within a single file:

```xs
@strict
pub fn settle(amount: float, currency: str) -> Result<Receipt, str> {
    ...
}
```

`@strict` on a function turns the strict checker on for that
function only; any inference fallback inside the body becomes an
error. Mix freely with non-strict code.

## What gets inferred

```xs
let x = 1                  -- inferred: int
let xs = [1, 2, 3]         -- inferred: [int]
let m = #{a: 1, b: 2}      -- inferred: Map<str, int>
let f = |x| x * 2          -- inferred: fn(int) -> int  (after first call site)
```

For closures, inference looks at the call sites: if you write
`f(1.5)` later, the inferred type updates to `fn(float) -> float`.
Mixed call sites with incompatible types are an error under `--check`.

## Generics

```xs
fn first<T>(xs: [T]) -> T? {
    if xs.is_empty() { null } else { xs[0] }
}

let n: int? = first([1, 2, 3])
let s: str? = first(["a", "b"])
```

Type parameters are introduced with `<T, U, ...>`. Constraints go in
`where`:

```xs
fn sort<T>(xs: [T]) -> [T] where T: Ord { ... }
```

## Trait bounds

```xs
trait Show {
    fn show(self) -> str
}

fn dump<T>(value: T) where T: Show {
    println(value.show())
}
```

## Limits

- **No higher-rank polymorphism** today (`for<T>`-style). RFC open.
- **No associated types**. Workaround: extra type parameters.
- **No const generics**. Arrays are heap-allocated, length is
  runtime.

If you need any of these, watch the
[stability tier 2 modules](./policy.md) for opt-in experiments.
