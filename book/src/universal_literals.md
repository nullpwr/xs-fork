# Universal literals

Five typed literal shapes are recognised by the lexer; each compiles
to a typed runtime value, not a string.

| literal             | type      | example                |
|---------------------|-----------|------------------------|
| duration            | `Duration`| `500ms`, `30s`, `2h`   |
| color               | `Color`   | `#ff6600`, `#abc`      |
| date / datetime     | `Date`    | `2025-12-31`, `2025-12-31T10:00` |
| size                | `Size`    | `10MB`, `4.5GiB`       |
| angle               | `Angle`   | `45deg`, `1.57rad`     |

## Why

`time.sleep(500)` is ambiguous: 500 ms, 500 s, 500 ns? The function
signature has to pick one and the call site loses information.

```xs
time.sleep(500ms)          -- typed; the function takes Duration
time.sleep(2.5s)
```

```xs
let bg: Color = #1a1a2e
let layout = #{padding: 10MB / file_size, angle: 45deg}
```

## What you can do

```xs
500ms + 250ms              -- 750ms
500ms.to_seconds()         -- 0.5
500ms.to_ns()              -- 500_000_000
2h.minutes                 -- 120

#ff6600.rgb()              -- (255, 102, 0)
#ff6600.lighten(0.2)       -- a Color, brighter

2025-12-31.weekday()       -- "Wed"
2025-12-31 - 2025-12-25    -- 6.days

10MB.to_bytes()            -- 10485760
10MB + 512KB               -- 10.5 MB

45deg.to_radians()         -- 0.785398
45deg.sin()                -- 0.707
```

The arithmetic respects units — adding `10MB + 30s` is a type error.

## Why this works

Each shape has a tag the lexer recognises. The parser produces a
typed AST node. The interpreter constructs a `Duration{ns: ...}`
struct (or equivalent) — same memory layout as a regular struct,
just with a literal-friendly constructor.

You can implement your own through plugins; the lexer hooks let you
register a new suffix and the parser hook produces the matching
constructor call.

## Strings still work

If you really want a string, quote it:

```xs
"500ms"                    -- string
500ms                      -- Duration
```
