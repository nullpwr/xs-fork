-- WHERE clause column-name resolution. Before the fix, queries like
-- WHERE id = 1 returned zero rows because the SQL runtime only
-- matched on positional cN names and CREATE TABLE column names were
-- silently dropped.

import db

let conn = db.open("test_where")
db.exec(conn, "CREATE TABLE users (id, name, age)")
db.exec(conn, "INSERT INTO users VALUES (1, 'alice', 30)")
db.exec(conn, "INSERT INTO users VALUES (2, 'bob', 25)")
db.exec(conn, "INSERT INTO users VALUES (3, 'carol', 35)")

-- WHERE on the real column name
let by_id = db.query(conn, "SELECT * FROM users WHERE id = 1")
assert_eq(by_id.len(), 1)
assert_eq(by_id[0].c1, "alice")

let by_name = db.query(conn, "SELECT * FROM users WHERE name = 'bob'")
assert_eq(by_name.len(), 1)
assert_eq(by_name[0].c2, 25)

-- WHERE on the positional name still works (backwards-compat)
let by_c0 = db.query(conn, "SELECT * FROM users WHERE c0 = 3")
assert_eq(by_c0.len(), 1)
assert_eq(by_c0[0].c1, "carol")

-- DELETE also honours real column names
db.exec(conn, "DELETE FROM users WHERE id = 2")
let remaining = db.query(conn, "SELECT * FROM users")
assert_eq(remaining.len(), 2)

println("test_db_where: all passed")
