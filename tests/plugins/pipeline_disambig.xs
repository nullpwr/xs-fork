-- disambiguation test: two plugins with same keyword in one file
plugin.runtime.global.set("disambig_ok", true)

plugin "disambig_a" {
  meta {
    id: "disambig_a"
    version: "0.1.0"
  }
  parser {
    production greet_stmt(parser, token) {
      return plugin.ast.let_decl("greet_result", plugin.ast.str_node("from_a"))
    }
  }
}

plugin "disambig_b" {
  meta {
    id: "disambig_b"
    version: "0.1.0"
  }
  parser {
    production greet_stmt(parser, token) {
      return plugin.ast.let_decl("greet_result", plugin.ast.str_node("from_b"))
    }
  }
}
