-- plugin with override exclusive rule syntax
plugin.runtime.global.set("exclusive_parse_ok", true)

plugin "exclusive-test" {
  meta {
    id: "exclusive-test"
    version: "0.1.0"
  }

  sema {
    override exclusive rule CustomNode(node) {
      true
    }
  }
}
