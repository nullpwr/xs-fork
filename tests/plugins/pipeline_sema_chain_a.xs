-- sema override chain plugin A (priority 10 = runs first)
plugin.runtime.global.set("chain_log", "")
plugin.runtime.global.set("chain_a_done", false)
plugin.runtime.global.set("chain_b_done", false)

plugin "chain-a" {
  meta {
    id: "chain-a"
    version: "0.1.0"
    priority: 10
  }
  sema {
    override rule assign(node) {
      if plugin.runtime.global.get("chain_a_done") == false {
        plugin.runtime.global.set("chain_a_done", true)
        let cur = plugin.runtime.global.get("chain_log")
        plugin.runtime.global.set("chain_log", cur + "A")
      }
      default(node)
    }
  }
}
