-- plugin with full meta block (all fields)
plugin.runtime.global.set("meta_full_ok", true)

plugin "full-meta" {
  meta {
    id: "full-meta"
    version: "2.1.0"
    priority: 25
    provides: ["CustomNode", "custom_pass"]
    modifies: ["var_check"]
  }
}
