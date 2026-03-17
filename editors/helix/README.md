# XS for Helix

Drop-in language config for the Helix editor. Includes the
tree-sitter grammar reference, LSP wiring, and standard query files.

## Install

Add to your Helix config (`~/.config/helix/languages.toml`):

```toml
[[language]]
name = "xs"
scope = "source.xs"
file-types = ["xs"]
roots = ["xs.toml", ".git"]
comment-tokens = ["--"]
indent = { tab-width = 2, unit = "  " }
auto-format = true
formatter = { command = "xs", args = ["fmt", "-"] }
language-servers = ["xs"]

[language-server.xs]
command = "xs"
args = ["lsp"]

[[grammar]]
name = "xs"
source = { git = "https://github.com/xs-lang0/xs", subpath = "editors/tree-sitter-xs" }
```

Then:

```sh
hx --grammar fetch
hx --grammar build
```

Drop the `editors/helix/queries/xs/*.scm` files into
`~/.config/helix/runtime/queries/xs/` (or use `:xs --output | tar`
once we ship a packaged release).

## Files

- `languages.toml.snippet` — copy-paste source for your config.
- `queries/xs/highlights.scm`, `injections.scm`, `indents.scm`,
  `textobjects.scm` — Helix runtime queries.
