# XS for Zed

Zed extension for the XS programming language. Provides syntax
highlighting via the bundled tree-sitter grammar and the `xs lsp`
language server.

## Install

Until the extension is published to Zed's registry, install via dev
mode:

1. Clone this repo.
2. In Zed: `cmd-shift-x` -> `extensions: install dev extension`,
   point it at `editors/zed/`.

## Layout

- `extension.toml` declares the language, the grammar, and the LSP.
- `languages/xs/config.toml` declares filetypes, comment markers, etc.
- `languages/xs/highlights.scm`, `injections.scm`, `outline.scm`,
  `indents.scm`, `brackets.scm`: tree-sitter queries.
- `src/lib.rs`: minimal extension entry point that runs `xs lsp`.

## Requires

- `xs` on `$PATH` for the language server.
