-- async runtime / promise tests

-- resolved promise
let p = Promise.resolve(42)
assert_eq(Promise.state(p), "resolved")
assert_eq(Promise.value(p), 42)

-- rejected promise
let err_p = Promise.reject("something went wrong")
assert_eq(Promise.state(err_p), "rejected")
assert_eq(Promise.value(err_p), "something went wrong")

-- Promise.all with already-resolved values
let all_input = [
    Promise.resolve(1),
    Promise.resolve(2),
    Promise.resolve(3)
]
let all_result = Promise.all(all_input)
assert_eq(Promise.state(all_result), "resolved")
let all_val = Promise.value(all_result)
assert_eq(len(all_val), 3)
assert_eq(all_val[0], 1)
assert_eq(all_val[1], 2)
assert_eq(all_val[2], 3)

-- Promise.race - first resolved wins
let race_input = [
    Promise.resolve("first"),
    Promise.resolve("second")
]
let race_result = Promise.race(race_input)
assert_eq(Promise.state(race_result), "resolved")
assert_eq(Promise.value(race_result), "first")

-- Promise.any - first success
let any_input = [
    Promise.reject("err1"),
    Promise.resolve("ok"),
    Promise.reject("err2")
]
let any_result = Promise.any(any_input)
assert_eq(Promise.state(any_result), "resolved")
assert_eq(Promise.value(any_result), "ok")

-- Promise.any - all rejected
let all_rej = [
    Promise.reject("e1"),
    Promise.reject("e2")
]
let rej_result = Promise.any(all_rej)
assert_eq(Promise.state(rej_result), "rejected")

-- Promise.all_settled
let settled_input = [
    Promise.resolve("yes"),
    Promise.reject("no"),
    Promise.resolve(42)
]
let settled_result = Promise.all_settled(settled_input)
assert_eq(Promise.state(settled_result), "resolved")
let settled_val = Promise.value(settled_result)
assert_eq(len(settled_val), 3)
assert_eq(settled_val[0]["status"], "fulfilled")
assert_eq(settled_val[0]["value"], "yes")
assert_eq(settled_val[1]["status"], "rejected")
assert_eq(settled_val[1]["reason"], "no")
assert_eq(settled_val[2]["status"], "fulfilled")
assert_eq(settled_val[2]["value"], 42)

-- Promise.all with empty array
let empty_all = Promise.all([])
assert_eq(Promise.state(empty_all), "resolved")
let empty_val = Promise.value(empty_all)
assert_eq(len(empty_val), 0)

-- drain
Promise.drain()

println("  async: all tests passed")
