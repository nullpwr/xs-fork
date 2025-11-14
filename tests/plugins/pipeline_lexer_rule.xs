-- plugin with a lexer rule declaration
plugin.runtime.global.set("lexer_rule_ok", false)

plugin "lexer-rules" {
  meta {
    id: "lexer-rules"
    version: "0.1.0"
  }

  lexer {
    token AWAIT = "await"
    rule after(AWAIT, IDENT) {
      true
    }
  }
}

plugin.runtime.global.set("lexer_rule_ok", true)
