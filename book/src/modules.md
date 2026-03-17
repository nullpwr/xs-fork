# Modules and imports

XS has three kinds of source-level modules:

| keyword         | what it loads                                        |
|-----------------|------------------------------------------------------|
| `import name`   | a stdlib module (`json`, `time`, `crypto`, ...)      |
| `use ./path`    | a local file (`./helpers`, `../shared/util`)         |
| `use plugin "x"`| a compile-time plugin (changes the parser)           |

## Stdlib

```xs
import json
import time
import { hash, sign } from "crypto"        -- named imports
import re as regex                         -- alias
```

The runtime registers stdlib modules at startup, so `import` is
zero-cost lookup. The full list is in `xs --help` (or `xs doc`).

## Local files

```xs
use ./helpers                              -- ./helpers.xs
use ../shared/util                         -- ../shared/util.xs
use ./db/connection as conn                -- aliased
```

A `use` statement evaluates the file once and exposes its top-level
declarations. Subsequent `use`s of the same file return the cached
result.

A file's top-level public surface is everything declared with `pub`:

```xs
-- in ./helpers.xs
pub fn add(a, b) { a + b }
fn private_helper() { ... }                -- not exported

pub const VERSION = "1.0"
```

## Plugins

```xs
use plugin "pipeline"
```

Loads the plugin (an XS file or a `.so`/`.dylib`/`.dll`) and runs
its registration logic at *compile time*. The plugin can introduce
new keywords, replace parser productions, register passes that fire
during semantic analysis, etc. See [Plugins](./plugins.md) for the
sharp edges.

## Package layout

A package has an `xs.toml` and source under `src/`:

```
my-thing/
├── xs.toml
└── src/
    └── lib.xs               -- entry point
```

`xs.toml` looks like:

```toml
[package]
name = "my-thing"
version = "0.2.1"
license = "Apache-2.0"

[lib]
path = "src/lib.xs"
```

`xs install my-thing` resolves the package against the registry,
fetches it, and unpacks it into `xs_lib/my-thing/`. Subsequent
`import my_thing` (note the underscore — names normalize) finds it
via the local `xs_lib/`.

## Search order

When you `import name`, the resolver looks in:

1. Stdlib modules registered at startup.
2. `xs_lib/<name>/{main.xs,lib.xs,src/lib.xs,src/main.xs}`
3. `.xs_lib/...` (same shape, hidden directory).
4. `~/.xs/lib/<name>/...` (per-user installs).
5. `$XS_LIB_PATH/<name>/...` (env var, colon-separated).
6. Plugin `resolve_import` hooks (last — they can synthesise modules).

The first hit wins.
