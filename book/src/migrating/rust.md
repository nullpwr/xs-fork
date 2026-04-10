# Migrating from Rust

XS is *not* a memory-safe systems language. There's a garbage
collector. If you're holding out for "Rust without lifetimes," XS
isn't that. What it is:

- **Same trait/impl pattern** for ad-hoc polymorphism.
- **Same exhaustive pattern matching** for enums.
- **Same Result/Option flavour** (we use `T?` for Option; Result is
  via the `try`/`catch` pair or an explicit `Result<T, E>` enum).
- **No borrow checker**, no lifetimes. Memory is reclaimed by a
  generational GC.

The pitch for Rust users: write the bulk of your application in XS
where Rust's friction doesn't pay off (HTTP handlers, glue code, CLI
tools), and drop into Rust via FFI for the parts that actually need
it.

## Cheat sheet

| Rust                                  | XS                              |
|---------------------------------------|---------------------------------|
| `let x = 1;`                          | `let x = 1`                     |
| `let mut x = 1;`                      | `var x = 1`                     |
| `fn add(a: i32, b: i32) -> i32 { ... }` | `fn add(a: int, b: int) -> int { ... }` |
| `match x { ... }`                     | `match x { ... }`               |
| `Option<T>` / `Some(x)` / `None`      | `T?` / `x` / `null`             |
| `Result<T, E>` / `Ok(x)` / `Err(e)`   | `try { ok } catch e { ... }` or define `enum Result<T, E>` |
| `if let Some(x) = opt { ... }`        | `if opt != null { let x = opt; ... }` |
| `?` operator                          | `try { ... } catch e { return e }` |
| `Vec<T>`                              | `[T]`                           |
| `HashMap<K, V>`                       | `Map<K, V>` / map literal       |
| `&str` / `String`                     | `str` (always heap-allocated)   |
| `derive(Debug, Clone)`                | implement the traits, or use the `derive` plugin |
| `Box::new(x)`                         | XS values are boxed by default  |
| `Arc<Mutex<T>>` for shared state      | `actor` instead                 |
| `tokio::spawn`                        | `spawn { ... }` (real thread)   |
| `async fn`                            | `async fn`                      |

## Patterns are the same

```xs
match parse(input) {
    Ok(value) -> use(value),
    Err(ParseError.Eof)        -> println("incomplete"),
    Err(ParseError.Unexpected(c)) -> println("unexpected {c}"),
}
```

```xs
enum Tree<T> {
    Leaf,
    Node(Tree<T>, T, Tree<T>),
}

fn depth(t) {
    match t {
        Leaf -> 0,
        Node(l, _, r) -> 1 + max(depth(l), depth(r)),
    }
}
```

## Where XS is *more* permissive

- No mutable-vs-immutable borrow distinction. You can pass the same
  array to two functions; both can mutate it.
- No lifetimes. Closures capture by reference; the GC keeps things
  alive.
- No `unsafe`. Anything that would be unsafe in Rust either works in
  XS (because the GC handles it) or doesn't exist (because it'd need
  manual memory management).

## Where XS is *less* expressive

- No higher-rank polymorphism (yet, RFC open).
- No const generics. `[T; N]` is not a type; you have `Array<T>` of
  any length.
- No zero-cost abstractions. Iterator chains allocate.

## A small port

Rust:

```rust
use serde_json::Value;

fn pretty_users(json: &str) -> Result<String, serde_json::Error> {
    let v: Value = serde_json::from_str(json)?;
    let users = v["users"].as_array().ok_or_else(|| ...)?;
    let names: Vec<&str> = users.iter().filter_map(|u| u["name"].as_str()).collect();
    Ok(names.join(", "))
}
```

XS:

```xs
import json

fn pretty_users(s: str) -> str {
    let v = json.parse(s)
    let users = v.get("users") ?? []
    return users.map(|u| u.get("name") ?? "").filter(|n| n.len() > 0).join(", ")
}
```

Half the lines, no allocations except the result, and it's
straightforward to read.

## When to keep using Rust

- Real-time audio / video where pause times must be bounded.
- Embedded firmware that can't carry a 2 MB runtime.
- Anywhere the borrow checker actually catches bugs you're not
  willing to catch with tests instead.

For everything else: write XS, drop into Rust through `ffi.load()`
when you genuinely need it.
