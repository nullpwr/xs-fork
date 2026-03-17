# xs-dotenv

```xs
import dotenv
let cfg = dotenv.parse_file(".env")
println(cfg.get("DATABASE_URL"))

// or, also populate process env (without overriding anything that's
// already set in the real environment):
dotenv.load(".env")
```
