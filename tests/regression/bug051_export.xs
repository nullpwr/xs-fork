-- bug051: @export("alias") binds the decorated fn under the alias
-- alongside its original name. resolution sees both, so the alias
-- can be called like any other top-level fn. parity covers
-- interp / vm / jit.

@export("publicName") fn local_name() {
    return 42
}

assert_eq(publicName(), 42)
assert_eq(local_name(), 42)

println("bug051: ok")
exit(0)
