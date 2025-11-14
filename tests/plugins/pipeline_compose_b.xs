-- Dependent plugin: uses base-utils
plugin.runtime.global.set("compose_b_loaded", true)

plugin "ext-utils" {
  meta {
    id: "ext-utils"
    version: "1.0.0"
    depends_on: ["base-utils"]
  }
}
