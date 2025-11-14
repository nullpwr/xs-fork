-- plugin with extend that stores target production name
plugin.runtime.global.set("extend_target_ok", false)

plugin "extend-target" {
  meta {
    id: "extend-target"
    version: "0.1.0"
  }

  parser {
    extend statement(parser, token) {
      null
    }
  }
}

plugin.runtime.global.set("extend_target_ok", true)
