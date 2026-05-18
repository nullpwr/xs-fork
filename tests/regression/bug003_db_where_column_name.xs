-- bug003: SELECT ... WHERE col = v returned 0 rows when col was a real
-- column name (CREATE TABLE dropped column names silently). Fix: wire
-- column-name -> positional lookup via db.open._schemas.

import db

let conn = db.open("bug003_test")
db.exec(conn, "CREATE TABLE users (id, name)")
db.exec(conn, "INSERT INTO users VALUES (1, 'alice')")
db.exec(conn, "INSERT INTO users VALUES (2, 'bob')")

let rows = db.query(conn, "SELECT * FROM users WHERE id = 1")
assert_eq(rows.len(), 1)
assert_eq(rows[0].c1, "alice")
println("bug003: ok")
