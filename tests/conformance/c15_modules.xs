-- The stdlib module namespace: every documented stdlib entry must
-- resolve. Values need not match across backends; the existence of
-- the binding is the contract.

import math
import json
import fs
import time

assert_eq(math.abs(-3), 3)
assert_eq(math.max(1, 9), 9)
assert_eq(math.min(4, 2), 2)

let obj = #{"x": 1, "y": [2, 3]}
let encoded = json.stringify(obj)
assert_eq(encoded.len() > 0, true)
let decoded = json.parse(encoded)
assert_eq(decoded["x"], 1)

assert_eq(time.now() >= 0, true)

println("CONFORMANCE OK")
