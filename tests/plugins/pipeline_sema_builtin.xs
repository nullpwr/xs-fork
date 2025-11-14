-- plugin with sema rule targeting built-in fn_decl type
plugin.runtime.global.set("sema_builtin_ok", false)
plugin.runtime.global.set("sema_fn_count", 0)

plugin "sema-builtin" {
  meta {
    id: "sema-builtin"
    version: "0.1.0"
  }

  sema {
    rule let(node) {
      let cur = plugin.runtime.global.get("sema_fn_count")
      plugin.runtime.global.set("sema_fn_count", cur + 1)
    }
  }
}

-- define a function so the sema dispatch has fn_decl to visit
fn __sema_test_dummy() { 1 }

plugin.runtime.global.set("sema_builtin_ok", true)
