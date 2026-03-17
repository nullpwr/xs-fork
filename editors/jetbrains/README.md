# XS plugin for JetBrains IDEs

LSP-based plugin for IntelliJ IDEA Ultimate, GoLand, PyCharm Pro,
WebStorm, etc. (any JetBrains IDE with LSP support, 2023.2+).

Provides:

- `*.xs` file type registration with the bundled icon
- TextMate-grammar-based syntax highlighting (uses
  `editors/vscode/syntaxes/xs.tmLanguage.json`)
- Language server features (completion, diagnostics, go-to-def, etc.)
  via `xs lsp`

Community editions (free IDEAs) don't ship the LSP API; on those,
syntax highlighting still works through the TextMate bundle.

## Build

```sh
cd editors/jetbrains
./gradlew buildPlugin
```

The packaged plugin zip lands in `build/distributions/`.

## Install (from disk)

`File -> Settings -> Plugins -> gear icon -> Install Plugin from Disk`,
pick the zip.

## Requires

- JDK 17 (Gradle wrapper picks it up).
- `xs` on `$PATH` for LSP features.
