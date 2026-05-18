-- bug057: Deque and Stack constructors silently dropped their init
-- argument; Set already accepted one. The constructors now mirror
-- each other: Deque([...]) seeds the buffer, Stack([...]) pushes
-- the items in order.

import collections

let d = collections.Deque([1, 2, 3])
assert_eq(d.front(), 1)
assert_eq(d.back(), 3)
assert_eq(d.len(), 3)

let s = collections.Stack([10, 20, 30])
assert_eq(s.peek(), 30)
assert_eq(s.len(), 3)

-- empty constructor still works
let d2 = collections.Deque()
assert_eq(d2.len(), 0)
let s2 = collections.Stack()
assert_eq(s2.len(), 0)

println("bug057: ok")
