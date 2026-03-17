# Your first program

Save the following as `hello.xs`:

```xs
fn main() {
    let names = ["Aria", "Bo", "Cy"]
    for name in names {
        println("hello, {name}")
    }
}

main()
```

Run it:

```sh
xs hello.xs
```

```text
hello, Aria
hello, Bo
hello, Cy
```

## What just happened

- `fn main()` declares a function. There's no special "entry point"
  — `main()` runs because we called it explicitly.
- `let names = [...]` is an immutable binding to an array. `var`
  would make it reassignable.
- `"hello, {name}"` is string interpolation. Anything in `{}` is an
  XS expression evaluated at format time.
- `for name in names` iterates the array.

## Try the REPL

```sh
xs
```

```text
xs 0.6.0 — type :help, :q to quit
> 1 + 2
3
> [1,2,3].map(|x| x * x)
[1, 4, 9]
> :q
```

## Next: the tour

The [language tour](./tour.md) walks through every major feature in
~30 minutes. If you've used Python, Node, Go, or Rust you'll spot
familiar shapes; the differences are called out as we go.
