-- plugin with on scope_exit callback
plugin.runtime.global.set("scope_exit_ok", false)
plugin.runtime.global.set("scope_exit_count", 0)

plugin "scope-exit" {
  meta {
    id: "scope-exit"
    version: "0.1.0"
  }

  pass "scope-tracker" {
    phase: after(parser)
    kind: analyze

    visit fn_decl(node) {
      true
    }

    on scope_exit(scope) {
      let cur = plugin.runtime.global.get("scope_exit_count")
      plugin.runtime.global.set("scope_exit_count", cur + 1)
    }
  }
}

plugin.runtime.global.set("scope_exit_ok", true)
