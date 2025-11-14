-- plugin that uses cancel() in a before_eval hook
-- cancels evaluation of int literal 9999 so it returns null
plugin.runtime.global.set("cancel_hook_loaded", true)
plugin.runtime.global.set("cancel_count", 0)

plugin.runtime.before_eval("int", fn(node) {
    if node.value == 9999 {
        let c = plugin.runtime.global.get("cancel_count")
        plugin.runtime.global.set("cancel_count", c + 1)
        cancel()
    }
    true
})
