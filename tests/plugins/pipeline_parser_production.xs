-- plugin with a parser production that adds the 'greet' keyword
plugin.runtime.global.set("parser_production_ok", false)

plugin "greet-stmt" {
  meta {
    id: "greet-stmt"
    version: "0.1.0"
  }

  parser {
    production greet_stmt(parser, token) {
      -- when we see 'greet', build: print("hello from greet")
      let callee = #{"tag": "ident", "name": "print"}
      let arg = #{"tag": "str", "value": "hello from greet"}
      return #{"tag": "call", "callee": callee, "args": [arg]}
    }
  }
}

plugin.runtime.global.set("parser_production_ok", true)
