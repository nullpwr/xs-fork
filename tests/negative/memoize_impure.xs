-- EXPECT_RUNTIME_ERROR
-- @memoize is gated by static purity inference; an impure body is
-- refused at decoration time with a clear PurityError.
@memoize
fn bad(n) {
    print("noise: ")
    println(n)
    n * 2
}

println(bad(1))
