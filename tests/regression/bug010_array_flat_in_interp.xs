-- skip-emit: wasm (TODO: wasm transpiler doesn't lower arr.flat/.flatten)
-- bug010: arr.flat() worked in the VM but not the interpreter (the
-- interpreter only knew "flatten"). Fix: alias "flat" to "flatten" in
-- both backends so cross-backend code stays uniform.
let nested = [[1, 2], [3, 4], [5]]
assert_eq(nested.flat(), [1, 2, 3, 4, 5])
assert_eq(nested.flatten(), [1, 2, 3, 4, 5])
println("bug010: ok")
