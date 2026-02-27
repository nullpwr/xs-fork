/* interp_ast.c -- AST node constructors exposed to plugins.
   Each native_ast_* returns a small map describing a node tag and
   operands, used by parser-override plugins building AST fragments
   without round-tripping through the parser. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>

/* AST constructors */
Value *native_ast_int_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("int"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_int(0));
    return m;
}
Value *native_ast_float_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("float"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_float(0.0));
    return m;
}
Value *native_ast_str_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("str"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    return m;
}
Value *native_ast_bool_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("bool"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_FALSE_VAL));
    return m;
}
Value *native_ast_null_node(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("null"));
    return m;
}
Value *native_ast_ident(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("ident"));
    map_set(m->map, "name", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    return m;
}
Value *native_ast_binop(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("binop"));
    map_set(m->map, "op", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str("+"));
    map_set(m->map, "left", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "right", (argc > 2 && args[2]) ? value_incref(args[2]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_unary(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("unary"));
    map_set(m->map, "op", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str("-"));
    map_set(m->map, "expr", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_call(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("call"));
    map_set(m->map, "callee", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "args", (argc > 1 && args[1]) ? value_incref(args[1]) : xs_array_new());
    return m;
}
Value *native_ast_method_call(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("method_call"));
    map_set(m->map, "obj", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "method", (argc > 1 && args[1]) ? value_incref(args[1]) : xs_str(""));
    map_set(m->map, "args", (argc > 2 && args[2]) ? value_incref(args[2]) : xs_array_new());
    return m;
}
Value *native_ast_if_expr(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("if"));
    map_set(m->map, "cond", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "then", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_if_else(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("if"));
    map_set(m->map, "cond", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "then", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "else", (argc > 2 && args[2]) ? value_incref(args[2]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_block(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("block"));
    map_set(m->map, "stmts", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_array_new());
    map_set(m->map, "expr", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_let_decl(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("let"));
    map_set(m->map, "name", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    map_set(m->map, "value", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "type_ann", value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_var_decl(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("var"));
    map_set(m->map, "name", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    map_set(m->map, "value", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "type_ann", value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_fn_decl(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("fn_decl"));
    map_set(m->map, "name", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    map_set(m->map, "params", (argc > 1 && args[1]) ? value_incref(args[1]) : xs_array_new());
    map_set(m->map, "body", (argc > 2 && args[2]) ? value_incref(args[2]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "ret_type", value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_lambda(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("lambda"));
    map_set(m->map, "params", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_array_new());
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_return_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("return"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_assign(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("assign"));
    map_set(m->map, "target", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "value", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_for_loop(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("for"));
    map_set(m->map, "pattern", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "iter", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 2 && args[2]) ? value_incref(args[2]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_while_loop(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("while"));
    map_set(m->map, "cond", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_array(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("array"));
    map_set(m->map, "elements", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_array_new());
    return m;
}
Value *native_ast_map_node(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("map"));
    map_set(m->map, "keys", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_array_new());
    map_set(m->map, "values", (argc > 1 && args[1]) ? value_incref(args[1]) : xs_array_new());
    return m;
}
Value *native_ast_duration(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("duration"));
    map_set(m->map, "ms", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_float(0.0));
    return m;
}
Value *native_ast_color(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("color"));
    map_set(m->map, "r", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_int(0));
    map_set(m->map, "g", (argc > 1 && args[1]) ? value_incref(args[1]) : xs_int(0));
    map_set(m->map, "b", (argc > 2 && args[2]) ? value_incref(args[2]) : xs_int(0));
    map_set(m->map, "a", (argc > 3 && args[3]) ? value_incref(args[3]) : xs_int(255));
    return m;
}
Value *native_ast_date(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("date"));
    map_set(m->map, "value", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_str(""));
    return m;
}
Value *native_ast_size(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("size"));
    map_set(m->map, "bytes", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_float(0.0));
    return m;
}
Value *native_ast_angle(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("angle"));
    map_set(m->map, "radians", (argc > 0 && args[0]) ? value_incref(args[0]) : xs_float(0.0));
    return m;
}
Value *native_ast_every(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("every"));
    map_set(m->map, "interval", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_after(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("after"));
    map_set(m->map, "delay", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
Value *native_ast_timeout(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("timeout"));
    map_set(m->map, "duration", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    if (argc > 2 && args[2])
        map_set(m->map, "fallback", value_incref(args[2]));
    return m;
}
Value *native_ast_debounce(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str("debounce"));
    map_set(m->map, "delay", (argc > 0 && args[0]) ? value_incref(args[0]) : value_incref(XS_NULL_VAL));
    map_set(m->map, "body", (argc > 1 && args[1]) ? value_incref(args[1]) : value_incref(XS_NULL_VAL));
    return m;
}
