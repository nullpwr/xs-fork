# Install

Pick the path that fits where you're running XS.

## macOS / Linux

```sh
curl -fsSL https://xslang.org/install | sh
```

That writes `xs` to `~/.local/bin/xs`. Add it to your `$PATH` if it
isn't already.

## Windows

Download the latest `xs-windows-x86_64.zip` from the
[releases page](https://github.com/xs-lang0/xs/releases/latest) and
unzip somewhere on your `%PATH%`.

## Verify

```sh
xs --version
```

You should see something like `xs 0.6.0`.

## Build from source

If you'd rather build it yourself:

```sh
git clone https://github.com/xs-lang0/xs.git
cd xs
make release
```

You'll need a C11 compiler (gcc or clang). No external libraries:
BearSSL is bundled, the regex engine is in-tree, sockets use raw
POSIX. The release build produces `./xs`, around 2 MB, with no shared
library dependencies.

## Verify signatures (optional)

Releases ship signed with [minisign](https://jedisct1.github.io/minisign/):

```sh
minisign -V -P "$(curl -s https://xslang.org/pubkey)" -m xs-linux-x86_64
```

The public key is also checked in at `MINISIGN.pub`.

## Editor setup

The repo's `editors/` directory has plug-and-play configs for
**VSCode**, **Neovim**, **Helix**, **Zed**, and **JetBrains IDEs**.
See each subdirectory's README for installation. They all share
syntax highlighting via tree-sitter and language server features
through `xs lsp`.
