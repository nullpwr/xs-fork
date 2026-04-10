# Packages and the registry

XS has a built-in package manager. `xs install` resolves
dependencies, `xs publish` ships them, the registry at
`registry.xslang.org` indexes everything the community has put up.

The unit is the *package*: a directory with an `xs.toml` manifest
and source under `src/`. The format is git-friendly, version
controlled, and looks like every other package manager you have
used.

## A new package

```sh
xs new my-thing
cd my-thing
```

Creates:

```
my-thing/
├── xs.toml
├── README.md
├── .gitignore
├── src/
│   └── main.xs
└── tests/
    └── test_main.xs
```

`xs init` does the same in the current directory if you already have
one.

## The manifest

```toml
[package]
name        = "my-thing"
version     = "0.1.0"
license     = "Apache-2.0"
description = "a thing that does things"
authors     = ["aria <aria@example.com>"]
repository  = "https://github.com/aria/my-thing"

[lib]
path = "src/lib.xs"

[bin]
name = "my-thing"
path = "src/main.xs"

[dependencies]
http-client = "0.4"
json-schema = { version = "1.2", features = ["validation"] }
my-fork     = { git = "https://github.com/me/my-fork", rev = "abc123" }
local-dep   = { path = "../local-dep" }

[dev-dependencies]
xs-assert = "0.3"

[features]
default     = ["tls"]
tls         = []
postgres    = ["dep:xs-postgres"]
```

A package can have `[lib]`, `[bin]`, or both. `[lib]` exposes a
top-level entry point importable by other packages; `[bin]` builds
to an executable. A workspace package with multiple binaries uses
`[[bin]]` (TOML array of tables) instead.

`license` should be an SPDX identifier. The registry validates
against the list before publishing.

## Adding a dependency

```sh
xs add http-client                 # latest matching minor
xs add http-client@0.4.2           # exact version
xs add user/repo                   # GitHub shorthand
xs add ./local-thing               # path dependency
```

Updates `xs.toml` and resolves the dependency tree. Locked versions
go to `xs.lock` (commit this). Source unpacks under `xs_lib/`
(do not commit; gitignore by default).

`xs install` (no argument) resolves and fetches everything in
`xs.toml`; useful after a fresh clone or a manifest change.

## Importing what you installed

```xs
import http_client                 -- name normalised: hyphens become underscores
import json_schema as jschema      -- aliased
import { hash, sign } from "crypto"
```

The resolver walks:

1. Stdlib (built-in)
2. `xs_lib/` (project-local, what `xs install` writes)
3. `.xs_lib/` (hidden variant)
4. `~/.xs/lib/` (per-user installs from `xsi get -g`)
5. `$XS_LIB_PATH` entries
6. Plugin `resolve_import` hooks (last resort)

First match wins. A name collision between project-local and stdlib
favours project-local, so a package can shadow a stdlib module if
it really needs to.

## Versioning

Caret ranges by default (`"0.4"` matches `>=0.4.0, <0.5.0`; `"1.2"`
matches `>=1.2, <2.0.0`). Tilde for finer pinning (`"~0.4.2"` matches
`>=0.4.2, <0.5.0`). Exact pins via `=`. Pre-release versions
(`0.4.0-alpha.1`) are excluded from caret ranges unless the user
explicitly opts in.

The lockfile records the resolved version for every transitive
dependency along with a content hash. `xs install` re-fetches if a
hash mismatches; this catches a registry tarball getting silently
replaced.

## Features

A feature is a named compile flag a package exports. Other packages
opt in:

```toml
[dependencies]
big-thing = { version = "1", features = ["postgres", "metrics"] }
```

Features compose with `dep:` syntax (a feature pulls in an optional
dependency) and can chain (`feature_a` enables `feature_b`).

Defaults are the union of `default = [...]` plus any explicit picks.
Set `default-features = false` to start clean.

## Publishing

```sh
xs login                       # paste your registry token
xs publish                     # uploads the current package
```

The registry requires:

- `[package]` block with `name`, `version`, `license`, `description`
- A `README.md` (or `[package] readme = "path"`)
- Source compiles cleanly under `xs --check`
- Tests pass under `xs test`
- Tag `v{version}` exists in the configured `repository`

Versions are immutable once published. Yanking is supported (`xs
yank my-thing@0.1.3`) for security issues; it stops new resolutions
from picking the version but does not break existing lockfiles.

## Workspaces

A repository with several related packages uses a workspace root:

```toml
# top-level xs.toml
[workspace]
members = ["core", "client", "server", "examples/*"]
exclude = ["examples/wip"]

[workspace.dependencies]
http-client = "0.4"
```

Member packages inherit dependencies from `[workspace.dependencies]`
with `http-client.workspace = true` in their own manifest. Versions
stay in sync across the workspace. `xs build` at the root builds
every member; `xs test` runs every member's tests.

## The registry

`registry.xslang.org` runs the canonical index. Source-of-truth lives
in the registry repo; the index is content-addressed, so a mirror is
straightforward.

```sh
xs search json                 # full-text against name and description
xs info http-client            # versions, downloads, links
```

To run a private registry, point `xs.toml` at it:

```toml
[registry]
default = "https://registry.example.internal"
```

Or per-dependency: `{ version = "1", registry = "internal" }`.

## Lockfile shape

`xs.lock` is TOML, sorted, deterministic. An excerpt:

```toml
[[package]]
name      = "http-client"
version   = "0.4.2"
source    = "registry+https://registry.xslang.org"
checksum  = "sha256:c1f8..."
dependencies = ["bearssl@1.0.0", "json@0.8.1"]
```

Reproducible installs follow from this; CI should
`xs install --frozen` to refuse any resolver changes.

## Recommended layout for a real package

```
my-thing/
├── xs.toml
├── xs.lock
├── README.md
├── LICENSE
├── CHANGELOG.md
├── src/
│   ├── lib.xs               -- entry point
│   ├── core.xs              -- internal modules
│   └── plugins/
│       └── derive.xs        -- compile-time plugin shipped with the package
├── tests/
│   ├── test_basic.xs
│   ├── test_property.xs
│   └── golden/              -- snapshot fixtures
├── examples/
│   └── hello.xs
├── benchmarks/
│   └── bench_hot.xs
└── .github/
    └── workflows/ci.yml
```

The convention is: tests in `tests/`, examples in `examples/`,
benchmarks in `benchmarks/`, plugin code in `src/plugins/`. The
package manager makes no demands beyond `[package]` and an entry
point, but the registry's automated review checks for the obvious
shapes (README, LICENSE, tests, version tag) before promoting a new
package.
