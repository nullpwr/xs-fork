# Error handling

You have three options:

1. **`try` / `catch` / `finally`**, like Python or JavaScript.
2. **`Result<T, E>` + `match`**, like Rust.
3. **Algebraic effects**, see [the effects chapter](./effects.md).

Pick per call site. They compose cleanly.

## Try / catch

```xs
import http
import json

try {
    let body = http.get("https://api.example.com").body
    println(json.parse(body))
} catch e {
    eprintln("fetch failed: {e}")
} finally {
    cleanup()
}
```

`throw` raises any value. The catch binds it as `e`. There's no
exception type hierarchy; match on the value to narrow.

```xs
try {
    parse(input)
} catch e {
    match e {
        ParseError.Eof        -> println("ran out of input"),
        ParseError.Bad(token) -> println("unexpected {token}"),
        _                     -> println("unknown failure: {e}"),
    }
}
```

## Result-style

```xs
enum Result<T, E> {
    Ok(T),
    Err(E),
}

fn divide(a, b) -> Result<float, str> {
    if b == 0 { return Result.Err("divide by zero") }
    return Result.Ok(a / b)
}

match divide(10, 0) {
    Ok(x)  -> println("got {x}"),
    Err(e) -> println("oops: {e}"),
}
```

When you find yourself writing nested `match` on `Result`, the
question-mark operator helps:

```xs
fn pipeline(input) -> Result<Output, str> {
    let parsed = parse(input)?
    let validated = validate(parsed)?
    let saved = save(validated)?
    return Result.Ok(saved)
}
```

`expr?` returns the inner value on `Ok`, propagates the `Err` to the
enclosing function. Equivalent to:

```xs
let parsed = match parse(input) {
    Ok(v)  -> v,
    Err(e) -> return Result.Err(e),
}
```

## Defer

`defer expr` runs `expr` when the surrounding scope exits: on
return, on throw, on fall-through. Use for cleanup:

```xs
fn write_log(path, msg) {
    let f = io.open(path, "a")
    defer f.close()
    f.write(msg + "\n")
}
```

Multiple defers run in reverse order (LIFO).

## Panics

`panic("...")` is a non-recoverable error. It terminates the
program with the message. Use it for "this should never happen"
invariants, not for normal failure paths.

```xs
fn require_positive(n) {
    if n <= 0 { panic("expected positive, got {n}") }
    return n
}
```

`assert(cond, msg)` is equivalent to `if not cond { panic(msg) }`.
`assert_eq(a, b)` panics with a diff if they differ. Used heavily
in tests.
