-- plugin with two passes that have ordering constraints
-- pass_b depends on pass_a via phase ordering
plugin.runtime.global.set("pass_order_log", "")

plugin "pass-order" {
  meta {
    id: "pass-order"
    version: "0.1.0"
  }

  pass "pass_a" {
    phase: after(parser)
    kind: analyze

    visit fn_decl(node) {
      let cur = plugin.runtime.global.get("pass_order_log")
      plugin.runtime.global.set("pass_order_log", cur + "A")
      true
    }
  }

  pass "pass_b" {
    phase: after(parser)
    kind: analyze

    visit fn_decl(node) {
      let cur = plugin.runtime.global.get("pass_order_log")
      plugin.runtime.global.set("pass_order_log", cur + "B")
      true
    }
  }
}
