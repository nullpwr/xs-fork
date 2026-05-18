-- bug022: reactive `bind` only fired when the dependency was rebound
-- via env_set (i.e. plain `x = v`). Mutations through `arr[i] = v` or
-- `m.k = v` modified the value in place without notifying, so any bind
-- referencing the parent name silently went stale. Fix: NODE_ASSIGN's
-- INDEX/FIELD branches walk the target back to its root identifier
-- and call env_notify_reactive on it.
var arr = [1, 2, 3]
bind sum = arr[0] + arr[1] + arr[2]
assert_eq(sum, 6)

arr[1] = 99
assert_eq(sum, 103)

var m = #{x: 5}
bind doubled = m.x * 2
assert_eq(doubled, 10)
m.x = 7
assert_eq(doubled, 14)

println("bug022: ok")
