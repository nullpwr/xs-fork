-- dynamic token registration test
plugin.runtime.global.set("token_test_ok", true)

plugin "custom-tokens" {
  meta {
    id: "custom-tokens"
    version: "0.1.0"
  }

  lexer {
    token LAZY = "lazy"
  }
}
