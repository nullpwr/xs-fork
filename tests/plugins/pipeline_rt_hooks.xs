-- runtime hooks test plugin
plugin.runtime.global.set("call_count", 0)

plugin.runtime.before_eval("call", fn(node) {
    let c = plugin.runtime.global.get("call_count")
    plugin.runtime.global.set("call_count", c + 1)
    true
})

plugin "call-counter" {
  meta {
    id: "call-counter"
    version: "0.1.0"
  }
}
