-- test embedded database module

import db

-- open in-memory db
let conn = db.open("test_mem")
assert(conn != null, "db.open should return a connection")

-- create table
let r1 = db.exec(conn, "CREATE TABLE users (id, name, age)")
assert_eq(r1, "ok")

-- insert rows
let r2 = db.exec(conn, "INSERT INTO users VALUES (1, 'alice', 30)")
assert_eq(r2, "ok")
db.exec(conn, "INSERT INTO users VALUES (2, 'bob', 25)")
db.exec(conn, "INSERT INTO users VALUES (3, 'carol', 35)")
db.exec(conn, "INSERT INTO users VALUES (4, 'dave', 28)")

-- select all
let rows = db.query(conn, "SELECT * FROM users")
assert_eq(rows.len(), 4)

-- rows are modules with c0, c1, c2 fields
let first = rows[0]
assert_eq(first.c0, 1)
assert_eq(first.c1, "alice")
assert_eq(first.c2, 30)

-- select with where (use column index names)
let young = db.query(conn, "SELECT * FROM users WHERE c1 = 'bob'")
assert_eq(young.len(), 1)
assert_eq(young[0].c1, "bob")
assert_eq(young[0].c2, 25)

-- delete
let r3 = db.exec(conn, "DELETE FROM users WHERE c1 = 'dave'")
assert_eq(r3, "ok")

-- verify delete
let after_del = db.query(conn, "SELECT * FROM users")
assert_eq(after_del.len(), 3)

-- create another table
db.exec(conn, "CREATE TABLE items (id, label, price)")
db.exec(conn, "INSERT INTO items VALUES (1, 'widget', 9)")
db.exec(conn, "INSERT INTO items VALUES (2, 'gadget', 15)")
db.exec(conn, "INSERT INTO items VALUES (3, 'doohickey', 5)")

-- query items
let items = db.query(conn, "SELECT * FROM items")
assert_eq(items.len(), 3)

-- select with numeric where
let cheap = db.query(conn, "SELECT * FROM items WHERE c2 = 5")
assert_eq(cheap.len(), 1)
assert_eq(cheap[0].c1, "doohickey")

-- close
let r4 = db.close(conn)
assert_eq(r4, "ok")

-- multiple dbs
let db1 = db.open("one")
let db2 = db.open("two")
db.exec(db1, "CREATE TABLE t1 (x)")
db.exec(db2, "CREATE TABLE t2 (y)")
db.exec(db1, "INSERT INTO t1 VALUES ('a')")
db.exec(db2, "INSERT INTO t2 VALUES ('b')")

let r_db1 = db.query(db1, "SELECT * FROM t1")
let r_db2 = db.query(db2, "SELECT * FROM t2")
assert_eq(r_db1.len(), 1)
assert_eq(r_db2.len(), 1)
assert_eq(r_db1[0].c0, "a")
assert_eq(r_db2[0].c0, "b")

db.close(db1)
db.close(db2)

-- test empty result
let conn2 = db.open("empty_test")
db.exec(conn2, "CREATE TABLE empty_tbl (val)")
let empty = db.query(conn2, "SELECT * FROM empty_tbl")
assert_eq(empty.len(), 0)

-- insert then query
db.exec(conn2, "INSERT INTO empty_tbl VALUES ('first')")
let one = db.query(conn2, "SELECT * FROM empty_tbl")
assert_eq(one.len(), 1)

-- delete all
db.exec(conn2, "DELETE FROM empty_tbl")
let gone = db.query(conn2, "SELECT * FROM empty_tbl")
assert_eq(gone.len(), 0)

db.close(conn2)

print("test_db: all passed")
