-- plugin with sema rules that actually fire
plugin.runtime.global.set("sema_dispatch_ok", false)
plugin.runtime.global.set("sema_visit_count", 0)

plugin "sema-dispatch" {
  meta {
    id: "sema-dispatch"
    version: "0.1.0"
  }

  sema {
    rule fn_decl(node) {
      let cur = plugin.runtime.global.get("sema_visit_count")
      plugin.runtime.global.set("sema_visit_count", cur + 1)
    }
  }
}

plugin.runtime.global.set("sema_dispatch_ok", true)
