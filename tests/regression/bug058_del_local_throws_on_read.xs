-- bug058: `del x` on a local slot in the VM was storing null instead
-- of tombstoning the slot. A subsequent read of x returned null and
-- the catch block never fired. Fixed by introducing OP_DEL_LOCAL which
-- stores XS_DELETED_VAL, and checking for that sentinel in OP_LOAD_LOCAL.

var caught = false
var x = 42
assert_eq(x, 42)
del x
try {
    let _ = x
} catch e {
    caught = true
}
assert(caught, "reading a del'd local should raise a catchable error")

-- del inside a function (same slot logic, different frame)
fn check_fn_del() {
    var y = 99
    del y
    try {
        let _ = y
    } catch e {
        return true
    }
    return false
}
assert(check_fn_del(), "del inside a fn should make subsequent reads throw")

println("bug058: ok")
