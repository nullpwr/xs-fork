# Browser via WebAssembly

XS runs in any modern browser via the precompiled WASI binary
(`xs.wasm`, ~600 KB) plus a small JavaScript SDK (`xs.js`, ~35 KB).

## Hello world

```html
<script src="https://static.xslang.org/xs.js"></script>
<script>
  (async () => {
    const xs = await loadXS();
    const out = await xs.run(`println("hello from wasm")`);
    document.body.innerText = out;
  })();
</script>
```

That's the entire setup. No npm install, no bundler, no build step.

## What the SDK gives you

| API                                       | what it does                                  |
|-------------------------------------------|-----------------------------------------------|
| `loadXS({wasmUrl, fs, persist, worker})`  | construct an interpreter                      |
| `xs.run(code) -> Promise<string>`         | run code, return captured stdout              |
| `xs.exec(args)`                           | full CLI, returns exit code                   |
| `xs.writeFile/readFile/listFiles/deleteFile` | direct VFS access                          |
| `xs.fetch(url, path)`                     | fetch HTTP into the VFS                       |
| `xs.fetchAll({path: url, ...})`           | parallel batch fetch                          |
| `xs.terminate()`                          | shut down the worker                          |

## Modes

```js
const xs = await loadXS({
    persist: "myapp",        // VFS backed by IndexedDB; survives reloads
    worker: true,             // run in a Web Worker, non-blocking main thread
    stdout: line => append(line),
    stderr: line => append(line, "err"),
    stdin: async () => prompt("input?"),    // sync OR async (worker mode)
});
```

`persist` opens an IndexedDB database and loads any previously
written files at startup. Subsequent writes commit back asynchronously.

`worker` spins the wasm into a Web Worker and wires a SharedArrayBuffer
for synchronous stdin (the main thread fulfils the async callback,
the worker resumes through `Atomics.wait`). Async stdin requires
COOP/COEP headers for SAB; without them, async stdin returns "" and
sync stdin still works.

## Editor and code embeds

```html
<script src="https://static.xslang.org/xs-embed.js"></script>

<xs-editor>
println("Hello from an interactive editor")
for i in 0..5 { println("  iteration {i}") }
</xs-editor>

<xs-code>
let n = 10
println("the {n}th fibonacci is {fib(n)}")
</xs-code>
```

`<xs-editor>` is an interactive code editor with a Run button,
syntax highlighting, and CSS-customizable styling.
`<xs-code>` auto-runs on page load and shows the output below.

## Network through the VFS

`net.http_*` (raw sockets) doesn't work in WASI. Instead, use
`xs.fetch` from JS to populate the VFS, then read it from XS:

```js
await xs.fetch("https://api.github.com/zen", "zen.txt");
await xs.run(`import fs; println(fs.read("zen.txt"))`);
```

For batch loading:

```js
await xs.fetchAll({
    "data.json":   "https://example.com/data.json",
    "schema.toml": "https://example.com/schema.toml",
});
```

## Tree-sitter (optional)

Default highlighting is a 5 KB regex tokeniser. For AST-aware
highlighting, opt into the bundled tree-sitter grammar:

```js
await xsHighlight.useTreeSitter({baseUrl: "https://static.xslang.org"});
```

That lazy-loads `web-tree-sitter.{js,wasm}` (~350 KB combined) and
the XS grammar (~350 KB). Stylesheets use the same `xs-*` CSS
classes as the regex path, so themes carry over.

## Hosting your own assets

Mirror four files to your static origin:

- `xs.wasm` (~600 KB)
- `xs.js` (~35 KB)
- `xs-embed.js` (~20 KB)
- `xs-highlight.js` (~10 KB)

Optional, for tree-sitter highlighting:

- `tree-sitter-xs.wasm` (~350 KB)
- `web-tree-sitter.{js,wasm}` (~350 KB)

Then `loadXS({wasmUrl: "https://yourcdn.com/xs.wasm"})`.
