-- Numeric edges: int overflow into bigint, integer / float coercion,
-- modulo with negative operands, float NaN identity.

-- overflow to bigint
let big = 9223372036854775807 + 1
assert_eq(big.to_str(), "9223372036854775808")

-- integer promotion on mixed arithmetic
assert_eq(1 + 1.5, 2.5)
assert_eq((1.5 * 2).to_str(), "3")

-- negative mod semantics (truncated toward zero in XS)
assert_eq(-7 % 3, -1)
assert_eq( 7 % -3, 1)

-- bigint arithmetic stays exact
let huge = 10 ** 30
assert_eq(huge.to_str(), "1000000000000000000000000000000")
let prod = huge * huge
assert_eq(prod.to_str().len(), 61)

-- float nan is not equal to itself
let nan = 0.0 / 0.0
assert_eq(nan == nan, false)

println("CONFORMANCE OK")
