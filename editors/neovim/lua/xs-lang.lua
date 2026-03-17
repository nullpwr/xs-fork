-- XS Neovim integration. Wires filetype, tree-sitter, LSP, and helper
-- commands. Idempotent: setup() can be called multiple times.

local M = {}

local DEFAULTS = {
  lsp = true,
  treesitter = true,
  formatter = true,
  -- hooks
  on_attach = nil,    -- function(client, bufnr)
  capabilities = nil,
  -- LSP cmd; default is `xs lsp` on $PATH
  cmd = { "xs", "lsp" },
}

local function detect_filetype()
  vim.filetype.add({
    extension = { xs = "xs" },
    filename = { ["xs.toml"] = "toml" },
  })
end

local function register_treesitter()
  local ok, parsers = pcall(require, "nvim-treesitter.parsers")
  if not ok then return end
  local cfg = parsers.get_parser_configs()
  cfg.xs = {
    install_info = {
      url = "https://github.com/xs-lang0/xs",
      files = { "src/parser.c" },
      location = "editors/tree-sitter-xs",
      branch = "main",
      generate_requires_npm = false,
      requires_generate_from_grammar = false,
    },
    filetype = "xs",
  }
end

local function register_lsp(opts)
  local ok, lspconfig = pcall(require, "lspconfig")
  if not ok then return end
  local configs = require("lspconfig.configs")
  if not configs.xs then
    configs.xs = {
      default_config = {
        cmd = opts.cmd,
        filetypes = { "xs" },
        root_dir = function(fname)
          return lspconfig.util.root_pattern("xs.toml", ".git")(fname)
            or vim.fn.getcwd()
        end,
        settings = {},
      },
      docs = {
        description = "Language server for the XS programming language.",
      },
    }
  end
  lspconfig.xs.setup({
    on_attach = opts.on_attach,
    capabilities = opts.capabilities,
  })
end

local function register_commands(opts)
  vim.api.nvim_create_user_command("XsRun", function()
    local file = vim.api.nvim_buf_get_name(0)
    if file == "" then
      vim.notify("XsRun: buffer has no file", vim.log.levels.WARN)
      return
    end
    vim.cmd("split | terminal xs " .. vim.fn.shellescape(file))
  end, { desc = "Run current XS buffer" })

  vim.api.nvim_create_user_command("XsFmt", function()
    local file = vim.api.nvim_buf_get_name(0)
    if file == "" then return end
    local out = vim.fn.system({ "xs", "fmt", file })
    if vim.v.shell_error == 0 then
      vim.cmd("e!")
    else
      vim.notify("xs fmt: " .. out, vim.log.levels.ERROR)
    end
  end, { desc = "Format current XS buffer" })

  vim.api.nvim_create_user_command("XsCheck", function()
    local file = vim.api.nvim_buf_get_name(0)
    if file == "" then return end
    vim.cmd("compiler! xs")
    vim.cmd("make! check " .. vim.fn.shellescape(file))
  end, { desc = "Run xs --check on the buffer" })

  if opts.formatter then
    vim.api.nvim_create_autocmd("BufWritePre", {
      pattern = "*.xs",
      callback = function()
        vim.cmd("silent! XsFmt")
      end,
    })
  end
end

function M.setup(user_opts)
  local opts = vim.tbl_deep_extend("force", DEFAULTS, user_opts or {})
  detect_filetype()
  if opts.treesitter then register_treesitter() end
  if opts.lsp        then register_lsp(opts) end
  register_commands(opts)
end

return M
