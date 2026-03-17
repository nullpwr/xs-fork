# xs-sql

```xs
import sql
import db

let h = db.open()
db.exec(h, sql.insert("users", #{id: 1, name: "alice", active: true}))
let q = sql.select("users").where("active", "=", true).order_by("id", "DESC").limit(10).sql()
let rows = db.query(h, q)
```
