-- transform pass that replaces integer literal 42 with 99
plugin.runtime.global.set("transform_ok", true)

plugin "transform-test" {
  meta {
    id: "transform-test"
    version: "0.1.0"
  }
  pass "replace_42" {
    phase: after(parser)
    kind: transform

    visit int(node) {
      if node.value == 42 {
        return plugin.ast.int_node(99)
      }
      null
    }
  }
}
