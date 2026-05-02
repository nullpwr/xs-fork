#include "transpiler/js.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "core/strbuf.h"

/* forward declarations */
static void emit_node(SB *s, Node *n, int depth);
static void emit_expr(SB *s, Node *n, int depth);
static void emit_stmt(SB *s, Node *n, int depth);
static void emit_block_body(SB *s, Node *block, int depth);
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth);
static int node_has_perform(Node *n);
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth);

/* state: are we inside a class method body? */
static int in_class_method = 0;

/* helpers */
static int is_callee_name(Node *callee, const char *name) {
    return callee && VAL_TAG(callee) == NODE_IDENT && strcmp(callee->ident.name, name) == 0;
}

static void emit_params_ex(SB *s, ParamList *pl, int skip_self) {
    sb_addc(s, '(');
    int first = 1;
    for (int i = 0; i < pl->len; i++) {
        Param *p = &pl->items[i];
        if (skip_self && p->name && strcmp(p->name, "self") == 0) continue;
        if (!first) sb_add(s, ", ");
        first = 0;
        if (p->variadic) sb_add(s, "...");
        if (p->name) sb_add(s, p->name);
        else sb_add(s, "_");
        if (p->default_val) {
            sb_add(s, " = ");
            emit_expr(s, p->default_val, 0);
        }
    }
    sb_addc(s, ')');
}

static void emit_params(SB *s, ParamList *pl) {
    emit_params_ex(s, pl, 0);
}

/* expression emitter */
static void emit_expr(SB *s, Node *n, int depth) {
    if (!n) { sb_add(s, "undefined"); return; }
    switch (VAL_TAG(n)) {
    case NODE_LIT_INT:
        sb_printf(s, "%" PRId64, n->lit_int.ival);
        break;
    case NODE_LIT_BIGINT:
        sb_printf(s, "%sn", n->lit_bigint.bigint_str);
        break;
    case NODE_LIT_FLOAT:
        sb_printf(s, "%g", n->lit_float.fval);
        break;
    case NODE_LIT_STRING:
        sb_addc(s, '"');
        if (n->lit_string.sval) {
            for (const char *p = n->lit_string.sval; *p; p++) {
                if (*p == '"') sb_add(s, "\\\"");
                else if (*p == '\\') sb_add(s, "\\\\");
                else if (*p == '\n') sb_add(s, "\\n");
                else if (*p == '\r') sb_add(s, "\\r");
                else if (*p == '\t') sb_add(s, "\\t");
                else sb_addc(s, *p);
            }
        }
        sb_addc(s, '"');
        break;
    case NODE_INTERP_STRING: {
        /* template literal using parts list */
        sb_addc(s, '`');
        NodeList *parts = &n->lit_string.parts;
        for (int i = 0; i < parts->len; i++) {
            Node *part = parts->items[i];
            if (VAL_TAG(part) == NODE_LIT_STRING) {
                /* literal segment: escape backticks */
                if (part->lit_string.sval) {
                    for (const char *p = part->lit_string.sval; *p; p++) {
                        if (*p == '`') sb_add(s, "\\`");
                        else if (*p == '$') sb_add(s, "\\$");
                        else sb_addc(s, *p);
                    }
                }
            } else {
                sb_add(s, "${__xs_repr(");
                emit_expr(s, part, depth);
                sb_add(s, ")}");
            }
        }
        sb_addc(s, '`');
        break;
    }
    case NODE_LIT_BOOL:
        sb_add(s, n->lit_bool.bval ? "true" : "false");
        break;
    case NODE_LIT_NULL:
        sb_add(s, "null");
        break;
    case NODE_LIT_CHAR:
        sb_printf(s, "'%c'", n->lit_char.cval);
        break;
    case NODE_LIT_ARRAY:
    case NODE_LIT_TUPLE:
        sb_addc(s, '[');
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->lit_array.elems.items[i], depth);
        }
        sb_addc(s, ']');
        break;
    case NODE_LIT_MAP:
        sb_add(s, "new Map([");
        for (int i = 0; i < n->lit_map.keys.len; i++) {
            if (i) sb_add(s, ", ");
            sb_addc(s, '[');
            emit_expr(s, n->lit_map.keys.items[i], depth);
            sb_add(s, ", ");
            emit_expr(s, n->lit_map.vals.items[i], depth);
            sb_addc(s, ']');
        }
        sb_add(s, "])");
        break;
    case NODE_IDENT:
        if (in_class_method && n->ident.name && strcmp(n->ident.name, "self") == 0)
            sb_add(s, "this");
        else
            sb_add(s, n->ident.name);
        break;
    case NODE_BINOP: {
        const char *op = n->binop.op;
        if (strcmp(op, "++") == 0) {
            /* string concat -> + */
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " + ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "//") == 0) {
            /* integer division */
            sb_add(s, "Math.floor(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " / ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "and") == 0) {
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " && ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "or") == 0) {
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " || ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "**") == 0) {
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " ** ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "??") == 0) {
            /* null coalescing -> direct JS ?? */
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, " ?? ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "?.") == 0) {
            /* optional chaining */
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_add(s, "?.");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "+") == 0) {
            sb_add(s, "__xs_add(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "/") == 0) {
            sb_add(s, "__xs_div(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else {
            /* default: -, *, %, ==, !=, <, >, <=, >=, &, |, ^, <<, >> */
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_addc(s, ' ');
            /* map == to === and != to !== for JS */
            if (strcmp(op, "==") == 0) sb_add(s, "===");
            else if (strcmp(op, "!=") == 0) sb_add(s, "!==");
            else sb_add(s, op);
            sb_addc(s, ' ');
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_UNARY: {
        const char *op = n->unary.op;
        if (strcmp(op, "not") == 0) {
            sb_add(s, "(!");
            emit_expr(s, n->unary.expr, depth);
            sb_addc(s, ')');
        } else if (n->unary.prefix) {
            sb_add(s, op);
            emit_expr(s, n->unary.expr, depth);
        } else {
            emit_expr(s, n->unary.expr, depth);
            sb_add(s, op);
        }
        break;
    }
    case NODE_CALL: {
        /* special builtins */
        if (is_callee_name(n->call.callee, "println")) {
            sb_add(s, "__xs_print(");
        } else if (is_callee_name(n->call.callee, "print")) {
            sb_add(s, "__xs_write(");
        } else if (is_callee_name(n->call.callee, "str")) {
            sb_add(s, "String(");
        } else if (is_callee_name(n->call.callee, "int")) {
            sb_add(s, "Math.trunc(Number(");
        } else if (is_callee_name(n->call.callee, "float")) {
            sb_add(s, "Number(");
        } else if (is_callee_name(n->call.callee, "type")) {
            sb_add(s, "typeof(");
        } else if (is_callee_name(n->call.callee, "len")) {
            /* len(x) -> (x).length - handled specially below */
            sb_addc(s, '(');
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ").length");
            break;
        } else if (is_callee_name(n->call.callee, "assert")) {
            sb_add(s, "(function(){ if (!(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ")) throw new Error(\"assertion failed\"); })()");
            break;
        } else if (is_callee_name(n->call.callee, "assert_eq")) {
            sb_add(s, "(function(){ if ((");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ") !== (");
            if (n->call.args.len > 1) emit_expr(s, n->call.args.items[1], depth);
            sb_add(s, ")) throw new Error(\"assertion failed: \" + (");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ") + \" !== \" + (");
            if (n->call.args.len > 1) emit_expr(s, n->call.args.items[1], depth);
            sb_add(s, ")); })()");
            break;
        } else if (is_callee_name(n->call.callee, "push") && n->call.args.len == 2) {
            emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ".push(");
            emit_expr(s, n->call.args.items[1], depth);
            sb_addc(s, ')');
            break;
        } else if (is_callee_name(n->call.callee, "input")) {
            sb_add(s, "prompt(");
        } else {
            /* check if callee is a capitalized identifier (constructor call) */
            int is_ctor = 0;
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT &&
                n->call.callee->ident.name && n->call.callee->ident.name[0] >= 'A' &&
                n->call.callee->ident.name[0] <= 'Z') {
                is_ctor = 1;
            }
            if (is_ctor) sb_add(s, "new ");
            emit_expr(s, n->call.callee, depth);
            sb_addc(s, '(');
        }
        for (int i = 0; i < n->call.args.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->call.args.items[i], depth);
        }
        /* kwargs */
        if (n->call.kwargs.len > 0) {
            if (n->call.args.len > 0) sb_add(s, ", ");
            sb_addc(s, '{');
            for (int i = 0; i < n->call.kwargs.len; i++) {
                if (i) sb_add(s, ", ");
                sb_add(s, n->call.kwargs.items[i].key);
                sb_add(s, ": ");
                emit_expr(s, n->call.kwargs.items[i].val, depth);
            }
            sb_addc(s, '}');
        }
        /* close extra paren for int() which wraps in Math.trunc(Number(...)) */
        if (is_callee_name(n->call.callee, "int")) {
            sb_add(s, "))");
        } else {
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_METHOD_CALL: {
        /* Map a handful of XS method names to their JS equivalents. JS arrays
           and strings don't have `.len()` but they do have `.length`, and
           `.push/pop/reverse/sort` exist on Array.prototype, etc. This is a
           shim, not a faithful XS runtime, so it covers the common homepage
           cases and leaves the rest as-is. */
        const char *m = n->method_call.method;
        if (m && strcmp(m, "len") == 0 && n->method_call.args.len == 0) {
            sb_addc(s, '(');
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ").length");
            break;
        }
        emit_expr(s, n->method_call.obj, depth);
        if (n->method_call.optional) sb_add(s, "?.");
        else sb_addc(s, '.');
        sb_add(s, n->method_call.method);
        sb_addc(s, '(');
        for (int i = 0; i < n->method_call.args.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[i], depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_INDEX:
        sb_add(s, "__xs_idx(");
        emit_expr(s, n->index.obj, depth);
        sb_add(s, ", ");
        emit_expr(s, n->index.index, depth);
        sb_addc(s, ')');
        break;
    case NODE_FIELD:
        emit_expr(s, n->field.obj, depth);
        if (n->field.optional) sb_add(s, "?.");
        else sb_addc(s, '.');
        sb_add(s, n->field.name);
        break;
    case NODE_SCOPE:
        for (int i = 0; i < n->scope.nparts; i++) {
            if (i) sb_addc(s, '.');
            sb_add(s, n->scope.parts[i]);
        }
        break;
    case NODE_ASSIGN: {
        Node *tgt = n->assign.target;
        if (tgt && VAL_TAG(tgt) == NODE_INDEX && strcmp(n->assign.op, "=") == 0) {
            sb_add(s, "__xs_setidx(");
            emit_expr(s, tgt->index.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, tgt->index.index, depth);
            sb_add(s, ", ");
            emit_expr(s, n->assign.value, depth);
            sb_addc(s, ')');
        } else {
            emit_expr(s, n->assign.target, depth);
            sb_addc(s, ' ');
            sb_add(s, n->assign.op);
            sb_addc(s, ' ');
            emit_expr(s, n->assign.value, depth);
        }
        break;
    }
    case NODE_RANGE: {
        if (n->range.inclusive) {
            sb_add(s, "Array.from({length: (");
            emit_expr(s, n->range.end, depth);
            sb_add(s, ") - (");
            emit_expr(s, n->range.start, depth);
            sb_add(s, ") + 1}, (_, i) => i + (");
            emit_expr(s, n->range.start, depth);
            sb_add(s, "))");
        } else {
            sb_add(s, "Array.from({length: (");
            emit_expr(s, n->range.end, depth);
            sb_add(s, ") - (");
            emit_expr(s, n->range.start, depth);
            sb_add(s, ")}, (_, i) => i + (");
            emit_expr(s, n->range.start, depth);
            sb_add(s, "))");
        }
        break;
    }
    case NODE_LAMBDA: {
        int lam_is_gen = n->lambda.is_generator ||
                          node_has_perform(n->lambda.body);
        sb_addc(s, '(');
        if (lam_is_gen) {
            sb_add(s, "function*");
            emit_params(s, &n->lambda.params);
            sb_add(s, " {\n");
            if (n->lambda.body && VAL_TAG(n->lambda.body) == NODE_BLOCK) {
                emit_block_body(s, n->lambda.body, depth + 1);
                if (n->lambda.body->block.expr) {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, n->lambda.body->block.expr, depth + 1);
                    sb_add(s, ";\n");
                }
            } else {
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
                emit_expr(s, n->lambda.body, depth + 1);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        } else {
            emit_params(s, &n->lambda.params);
            sb_add(s, " => ");
            if (n->lambda.body && VAL_TAG(n->lambda.body) == NODE_BLOCK) {
                sb_add(s, "{\n");
                emit_block_body(s, n->lambda.body, depth + 1);
                if (n->lambda.body->block.expr) {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, n->lambda.body->block.expr, depth + 1);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth);
                sb_addc(s, '}');
            } else {
                sb_add(s, "{ return ");
                emit_expr(s, n->lambda.body, depth);
                sb_add(s, "; }");
            }
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_IF: {
        /* if used as expression -> IIFE */
        sb_add(s, "(() => { ");
        sb_add(s, "if (");
        emit_expr(s, n->if_expr.cond, depth);
        sb_add(s, ") ");
        if (n->if_expr.then && VAL_TAG(n->if_expr.then) == NODE_BLOCK) {
            sb_add(s, "{\n");
            emit_block_body(s, n->if_expr.then, depth + 1);
            if (n->if_expr.then->block.expr) {
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
                emit_expr(s, n->if_expr.then->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        } else {
            sb_add(s, "{ return ");
            emit_expr(s, n->if_expr.then, depth);
            sb_add(s, "; }");
        }
        for (int i = 0; i < n->if_expr.elif_conds.len; i++) {
            sb_add(s, " else if (");
            emit_expr(s, n->if_expr.elif_conds.items[i], depth);
            sb_add(s, ") ");
            Node *et = n->if_expr.elif_thens.items[i];
            if (et && VAL_TAG(et) == NODE_BLOCK) {
                sb_add(s, "{\n");
                emit_block_body(s, et, depth + 1);
                if (et->block.expr) {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, et->block.expr, depth + 1);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth);
                sb_addc(s, '}');
            } else {
                sb_add(s, "{ return ");
                emit_expr(s, et, depth);
                sb_add(s, "; }");
            }
        }
        if (n->if_expr.else_branch) {
            sb_add(s, " else ");
            Node *eb = n->if_expr.else_branch;
            if (VAL_TAG(eb) == NODE_BLOCK) {
                sb_add(s, "{\n");
                emit_block_body(s, eb, depth + 1);
                if (eb->block.expr) {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, eb->block.expr, depth + 1);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth);
                sb_addc(s, '}');
            } else {
                sb_add(s, "{ return ");
                emit_expr(s, eb, depth);
                sb_add(s, "; }");
            }
        }
        sb_add(s, " })()");
        break;
    }
    case NODE_MATCH: {
        /* IIFE: each arm gets its own block so pattern bindings are in scope
           when the guard is evaluated. JS `const` is in TDZ, not hoisted, so
           the old "if (cond && guard) { const x = __subject; ... }" form
           referenced bindings before declaration. */
        sb_add(s, "(() => {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __subject = ");
        emit_expr(s, n->match.subject, depth + 1);
        sb_add(s, ";\n");
        for (int i = 0; i < n->match.arms.len; i++) {
            MatchArm *arm = &n->match.arms.items[i];
            sb_indent(s, depth + 1);
            sb_add(s, "{\n");
            sb_indent(s, depth + 2);
            sb_add(s, "if (");
            emit_pattern_cond(s, arm->pattern, "__subject", depth + 2);
            sb_add(s, ") {\n");
            /* emit bindings inside the pattern-match block so the guard can use them */
            emit_pattern_bindings(s, arm->pattern, "__subject", depth + 3);
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "if (");
                emit_expr(s, arm->guard, depth + 3);
                sb_add(s, ") {\n");
            }
            int body_depth = arm->guard ? depth + 4 : depth + 3;
            if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                emit_block_body(s, arm->body, body_depth);
                if (arm->body->block.expr) {
                    sb_indent(s, body_depth);
                    sb_add(s, "return ");
                    emit_expr(s, arm->body->block.expr, body_depth);
                    sb_add(s, ";\n");
                }
            } else {
                sb_indent(s, body_depth);
                sb_add(s, "return ");
                emit_expr(s, arm->body, body_depth);
                sb_add(s, ";\n");
            }
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "}\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    }
    case NODE_BLOCK: {
        /* block as expression -> IIFE */
        sb_add(s, "(() => {\n");
        emit_block_body(s, n, depth + 1);
        if (n->block.expr) {
            sb_indent(s, depth + 1);
            sb_add(s, "return ");
            emit_expr(s, n->block.expr, depth + 1);
            sb_add(s, ";\n");
        }
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    }
    case NODE_CAST:
        emit_expr(s, n->cast.expr, depth);
        sb_printf(s, " /* as %s */", n->cast.type_name ? n->cast.type_name : "?");
        break;
    case NODE_STRUCT_INIT:
        sb_printf(s, "new %s(", n->struct_init.path ? n->struct_init.path : "Object");
        sb_addc(s, '{');
        for (int i = 0; i < n->struct_init.fields.len; i++) {
            if (i) sb_add(s, ", ");
            sb_add(s, n->struct_init.fields.items[i].key);
            sb_add(s, ": ");
            emit_expr(s, n->struct_init.fields.items[i].val, depth);
        }
        if (n->struct_init.rest) {
            if (n->struct_init.fields.len > 0) sb_add(s, ", ");
            sb_add(s, "...");
            emit_expr(s, n->struct_init.rest, depth);
        }
        sb_add(s, "})");
        break;
    case NODE_SPREAD:
        sb_add(s, "...");
        emit_expr(s, n->spread.expr, depth);
        break;
    case NODE_LIST_COMP: {
        /* [expr for pat in iter if cond] -> IIFE with loops */
        sb_add(s, "(() => { const __r = [];\n");
        for (int i = 0; i < n->list_comp.clause_pats.len; i++) {
            sb_indent(s, depth + 1 + i);
            sb_add(s, "for (const ");
            emit_expr(s, n->list_comp.clause_pats.items[i], depth);
            sb_add(s, " of ");
            emit_expr(s, n->list_comp.clause_iters.items[i], depth);
            sb_add(s, ") {\n");
            if (i < n->list_comp.clause_conds.len && n->list_comp.clause_conds.items[i]) {
                sb_indent(s, depth + 2 + i);
                sb_add(s, "if (");
                emit_expr(s, n->list_comp.clause_conds.items[i], depth);
                sb_add(s, ") {\n");
            }
        }
        int indent_inner = depth + 1 + n->list_comp.clause_pats.len;
        sb_indent(s, indent_inner);
        sb_add(s, "__r.push(");
        emit_expr(s, n->list_comp.element, depth);
        sb_add(s, ");\n");
        for (int i = n->list_comp.clause_pats.len - 1; i >= 0; i--) {
            if (i < n->list_comp.clause_conds.len && n->list_comp.clause_conds.items[i]) {
                sb_indent(s, depth + 2 + i);
                sb_add(s, "}\n");
            }
            sb_indent(s, depth + 1 + i);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth);
        sb_add(s, "return __r; })()");
        break;
    }
    case NODE_MAP_COMP: {
        /* {k: v for pat in iter if cond} -> IIFE with object builder */
        sb_add(s, "(() => { const __r = {};\n");
        for (int i = 0; i < n->map_comp.clause_pats.len; i++) {
            sb_indent(s, depth + 1 + i);
            sb_add(s, "for (const ");
            emit_expr(s, n->map_comp.clause_pats.items[i], depth);
            sb_add(s, " of ");
            emit_expr(s, n->map_comp.clause_iters.items[i], depth);
            sb_add(s, ") {\n");
            if (i < n->map_comp.clause_conds.len && n->map_comp.clause_conds.items[i]) {
                sb_indent(s, depth + 2 + i);
                sb_add(s, "if (");
                emit_expr(s, n->map_comp.clause_conds.items[i], depth);
                sb_add(s, ") {\n");
            }
        }
        int indent_inner2 = depth + 1 + n->map_comp.clause_pats.len;
        sb_indent(s, indent_inner2);
        sb_add(s, "__r[");
        emit_expr(s, n->map_comp.key, depth);
        sb_add(s, "] = ");
        emit_expr(s, n->map_comp.value, depth);
        sb_add(s, ";\n");
        for (int i = n->map_comp.clause_pats.len - 1; i >= 0; i--) {
            if (i < n->map_comp.clause_conds.len && n->map_comp.clause_conds.items[i]) {
                sb_indent(s, depth + 2 + i);
                sb_add(s, "}\n");
            }
            sb_indent(s, depth + 1 + i);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth);
        sb_add(s, "return __r; })()");
        break;
    }
    case NODE_AWAIT:
        sb_add(s, "(await ");
        emit_expr(s, n->await_.expr, depth);
        sb_addc(s, ')');
        break;
    case NODE_YIELD:
        sb_add(s, "(yield ");
        if (n->yield_.value) emit_expr(s, n->yield_.value, depth);
        sb_addc(s, ')');
        break;
    case NODE_SPAWN:
        sb_add(s, "(async () => ");
        emit_expr(s, n->spawn_.expr, depth);
        sb_add(s, ")()");
        break;
    case NODE_DO_EXPR:
        sb_add(s, "(function() {\n");
        if (n->do_expr.body && VAL_TAG(n->do_expr.body) == NODE_BLOCK) {
            emit_block_body(s, n->do_expr.body, depth + 1);
            if (n->do_expr.body->block.expr) {
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
                emit_expr(s, n->do_expr.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    case NODE_WITH:
        sb_add(s, "(function() {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const ");
        sb_add(s, n->with_.name ? n->with_.name : "__resource");
        sb_add(s, " = ");
        emit_expr(s, n->with_.expr, depth + 1);
        sb_add(s, ";\n");
        sb_indent(s, depth + 1);
        sb_add(s, "try {\n");
        if (n->with_.body && VAL_TAG(n->with_.body) == NODE_BLOCK) {
            emit_block_body(s, n->with_.body, depth + 2);
            if (n->with_.body->block.expr) {
                sb_indent(s, depth + 2);
                sb_add(s, "return ");
                emit_expr(s, n->with_.body->block.expr, depth + 2);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth + 1);
        sb_add(s, "} finally {\n");
        sb_indent(s, depth + 2);
        sb_printf(s, "if (%s && typeof %s.close === 'function') %s.close();\n",
                  n->with_.name ? n->with_.name : "__resource",
                  n->with_.name ? n->with_.name : "__resource",
                  n->with_.name ? n->with_.name : "__resource");
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    case NODE_ACTOR_DECL: {
        /* Emit a JS class for the actor */
        sb_add(s, "(function() {\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "class %s {\n", n->actor_decl.name ? n->actor_decl.name : "Actor");
        /* constructor with state initialization */
        sb_indent(s, depth + 2);
        sb_add(s, "constructor() {\n");
        for (int i = 0; i < n->actor_decl.state_fields.len; i++) {
            sb_indent(s, depth + 3);
            sb_printf(s, "this.%s = ", n->actor_decl.state_fields.items[i].key);
            if (n->actor_decl.state_fields.items[i].val)
                emit_expr(s, n->actor_decl.state_fields.items[i].val, depth + 3);
            else
                sb_add(s, "undefined");
            sb_add(s, ";\n");
        }
        sb_indent(s, depth + 2);
        sb_add(s, "}\n");
        /* methods */
        for (int i = 0; i < n->actor_decl.methods.len; i++) {
            Node *m = n->actor_decl.methods.items[i];
            if (VAL_TAG(m) != NODE_FN_DECL) continue;
            sb_indent(s, depth + 2);
            if (m->fn_decl.is_async) sb_add(s, "async ");
            sb_printf(s, "%s", m->fn_decl.name ? m->fn_decl.name : "anonymous");
            emit_params(s, &m->fn_decl.params);
            sb_add(s, " {\n");
            if (m->fn_decl.body)
                emit_block_body(s, m->fn_decl.body, depth + 3);
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "return %s;\n", n->actor_decl.name ? n->actor_decl.name : "Actor");
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    }
    case NODE_SEND_EXPR:
        emit_expr(s, n->send_expr.target, depth);
        sb_add(s, ".handle(");
        emit_expr(s, n->send_expr.message, depth);
        sb_addc(s, ')');
        break;
    case NODE_RESUME:
        /* In generator-based effect simulation, resume sends a value */
        sb_add(s, "__xs_resume(");
        if (n->resume_.value) emit_expr(s, n->resume_.value, depth);
        else sb_add(s, "undefined");
        sb_addc(s, ')');
        break;
    case NODE_PERFORM: {
        /* perform Effect.op(args) -> yield {effect, op, args} for generator simulation */
        sb_add(s, "(yield {__effect: \"");
        if (n->perform.effect_name) sb_add(s, n->perform.effect_name);
        sb_add(s, "\", __op: \"");
        if (n->perform.op_name) sb_add(s, n->perform.op_name);
        sb_add(s, "\", __args: [");
        for (int i = 0; i < n->perform.args.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->perform.args.items[i], depth);
        }
        sb_add(s, "]})");
        break;
    }
    case NODE_THROW:
        /* throw as expression -> IIFE */
        sb_add(s, "(() => { throw ");
        emit_expr(s, n->throw_.value, depth);
        sb_add(s, "; })()");
        break;
    case NODE_RETURN:
        /* return as expression -> IIFE (rare, but handle) */
        sb_add(s, "(() => { return ");
        if (n->ret.value) emit_expr(s, n->ret.value, depth);
        else sb_add(s, "undefined");
        sb_add(s, "; })()");
        break;
    /* patterns used as expressions (rare, but handle gracefully) */
    case NODE_PAT_IDENT:
        sb_add(s, n->pat_ident.name);
        break;
    case NODE_PAT_WILD:
        sb_add(s, "_");
        break;
    case NODE_PAT_LIT:
        switch (n->pat_lit.tag) {
        case 0: sb_printf(s, "%" PRId64, n->pat_lit.ival); break;
        case 1: sb_printf(s, "%g", n->pat_lit.fval); break;
        case 2: sb_printf(s, "\"%s\"", n->pat_lit.sval ? n->pat_lit.sval : ""); break;
        case 3: sb_add(s, n->pat_lit.bval ? "true" : "false"); break;
        case 4: sb_add(s, "null"); break;
        default: sb_add(s, "undefined"); break;
        }
        break;
    case NODE_PAT_TUPLE:
        sb_addc(s, '[');
        for (int i = 0; i < n->pat_tuple.elems.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->pat_tuple.elems.items[i], depth);
        }
        sb_addc(s, ']');
        break;
    case NODE_PAT_STRUCT:
        sb_addc(s, '{');
        for (int i = 0; i < n->pat_struct.fields.len; i++) {
            if (i) sb_add(s, ", ");
            sb_add(s, n->pat_struct.fields.items[i].key);
        }
        sb_addc(s, '}');
        break;
    case NODE_PAT_ENUM:
        if (n->pat_enum.path) sb_add(s, n->pat_enum.path);
        break;
    case NODE_PAT_OR:
        emit_expr(s, n->pat_or.left, depth);
        break;
    case NODE_PAT_RANGE:
        emit_expr(s, n->pat_range.start, depth);
        break;
    case NODE_PAT_SLICE:
        sb_addc(s, '[');
        for (int i = 0; i < n->pat_slice.elems.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->pat_slice.elems.items[i], depth);
        }
        if (n->pat_slice.rest) {
            if (n->pat_slice.elems.len > 0) sb_add(s, ", ");
            sb_add(s, "...");
            sb_add(s, n->pat_slice.rest);
        }
        sb_addc(s, ']');
        break;
    case NODE_PAT_GUARD:
        emit_expr(s, n->pat_guard.pattern, depth);
        break;
    case NODE_PAT_EXPR:
        emit_expr(s, n->pat_expr.expr, depth);
        break;
    case NODE_PAT_CAPTURE:
        if (n->pat_capture.name) sb_add(s, n->pat_capture.name);
        break;
    case NODE_PAT_STRING_CONCAT:
    case NODE_PAT_REGEX:
        sb_add(s, "undefined");
        break;
    case NODE_PROGRAM:
    case NODE_LOAD:
    case NODE_PLUGIN_DECL:
        sb_add(s, "undefined");
        break;
    case NODE_DEFER:
        /* defer as expression is unusual, emit as undefined */
        sb_add(s, "undefined");
        break;
    case NODE_TRY:
        /* try as expression -> IIFE */
        sb_add(s, "(() => { try {\n");
        if (n->try_.body) emit_block_body(s, n->try_.body, depth + 1);
        sb_indent(s, depth);
        sb_addc(s, '}');
        if (n->try_.catch_arms.len > 0) {
            sb_add(s, " catch (__e) {\n");
            for (int i = 0; i < n->try_.catch_arms.len; i++) {
                MatchArm *arm = &n->try_.catch_arms.items[i];
                emit_pattern_bindings(s, arm->pattern, "__e", depth + 1);
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                    emit_block_body(s, arm->body, depth + 1);
                    if (arm->body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, arm->body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                } else if (arm->body) {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, arm->body, depth + 1);
                    sb_add(s, ";\n");
                }
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        if (n->try_.finally_block) {
            sb_add(s, " finally {\n");
            emit_block_body(s, n->try_.finally_block, depth + 1);
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        sb_add(s, " })()");
        break;
    case NODE_NURSERY:
        /* nursery as expression -> Promise.all simulation */
        sb_add(s, "(async () => {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __nursery_tasks = [];\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const spawn = (fn) => __nursery_tasks.push(fn());\n");
        if (n->nursery_.body) emit_block_body(s, n->nursery_.body, depth + 1);
        sb_indent(s, depth + 1);
        sb_add(s, "await Promise.all(__nursery_tasks);\n");
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    case NODE_HANDLE: {
        /* handle expr { arms } -> generator-based effect handler IIFE.
           Wrap the handled expression in a generator function so that any
           `perform` nodes inside it (which emit `(yield {...})`) have an
           enclosing generator to yield into. The wrapper itself is a
           generator (function*) so direct performs in the body work; for
           helper functions that themselves perform, the call site emits
           a generator-call and we delegate to it via `yield*` so any
           markers it yields propagate up to the handler loop. The runtime
           tag check below treats a non-generator return value as a plain
           value, which keeps non-perform expressions working too. */
        sb_add(s, "(() => {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __body = (function*() { const __r = ");
        emit_expr(s, n->handle.expr, depth + 1);
        sb_add(s, "; if (__r && typeof __r.next === \"function\") return yield* __r; return __r; });\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __gen = __body();\n");
        sb_indent(s, depth + 1);
        sb_add(s, "let __result = __gen.next();\n");
        sb_indent(s, depth + 1);
        sb_add(s, "while (!__result.done) {\n");
        sb_indent(s, depth + 2);
        sb_add(s, "const __eff = __result.value;\n");
        for (int i = 0; i < n->handle.arms.len; i++) {
            EffectArm *earm = &n->handle.arms.items[i];
            sb_indent(s, depth + 2);
            if (i == 0) sb_add(s, "if (");
            else sb_add(s, "else if (");
            sb_printf(s, "__eff.__effect === \"%s\" && __eff.__op === \"%s\"",
                      earm->effect_name ? earm->effect_name : "",
                      earm->op_name ? earm->op_name : "");
            sb_add(s, ") {\n");
            /* bind handler parameters from __eff.__args */
            for (int p = 0; p < earm->params.len; p++) {
                sb_indent(s, depth + 3);
                sb_add(s, "const ");
                if (earm->params.items[p].name) sb_add(s, earm->params.items[p].name);
                else sb_add(s, "_");
                sb_printf(s, " = __eff.__args[%d];\n", p);
            }
            sb_indent(s, depth + 3);
            sb_add(s, "const __xs_resume = (v) => { __result = __gen.next(v); };\n");
            if (earm->body && VAL_TAG(earm->body) == NODE_BLOCK) {
                emit_block_body(s, earm->body, depth + 3);
                /* the trailing expr of a block (e.g. `resume(null)` as the
                 * last position) is parsed as block.expr, not a statement.
                 * emit it so the handler arm's last action runs. */
                if (earm->body->block.expr) {
                    sb_indent(s, depth + 3);
                    emit_expr(s, earm->body->block.expr, depth + 3);
                    sb_add(s, ";\n");
                }
            } else if (earm->body) {
                sb_indent(s, depth + 3);
                emit_expr(s, earm->body, depth + 3);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth + 1);
        sb_add(s, "return __result.value;\n");
        sb_indent(s, depth);
        sb_add(s, "})()");
        break;
    }
    /* declaration nodes used in expression context emit their names,
       except for anonymous fn() {...} which must become a function
       expression -- emitting the name "<anonymous>" produced invalid
       JS, and falling back to "undefined" silently dropped the body. */
    case NODE_FN_DECL: {
        const char *fn_name = n->fn_decl.name;
        int named = fn_name && fn_name[0] && fn_name[0] != '<';
        if (named) { sb_add(s, fn_name); break; }
        /* Anonymous: emit a function expression. */
        int fn_is_gen = n->fn_decl.is_generator ||
                        node_has_perform(n->fn_decl.body);
        sb_add(s, fn_is_gen ? "(function*" : "(function");
        emit_params(s, &n->fn_decl.params);
        sb_add(s, " {\n");
        if (n->fn_decl.body) emit_stmt(s, n->fn_decl.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "})");
        break;
    }
    case NODE_STRUCT_DECL:
        if (n->struct_decl.name) sb_add(s, n->struct_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_ENUM_DECL:
        if (n->enum_decl.name) sb_add(s, n->enum_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_CLASS_DECL:
        if (n->class_decl.name) sb_add(s, n->class_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_TRAIT_DECL:
        if (n->trait_decl.name) sb_add(s, n->trait_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_IMPL_DECL:
        if (n->impl_decl.type_name) sb_add(s, n->impl_decl.type_name);
        else sb_add(s, "undefined");
        break;
    case NODE_TYPE_ALIAS:
        sb_add(s, "undefined");
        break;
    case NODE_IMPORT:
    case NODE_USE:
        sb_add(s, "undefined");
        break;
    case NODE_MODULE_DECL:
        if (n->module_decl.name) sb_add(s, n->module_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_EFFECT_DECL:
        if (n->effect_decl.name) sb_add(s, n->effect_decl.name);
        else sb_add(s, "undefined");
        break;
    case NODE_LET:
    case NODE_VAR:
        if (n->let.name) sb_add(s, n->let.name);
        else sb_add(s, "undefined");
        break;
    case NODE_CONST:
        if (n->const_.name) sb_add(s, n->const_.name);
        else sb_add(s, "undefined");
        break;
    case NODE_EXPR_STMT:
        emit_expr(s, n->expr_stmt.expr, depth);
        break;
    case NODE_WHILE:
        /* while-as-expression -> IIFE */
        sb_add(s, "(function() { ");
        if (n->while_loop.label) sb_printf(s, "%s: ", n->while_loop.label);
        sb_add(s, "while (");
        emit_expr(s, n->while_loop.cond, depth);
        sb_add(s, ") {\n");
        if (n->while_loop.body) {
            emit_block_body(s, n->while_loop.body, depth + 1);
            if (VAL_TAG(n->while_loop.body) == NODE_BLOCK && n->while_loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->while_loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "} })()");
        break;
    case NODE_FOR:
        /* for-as-expression -> IIFE */
        sb_add(s, "(function() { ");
        if (n->for_loop.label) sb_printf(s, "%s: ", n->for_loop.label);
        sb_add(s, "for (const ");
        if (n->for_loop.pattern) emit_expr(s, n->for_loop.pattern, depth);
        else sb_add(s, "_");
        sb_add(s, " of ");
        emit_expr(s, n->for_loop.iter, depth);
        sb_add(s, ") {\n");
        if (n->for_loop.body) {
            emit_block_body(s, n->for_loop.body, depth + 1);
            if (VAL_TAG(n->for_loop.body) == NODE_BLOCK && n->for_loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->for_loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "} })()");
        break;
    case NODE_LOOP:
        /* loop-as-expression -> IIFE */
        sb_add(s, "(function() { ");
        if (n->loop.label) sb_printf(s, "%s: ", n->loop.label);
        sb_add(s, "while (true) {\n");
        if (n->loop.body) {
            emit_block_body(s, n->loop.body, depth + 1);
            if (VAL_TAG(n->loop.body) == NODE_BLOCK && n->loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "} })()");
        break;
    case NODE_BREAK:
        /* break-as-expression -> return from IIFE */
        sb_add(s, "(function() { ");
        if (n->brk.value) {
            sb_add(s, "return ");
            emit_expr(s, n->brk.value, depth);
        } else {
            sb_add(s, "return undefined");
        }
        sb_add(s, "; })()");
        break;
    case NODE_CONTINUE:
        sb_add(s, "undefined");
        break;
    }
}

/* pattern condition emitter for match */
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth) {
    if (!pat) { sb_add(s, "true"); return; }
    switch (VAL_TAG(pat)) {
    case NODE_PAT_WILD:
        sb_add(s, "true");
        break;
    case NODE_PAT_IDENT:
        /* binds: always matches */
        sb_add(s, "true");
        break;
    case NODE_PAT_LIT:
        switch (pat->pat_lit.tag) {
        case 0: sb_printf(s, "(%s === %" PRId64 ")", subject, pat->pat_lit.ival); break;
        case 1: sb_printf(s, "(%s === %g)", subject, pat->pat_lit.fval); break;
        case 2: sb_printf(s, "(%s === \"%s\")", subject, pat->pat_lit.sval ? pat->pat_lit.sval : ""); break;
        case 3: sb_printf(s, "(%s === %s)", subject, pat->pat_lit.bval ? "true" : "false"); break;
        case 4: sb_printf(s, "(%s === null)", subject); break;
        default: sb_add(s, "true"); break;
        }
        break;
    case NODE_PAT_OR:
        sb_addc(s, '(');
        emit_pattern_cond(s, pat->pat_or.left, subject, depth);
        sb_add(s, " || ");
        emit_pattern_cond(s, pat->pat_or.right, subject, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_RANGE:
        sb_printf(s, "(%s >= ", subject);
        emit_expr(s, pat->pat_range.start, depth);
        if (pat->pat_range.inclusive) sb_printf(s, " && %s <= ", subject);
        else sb_printf(s, " && %s < ", subject);
        emit_expr(s, pat->pat_range.end, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_EXPR:
        sb_printf(s, "(%s === ", subject);
        emit_expr(s, pat->pat_expr.expr, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_GUARD:
        emit_pattern_cond(s, pat->pat_guard.pattern, subject, depth);
        sb_add(s, " && (");
        emit_expr(s, pat->pat_guard.guard, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_CAPTURE:
        emit_pattern_cond(s, pat->pat_capture.pattern, subject, depth);
        break;
    case NODE_PAT_TUPLE: {
        sb_printf(s, "(Array.isArray(%s) && %s.length === %d",
                  subject, subject, pat->pat_tuple.elems.len);
        for (int i = 0; i < pat->pat_tuple.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s[%d]", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_tuple.elems.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_STRUCT: {
        sb_printf(s, "(%s != null", subject);
        if (pat->pat_struct.path) {
            sb_printf(s, " && %s.__type__ === \"%s\"", subject, pat->pat_struct.path);
        }
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s.%s", subject, pat->pat_struct.fields.items[i].key);
            if (pat->pat_struct.fields.items[i].val) {
                sb_add(s, " && ");
                emit_pattern_cond(s, pat->pat_struct.fields.items[i].val, sub, depth);
            }
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_ENUM: {
        if (pat->pat_enum.path) {
            sb_printf(s, "(%s != null && %s.tag === \"%s\"", subject, subject, pat->pat_enum.path);
        } else {
            sb_printf(s, "(%s != null", subject);
        }
        for (int i = 0; i < pat->pat_enum.args.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s.data[%d]", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_enum.args.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_SLICE: {
        if (pat->pat_slice.rest) {
            sb_printf(s, "(Array.isArray(%s) && %s.length >= %d",
                      subject, subject, pat->pat_slice.elems.len);
        } else {
            sb_printf(s, "(Array.isArray(%s) && %s.length === %d",
                      subject, subject, pat->pat_slice.elems.len);
        }
        for (int i = 0; i < pat->pat_slice.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s[%d]", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_slice.elems.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_MAP: {
        sb_printf(s, "(%s instanceof Map", subject);
        for (int i = 0; i < pat->pat_map.nfields; i++) {
            const char *k = pat->pat_map.keys[i] ? pat->pat_map.keys[i] : "";
            sb_printf(s, " && %s.has(\"%s\")", subject, k);
            if (pat->pat_map.sub[i]) {
                char sub[256];
                snprintf(sub, sizeof sub, "%s.get(\"%s\")", subject, k);
                sb_add(s, " && ");
                emit_pattern_cond(s, pat->pat_map.sub[i], sub, depth);
            }
        }
        sb_addc(s, ')');
        break;
    }
    default:
        sb_add(s, "true");
        break;
    }
}

/* pattern bindings */
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth) {
    if (!pat) return;
    switch (VAL_TAG(pat)) {
    case NODE_PAT_IDENT:
        sb_indent(s, depth);
        sb_printf(s, "const %s = %s;\n", pat->pat_ident.name, subject);
        break;
    case NODE_PAT_CAPTURE:
        if (pat->pat_capture.name) {
            sb_indent(s, depth);
            sb_printf(s, "const %s = %s;\n", pat->pat_capture.name, subject);
        }
        emit_pattern_bindings(s, pat->pat_capture.pattern, subject, depth);
        break;
    case NODE_PAT_TUPLE:
        for (int i = 0; i < pat->pat_tuple.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s[%d]", subject, i);
            emit_pattern_bindings(s, pat->pat_tuple.elems.items[i], sub, depth);
        }
        break;
    case NODE_PAT_STRUCT:
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s.%s", subject, pat->pat_struct.fields.items[i].key);
            if (pat->pat_struct.fields.items[i].val) {
                emit_pattern_bindings(s, pat->pat_struct.fields.items[i].val, sub, depth);
            }
        }
        break;
    case NODE_PAT_ENUM:
        for (int i = 0; i < pat->pat_enum.args.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s.data[%d]", subject, i);
            emit_pattern_bindings(s, pat->pat_enum.args.items[i], sub, depth);
        }
        break;
    case NODE_PAT_SLICE:
        for (int i = 0; i < pat->pat_slice.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s[%d]", subject, i);
            emit_pattern_bindings(s, pat->pat_slice.elems.items[i], sub, depth);
        }
        if (pat->pat_slice.rest) {
            sb_indent(s, depth);
            sb_printf(s, "const %s = %s.slice(%d);\n",
                      pat->pat_slice.rest, subject, pat->pat_slice.elems.len);
        }
        break;
    case NODE_PAT_OR:
        emit_pattern_bindings(s, pat->pat_or.left, subject, depth);
        break;
    case NODE_PAT_GUARD:
        emit_pattern_bindings(s, pat->pat_guard.pattern, subject, depth);
        break;
    case NODE_PAT_MAP:
        for (int i = 0; i < pat->pat_map.nfields; i++) {
            if (!pat->pat_map.sub[i]) continue;
            char sub[256];
            snprintf(sub, sizeof sub, "%s.get(\"%s\")",
                     subject, pat->pat_map.keys[i] ? pat->pat_map.keys[i] : "");
            emit_pattern_bindings(s, pat->pat_map.sub[i], sub, depth);
        }
        break;
    default:
        break;
    }
}

/* emit block body (statements inside { })
   NOTE: does NOT emit block.expr - callers must handle implicit return/tail expr */
static void emit_block_body(SB *s, Node *block, int depth) {
    if (!block) return;
    if (VAL_TAG(block) != NODE_BLOCK) {
        emit_stmt(s, block, depth);
        return;
    }
    for (int i = 0; i < block->block.stmts.len; i++) {
        emit_stmt(s, block->block.stmts.items[i], depth);
    }
}

/* collect defer bodies from a block */
static void emit_deferred(SB *s, Node *block, int depth) {
    if (!block || VAL_TAG(block) != NODE_BLOCK) return;
    /* Emit deferred bodies in reverse order (LIFO) */
    for (int i = block->block.stmts.len - 1; i >= 0; i--) {
        Node *st = block->block.stmts.items[i];
        if (st && VAL_TAG(st) == NODE_DEFER && st->defer_.body) {
            sb_indent(s, depth);
            emit_expr(s, st->defer_.body, depth);
            sb_add(s, ";\n");
        }
    }
}

/* control-flow nodes that have a stmt-form emitter; emitting them
   through emit_expr wraps them in an IIFE, which silently breaks
   `yield` inside generators. At tail position we can just emit them
   as statements and skip the implicit return. */
static int is_stmt_only_expr(Node *n) {
    if (!n) return 0;
    switch (VAL_TAG(n)) {
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_LOOP:
    case NODE_BREAK:
    case NODE_CONTINUE:
        return 1;
    default:
        return 0;
    }
}

static int block_has_defers(Node *block) {
    if (!block || VAL_TAG(block) != NODE_BLOCK) return 0;
    for (int i = 0; i < block->block.stmts.len; i++) {
        if (block->block.stmts.items[i] && VAL_TAG(block->block.stmts.items[i]) == NODE_DEFER)
            return 1;
    }
    return 0;
}

/* does this subtree contain a NODE_PERFORM (outside of a nested fn)?
   Used to decide whether to emit a function as a JS generator so that
   effect handlers can intercept performs via yield. We do NOT descend
   into nested NODE_FN_DECL / NODE_LAMBDA bodies because those get their
   own generator decision. */
static int node_has_perform(Node *n) {
    if (!n) return 0;
    if (VAL_TAG(n) == NODE_PERFORM) return 1;
    switch (VAL_TAG(n)) {
    case NODE_FN_DECL: case NODE_LAMBDA: return 0;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            if (node_has_perform(n->block.stmts.items[i])) return 1;
        return 0;
    case NODE_EXPR_STMT:  return node_has_perform(n->expr_stmt.expr);
    case NODE_RETURN:     return node_has_perform(n->ret.value);
    case NODE_LET: case NODE_VAR: return node_has_perform(n->let.value);
    case NODE_CONST:      return node_has_perform(n->const_.value);
    case NODE_ASSIGN:     return node_has_perform(n->assign.value) ||
                                 node_has_perform(n->assign.target);
    case NODE_IF:
        if (node_has_perform(n->if_expr.cond)) return 1;
        if (node_has_perform(n->if_expr.then)) return 1;
        for (int j = 0; j < n->if_expr.elif_conds.len; j++)
            if (node_has_perform(n->if_expr.elif_conds.items[j])) return 1;
        for (int j = 0; j < n->if_expr.elif_thens.len; j++)
            if (node_has_perform(n->if_expr.elif_thens.items[j])) return 1;
        if (node_has_perform(n->if_expr.else_branch)) return 1;
        return 0;
    case NODE_WHILE:      return node_has_perform(n->while_loop.cond) ||
                                 node_has_perform(n->while_loop.body);
    case NODE_FOR:        return node_has_perform(n->for_loop.iter) ||
                                 node_has_perform(n->for_loop.body);
    case NODE_BINOP:      return node_has_perform(n->binop.left) ||
                                 node_has_perform(n->binop.right);
    case NODE_UNARY:      return node_has_perform(n->unary.expr);
    case NODE_CALL:
        if (node_has_perform(n->call.callee)) return 1;
        for (int j = 0; j < n->call.args.len; j++)
            if (node_has_perform(n->call.args.items[j])) return 1;
        return 0;
    case NODE_METHOD_CALL:
        if (node_has_perform(n->method_call.obj)) return 1;
        for (int j = 0; j < n->method_call.args.len; j++)
            if (node_has_perform(n->method_call.args.items[j])) return 1;
        return 0;
    case NODE_TRY:
        if (node_has_perform(n->try_.body)) return 1;
        if (node_has_perform(n->try_.finally_block)) return 1;
        return 0;
    case NODE_HANDLE:
        return node_has_perform(n->handle.expr);
    default:
        return 0;
    }
}

/* statement emitter */
static void emit_stmt(SB *s, Node *n, int depth) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_LET:
        sb_indent(s, depth);
        sb_add(s, "const ");
        if (n->let.name) sb_add(s, n->let.name);
        else if (n->let.pattern) emit_expr(s, n->let.pattern, depth);
        else sb_add(s, "_");
        if (n->let.value) {
            sb_add(s, " = ");
            emit_expr(s, n->let.value, depth);
        }
        sb_add(s, ";\n");
        break;
    case NODE_VAR:
        sb_indent(s, depth);
        sb_add(s, "let ");
        if (n->let.name) sb_add(s, n->let.name);
        else if (n->let.pattern) emit_expr(s, n->let.pattern, depth);
        else sb_add(s, "_");
        if (n->let.value) {
            sb_add(s, " = ");
            emit_expr(s, n->let.value, depth);
        }
        sb_add(s, ";\n");
        break;
    case NODE_CONST:
        sb_indent(s, depth);
        sb_printf(s, "const %s", n->const_.name);
        if (n->const_.value) {
            sb_add(s, " = ");
            emit_expr(s, n->const_.value, depth);
        }
        sb_add(s, ";\n");
        break;
    case NODE_FN_DECL: {
        sb_indent(s, depth);
        if (n->fn_decl.is_pub) sb_add(s, "export ");
        if (n->fn_decl.is_async) sb_add(s, "async ");
        /* Auto-promote to generator if the body contains a perform.
           That lets a handle-block IIFE iterate the generator and
           dispatch effects at any call depth, not just inline ones. */
        int fn_is_gen = n->fn_decl.is_generator ||
                        node_has_perform(n->fn_decl.body);
        /* An anonymous fn() reaching stmt context is almost always the
           last expression of a block body (the interp treats
           trailing expressions as implicit returns). JS has no such
           rule and rejects both "<anonymous>" and nameless function
           declarations, so emit `return (function(){...})` and let
           the surrounding block's control flow do the right thing. */
        const char *fn_name = n->fn_decl.name;
        int named = fn_name && fn_name[0] && fn_name[0] != '<';
        if (!named) sb_add(s, "return (");
        if (fn_is_gen) {
            if (named) sb_printf(s, "function* %s", fn_name);
            else       sb_add(s, "function*");
        } else {
            if (named) sb_printf(s, "function %s", fn_name);
            else       sb_add(s, "function");
        }
        emit_params(s, &n->fn_decl.params);
        sb_add(s, " {\n");
        if (n->fn_decl.body) {
            int has_defer = block_has_defers(n->fn_decl.body);
            if (has_defer) {
                sb_indent(s, depth + 1);
                sb_add(s, "try {\n");
                if (VAL_TAG(n->fn_decl.body) == NODE_BLOCK) {
                    /* emit non-defer statements */
                    for (int i = 0; i < n->fn_decl.body->block.stmts.len; i++) {
                        Node *st = n->fn_decl.body->block.stmts.items[i];
                        if (st && VAL_TAG(st) != NODE_DEFER)
                            emit_stmt(s, st, depth + 2);
                    }
                    if (n->fn_decl.body->block.expr) {
                        if (is_stmt_only_expr(n->fn_decl.body->block.expr)) {
                            emit_stmt(s, n->fn_decl.body->block.expr, depth + 2);
                        } else {
                            sb_indent(s, depth + 2);
                            sb_add(s, "return ");
                            emit_expr(s, n->fn_decl.body->block.expr, depth + 2);
                            sb_add(s, ";\n");
                        }
                    }
                }
                sb_indent(s, depth + 1);
                sb_add(s, "} finally {\n");
                emit_deferred(s, n->fn_decl.body, depth + 2);
                sb_indent(s, depth + 1);
                sb_add(s, "}\n");
            } else {
                if (VAL_TAG(n->fn_decl.body) == NODE_BLOCK) {
                    emit_block_body(s, n->fn_decl.body, depth + 1);
                    if (n->fn_decl.body->block.expr) {
                        if (is_stmt_only_expr(n->fn_decl.body->block.expr)) {
                            emit_stmt(s, n->fn_decl.body->block.expr, depth + 1);
                        } else {
                            sb_indent(s, depth + 1);
                            sb_add(s, "return ");
                            emit_expr(s, n->fn_decl.body->block.expr, depth + 1);
                            sb_add(s, ";\n");
                        }
                    }
                } else {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return ");
                    emit_expr(s, n->fn_decl.body, depth + 1);
                    sb_add(s, ";\n");
                }
            }
        }
        sb_indent(s, depth);
        sb_add(s, named ? "}\n\n" : "});\n");
        break;
    }
    case NODE_RETURN:
        sb_indent(s, depth);
        sb_add(s, "return");
        if (n->ret.value) {
            sb_addc(s, ' ');
            emit_expr(s, n->ret.value, depth);
        }
        sb_add(s, ";\n");
        break;
    case NODE_BREAK:
        sb_indent(s, depth);
        sb_add(s, "break");
        if (n->brk.label) sb_printf(s, " %s", n->brk.label);
        sb_add(s, ";\n");
        break;
    case NODE_CONTINUE:
        sb_indent(s, depth);
        sb_add(s, "continue");
        if (n->cont.label) sb_printf(s, " %s", n->cont.label);
        sb_add(s, ";\n");
        break;
    case NODE_IF: {
        sb_indent(s, depth);
        sb_add(s, "if (");
        emit_expr(s, n->if_expr.cond, depth);
        sb_add(s, ") {\n");
        if (n->if_expr.then) {
            emit_block_body(s, n->if_expr.then, depth + 1);
            if (VAL_TAG(n->if_expr.then) == NODE_BLOCK && n->if_expr.then->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->if_expr.then->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_addc(s, '}');
        for (int i = 0; i < n->if_expr.elif_conds.len; i++) {
            sb_add(s, " else if (");
            emit_expr(s, n->if_expr.elif_conds.items[i], depth);
            sb_add(s, ") {\n");
            Node *et = n->if_expr.elif_thens.items[i];
            emit_block_body(s, et, depth + 1);
            if (et && VAL_TAG(et) == NODE_BLOCK && et->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, et->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        if (n->if_expr.else_branch) {
            sb_add(s, " else {\n");
            Node *eb = n->if_expr.else_branch;
            emit_block_body(s, eb, depth + 1);
            if (VAL_TAG(eb) == NODE_BLOCK && eb->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, eb->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        sb_addc(s, '\n');
        break;
    }
    case NODE_WHILE:
        sb_indent(s, depth);
        if (n->while_loop.label) sb_printf(s, "%s: ", n->while_loop.label);
        sb_add(s, "while (");
        emit_expr(s, n->while_loop.cond, depth);
        sb_add(s, ") {\n");
        if (n->while_loop.body) {
            emit_block_body(s, n->while_loop.body, depth + 1);
            if (VAL_TAG(n->while_loop.body) == NODE_BLOCK && n->while_loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->while_loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_FOR:
        sb_indent(s, depth);
        if (n->for_loop.label) sb_printf(s, "%s: ", n->for_loop.label);
        sb_add(s, "for (const ");
        if (n->for_loop.pattern) emit_expr(s, n->for_loop.pattern, depth);
        else sb_add(s, "_");
        sb_add(s, " of ");
        emit_expr(s, n->for_loop.iter, depth);
        sb_add(s, ") {\n");
        if (n->for_loop.body) {
            emit_block_body(s, n->for_loop.body, depth + 1);
            if (VAL_TAG(n->for_loop.body) == NODE_BLOCK && n->for_loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->for_loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_LOOP:
        sb_indent(s, depth);
        if (n->loop.label) sb_printf(s, "%s: ", n->loop.label);
        sb_add(s, "while (true) {\n");
        if (n->loop.body) {
            emit_block_body(s, n->loop.body, depth + 1);
            if (VAL_TAG(n->loop.body) == NODE_BLOCK && n->loop.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->loop.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_MATCH: {
        /* Each arm gets its own nested block so pattern bindings are in scope
           when the guard is evaluated (JS `const` is in TDZ). Uses a labeled
           block + break to skip the remaining arms once one matches. */
        sb_indent(s, depth);
        sb_add(s, "__match: {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __subject = ");
        emit_expr(s, n->match.subject, depth + 1);
        sb_add(s, ";\n");
        for (int i = 0; i < n->match.arms.len; i++) {
            MatchArm *arm = &n->match.arms.items[i];
            sb_indent(s, depth + 1);
            sb_add(s, "{\n");
            sb_indent(s, depth + 2);
            sb_add(s, "if (");
            emit_pattern_cond(s, arm->pattern, "__subject", depth + 2);
            sb_add(s, ") {\n");
            emit_pattern_bindings(s, arm->pattern, "__subject", depth + 3);
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "if (");
                emit_expr(s, arm->guard, depth + 3);
                sb_add(s, ") {\n");
            }
            int body_depth = arm->guard ? depth + 4 : depth + 3;
            if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                emit_block_body(s, arm->body, body_depth);
                if (arm->body->block.expr) {
                    sb_indent(s, body_depth);
                    emit_expr(s, arm->body->block.expr, body_depth);
                    sb_add(s, ";\n");
                }
            } else if (arm->body) {
                sb_indent(s, body_depth);
                emit_expr(s, arm->body, body_depth);
                sb_add(s, ";\n");
            }
            sb_indent(s, body_depth);
            sb_add(s, "break __match;\n");
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "}\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_TRY: {
        sb_indent(s, depth);
        sb_add(s, "try {\n");
        if (n->try_.body) {
            emit_block_body(s, n->try_.body, depth + 1);
            if (VAL_TAG(n->try_.body) == NODE_BLOCK && n->try_.body->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->try_.body->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth);
        sb_addc(s, '}');
        if (n->try_.catch_arms.len > 0) {
            sb_add(s, " catch (__e) {\n");
            for (int i = 0; i < n->try_.catch_arms.len; i++) {
                MatchArm *arm = &n->try_.catch_arms.items[i];
                /* emit pattern bindings for catch */
                emit_pattern_bindings(s, arm->pattern, "__e", depth + 1);
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                    emit_block_body(s, arm->body, depth + 1);
                    if (arm->body->block.expr) {
                        sb_indent(s, depth + 1);
                        emit_expr(s, arm->body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                } else if (arm->body) {
                    sb_indent(s, depth + 1);
                    emit_expr(s, arm->body, depth + 1);
                    sb_add(s, ";\n");
                }
            }
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        if (n->try_.finally_block) {
            sb_add(s, " finally {\n");
            emit_block_body(s, n->try_.finally_block, depth + 1);
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        sb_addc(s, '\n');
        break;
    }
    case NODE_THROW:
        sb_indent(s, depth);
        sb_add(s, "throw ");
        emit_expr(s, n->throw_.value, depth);
        sb_add(s, ";\n");
        break;
    case NODE_DEFER: {
        /* defer in statement context: wrap remaining code in try/finally.
           When used inside a function body the FN_DECL handler takes care of it.
           Standalone defers outside function bodies are emitted as comments
           with an immediate try/finally for the single deferred expression. */
        sb_indent(s, depth);
        sb_add(s, "/* defer */ (() => {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "try {} finally {\n");
        sb_indent(s, depth + 2);
        if (n->defer_.body) emit_expr(s, n->defer_.body, depth + 2);
        else sb_add(s, "undefined");
        sb_add(s, ";\n");
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth);
        sb_add(s, "})();\n");
        break;
    }
    case NODE_YIELD:
        sb_indent(s, depth);
        sb_add(s, "yield ");
        if (n->yield_.value) emit_expr(s, n->yield_.value, depth);
        sb_add(s, ";\n");
        break;
    case NODE_STRUCT_DECL: {
        sb_indent(s, depth);
        sb_printf(s, "class %s {\n", n->struct_decl.name);
        sb_indent(s, depth + 1);
        sb_add(s, "constructor(");
        sb_add(s, "{");
        for (int i = 0; i < n->struct_decl.fields.len; i++) {
            if (i) sb_add(s, ", ");
            sb_add(s, n->struct_decl.fields.items[i].key);
        }
        sb_add(s, "}) {\n");
        for (int i = 0; i < n->struct_decl.fields.len; i++) {
            sb_indent(s, depth + 2);
            sb_printf(s, "this.%s = %s;\n",
                      n->struct_decl.fields.items[i].key,
                      n->struct_decl.fields.items[i].key);
        }
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth);
        sb_add(s, "}\n\n");
        break;
    }
    case NODE_ENUM_DECL: {
        sb_indent(s, depth);
        sb_printf(s, "const %s = Object.freeze({\n", n->enum_decl.name);
        for (int i = 0; i < n->enum_decl.variants.len; i++) {
            EnumVariant *v = &n->enum_decl.variants.items[i];
            sb_indent(s, depth + 1);
            if (v->fields.len == 0) {
                sb_printf(s, "%s: Object.freeze({tag: \"%s\", data: []})", v->name, v->name);
            } else {
                sb_printf(s, "%s: (", v->name);
                for (int j = 0; j < v->fields.len; j++) {
                    if (j) sb_add(s, ", ");
                    sb_printf(s, "v%d", j);
                }
                sb_add(s, ") => Object.freeze({tag: \"");
                sb_add(s, v->name);
                sb_add(s, "\", data: [");
                for (int j = 0; j < v->fields.len; j++) {
                    if (j) sb_add(s, ", ");
                    sb_printf(s, "v%d", j);
                }
                sb_add(s, "]})");
            }
            if (i < n->enum_decl.variants.len - 1) sb_addc(s, ',');
            sb_addc(s, '\n');
        }
        sb_indent(s, depth);
        sb_add(s, "});\n\n");
        break;
    }
    case NODE_IMPL_DECL: {
        /* emit methods as prototype assignments */
        sb_indent(s, depth);
        if (n->impl_decl.trait_name)
            sb_printf(s, "// impl %s for %s\n", n->impl_decl.trait_name, n->impl_decl.type_name);
        else
            sb_printf(s, "// impl %s\n", n->impl_decl.type_name);
        for (int i = 0; i < n->impl_decl.members.len; i++) {
            Node *m = n->impl_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL) {
                sb_indent(s, depth);
                if (m->fn_decl.is_async) sb_add(s, "/* async */ ");
                sb_printf(s, "%s.prototype.%s = function",
                          n->impl_decl.type_name, m->fn_decl.name);
                emit_params_ex(s, &m->fn_decl.params, 1);
                sb_add(s, " {\n");
                int save_cm = in_class_method;
                in_class_method = 1;
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    emit_block_body(s, m->fn_decl.body, depth + 1);
                    if (m->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, m->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                }
                in_class_method = save_cm;
                sb_indent(s, depth);
                sb_add(s, "};\n");
            } else if (m) {
                emit_stmt(s, m, depth);
            }
        }
        sb_addc(s, '\n');
        break;
    }
    case NODE_TRAIT_DECL: {
        sb_indent(s, depth);
        sb_printf(s, "// trait %s", n->trait_decl.name);
        if (n->trait_decl.super_trait) sb_printf(s, " extends %s", n->trait_decl.super_trait);
        sb_addc(s, '\n');
        /* Emit trait as a mixin symbol for documentation */
        sb_indent(s, depth);
        sb_printf(s, "const __%s_trait = Symbol(\"%s\");\n", n->trait_decl.name, n->trait_decl.name);
        if (n->trait_decl.n_methods > 0) {
            sb_indent(s, depth);
            sb_printf(s, "// required methods: ");
            for (int i = 0; i < n->trait_decl.n_methods; i++) {
                if (i) sb_add(s, ", ");
                sb_add(s, n->trait_decl.method_names[i]);
            }
            sb_addc(s, '\n');
        }
        break;
    }
    case NODE_TYPE_ALIAS:
        sb_indent(s, depth);
        sb_printf(s, "// type %s = %s\n", n->type_alias.name,
                  n->type_alias.target ? n->type_alias.target : "?");
        break;
    case NODE_IMPORT: {
        sb_indent(s, depth);
        /* Emit as ES module import */
        if (n->import.nitems > 0) {
            sb_add(s, "import { ");
            for (int i = 0; i < n->import.nitems; i++) {
                if (i) sb_add(s, ", ");
                sb_add(s, n->import.items[i]);
            }
            sb_add(s, " } from \"./");
            for (int i = 0; i < n->import.nparts; i++) {
                if (i) sb_addc(s, '/');
                sb_add(s, n->import.path[i]);
            }
            sb_add(s, ".js\";\n");
        } else if (n->import.alias) {
            sb_printf(s, "import * as %s from \"./", n->import.alias);
            for (int i = 0; i < n->import.nparts; i++) {
                if (i) sb_addc(s, '/');
                sb_add(s, n->import.path[i]);
            }
            sb_add(s, ".js\";\n");
        } else {
            sb_add(s, "import ");
            if (n->import.nparts > 0) sb_add(s, n->import.path[n->import.nparts - 1]);
            sb_add(s, " from \"./");
            for (int i = 0; i < n->import.nparts; i++) {
                if (i) sb_addc(s, '/');
                sb_add(s, n->import.path[i]);
            }
            sb_add(s, ".js\";\n");
        }
        break;
    }
    case NODE_MODULE_DECL:
        sb_indent(s, depth);
        sb_printf(s, "const %s = (() => {\n", n->module_decl.name);
        for (int i = 0; i < n->module_decl.body.len; i++)
            emit_stmt(s, n->module_decl.body.items[i], depth + 1);
        sb_indent(s, depth + 1);
        sb_add(s, "return {");
        /* collect exported names */
        for (int i = 0; i < n->module_decl.body.len; i++) {
            Node *m = n->module_decl.body.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.name) {
                sb_printf(s, " %s,", m->fn_decl.name);
            } else if (m && VAL_TAG(m) == NODE_STRUCT_DECL && m->struct_decl.name) {
                sb_printf(s, " %s,", m->struct_decl.name);
            } else if (m && VAL_TAG(m) == NODE_ENUM_DECL && m->enum_decl.name) {
                sb_printf(s, " %s,", m->enum_decl.name);
            } else if (m && VAL_TAG(m) == NODE_CLASS_DECL && m->class_decl.name) {
                sb_printf(s, " %s,", m->class_decl.name);
            }
        }
        sb_add(s, " };\n");
        sb_indent(s, depth);
        sb_add(s, "})();\n\n");
        break;
    case NODE_CLASS_DECL:
        sb_indent(s, depth);
        if (n->class_decl.nbases > 0 && n->class_decl.bases && n->class_decl.bases[0])
            sb_printf(s, "class %s extends %s {\n", n->class_decl.name, n->class_decl.bases[0]);
        else
            sb_printf(s, "class %s {\n", n->class_decl.name);
        for (int i = 0; i < n->class_decl.members.len; i++) {
            Node *m = n->class_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL) {
                int is_ctor = m->fn_decl.name &&
                    (strcmp(m->fn_decl.name, "new") == 0 || strcmp(m->fn_decl.name, "init") == 0);
                /* method inside class */
                sb_indent(s, depth + 1);
                if (m->fn_decl.is_async) sb_add(s, "async ");
                if (m->fn_decl.is_generator) sb_addc(s, '*');
                if (is_ctor) {
                    sb_add(s, "constructor");
                } else {
                    sb_add(s, m->fn_decl.name ? m->fn_decl.name : "_");
                }
                emit_params_ex(s, &m->fn_decl.params, 1);
                sb_add(s, " {\n");
                in_class_method = 1;
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    emit_block_body(s, m->fn_decl.body, depth + 2);
                    if (m->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 2);
                        if (!is_ctor) sb_add(s, "return ");
                        emit_expr(s, m->fn_decl.body->block.expr, depth + 2);
                        sb_add(s, ";\n");
                    }
                } else if (m->fn_decl.body) {
                    sb_indent(s, depth + 2);
                    if (!is_ctor) sb_add(s, "return ");
                    emit_expr(s, m->fn_decl.body, depth + 2);
                    sb_add(s, ";\n");
                }
                in_class_method = 0;
                sb_indent(s, depth + 1);
                sb_add(s, "}\n");
            } else if (m) {
                emit_stmt(s, m, depth + 1);
            }
        }
        sb_indent(s, depth);
        sb_add(s, "}\n\n");
        break;
    case NODE_EFFECT_DECL: {
        /* Effect declaration -> symbol constant + documentation */
        sb_indent(s, depth);
        sb_printf(s, "const %s = {\n", n->effect_decl.name);
        for (int i = 0; i < n->effect_decl.ops.len; i++) {
            Node *op = n->effect_decl.ops.items[i];
            sb_indent(s, depth + 1);
            if (op && VAL_TAG(op) == NODE_FN_DECL && op->fn_decl.name) {
                sb_printf(s, "%s: Symbol(\"%s.%s\")",
                          op->fn_decl.name, n->effect_decl.name, op->fn_decl.name);
            } else {
                sb_printf(s, "op%d: Symbol(\"%s.op%d\")", i, n->effect_decl.name, i);
            }
            if (i < n->effect_decl.ops.len - 1) sb_addc(s, ',');
            sb_addc(s, '\n');
        }
        sb_indent(s, depth);
        sb_add(s, "};\n\n");
        break;
    }
    case NODE_HANDLE: {
        /* handle as statement */
        sb_indent(s, depth);
        emit_expr(s, (Node*)n, depth);
        sb_add(s, ";\n");
        break;
    }
    case NODE_NURSERY: {
        /* nursery { body } -> async IIFE with Promise.all */
        sb_indent(s, depth);
        sb_add(s, "(async () => {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const __nursery_tasks = [];\n");
        sb_indent(s, depth + 1);
        sb_add(s, "const spawn = (fn) => __nursery_tasks.push(fn());\n");
        if (n->nursery_.body) emit_block_body(s, n->nursery_.body, depth + 1);
        sb_indent(s, depth + 1);
        sb_add(s, "await Promise.all(__nursery_tasks);\n");
        sb_indent(s, depth);
        sb_add(s, "})();\n");
        break;
    }
    case NODE_ASSIGN:
        sb_indent(s, depth);
        emit_expr(s, n->assign.target, depth);
        sb_addc(s, ' ');
        sb_add(s, n->assign.op);
        sb_addc(s, ' ');
        emit_expr(s, n->assign.value, depth);
        sb_add(s, ";\n");
        break;
    case NODE_EXPR_STMT: {
        Node *inner = n->expr_stmt.expr;
        if (inner && (VAL_TAG(inner) == NODE_IF || VAL_TAG(inner) == NODE_MATCH ||
                      VAL_TAG(inner) == NODE_FOR || VAL_TAG(inner) == NODE_WHILE ||
                      VAL_TAG(inner) == NODE_LOOP || VAL_TAG(inner) == NODE_TRY ||
                      VAL_TAG(inner) == NODE_BLOCK ||
                      VAL_TAG(inner) == NODE_RETURN)) {
            /* NODE_RETURN inside NODE_EXPR_STMT happens when the parser
               desugars `fn name(...) = expr` into a one-statement block
               wrapping a return. Emit it as a plain statement so the
               enclosing function actually returns. */
            emit_stmt(s, inner, depth);
        } else {
            sb_indent(s, depth);
            emit_expr(s, inner, depth);
            sb_add(s, ";\n");
        }
        break;
    }
    case NODE_BLOCK:
        sb_indent(s, depth);
        sb_add(s, "{\n");
        emit_block_body(s, n, depth + 1);
        if (n->block.expr) {
            sb_indent(s, depth + 1);
            emit_expr(s, n->block.expr, depth + 1);
            sb_add(s, ";\n");
        }
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_SPAWN:
    case NODE_ACTOR_DECL:
    case NODE_SEND_EXPR:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_PERFORM:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_RESUME:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_AWAIT:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_INLINE_C:
        sb_indent(s, depth);
        sb_add(s, "throw new Error('inline C blocks not supported in JS target');\n");
        break;
    case NODE_BIND:
        sb_indent(s, depth);
        sb_printf(s, "let %s = ", n->bind_decl.name ? n->bind_decl.name : "_bind");
        emit_expr(s, n->bind_decl.expr, depth);
        sb_add(s, ";\n");
        break;
    case NODE_TAG_DECL:
        /* Emit as a regular function with __block as last param */
        sb_indent(s, depth);
        sb_printf(s, "function %s", n->tag_decl.name ? n->tag_decl.name : "_tag");
        sb_addc(s, '(');
        for (int ti = 0; ti < n->tag_decl.params.len; ti++) {
            if (ti > 0) sb_add(s, ", ");
            Param *pm = &n->tag_decl.params.items[ti];
            sb_add(s, pm->name ? pm->name : "_");
        }
        if (n->tag_decl.params.len > 0) sb_add(s, ", ");
        sb_add(s, "__block");
        sb_add(s, ") {\n");
        if (n->tag_decl.body) emit_stmt(s, n->tag_decl.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_ADAPT_FN: {
        /* Select "js" branch, fallback to "native", then first */
        int sel = 0;
        for (int ai = 0; ai < n->adapt_fn.nbranches; ai++) {
            if (strcmp(n->adapt_fn.targets[ai], "js") == 0) { sel = ai; break; }
            if (strcmp(n->adapt_fn.targets[ai], "native") == 0) sel = ai;
        }
        if (n->adapt_fn.nbranches == 0) break;
        sb_indent(s, depth);
        sb_printf(s, "function %s(", n->adapt_fn.name ? n->adapt_fn.name : "_adapt");
        for (int ai = 0; ai < n->adapt_fn.params.len; ai++) {
            if (ai > 0) sb_add(s, ", ");
            Param *pm = &n->adapt_fn.params.items[ai];
            sb_add(s, pm->name ? pm->name : "_");
        }
        sb_add(s, ") {\n");
        if (n->adapt_fn.bodies[sel]) emit_stmt(s, n->adapt_fn.bodies[sel], depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    default:
        /* for any remaining node types, emit as expression statement */
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    }
}

/* top-level node emitter */
static void emit_node(SB *s, Node *n, int depth) {
    emit_stmt(s, n, depth);
}

/* public entry point */
char *transpile_js(Node *program, const char *filename) {
    SB s;
    sb_init(&s);

    /* preamble */
    sb_add(&s, "// Generated by xs transpile --target js\n");
    if (filename) sb_printf(&s, "// Source: %s\n", filename);
    sb_add(&s, "const __xs_repr = (v) => {\n");
    sb_add(&s, "    if (v === null || v === undefined) return \"null\";\n");
    sb_add(&s, "    if (typeof v === \"string\") return v;\n");
    sb_add(&s, "    if (typeof v === \"number\" || typeof v === \"bigint\" || typeof v === \"boolean\") return String(v);\n");
    sb_add(&s, "    if (Array.isArray(v)) return \"[\" + v.map(__xs_repr).join(\", \") + \"]\";\n");
    sb_add(&s, "    if (v instanceof Map) {\n");
    sb_add(&s, "        const parts = [];\n");
    sb_add(&s, "        for (const [k, val] of v) parts.push(__xs_repr(k) + \": \" + __xs_repr(val));\n");
    sb_add(&s, "        return \"{\" + parts.join(\", \") + \"}\";\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return String(v);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_print = (...args) => console.log(args.map(__xs_repr).join(\" \"));\n");
    sb_add(&s, "const __xs_write = (...args) => {\n");
    sb_add(&s, "    const s = args.map(__xs_repr).join(\" \");\n");
    sb_add(&s, "    if (typeof process !== 'undefined') process.stdout.write(s);\n");
    sb_add(&s, "    else console.log(s);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_add = (a, b) => {\n");
    sb_add(&s, "    if (typeof a === \"string\" || typeof b === \"string\") return __xs_repr(a) + __xs_repr(b);\n");
    sb_add(&s, "    return a + b;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_div = (a, b) => {\n");
    sb_add(&s, "    if (b === 0 || b === 0n) throw new Error(\"division by zero\");\n");
    sb_add(&s, "    if (typeof a === \"bigint\" && typeof b === \"bigint\") return a / b;\n");
    sb_add(&s, "    if (Number.isInteger(a) && Number.isInteger(b)) return Math.trunc(a / b);\n");
    sb_add(&s, "    return a / b;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_idx = (o, i) => {\n");
    sb_add(&s, "    if ((Array.isArray(o) || typeof o === \"string\") && typeof i === \"number\" && i < 0) i = o.length + i;\n");
    sb_add(&s, "    if (o instanceof Map) return o.get(i);\n");
    sb_add(&s, "    return o[i];\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_setidx = (o, i, v) => {\n");
    sb_add(&s, "    if (Array.isArray(o) && typeof i === \"number\" && i < 0) i = o.length + i;\n");
    sb_add(&s, "    if (o instanceof Map) { o.set(i, v); return v; }\n");
    sb_add(&s, "    o[i] = v;\n");
    sb_add(&s, "    return v;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_resume = (v) => v;\n");
    /* channel runtime */
    sb_add(&s, "function __xs_channel() {\n");
    sb_add(&s, "    const buf = [];\n");
    sb_add(&s, "    return {\n");
    sb_add(&s, "        send(v) { buf.push(v); },\n");
    sb_add(&s, "        recv() { return buf.shift(); },\n");
    sb_add(&s, "        len() { return buf.length; },\n");
    sb_add(&s, "        is_empty() { return buf.length === 0; },\n");
    sb_add(&s, "        is_full() { return false; }\n");
    sb_add(&s, "    };\n");
    sb_add(&s, "}\n");
    sb_add(&s, "const channel = __xs_channel;\n\n");

    if (!program) {
        sb_add(&s, "// (empty program)\n");
        return s.data;
    }

    /* reset class method state */
    in_class_method = 0;

    int has_main = 0;

    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            emit_node(&s, st, 0);
            /* check if this is a main() function declaration */
            if (st && VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.name &&
                strcmp(st->fn_decl.name, "main") == 0) {
                has_main = 1;
            }
        }
    } else {
        emit_node(&s, program, 0);
    }

    /* auto-call main if defined */
    if (has_main) {
        sb_add(&s, "main();\n");
    }

    /* source map reference */
    if (filename) {
        /* derive .xs.map name from source filename */
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        sb_printf(&s, "\n//# sourceMappingURL=%s.map\n", base);
    }

    return s.data;
}
