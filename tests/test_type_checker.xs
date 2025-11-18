-- type checker basic tests (the checker runs at semantic analysis time)
-- these tests verify that correctly-typed code runs fine

-- basic type annotations
let x: int = 42
assert_eq(x, 42)

let s: string = "hello"
assert_eq(s, "hello")

let b: bool = true
assert(b, "bool type")

let f: float = 3.14
assert(f > 3.0, "float type")

-- function with typed params
fn add(a: int, b: int) -> int {
    return a + b
}
assert_eq(add(3, 4), 7)

-- inferred types
let inferred = 100
assert_eq(inferred, 100)

let arr = [1, 2, 3]
assert_eq(len(arr), 3)

-- struct types
struct Point {
    x: int,
    y: int
}

let p = Point { x: 10, y: 20 }
assert_eq(p.x, 10)
assert_eq(p.y, 20)

-- generic-like usage
fn identity(x) {
    return x
}
assert_eq(identity(42), 42)
assert_eq(identity("hi"), "hi")

-- option types (nullable)
fn maybe_find(items, target) {
    for item in items {
        if item == target {
            return item
        }
    }
    return null
}

let found = maybe_find([1, 2, 3], 2)
assert_eq(found, 2)
let not_found = maybe_find([1, 2, 3], 99)
assert(not_found == null, "not found returns null")

-- map types
let scores: map = {"alice": 100, "bob": 95}
assert_eq(scores["alice"], 100)

-- nested types
let matrix = [[1, 2], [3, 4]]
assert_eq(matrix[0][0], 1)
assert_eq(matrix[1][1], 4)

-- function as value
fn double(n) { return n * 2 }
let f2 = double
assert_eq(f2(5), 10)

-- tuple
let pair = (1, "hello")
assert_eq(pair.0, 1)
assert_eq(pair.1, "hello")

println("  type_checker: all tests passed")
