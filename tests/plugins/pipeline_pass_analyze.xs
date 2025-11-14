-- plugin with an analyze pass that counts fn_decl nodes
plugin.runtime.global.set("pass_analyze_ok", true)
plugin.runtime.global.set("fn_count", 0)
plugin.runtime.global.set("pass_visitor_fired", false)

plugin "fn-counter" {
  meta {
    id: "fn-counter"
    version: "0.1.0"
  }

  pass "count_fns" {
    phase: after(parser)
    kind: analyze

    visit fn_decl(node) {
      let cur = plugin.runtime.global.get("fn_count")
      plugin.runtime.global.set("fn_count", cur + 1)
      plugin.runtime.global.set("pass_visitor_fired", true)
      true
    }
  }
}
