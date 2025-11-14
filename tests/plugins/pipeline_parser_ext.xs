-- plugin with parser section (extend/production)
plugin.runtime.global.set("parser_ext_ok", false)

plugin "parser-ext" {
  meta {
    id: "parser-ext"
    version: "0.1.0"
  }

  parser {
    extend var_decl(parser, token) {
      -- callback for extending var_decl
      null
    }
    production drop_stmt(parser, token) {
      -- callback for new drop_stmt production
      null
    }
  }
}

plugin.runtime.global.set("parser_ext_ok", true)
