-- plugin with a before(sema) pass that sets a global
plugin.runtime.global.set("before_sema_fired", false)

plugin "before-sema" {
  meta {
    id: "before-sema"
    version: "0.1.0"
  }

  pass "pre_sema" {
    phase: before(sema)
    kind: analyze

    visit fn_decl(node) {
      plugin.runtime.global.set("before_sema_fired", true)
      true
    }
  }
}
