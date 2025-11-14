-- plugin with exec hook for call nodes
plugin.runtime.global.set("exec_hook_ok", false)
plugin.runtime.global.set("exec_hook_fired", false)

plugin "exec-hook" {
  meta {
    id: "exec-hook"
    version: "0.1.0"
  }

  runtime {
    exec assign(node, env) {
      plugin.runtime.global.set("exec_hook_fired", true)
      true
    }
  }
}

plugin.runtime.global.set("exec_hook_ok", true)
