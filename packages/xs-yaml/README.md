# xs-yaml

```xs
import yaml
let cfg = yaml.parse("
server:
  host: localhost
  port: 8080
features:
  - search
  - export
")
println(cfg.get("server").get("port"))    // 8080
```

Subset: nested maps, sequences, basic scalars, `#` comments. No
anchors, tags, multi-doc, flow-style. Use `xs-yaml` for config files
where you control the input; reach for a richer parser if you need
the full spec.
