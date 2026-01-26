-- Async: async fns produce promises, await suspends until fulfilled.
-- Results propagate exactly once in order.

async fn fetch_val(n) { n * 10 }

async fn main() {
    let a = await fetch_val(1)
    let b = await fetch_val(2)
    a + b
}

assert_eq(await main(), 30)

-- parallel spawn + join
async fn delayed(v) { v }

async fn join_two() {
    let p1 = spawn delayed(1)
    let p2 = spawn delayed(2)
    (await p1) + (await p2)
}

assert_eq(await join_two(), 3)

println("CONFORMANCE OK")
