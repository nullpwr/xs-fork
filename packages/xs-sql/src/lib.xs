-- xs-sql: parameterised query builder for `db` (the in-memory KV store)
-- and any other backend that takes plain SQL strings.

fn _quote(v) {
    if v == null { return "NULL" }
    if typeof(v) == "int" or typeof(v) == "float" { return str(v) }
    if typeof(v) == "bool" { return if v { "1" } else { "0" } }
    return "'" + str(v).replace("'", "''") + "'"
}

fn select(table) {
    var cols = "*"
    var where_clauses = []
    var order = ""
    var limit_n = -1
    return #{
        columns: |cs| { cols = cs.join(", "); return _select_self() },
        where:   |k, op, v| { where_clauses.push(k + " " + op + " " + _quote(v)); return _select_self() },
        order_by: |c, dir| { order = " ORDER BY " + c + " " + (dir ?? "ASC"); return _select_self() },
        limit:    |n| { limit_n = n; return _select_self() },
        sql:      || {
            var s = "SELECT " + cols + " FROM " + table
            if not where_clauses.is_empty() { s = s + " WHERE " + where_clauses.join(" AND ") }
            s = s + order
            if limit_n > 0 { s = s + " LIMIT " + str(limit_n) }
            return s
        },
    }
}

fn _select_self() { return select("__placeholder__") } -- workaround for fluent return

fn insert(table, row) {
    let keys = row.keys()
    var vals = []
    for k in keys { vals.push(_quote(row.get(k))) }
    return "INSERT INTO " + table + " (" + keys.join(", ")
         + ") VALUES (" + vals.join(", ") + ")"
}

fn update(table, set_map, where_map) {
    var sets = []
    for k in set_map.keys() { sets.push(k + " = " + _quote(set_map.get(k))) }
    var s = "UPDATE " + table + " SET " + sets.join(", ")
    if where_map != null and not where_map.is_empty() {
        var ws = []
        for k in where_map.keys() { ws.push(k + " = " + _quote(where_map.get(k))) }
        s = s + " WHERE " + ws.join(" AND ")
    }
    return s
}

fn delete(table, where_map) {
    var s = "DELETE FROM " + table
    if where_map != null and not where_map.is_empty() {
        var ws = []
        for k in where_map.keys() { ws.push(k + " = " + _quote(where_map.get(k))) }
        s = s + " WHERE " + ws.join(" AND ")
    }
    return s
}
