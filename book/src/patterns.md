# Pattern matching

`match` is the primary control-flow construct for case analysis. It
binds variables, deconstructs values, and exhaustively covers
closed enums.

```xs
match value {
    0          -> "zero",
    1..=9      -> "single digit",
    n when n % 2 == 0 -> "even {n}",
    _          -> "other",
}
```

## What's a pattern

| pattern              | matches                                      |
|----------------------|----------------------------------------------|
| `42`, `"hi"`, `true` | the literal value                            |
| `_`                  | anything (no binding)                        |
| `name`               | anything, binds to `name`                    |
| `1..=10`             | inclusive numeric range                      |
| `1..10`              | exclusive numeric range                      |
| `(a, b, c)`          | tuple of exactly 3                           |
| `[a, b, ...rest]`    | array with rest binding                      |
| `Point { x, y }`     | struct deconstruction                        |
| `Color.Red`          | unit enum variant                            |
| `Result.Ok(x)`       | tagged enum variant with payload             |
| `Some(x) \| None`    | either-or                                    |

## Guards

```xs
match http_status {
    n when 200 <= n and n < 300 -> "success",
    n when 400 <= n and n < 500 -> "client error",
    _ -> "other",
}
```

A guard is an arbitrary boolean expression after `when`. The arm only
fires if the pattern matches *and* the guard is true.

## Exhaustiveness

The compiler errors when a `match` over a closed enum misses a case:

```xs
enum Shape { Circle, Square }

match s {
    Circle -> "round",
    -- error: missing Square
}
```

Use `_` as a catch-all when you really want to ignore the rest.

## Match expressions

`match` is an expression; every arm produces a value:

```xs
let label = match score {
    90.. -> "A",
    80.. -> "B",
    _    -> "C",
}
```

## Destructuring in `let`

```xs
let (x, y) = pos
let { name, age } = user                -- map destructure
let [head, ...tail] = list
```

## In function parameters

```xs
fn distance((x1, y1), (x2, y2)) -> float {
    return ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5
}

distance((0, 0), (3, 4))    -- 5.0
```
