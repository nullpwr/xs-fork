-- runtime hooks via new plugin block syntax
plugin.runtime.global.set("block_hook_fired", false)

plugin "block-hooks" {
  meta {
    id: "block-hooks"
    version: "0.1.0"
  }

  runtime {
    before exec fn_call(node) {
      plugin.runtime.global.set("block_hook_fired", true)
      true
    }
  }
}
