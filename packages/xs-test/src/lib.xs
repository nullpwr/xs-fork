-- xs-test: tiny test runner.
--
--   import test
--   test.describe("array", || {
--       test.it("pushes", || {
--           let a = []
--           a.push(1); a.push(2)
--           test.eq(a.len(), 2)
--       })
--       test.it("pops", || {
--           let a = [1, 2, 3]
--           test.eq(a.pop(), 3)
--       })
--   })
--   test.run()

import time

var _suites = []
var _current = null
var _results = []

fn describe(name, body) {
    let prev = _current
    _current = #{name: name, tests: [], hooks: #{before: null, after: null}}
    body()
    _suites.push(_current)
    _current = prev
}

fn it(name, body) {
    if _current == null { describe("(top level)", || it(name, body)); return }
    _current.tests.push(#{name: name, body: body})
}

fn before_each(fn) { _current.hooks.before = fn }
fn after_each(fn)  { _current.hooks.after  = fn }

fn eq(a, b) {
    if a != b { throw "expected {b}, got {a}" }
}

fn neq(a, b) {
    if a == b { throw "expected != {b}, got {a}" }
}

fn truthy(v) { if not v { throw "expected truthy, got {v}" } }
fn falsy(v)  { if v     { throw "expected falsy, got {v}" } }

fn throws(fn, msg) {
    var caught = false
    var actual = null
    try { fn() }
    catch e { caught = true; actual = e }
    if not caught { throw "expected to throw, did not" }
    if msg != null and not str(actual).contains(msg) {
        throw "expected error to contain {msg}, got {actual}"
    }
}

fn run() {
    var passed = 0
    var failed = 0
    let start = time.monotonic()
    for s in _suites {
        println("\n  " + s.name)
        for t in s.tests {
            let label = "    " + t.name
            try {
                if s.hooks.before != null { s.hooks.before() }
                t.body()
                if s.hooks.after != null { s.hooks.after() }
                println("  ✓ " + t.name)
                passed = passed + 1
            } catch e {
                println("  ✗ " + t.name + "  -- " + str(e))
                failed = failed + 1
                _results.push(#{suite: s.name, test: t.name, error: str(e)})
            }
        }
    }
    let dur = time.monotonic() - start
    println("\n  {passed} passed, {failed} failed  ({dur}s)")
    return #{passed: passed, failed: failed, results: _results}
}
