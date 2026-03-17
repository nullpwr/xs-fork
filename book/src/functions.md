# Functions and closures

Functions are first-class values. They can be returned, stored in
data structures, passed as arguments.

## Declaring

```xs
fn add(a, b) { a + b }                      -- last expression returns
fn add_typed(a: int, b: int) -> int { a + b }
fn greet(name, greeting = "hi") {           -- default param
    println("{greeting}, {name}")
}
fn sum(...xs) {                             -- variadic
    xs.fold(0, |acc, x| acc + x)
}
```

The body can be a block (`{ ... }`) or a single expression after `=`:

```xs
fn square(x) = x * x
```

## Closures

```xs
let inc = |x| x + 1
let mul = |x, y| x * y
let fac = fn(n) {
    if n <= 1 { 1 } else { n * fac(n - 1) }
}
```

Pipe-form lambdas can have a block body:

```xs
let compute = |x, y| {
    let s = x + y
    s * s
}
```

## Capturing

Closures capture variables by reference. The captured variable lives
on the heap in an *upvalue*, which means closures can outlive their
defining scope:

```xs
fn counter() {
    var n = 0
    return || { n = n + 1; n }
}

let c = counter()
println(c(), c(), c())   -- 1 2 3
```

## Returning multiple values

Use a tuple:

```xs
fn divmod(a, b) -> (int, int) {
    return (a / b, a % b)
}

let (q, r) = divmod(17, 5)   -- q=3, r=2
```

## Function overloading by arity

```xs
fn area(side) = side * side
fn area(width, height) = width * height
```

The dispatcher picks based on argument count.

## Tail-call optimisation

XS rewrites tail-position calls into a loop, so deeply recursive
self-calls don't blow the stack:

```xs
fn loop_until(p, n) {
    if p(n) { return n }
    return loop_until(p, n + 1)        -- tail call: no growing stack
}
```

This is automatic — no `@tco` annotation, no opt-in.

## Function types

```xs
fn apply(f: fn(int) -> int, x: int) -> int { f(x) }
apply(|n| n * 2, 21)     -- 42
```

A function type spells out the arrow:

```xs
type Mapper<T, U> = fn(T) -> U
let upper: Mapper<str, str> = |s| s.upper()
```
