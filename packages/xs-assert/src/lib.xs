-- xs-assert: deep equal + helpful failure messages.

fn _type(v) {
    if v == null { return "null" }
    return typeof(v)
}

fn deep_eq(a, b) {
    if _type(a) != _type(b) { return false }
    if typeof(a) == "array" {
        if a.len() != b.len() { return false }
        for i in 0..a.len() {
            if not deep_eq(a[i], b[i]) { return false }
        }
        return true
    }
    if typeof(a) == "map" {
        let ka = a.keys()
        if ka.len() != b.keys().len() { return false }
        for k in ka {
            if not b.contains(k) { return false }
            if not deep_eq(a.get(k), b.get(k)) { return false }
        }
        return true
    }
    return a == b
}

fn eq(actual, expected, msg) {
    if not deep_eq(actual, expected) {
        let m = msg ?? "values differ"
        throw "{m}: expected {expected}, got {actual}"
    }
}

fn neq(a, b, msg) {
    if deep_eq(a, b) {
        throw (msg ?? "expected different values, both were {a}")
    }
}

fn truthy(v, msg) {
    if not v { throw (msg ?? "expected truthy, got {v}") }
}

fn falsy(v, msg) {
    if v { throw (msg ?? "expected falsy, got {v}") }
}

fn includes(haystack, needle, msg) {
    if typeof(haystack) == "str" {
        if not haystack.contains(needle) {
            throw (msg ?? "expected '{haystack}' to contain '{needle}'")
        }
    } else if typeof(haystack) == "array" {
        var found = false
        for x in haystack { if deep_eq(x, needle) { found = true; break } }
        if not found { throw (msg ?? "expected array to contain {needle}") }
    } else {
        throw "includes: unsupported type {typeof(haystack)}"
    }
}

fn throws_with(fn, expected_msg) {
    var caught = false
    var actual = null
    try { fn() }
    catch e { caught = true; actual = e }
    if not caught { throw "expected to throw" }
    if expected_msg != null and not str(actual).contains(expected_msg) {
        throw "expected '{expected_msg}', got '{actual}'"
    }
}

fn near(a, b, eps, msg) {
    let e = eps ?? 1e-9
    let d = if a > b { a - b } else { b - a }
    if d > e { throw (msg ?? "{a} not within {e} of {b}") }
}
