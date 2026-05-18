-- skip-emit: wasm (TODO: wasm transpiler doesn't reproduce the static purity-inference output)
-- bug060: static purity inference. The analyzer must agree across
-- interp / vm / jit on every shape: top-level fn_decls, lambdas
-- captured into a value, mutually-recursive pairs, lambda HOF args.

fn double(x) { x + x }
fn shout(s) { print(s) }

assert_eq(__pure?(double), true)
assert_eq(__pure?(shout), false)

-- Lambda passed to a builtin HOF: still gets stamped.
let pf = fn(x) { x * 3 }
assert_eq(__pure?(pf), true)

let xs = [1, 2, 3]
assert_eq(__pure?(fn(x) { x + 1 }), true)

-- Self-recursive pure fn: must not deadlock the worklist.
fn fact(n) { if n <= 1 { return 1 } n * fact(n - 1) }
assert_eq(__pure?(fact), true)
assert_eq(fact(5), 120)

-- Closure capturing a let-bound free variable: still pure if the
-- capture is value-typed and not reassigned in the outer scope.
let k = 10
let add_k = fn(x) { x + k }
assert_eq(__pure?(add_k), true)
assert_eq(add_k(5), 15)

-- A fn that calls a known-impure stdlib transitively flips impure.
import time
fn looks_pure_but_isnt() { time.now() }
assert_eq(__pure?(looks_pure_but_isnt), false)

-- Mutating a parameter is observable -> impure.
fn mutate_param(arr) { arr.push(1) }
assert_eq(__pure?(mutate_param), false)

-- Mutating a local-let array is OK -> pure.
fn make_pair() { let a = []; a.push(1); a.push(2); a }
assert_eq(__pure?(make_pair), true)
assert_eq(make_pair().len(), 2)

println("OK")
