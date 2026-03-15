-- json round-trip: parse a medium array of records, stringify, reparse
import json

var records = []
for i in 0..500 {
    records.push(#{
        id: i,
        name: "user_{i}",
        email: "user{i}@example.com",
        active: i % 3 == 0,
        tags: ["a", "b", "c"],
    })
}

let s = json.stringify(records)
let parsed = json.parse(s)
assert_eq(parsed.len(), 500)

-- round-trip again
let s2 = json.stringify(parsed)
assert_eq(s.len(), s2.len())

println("json records =", parsed.len())
