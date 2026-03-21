-- sql!(...): a procedural macro that parses a SQL-ish DSL into a
-- query-builder map. compile-time enough to be useful: the macro
-- inspects the source text, validates the basic shape, and returns
-- a structured representation that the runtime can hand off to a
-- driver. the actual db driver is intentionally out of scope here.

@[macro]
fn sql(text) {
    -- minimum viable parser: split on whitespace, look for the
    -- shape "SELECT cols FROM table [WHERE pred]"
    let toks = text.split(" ")
    var i = 0
    var query = #{kind: "select", columns: [], table: null, where: null}

    if toks[i].upper() != "SELECT" {
        throw "sql!: expected SELECT, got " + toks[i]
    }
    i = i + 1

    -- columns until FROM. each column may have a trailing comma.
    while i < len(toks) && toks[i].upper() != "FROM" {
        var col = toks[i]
        if col.ends_with(",") { col = col.slice(0, len(col) - 1) }
        if col != "" { query.columns.push(col) }
        i = i + 1
    }
    if i >= len(toks) { throw "sql!: missing FROM" }
    i = i + 1
    query.table = toks[i]
    i = i + 1

    -- optional WHERE
    if i < len(toks) && toks[i].upper() == "WHERE" {
        i = i + 1
        var pred_parts = []
        while i < len(toks) {
            pred_parts.push(toks[i])
            i = i + 1
        }
        query.where = pred_parts.join(" ")
    }
    return query
}

let q = sql("SELECT id, name FROM users WHERE id > 10")
println(q.kind)              -- select
println(q.columns)           -- [id, name]
println(q.table)             -- users
println(q.where)             -- id > 10
