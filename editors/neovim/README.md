# XS for Neovim

Adds tree-sitter highlighting, the `xs lsp` language server, and a
filetype detection rule for `*.xs`.

## Install with lazy.nvim

```lua
{
  "xs-lang0/xs",
  ft = "xs",
  config = function()
    require("xs-lang").setup({
      -- options shown are the defaults
      lsp = true,
      treesitter = true,
      formatter = true,    -- run `xs fmt` on :w
    })
  end,
}
```

## Or, lazy.nvim pulling just the Neovim subdir

```lua
{
  url = "https://github.com/xs-lang0/xs",
  ft = "xs",
  dir = vim.fn.stdpath("data") .. "/lazy/xs/editors/neovim",
}
```

## Manual setup

Drop `editors/neovim/lua/xs-lang.lua` somewhere on your `runtimepath`
and call `require("xs-lang").setup({})` from your config.

## What you get

- `*.xs` filetype detection
- tree-sitter parser registration (compiles automatically with
  `:TSInstall xs` once nvim-treesitter knows about us)
- `xs lsp` autostarts via `nvim-lspconfig` when `xs.toml` or `.git` is
  detected
- `:XsRun` runs the current buffer
- `:XsFmt` formats the buffer through `xs fmt`
- `:XsCheck` runs the type checker, populates the location list

## Requires

- Neovim 0.9+
- `xs` on `$PATH`
- `nvim-treesitter` and `nvim-lspconfig` for the corresponding features
