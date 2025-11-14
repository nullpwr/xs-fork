-- plugin with sema rules (stub, verifies parsing works)
plugin.runtime.global.set("sema_plugin_ok", true)

plugin "sema-rules" {
  meta {
    id: "sema-rules"
    version: "0.1.0"
  }

  sema {
    rule CustomCheck(node) {
      true
    }
  }
}
