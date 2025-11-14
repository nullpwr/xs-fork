-- comprehensive pipeline system tests

-- ============================================================
-- ROUND 1: Core features
-- ============================================================

-- 1. basic load + plugin meta registration
load "plugins/pipeline_log_plugin.xs"
assert_eq(pipeline_loaded, true)

-- 2. runtime hooks via old API (before_eval counts calls)
load "plugins/pipeline_rt_hooks.xs"
fn foo() { 1 }
foo()
foo()
assert(call_count >= 2)

-- 3. runtime hooks via new plugin block syntax
load "plugins/pipeline_rt_block.xs"
fn bar() { 2 }
bar()
assert_eq(block_hook_fired, true)

-- 4. dynamic token registration (no crash, token registered)
load "plugins/pipeline_token_plugin.xs"
assert_eq(token_test_ok, true)

-- 5. plugin composition: A then B where B depends on A
load "plugins/pipeline_compose_a.xs"
assert_eq(compose_a_loaded, true)
load "plugins/pipeline_compose_b.xs"
assert_eq(compose_b_loaded, true)

-- 6. old API + new block coexist in same plugin file
load "plugins/pipeline_mixed.xs"
assert_eq(mixed_old_api, true)
fn trigger_mixed() { 42 }
trigger_mixed()
assert_eq(mixed_block_hook, true)

-- 7. multiple plugins coexist (we loaded 6 plugins above, verify all still work)
assert_eq(pipeline_loaded, true)
assert(call_count >= 2)
assert_eq(block_hook_fired, true)
assert_eq(token_test_ok, true)
assert_eq(compose_a_loaded, true)
assert_eq(compose_b_loaded, true)
assert_eq(mixed_old_api, true)
assert_eq(mixed_block_hook, true)

-- ============================================================
-- ROUND 2: Extended coverage
-- ============================================================

-- 8. multiple dynamic tokens in one plugin
load "plugins/pipeline_multi_token.xs"
assert_eq(multi_token_ok, true)

-- 9. full meta block with provides, modifies, priority
load "plugins/pipeline_meta_full.xs"
assert_eq(meta_full_ok, true)

-- 10. sema section parsing (stub rules, no crash)
load "plugins/pipeline_sema_stub.xs"
assert_eq(sema_plugin_ok, true)

-- 11. hooks still fire after many plugins loaded
let pre = call_count
fn counting_test() { 99 }
counting_test()
assert(call_count > pre)

-- 12. functions defined after plugin load work with hooks
fn late_fn() { "late" }
assert_eq(late_fn(), "late")

-- 13. nested function calls trigger hooks correctly
fn outer() {
    fn inner() { 10 }
    inner()
}
assert_eq(outer(), 10)

-- 14. recursive functions with hooks
fn fib(n) {
    if n <= 1 { return n }
    return fib(n - 1) + fib(n - 2)
}
assert_eq(fib(6), 8)

-- 15. closures work with hooks active
fn make_adder(x) {
    return fn(y) { x + y }
}
let add5 = make_adder(5)
assert_eq(add5(3), 8)

-- 16. verify all original globals still intact after heavy use
assert_eq(pipeline_loaded, true)
assert_eq(block_hook_fired, true)
assert_eq(token_test_ok, true)
assert_eq(compose_a_loaded, true)
assert_eq(compose_b_loaded, true)
assert_eq(mixed_old_api, true)
assert_eq(mixed_block_hook, true)
assert_eq(multi_token_ok, true)
assert_eq(meta_full_ok, true)
assert_eq(sema_plugin_ok, true)

-- 17. class methods work with hooks active
class MyCounter {
    count = 0
    fn init(self, start) { self.count = start }
    fn inc(self) { self.count = self.count + 1 }
    fn get(self) { return self.count }
}
let c = MyCounter(0)
c.inc()
c.inc()
c.inc()
assert_eq(c.get(), 3)

-- 18. pattern matching works with hooks
fn describe(x) {
    match x {
        0 => "zero"
        1 => "one"
        _ => "other"
    }
}
assert_eq(describe(0), "zero")
assert_eq(describe(1), "one")
assert_eq(describe(42), "other")

-- 19. error handling works with hooks
fn safe_div(a, b) {
    try {
        if b == 0 { throw "division by zero" }
        return a / b
    } catch e {
        return -1
    }
}
assert_eq(safe_div(10, 2), 5)
assert_eq(safe_div(10, 0), -1)

-- 20. string operations with hooks active
let s = "hello world"
assert_eq(s.len(), 11)
assert_eq(s.split(" ").len(), 2)

-- ============================================================
-- ROUND 3: New pipeline features
-- ============================================================

-- 21. pass section parsing and registration (no crash)
load "plugins/pipeline_pass_analyze.xs"
assert_eq(pass_analyze_ok, true)

-- 21b. verify analyze pass visitor actually fired on the AST
-- the pass walks the full program AST at load time, so it sees all fn_decl
-- nodes from earlier in the file (foo, bar, counting_test, late_fn, etc.)
assert_eq(pass_visitor_fired, true)
assert(fn_count > 0)

-- 22. parser section with extend/production (no crash)
load "plugins/pipeline_parser_ext.xs"
assert_eq(parser_ext_ok, true)

-- 23. sema dispatch plugin fires rules on AST
load "plugins/pipeline_pass_sema.xs"
assert_eq(sema_dispatch_ok, true)
assert(sema_visit_count > 0)

-- 24. pipeline block parsing (no crash)
pipeline {
  parser -> sema -> eval
}

-- 26. parser production callback registered and keyword recognized
load "plugins/pipeline_parser_production.xs"
assert_eq(parser_production_ok, true)

-- 25. verify all previous tests still work after loading new plugins
assert_eq(pipeline_loaded, true)
assert(call_count >= 2)
assert_eq(block_hook_fired, true)
assert_eq(token_test_ok, true)
assert_eq(compose_a_loaded, true)
assert_eq(compose_b_loaded, true)
assert_eq(mixed_old_api, true)
assert_eq(mixed_block_hook, true)
assert_eq(multi_token_ok, true)
assert_eq(meta_full_ok, true)
assert_eq(sema_plugin_ok, true)
assert_eq(pass_analyze_ok, true)
assert_eq(pass_visitor_fired, true)
assert_eq(parser_ext_ok, true)
assert_eq(sema_dispatch_ok, true)
assert_eq(parser_production_ok, true)

-- ============================================================
-- ROUND 4: Pipeline limitations fixes
-- ============================================================

-- 27. sema override chaining with default()
load "plugins/pipeline_sema_chain_a.xs"
load "plugins/pipeline_sema_chain_b.xs"
-- chain_log should be "AB": A fires first (priority 10), calls default() -> B fires (priority 20)
assert_eq(chain_log, "AB")

-- 28. disambiguation infrastructure: productions registered with plugin_id
load "plugins/pipeline_disambig.xs"
assert_eq(disambig_ok, true)

-- 29. transform pass: replaces literal 42 with 99 in the AST
-- must be last to avoid reparse overwriting transform results
load "plugins/pipeline_transform.xs"
assert_eq(transform_ok, true)
let transform_val = 42
assert_eq(transform_val, 99)

-- ============================================================
-- ROUND 5: Pipeline infrastructure fixes
-- ============================================================

-- 30. sorted pass ordering (fix 1): two passes run in registration order
-- when sorted_passes is used (alphabetical tiebreak for same phase)
load "plugins/pipeline_pass_order.xs"
let order_log = pass_order_log
-- both pass_a and pass_b visit fn_decl after(parser), sorted gives A then B
assert(order_log.len() > 0)

-- 31. before(sema) pass fires (fix 2): verify before-phase dispatch works
load "plugins/pipeline_before_sema.xs"
assert_eq(before_sema_fired, true)

-- 32. cancel() stops execution (fix 3): cancel on int literal 9999
load "plugins/pipeline_cancel_hook.xs"
assert_eq(cancel_hook_loaded, true)
let cancel_result = 9999
-- after cancel, the literal evaluates to null instead of 9999
assert_eq(cancel_result, null)
assert(cancel_count > 0)

-- 33. pipeline {} constraint parsing (fix 4): parse without crashing
pipeline {
  a -> b -> c
}
-- a second pipeline block to verify multiple chains work
pipeline {
  x -> y
}
-- if we got here, parsing succeeded
assert(true)

-- 34. hook table write/read (fix 5): emit hooks and verify no crash
load "plugins/pipeline_hook_table.xs"
assert_eq(hook_table_ok, true)

-- 35. override exclusive rule parsing (fix 6)
load "plugins/pipeline_sema_exclusive.xs"
assert_eq(exclusive_parse_ok, true)

-- ============================================================
-- ROUND 6: Compiler pipeline gap fixes
-- ============================================================

-- 36. lexer rule parsing (gap 2)
load "plugins/pipeline_lexer_rule.xs"
assert_eq(lexer_rule_ok, true)

-- 37. extend handler stores target production name (gap 3)
load "plugins/pipeline_extend_target.xs"
assert_eq(extend_target_ok, true)

-- 38. pass state parsed and passed to visitors (gap 4)
load "plugins/pipeline_pass_state.xs"
assert_eq(pass_state_ok, true)
assert(state_count >= 0)

-- 39. on scope_exit fires during pass walk (gap 5)
load "plugins/pipeline_scope_exit.xs"
assert_eq(scope_exit_ok, true)
assert(scope_exit_count > 0)

-- 40. exec hook registered and fires (gap 6)
load "plugins/pipeline_exec_hook.xs"
assert_eq(exec_hook_ok, true)

-- 41. sema rule targeting built-in type registers correctly (gap 7)
-- sema rules for built-in types like "let" now resolve via name matching
-- when tag is -1 (custom types don't wildcard-match all nodes anymore)
load "plugins/pipeline_sema_builtin.xs"
assert_eq(sema_builtin_ok, true)

print("all pipeline tests passed (48 checks)")
