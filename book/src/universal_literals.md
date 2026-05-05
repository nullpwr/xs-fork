# Duration literals

A number followed by a time unit is a real `Duration` value, not
sugar for a float. Always on, no pragma needed.

| literal       | type        | example                  |
|---------------|-------------|--------------------------|
| duration      | `Duration`  | `1ns`, `500ms`, `2m30s`  |

Suffixes are `ns`, `us`, `ms`, `s`, `m`, `h`, `d`. Storage is an
int64 nanosecond count, so a duration round-trips losslessly through
`.ns`.

## Why

`time.sleep(500)` is ambiguous: 500 ms, 500 s, 500 ns? The function
signature has to pick one and the call site loses information.
Durations push the unit into the value:

```xs
import time

time.sleep(500ms)
time.sleep(2.5s)
time.sleep(1m30s)
```

The same value flows through scheduling primitives (`after`,
`every`, `timeout`), the time-based decorators (`@every`,
`@delayed`), `time.sleep`, and channel `recv_timeout`.

## Arithmetic

Durations add and subtract with each other, multiply and divide by
numbers, and divide by another duration to get a ratio. Ordering
works as you would expect.

```xs
2s + 500ms        -- 2.5s
1m - 30s          -- 30s
100ms * 3         -- 300ms
2s / 4            -- 500ms
1s / 250ms        -- 4

500ms < 1s        -- true
2h > 90m          -- true
```

## Field access

The integer `.ns` accessor is the canonical unit. Coarser fields
like `.s` and `.m` return floats so partial units don't silently
truncate.

```xs
(1500ms).s        -- 1.5
(90s).m           -- 1.5
(5s).ns           -- 5_000_000_000
```

## Compound and float forms

Adjacent units stack into a single duration. Floats work too with
the obvious meaning.

```xs
let warmup = 2m30s
let half   = 0.5s
let nps    = 1500ns

println(warmup)   -- 2m30s
println(half)     -- 500ms
println(nps)      -- 1.5us
```

## Strings still work

If you really want a string, quote it:

```xs
"500ms"           -- string
500ms             -- Duration
```
