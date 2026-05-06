/* interp_nodemap.c -- AST Node <-> XS map conversion.
   Used by eval hooks and plugin.parser.override handlers: they see
   nodes as {tag, ...} maps and can build new trees the interpreter
   then re-materialises via node_from_xs_map. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "core/value.h"
#include "core/ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>



/* phase 2: node tag string conversion */

const char *node_tag_to_string(NodeTag tag) {
    switch (tag) {
    case NODE_LIT_INT:    return "int";
    case NODE_LIT_FLOAT:  return "float";
    case NODE_LIT_STRING: return "str";
    case NODE_LIT_BOOL:   return "bool";
    case NODE_LIT_NULL:   return "null";
    case NODE_IDENT:      return "ident";
    case NODE_BINOP:      return "binop";
    case NODE_UNARY:      return "unary";
    case NODE_CALL:       return "call";
    case NODE_METHOD_CALL:return "method_call";
    case NODE_IF:         return "if";
    case NODE_BLOCK:      return "block";
    case NODE_LET:        return "let";
    case NODE_VAR:        return "var";
    case NODE_FN_DECL:    return "fn_decl";
    case NODE_LAMBDA:     return "lambda";
    case NODE_RETURN:     return "return";
    case NODE_ASSIGN:     return "assign";
    case NODE_FOR:        return "for";
    case NODE_WHILE:      return "while";
    case NODE_LOOP:       return "loop";
    case NODE_BREAK:      return "break";
    case NODE_CONTINUE:   return "continue";
    case NODE_INDEX:      return "index";
    case NODE_FIELD:      return "field";
    case NODE_LIT_ARRAY:  return "array";
    case NODE_LIT_MAP:    return "map";
    case NODE_RANGE:      return "range";
    case NODE_MATCH:      return "match";
    case NODE_THROW:      return "throw";
    case NODE_TRY:        return "try";
    case NODE_DEFER:      return "defer";
    case NODE_SPAWN:      return "spawn";
    case NODE_YIELD:      return "yield";
    case NODE_SPREAD:     return "spread";
    case NODE_STRUCT_INIT:return "struct_init";
    case NODE_EXPR_STMT:  return "expr_stmt";
    case NODE_BIND:       return "bind";
    case NODE_LIT_DURATION: return "duration";
    case NODE_EVERY:      return "every";
    case NODE_AFTER:      return "after";
    case NODE_TIMEOUT:    return "timeout";
    case NODE_DEBOUNCE:   return "debounce";
    case NODE_PAUSE:      return "pause";
    case NODE_DEL:        return "del";
    case NODE_DO_EXPR:    return "do";
    case NODE_WITH:       return "with";
    default:              return "unknown";
    }
}

int node_tag_from_string(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "int") == 0)    return NODE_LIT_INT;
    if (strcmp(s, "float") == 0)  return NODE_LIT_FLOAT;
    if (strcmp(s, "str") == 0)    return NODE_LIT_STRING;
    if (strcmp(s, "bool") == 0)   return NODE_LIT_BOOL;
    if (strcmp(s, "null") == 0)   return NODE_LIT_NULL;
    if (strcmp(s, "ident") == 0)  return NODE_IDENT;
    if (strcmp(s, "binop") == 0)  return NODE_BINOP;
    if (strcmp(s, "unary") == 0)  return NODE_UNARY;
    if (strcmp(s, "call") == 0)   return NODE_CALL;
    if (strcmp(s, "fn_call") == 0) return NODE_CALL;
    if (strcmp(s, "method_call") == 0) return NODE_METHOD_CALL;
    if (strcmp(s, "if") == 0)     return NODE_IF;
    if (strcmp(s, "block") == 0)  return NODE_BLOCK;
    if (strcmp(s, "let") == 0)    return NODE_LET;
    if (strcmp(s, "var") == 0)    return NODE_VAR;
    if (strcmp(s, "fn_decl") == 0) return NODE_FN_DECL;
    if (strcmp(s, "lambda") == 0) return NODE_LAMBDA;
    if (strcmp(s, "return") == 0) return NODE_RETURN;
    if (strcmp(s, "assign") == 0) return NODE_ASSIGN;
    if (strcmp(s, "for") == 0)    return NODE_FOR;
    if (strcmp(s, "while") == 0)  return NODE_WHILE;
    if (strcmp(s, "loop") == 0)   return NODE_LOOP;
    if (strcmp(s, "break") == 0)  return NODE_BREAK;
    if (strcmp(s, "continue") == 0) return NODE_CONTINUE;
    if (strcmp(s, "index") == 0)  return NODE_INDEX;
    if (strcmp(s, "field") == 0)  return NODE_FIELD;
    if (strcmp(s, "array") == 0)  return NODE_LIT_ARRAY;
    if (strcmp(s, "map") == 0)    return NODE_LIT_MAP;
    if (strcmp(s, "range") == 0)  return NODE_RANGE;
    if (strcmp(s, "match") == 0)  return NODE_MATCH;
    if (strcmp(s, "throw") == 0)  return NODE_THROW;
    if (strcmp(s, "try") == 0)    return NODE_TRY;
    if (strcmp(s, "defer") == 0)  return NODE_DEFER;
    if (strcmp(s, "spawn") == 0)  return NODE_SPAWN;
    if (strcmp(s, "yield") == 0)  return NODE_YIELD;
    if (strcmp(s, "spread") == 0) return NODE_SPREAD;
    if (strcmp(s, "struct_init") == 0) return NODE_STRUCT_INIT;
    if (strcmp(s, "expr_stmt") == 0) return NODE_EXPR_STMT;
    if (strcmp(s, "bind") == 0) return NODE_BIND;
    if (strcmp(s, "duration") == 0) return NODE_LIT_DURATION;
    if (strcmp(s, "every") == 0) return NODE_EVERY;
    if (strcmp(s, "after") == 0) return NODE_AFTER;
    if (strcmp(s, "timeout") == 0) return NODE_TIMEOUT;
    if (strcmp(s, "debounce") == 0) return NODE_DEBOUNCE;
    if (strcmp(s, "pause") == 0) return NODE_PAUSE;
    if (strcmp(s, "del") == 0) return NODE_DEL;
    return -1;
}

/* Convert a C Node* to an XS map for plugin consumption */
Value *node_to_xs_map(Node *n) {
    if (!n) return value_incref(XS_NULL_VAL);
    Value *m = xs_map_new();
    map_set(m->map, "tag", xs_str(node_tag_to_string((NodeTag)VAL_TAG(n))));
    if (n->span.line > 0) map_take(m->map, "line", xs_int(n->span.line));

    switch (VAL_TAG(n)) {
    case NODE_LIT_INT:
        map_take(m->map, "value", xs_int(n->lit_int.ival));
        break;
    case NODE_LIT_FLOAT:
        map_set(m->map, "value", xs_float(n->lit_float.fval));
        break;
    case NODE_LIT_STRING:
    case NODE_INTERP_STRING:
        map_set(m->map, "value", xs_str(n->lit_string.sval ? n->lit_string.sval : ""));
        break;
    case NODE_LIT_BOOL:
        map_set(m->map, "value", n->lit_bool.bval ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
        break;
    case NODE_LIT_NULL:
        break;
    case NODE_IDENT:
        map_set(m->map, "name", xs_str(n->ident.name ? n->ident.name : ""));
        break;
    case NODE_BINOP:
        map_set(m->map, "op", xs_str(n->binop.op));
        map_set(m->map, "left", node_to_xs_map(n->binop.left));
        map_set(m->map, "right", node_to_xs_map(n->binop.right));
        break;
    case NODE_UNARY:
        map_set(m->map, "op", xs_str(n->unary.op));
        map_set(m->map, "expr", node_to_xs_map(n->unary.expr));
        break;
    case NODE_CALL: {
        map_set(m->map, "callee", node_to_xs_map(n->call.callee));
        Value *args = xs_array_new();
        for (int j = 0; j < n->call.args.len; j++)
            array_push(args->arr, node_to_xs_map(n->call.args.items[j]));
        map_set(m->map, "args", args);
        break;
    }
    case NODE_IF:
        map_set(m->map, "cond", node_to_xs_map(n->if_expr.cond));
        map_set(m->map, "then", node_to_xs_map(n->if_expr.then));
        if (n->if_expr.else_branch)
            map_set(m->map, "else", node_to_xs_map(n->if_expr.else_branch));
        break;
    case NODE_BLOCK: {
        Value *stmts = xs_array_new();
        for (int j = 0; j < n->block.stmts.len; j++)
            array_push(stmts->arr, node_to_xs_map(n->block.stmts.items[j]));
        map_set(m->map, "stmts", stmts);
        if (n->block.expr)
            map_set(m->map, "expr", node_to_xs_map(n->block.expr));
        break;
    }
    case NODE_LET: case NODE_VAR:
        map_set(m->map, "name", xs_str(n->let.name ? n->let.name : ""));
        if (n->let.value) map_set(m->map, "value", node_to_xs_map(n->let.value));
        break;
    case NODE_RETURN:
        if (n->ret.value) map_set(m->map, "value", node_to_xs_map(n->ret.value));
        break;
    case NODE_ASSIGN:
        map_set(m->map, "target", node_to_xs_map(n->assign.target));
        map_set(m->map, "value", node_to_xs_map(n->assign.value));
        break;
    case NODE_FOR:
        map_set(m->map, "pattern", node_to_xs_map(n->for_loop.pattern));
        map_set(m->map, "iter", node_to_xs_map(n->for_loop.iter));
        map_set(m->map, "body", node_to_xs_map(n->for_loop.body));
        break;
    case NODE_WHILE:
        map_set(m->map, "cond", node_to_xs_map(n->while_loop.cond));
        map_set(m->map, "body", node_to_xs_map(n->while_loop.body));
        break;
    case NODE_LAMBDA: {
        Value *params = xs_array_new();
        for (int j = 0; j < n->lambda.params.len; j++) {
            Param *pm = &n->lambda.params.items[j];
            array_push(params->arr, xs_str(pm->name ? pm->name : "_"));
        }
        map_set(m->map, "params", params);
        map_set(m->map, "body", node_to_xs_map(n->lambda.body));
        break;
    }
    case NODE_FN_DECL: {
        map_set(m->map, "name", xs_str(n->fn_decl.name ? n->fn_decl.name : ""));
        Value *params = xs_array_new();
        for (int j = 0; j < n->fn_decl.params.len; j++) {
            Param *pm = &n->fn_decl.params.items[j];
            array_push(params->arr, xs_str(pm->name ? pm->name : "_"));
        }
        map_set(m->map, "params", params);
        map_set(m->map, "body", node_to_xs_map(n->fn_decl.body));
        break;
    }
    case NODE_LIT_DURATION:
        map_take(m->map, "ns", xs_int(n->lit_duration.ns));
        break;
    case NODE_EVERY:
        map_set(m->map, "interval", node_to_xs_map(n->every_.interval));
        map_set(m->map, "body", node_to_xs_map(n->every_.body));
        break;
    case NODE_AFTER:
        map_set(m->map, "delay", node_to_xs_map(n->after_.delay));
        map_set(m->map, "body", node_to_xs_map(n->after_.body));
        break;
    case NODE_TIMEOUT:
        map_set(m->map, "duration", node_to_xs_map(n->timeout_.duration));
        map_set(m->map, "body", node_to_xs_map(n->timeout_.body));
        if (n->timeout_.fallback)
            map_set(m->map, "fallback", node_to_xs_map(n->timeout_.fallback));
        break;
    case NODE_DEBOUNCE:
        map_set(m->map, "delay", node_to_xs_map(n->debounce_.delay));
        map_set(m->map, "body", node_to_xs_map(n->debounce_.body));
        break;
    case NODE_PAUSE:
        map_set(m->map, "duration", node_to_xs_map(n->pause_.duration));
        break;
    case NODE_DEL:
        map_set(m->map, "name", xs_str(n->del_.name));
        break;
    case NODE_METHOD_CALL: {
        map_set(m->map, "obj", node_to_xs_map(n->method_call.obj));
        map_set(m->map, "method", xs_str(n->method_call.method ? n->method_call.method : ""));
        Value *mc_args = xs_array_new();
        for (int j = 0; j < n->method_call.args.len; j++)
            array_push(mc_args->arr, node_to_xs_map(n->method_call.args.items[j]));
        map_set(m->map, "args", mc_args);
        break;
    }
    case NODE_INDEX:
        map_set(m->map, "obj", node_to_xs_map(n->index.obj));
        map_set(m->map, "index", node_to_xs_map(n->index.index));
        break;
    case NODE_THROW:
        if (n->throw_.value) map_set(m->map, "value", node_to_xs_map(n->throw_.value));
        break;
    default:
        break;
    }
    return m;
}

/* Convert an XS map back to a C Node* */
Node *node_from_xs_map(Value *map) {
    if (!map || VAL_TAG(map) != XS_MAP) return NULL;
    Value *tag_v = map_get(map->map, "tag");
    if (!tag_v || VAL_TAG(tag_v) != XS_STR) return NULL;
    const char *tag_s = tag_v->s;
    int tag_i = node_tag_from_string(tag_s);
    Span sp = span_zero();

    if (tag_i == NODE_LIT_INT) {
        Node *n = node_new(NODE_LIT_INT, sp);
        Value *v = map_get(map->map, "value");
        n->lit_int.ival = (v && VAL_TAG(v) == XS_INT) ? VAL_INT(v) : 0;
        return n;
    }
    if (tag_i == NODE_LIT_FLOAT) {
        Node *n = node_new(NODE_LIT_FLOAT, sp);
        Value *v = map_get(map->map, "value");
        n->lit_float.fval = (v && VAL_TAG(v) == XS_FLOAT) ? v->f : 0.0;
        return n;
    }
    if (tag_i == NODE_LIT_STRING) {
        Node *n = node_new(NODE_LIT_STRING, sp);
        Value *v = map_get(map->map, "value");
        n->lit_string.sval = xs_strdup((v && VAL_TAG(v) == XS_STR) ? v->s : "");
        n->lit_string.interpolated = 0;
        n->lit_string.parts = nodelist_new();
        return n;
    }
    if (tag_i == NODE_LIT_BOOL) {
        Node *n = node_new(NODE_LIT_BOOL, sp);
        Value *v = map_get(map->map, "value");
        n->lit_bool.bval = (v && value_truthy(v)) ? 1 : 0;
        return n;
    }
    if (tag_i == NODE_LIT_NULL) {
        return node_new(NODE_LIT_NULL, sp);
    }
    if (tag_i == NODE_IDENT) {
        Node *n = node_new(NODE_IDENT, sp);
        Value *v = map_get(map->map, "name");
        n->ident.name = xs_strdup((v && VAL_TAG(v) == XS_STR) ? v->s : "");
        return n;
    }
    if (tag_i == NODE_BINOP) {
        Node *n = node_new(NODE_BINOP, sp);
        Value *op = map_get(map->map, "op");
        strncpy(n->binop.op, (op && VAL_TAG(op) == XS_STR) ? op->s : "+", sizeof(n->binop.op)-1);
        Value *l = map_get(map->map, "left");
        Value *r = map_get(map->map, "right");
        n->binop.left = node_from_xs_map(l);
        n->binop.right = node_from_xs_map(r);
        if (!n->binop.left) n->binop.left = node_new(NODE_LIT_NULL, sp);
        if (!n->binop.right) n->binop.right = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_UNARY) {
        Node *n = node_new(NODE_UNARY, sp);
        Value *op = map_get(map->map, "op");
        const char *ops = (op && VAL_TAG(op) == XS_STR) ? op->s : "-";
        if (strcmp(ops, "not") == 0) ops = "!";
        strncpy(n->unary.op, ops, sizeof(n->unary.op)-1);
        Value *e = map_get(map->map, "expr");
        n->unary.expr = node_from_xs_map(e);
        if (!n->unary.expr) n->unary.expr = node_new(NODE_LIT_NULL, sp);
        n->unary.prefix = 1;
        return n;
    }
    if (tag_i == NODE_CALL) {
        Node *n = node_new(NODE_CALL, sp);
        Value *callee = map_get(map->map, "callee");
        n->call.callee = node_from_xs_map(callee);
        if (!n->call.callee) n->call.callee = node_new(NODE_LIT_NULL, sp);
        n->call.args = nodelist_new();
        n->call.kwargs = nodepairlist_new();
        Value *args = map_get(map->map, "args");
        if (args && VAL_TAG(args) == XS_ARRAY) {
            for (int j = 0; j < args->arr->len; j++) {
                Node *an = node_from_xs_map(args->arr->items[j]);
                if (an) nodelist_push(&n->call.args, an);
            }
        }
        return n;
    }
    if (tag_i == NODE_IF) {
        Node *n = node_new(NODE_IF, sp);
        Value *cond = map_get(map->map, "cond");
        Value *then = map_get(map->map, "then");
        Value *els = map_get(map->map, "else");
        n->if_expr.cond = node_from_xs_map(cond);
        n->if_expr.then = node_from_xs_map(then);
        n->if_expr.elif_conds = nodelist_new();
        n->if_expr.elif_thens = nodelist_new();
        n->if_expr.else_branch = els ? node_from_xs_map(els) : NULL;
        if (!n->if_expr.cond) n->if_expr.cond = node_new(NODE_LIT_NULL, sp);
        if (!n->if_expr.then) n->if_expr.then = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_BLOCK) {
        Node *n = node_new(NODE_BLOCK, sp);
        n->block.stmts = nodelist_new();
        n->block.expr = NULL;
        n->block.has_decls = -1;
        n->block.is_unsafe = 0;
        Value *stmts = map_get(map->map, "stmts");
        if (stmts && VAL_TAG(stmts) == XS_ARRAY) {
            for (int j = 0; j < stmts->arr->len; j++) {
                Node *sn = node_from_xs_map(stmts->arr->items[j]);
                if (sn) nodelist_push(&n->block.stmts, sn);
            }
        }
        Value *expr = map_get(map->map, "expr");
        if (expr && VAL_TAG(expr) == XS_MAP)
            n->block.expr = node_from_xs_map(expr);
        return n;
    }
    if (tag_i == NODE_LET || tag_i == NODE_VAR) {
        Node *n = node_new((NodeTag)tag_i, sp);
        Value *name = map_get(map->map, "name");
        Value *val = map_get(map->map, "value");
        const char *nm = (name && VAL_TAG(name) == XS_STR) ? name->s : "";
        Node *pat = node_new(NODE_PAT_IDENT, sp);
        pat->pat_ident.name = xs_strdup(nm);
        pat->pat_ident.mutable = (tag_i == NODE_VAR) ? 1 : 0;
        n->let.pattern = pat;
        n->let.name = xs_strdup(nm);
        n->let.value = (val && VAL_TAG(val) == XS_MAP) ? node_from_xs_map(val) : NULL;
        n->let.mutable = (tag_i == NODE_VAR) ? 1 : 0;
        n->let.type_ann = NULL;
        return n;
    }
    if (tag_i == NODE_RETURN) {
        Node *n = node_new(NODE_RETURN, sp);
        Value *val = map_get(map->map, "value");
        n->ret.value = (val && VAL_TAG(val) == XS_MAP) ? node_from_xs_map(val) : NULL;
        return n;
    }
    if (tag_i == NODE_ASSIGN) {
        Node *n = node_new(NODE_ASSIGN, sp);
        Value *tgt = map_get(map->map, "target");
        Value *val = map_get(map->map, "value");
        n->assign.target = node_from_xs_map(tgt);
        n->assign.value = node_from_xs_map(val);
        strncpy(n->assign.op, "=", sizeof(n->assign.op)-1);
        if (!n->assign.target) n->assign.target = node_new(NODE_LIT_NULL, sp);
        if (!n->assign.value) n->assign.value = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_FOR) {
        Node *n = node_new(NODE_FOR, sp);
        Value *pat = map_get(map->map, "pattern");
        Value *iter = map_get(map->map, "iter");
        Value *body = map_get(map->map, "body");
        n->for_loop.pattern = node_from_xs_map(pat);
        n->for_loop.iter = node_from_xs_map(iter);
        n->for_loop.body = node_from_xs_map(body);
        n->for_loop.label = NULL;
        if (!n->for_loop.pattern) n->for_loop.pattern = node_new(NODE_LIT_NULL, sp);
        if (!n->for_loop.iter) n->for_loop.iter = node_new(NODE_LIT_NULL, sp);
        if (!n->for_loop.body) n->for_loop.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_WHILE) {
        Node *n = node_new(NODE_WHILE, sp);
        Value *cond = map_get(map->map, "cond");
        Value *body = map_get(map->map, "body");
        n->while_loop.cond = node_from_xs_map(cond);
        n->while_loop.body = node_from_xs_map(body);
        n->while_loop.label = NULL;
        if (!n->while_loop.cond) n->while_loop.cond = node_new(NODE_LIT_NULL, sp);
        if (!n->while_loop.body) n->while_loop.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_LAMBDA) {
        Node *n = node_new(NODE_LAMBDA, sp);
        n->lambda.params = paramlist_new();
        n->lambda.is_generator = 0;
        Value *params = map_get(map->map, "params");
        if (params && VAL_TAG(params) == XS_ARRAY) {
            for (int j = 0; j < params->arr->len; j++) {
                Value *pv = params->arr->items[j];
                Param p = {0};
                p.span = sp;
                const char *pname = "_";
                if (pv && VAL_TAG(pv) == XS_STR) pname = pv->s;
                else if (pv && VAL_TAG(pv) == XS_MAP) {
                    Value *nv = map_get(pv->map, "name");
                    if (nv && VAL_TAG(nv) == XS_STR) pname = nv->s;
                }
                p.name = xs_strdup(pname);
                Node *pat = node_new(NODE_PAT_IDENT, sp);
                pat->pat_ident.name = xs_strdup(pname);
                pat->pat_ident.mutable = 0;
                p.pattern = pat;
                paramlist_push(&n->lambda.params, p);
            }
        }
        Value *body = map_get(map->map, "body");
        n->lambda.body = (body && VAL_TAG(body) == XS_MAP) ? node_from_xs_map(body) : NULL;
        if (!n->lambda.body) n->lambda.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_FN_DECL) {
        Node *n = node_new(NODE_FN_DECL, sp);
        Value *name = map_get(map->map, "name");
        n->fn_decl.name = xs_strdup((name && VAL_TAG(name) == XS_STR) ? name->s : "");
        n->fn_decl.params = paramlist_new();
        Value *params = map_get(map->map, "params");
        if (params && VAL_TAG(params) == XS_ARRAY) {
            for (int j = 0; j < params->arr->len; j++) {
                Value *pv = params->arr->items[j];
                Param p = {0};
                p.span = sp;
                const char *pname = "_";
                if (pv && VAL_TAG(pv) == XS_STR) pname = pv->s;
                p.name = xs_strdup(pname);
                Node *pat = node_new(NODE_PAT_IDENT, sp);
                pat->pat_ident.name = xs_strdup(pname);
                pat->pat_ident.mutable = 0;
                p.pattern = pat;
                paramlist_push(&n->fn_decl.params, p);
            }
        }
        Value *body = map_get(map->map, "body");
        n->fn_decl.body = (body && VAL_TAG(body) == XS_MAP) ? node_from_xs_map(body) : NULL;
        if (!n->fn_decl.body) n->fn_decl.body = node_new(NODE_LIT_NULL, sp);
        n->fn_decl.is_async = 0;
        n->fn_decl.is_pub = 0;
        n->fn_decl.is_generator = 0;
        n->fn_decl.is_pure = 0;
        n->fn_decl.is_test = 0;
        n->fn_decl.is_static = 0;
        n->fn_decl.deprecated_msg = NULL;
        n->fn_decl.ret_type = NULL;
        n->fn_decl.type_params = NULL;
        n->fn_decl.type_bounds = NULL;
        n->fn_decl.n_type_params = 0;
        return n;
    }
    if (tag_i == NODE_EXPR_STMT) {
        Value *expr = map_get(map->map, "expr");
        if (expr && VAL_TAG(expr) == XS_MAP) {
            Node *n = node_new(NODE_EXPR_STMT, sp);
            n->expr_stmt.expr = node_from_xs_map(expr);
            n->expr_stmt.has_semicolon = 0;
            return n;
        }
    }
    if (tag_i == NODE_LIT_DURATION) {
        Node *n = node_new(NODE_LIT_DURATION, sp);
        Value *v = map_get(map->map, "ns");
        n->lit_duration.ns = (v && VAL_TAG(v) == XS_INT) ? VAL_INT(v) : 0;
        return n;
    }
    if (tag_i == NODE_EVERY) {
        Node *n = node_new(NODE_EVERY, sp);
        Value *interval = map_get(map->map, "interval");
        Value *body = map_get(map->map, "body");
        n->every_.interval = node_from_xs_map(interval);
        n->every_.body = node_from_xs_map(body);
        if (!n->every_.interval) n->every_.interval = node_new(NODE_LIT_NULL, sp);
        if (!n->every_.body) n->every_.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_AFTER) {
        Node *n = node_new(NODE_AFTER, sp);
        Value *delay = map_get(map->map, "delay");
        Value *body = map_get(map->map, "body");
        n->after_.delay = node_from_xs_map(delay);
        n->after_.body = node_from_xs_map(body);
        if (!n->after_.delay) n->after_.delay = node_new(NODE_LIT_NULL, sp);
        if (!n->after_.body) n->after_.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_TIMEOUT) {
        Node *n = node_new(NODE_TIMEOUT, sp);
        Value *dur = map_get(map->map, "duration");
        Value *body = map_get(map->map, "body");
        Value *fb = map_get(map->map, "fallback");
        n->timeout_.duration = node_from_xs_map(dur);
        n->timeout_.body = node_from_xs_map(body);
        n->timeout_.fallback = (fb && VAL_TAG(fb) == XS_MAP) ? node_from_xs_map(fb) : NULL;
        if (!n->timeout_.duration) n->timeout_.duration = node_new(NODE_LIT_NULL, sp);
        if (!n->timeout_.body) n->timeout_.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }
    if (tag_i == NODE_DEBOUNCE) {
        Node *n = node_new(NODE_DEBOUNCE, sp);
        Value *delay = map_get(map->map, "delay");
        Value *body = map_get(map->map, "body");
        n->debounce_.delay = node_from_xs_map(delay);
        n->debounce_.body = node_from_xs_map(body);
        if (!n->debounce_.delay) n->debounce_.delay = node_new(NODE_LIT_NULL, sp);
        if (!n->debounce_.body) n->debounce_.body = node_new(NODE_LIT_NULL, sp);
        return n;
    }

    /* Unsupported tag: wrap as a plugin_eval node that calls the XS value directly.
       We store the XS map itself as a literal that interp_eval can handle. */
    return NULL;
}
