-- mixed plugin: uses both old API and new block syntax in one file
plugin.runtime.global.set("mixed_old_api", true)

plugin "mixed-plugin" {
  meta {
    id: "mixed-plugin"
    version: "0.2.0"
  }

  runtime {
    before exec fn_call(node) {
      plugin.runtime.global.set("mixed_block_hook", true)
      true
    }
  }
}
