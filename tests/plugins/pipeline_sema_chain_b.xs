-- sema override chain plugin B (priority 20 = runs second)
-- reset chain state so the combined dispatch gives a clean result
plugin.runtime.global.set("chain_log", "")
plugin.runtime.global.set("chain_a_done", false)
plugin.runtime.global.set("chain_b_done", false)

plugin "chain-b" {
  meta {
    id: "chain-b"
    version: "0.1.0"
    priority: 20
  }
  sema {
    override rule assign(node) {
      if plugin.runtime.global.get("chain_b_done") == false {
        plugin.runtime.global.set("chain_b_done", true)
        let cur = plugin.runtime.global.get("chain_log")
        plugin.runtime.global.set("chain_log", cur + "B")
      }
      default(node)
    }
  }
}
