-- plugin that registers multiple tokens
plugin.runtime.global.set("multi_token_ok", true)

plugin "multi-tokens" {
  meta {
    id: "multi-tokens"
    version: "0.1.0"
  }

  lexer {
    token ASYNC_PIPE = "async_pipe"
    token MEMO = "memo"
  }
}
