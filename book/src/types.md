# Values and types

Every XS value carries a runtime tag. The tags map onto the type
system as follows.

| value          | tag        | static type      | example                |
|----------------|------------|------------------|------------------------|
| `null`         | `XS_NULL`  | `null`, `T?`     | `null`                 |
| `true`/`false` | `XS_BOOL`  | `bool`           | `true`                 |
| 64-bit int     | `XS_INT`   | `int`, `i64`     | `42`                   |
| arbitrary-precision int | `XS_BIGINT` | `int`     | `1000000000000000000n` |
| double         | `XS_FLOAT` | `float`, `f64`   | `3.14`                 |
| string         | `XS_STR`   | `str`            | `"hi"`                 |
| char           | `XS_CHAR`  | `char`           | `'a'`                  |
| array          | `XS_ARRAY` | `[T]`            | `[1, 2, 3]`            |
| tuple          | `XS_TUPLE` | `(A, B)`         | `(1, "a")`             |
| map            | `XS_MAP`   | `Map<K, V>`      | `#{a: 1}`              |
| range          | `XS_RANGE` | `Range`          | `0..10`                |
| function       | `XS_FUNC`  | `fn(...) -> T`   | `fn add(a, b) { a+b }` |
| closure        | `XS_CLOSURE` | `fn(...) -> T` | `\|x\| x + 1`          |
| native fn      | `XS_NATIVE` | `fn(...) -> T`  | `println`              |
| struct value   | `XS_STRUCT_VAL` | name        | `Point { x: 1, y: 2 }` |
| enum value     | `XS_ENUM_VAL`   | enum name   | `Result.Ok(42)`        |
| class instance | `XS_INST`       | class name  | `Counter()`            |
| signal         | `XS_SIGNAL`     | `Signal<T>` | `signal(0)`            |
| regex          | `XS_REGEX`      | `re`        | `re("foo.*")`          |

## Numbers

`int` is 64-bit signed by default. Operations that would overflow
silently promote to `BigInt` (arbitrary precision). The `float`
type is a 64-bit double.

```xs
let big = 2 ** 100              -- BigInt
let small = 1 + 2               -- int
let f = 1.5                     -- float
```

Integer division: `/` between two ints produces an int (truncation
toward zero). Use `1.0 * a / b` for float result.

Division by zero raises a runtime error and returns `null`.

## Strings

Strings are UTF-8 encoded byte sequences. `s.len()` counts bytes;
`s.chars()` returns an array of Unicode codepoints; `s.upper()` /
`s.lower()` use full Unicode case mapping (Latin-1, Latin Extended,
and beyond — not ASCII-only).

```xs
let s = "héllo"
s.len()           -- 6  (héllo has a 2-byte é)
s.chars().len()   -- 5
s.upper()         -- "HÉLLO"
```

Triple-quoted strings preserve newlines:

```xs
let html = """
<p>line one</p>
<p>line two</p>
"""
```

Raw strings disable escape sequences:

```xs
let path = r"C:\Users\me"   -- backslashes are literal
```

## Arrays

Mutable, growable, refcounted. Indexed from 0.

```xs
var xs = [1, 2, 3]
xs.push(4)
xs[0] = 99
println(xs)         -- [99, 2, 3, 4]
```

Negative indices count from the end:

```xs
xs[-1]              -- 4
```

Range slicing:

```xs
xs[1..3]            -- [2, 3]
xs[..2]             -- [99, 2]
xs[1..]             -- [2, 3, 4]
```

## Maps

Open-addressed hash table; keys are strings. Field-style access works
when keys are valid identifiers:

```xs
let m = #{name: "ada", age: 30}
m.name              -- "ada"
m["age"]            -- 30
m.set("active", true)
```

## Tuples

Fixed-size, heterogeneous, deconstructable in `match` and bindings:

```xs
let t = (1, "two", 3.0)
let (a, b, c) = t
match t {
    (1, _, _) -> "starts with one",
    _ -> "other",
}
```

## Null

`null` is its own type. `T?` reads as "T or null"; functions return
`T?` when they can fail to produce a value:

```xs
fn lookup(m, k) -> str? {
    if m.contains(k) { m.get(k) } else { null }
}

let name = lookup(users, "alice") ?? "anonymous"
```

`??` is the null-coalescing operator. Combined with `?.` for safe
field access and `?[...]` for safe indexing, you can chain through
maybe-null chains:

```xs
let city = response?.user?.address?["city"] ?? "unknown"
```
