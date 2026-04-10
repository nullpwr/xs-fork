# Migrating from Node.js

XS will feel close to TypeScript with batteries included. The biggest
adjustments:

- **No `npm install` for stdlib**. `import http`, `import json`,
  `import crypto` work out of the box.
- **`println` not `console.log`**. (`console.log` is a JS-ism.)
- **Maps need `#{}` syntax**. `{ a: 1 }` is a *block*; the map literal
  is `#{a: 1}`.
- **Promises are everywhere but sync is the default**. Top-level
  `await` works; you opt in to async with `async fn`.

## Cheat sheet

| Node                         | XS                              |
|------------------------------|---------------------------------|
| `console.log(...)`           | `println(...)`                  |
| `JSON.parse / stringify`     | `json.parse / stringify`        |
| `fs.readFileSync(p)`         | `fs.read(p)`                    |
| `fs.writeFileSync(p, s)`     | `fs.write(p, s)`                |
| `Array.isArray(x)`           | `is_array(x)`                   |
| `arr.map / filter / reduce`  | `arr.map / filter / fold`       |
| `Object.keys(m)`             | `m.keys()`                      |
| `m[k] = v`                   | `m.set(k, v)` or `m[k] = v`     |
| `try / catch / finally`      | `try / catch / finally`         |
| `throw new Error("x")`       | `throw "x"`                     |
| `async function f() {...}`   | `async fn f() {...}`            |
| `Promise.all([...])`         | inside a `nursery { ... }` block |
| `setTimeout(fn, ms)`         | `time.sleep_ms(ms); fn()`       |
| `require("foo")`             | `import foo`                    |
| `crypto.createHash("sha256")`| `crypto.sha256(input)`          |
| `fetch(url).then(r => r.text())` | `net.http_get(url).body`    |

## TypeScript types translate

```ts
function parseUser(json: string): User | null { ... }
```

becomes

```xs
fn parse_user(s: str) -> User? { ... }
```

Optional types are `T?`. Generics use `<T>`. Trait bounds appear in
a `where` clause.

## A real port

Express-style HTTP server:

```js
import express from "express"
const app = express()
app.use(express.json())
app.post("/echo", (req, res) => res.json({echoed: req.body}))
app.listen(3000)
```

XS:

```xs
import http
import json

http.serve(3000, |req| {
    if req.path == "/echo" and req.method == "POST" {
        let body = json.parse(req.body)
        return #{
            status: 200,
            headers: #{"Content-Type": "application/json"},
            body: json.stringify(#{echoed: body}),
        }
    }
    return #{status: 404, body: "not found"}
})
```

No `npm install` for express; `http.serve` ships with the runtime.

## Browser

XS compiles to WebAssembly and runs in a `<script>` tag:

```html
<script src="https://static.xslang.org/xs.js"></script>
<script>
  (async () => {
    const xs = await loadXS();
    await xs.run(`
        import http
        let r = http.get("https://api.example.com/data")
        println(r.body)
    `);
  })();
</script>
```

The browser SDK provides a real virtual filesystem, persistent
storage via IndexedDB, worker mode for non-blocking execution, and
syntax-highlighted editor components. See
[Browser via WebAssembly](../browser.md).

## What's missing vs Node

- **Single-package build tooling**: there's no Vite/Webpack-equivalent
  for building a Node-style app. The transpiler-to-JS exists but
  isn't bundler-ready yet.
- **Mature ORM**: the `db` module is a small in-memory KV store;
  for SQL you'd plug into your own driver via `ffi`.
- **The `fs/promises` async file API**: only the sync version exists;
  it doesn't block the event loop because the runtime drops the GIL
  on syscalls.
