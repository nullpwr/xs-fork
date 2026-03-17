# xs-cli-args

Declarative argv parser with positional args, options, flags, and
nested subcommands. Built on `xs`'s standard map / array surface, no
runtime dependencies.

```xs
import cli_args

let spec = cli_args.spec("greet")
    .about("say hello")
    .arg("name", #{help: "who to greet", required: true})
    .opt("count", #{help: "how many times", default: "1"})
    .opt("upper", #{help: "shout it", flag: true, short: "U"})

let p = spec.parse(args)
let times = p.get("count").to_int()
for _ in 0..times {
    let msg = "hello, " + p.get("name")
    println(if p.flag("upper") { msg.upper() } else { msg })
}
```

Subcommands compose:

```xs
let root = cli_args.spec("git")
    .cmd("commit", cli_args.spec("commit").opt("message", #{short: "m"}))
    .cmd("push",   cli_args.spec("push").arg("remote"))

let p = root.parse(args)
match p.subcommand {
    "commit" -> println("commit:", p.sub.get("message")),
    "push"   -> println("push to:", p.sub.get("remote")),
    _        -> println(root.help()),
}
```
