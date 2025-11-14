-- plugin with pass state that visitors can read
plugin.runtime.global.set("pass_state_ok", false)
plugin.runtime.global.set("state_count", -1)

plugin "pass-state" {
  meta {
    id: "pass-state"
    version: "0.1.0"
  }

  pass "stateful" {
    phase: after(parser)
    kind: analyze

    state {
      count: 0
    }

    visit fn_decl(node, st) {
      plugin.runtime.global.set("state_count", 1)
      true
    }
  }
}

plugin.runtime.global.set("pass_state_ok", true)
