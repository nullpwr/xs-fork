#include "transpiler/js.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "core/strbuf.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/ast.h"
#include "semantic/purity.h"
#include "core/value.h"

/* forward declarations */
static void emit_node(SB *s, Node *n, int depth);
static void emit_expr(SB *s, Node *n, int depth);
static void emit_stmt(SB *s, Node *n, int depth);
static void emit_block_body(SB *s, Node *block, int depth);
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth);
static int node_has_perform(Node *n);
static int node_has_await(Node *n);
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth);

/* Names that have been `del`'d in the current emit scope. Mirrors the
 * C transpiler's tracking; reset per transpile_js call and saved /
 * restored across function bodies so a fn-local `del y` doesn't bleed
 * into other scopes. */
#define MAX_JS_DELETED_VARS 64
static const char *js_deleted_vars[MAX_JS_DELETED_VARS];
static int js_n_deleted_vars = 0;

/* Source-file directory for the program currently being transpiled.
   Used to resolve `use "./relative.xs"` paths in NODE_USE emit. */
static char g_js_src_dir[1024] = "";

/* Trait default-method registry. Filled by NODE_TRAIT_DECL emit; read
   by NODE_IMPL_DECL to inherit default bodies for methods the impl
   doesn't override. Bodies are weak refs into the program AST -- valid
   for the duration of one transpile pass. */
#define MAX_JS_TRAITS 64
typedef struct {
    const char *trait_name;
    Node       *trait_node;
} JsTraitEntry;
static JsTraitEntry js_traits[MAX_JS_TRAITS];
static int js_n_traits = 0;

static void js_register_trait(Node *trait) {
    if (!trait || !trait->trait_decl.name) return;
    if (js_n_traits >= MAX_JS_TRAITS) return;
    js_traits[js_n_traits].trait_name = trait->trait_decl.name;
    js_traits[js_n_traits].trait_node = trait;
    js_n_traits++;
}

static Node *js_find_trait(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < js_n_traits; i++)
        if (js_traits[i].trait_name && strcmp(js_traits[i].trait_name, name) == 0)
            return js_traits[i].trait_node;
    return NULL;
}

/* Loaded `use "..."` modules; we keep the parsed AST alive for the rest
   of the pass so child fn bodies (referenced from emitted code) stay
   valid. Also serves as a guard against re-loading the same path twice. */
#define MAX_JS_USE_MODS 64
typedef struct {
    char *path;
    Node *prog;
    char *src;  /* freed at pass end */
} JsUseMod;
static JsUseMod js_use_mods[MAX_JS_USE_MODS];
static int js_n_use_mods = 0;

static char *js_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)xs_malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

/* Resolve `path` relative to the importer's directory if it isn't absolute. */
static void js_resolve_use_path(const char *path, char *out, size_t cap) {
    if (!path) { out[0] = '\0'; return; }
    if (path[0] == '/' || g_js_src_dir[0] == '\0') {
        snprintf(out, cap, "%s", path);
    } else {
        snprintf(out, cap, "%s/%s", g_js_src_dir, path);
    }
}

static Node *js_load_use_module(const char *resolved) {
    for (int i = 0; i < js_n_use_mods; i++)
        if (js_use_mods[i].path && strcmp(js_use_mods[i].path, resolved) == 0)
            return js_use_mods[i].prog;
    if (js_n_use_mods >= MAX_JS_USE_MODS) return NULL;
    char *src = js_read_file(resolved);
    if (!src) return NULL;
    char *path_owned = xs_strdup(resolved);
    Lexer lex; lexer_init(&lex, src, path_owned);
    TokenArray ta = lexer_tokenize(&lex);
    Parser p; parser_init(&p, &ta, path_owned);
    Node *prog = parser_parse(&p);
    token_array_free(&ta);
    comment_list_free(&lex.comments);
    if (!prog || p.had_error) {
        if (prog) node_free(prog);
        free(src); free(path_owned);
        return NULL;
    }
    js_use_mods[js_n_use_mods].path = path_owned;
    js_use_mods[js_n_use_mods].prog = prog;
    js_use_mods[js_n_use_mods].src  = src;
    js_n_use_mods++;
    return prog;
}

/* Best-effort namespace name from `use "foo/bar.xs"` -> "bar". */
static void js_derive_use_alias(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t i = 0;
    while (base[i] && base[i] != '.' && i + 1 < cap) {
        char c = base[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) c = '_';
        out[i] = c; i++;
    }
    out[i] = '\0';
    if (out[0] == '\0') snprintf(out, cap, "_mod");
}

/* Walk the imported program for `export { ... }` lists. If any exist,
   only the listed names (under their public aliases) are exposed.
   Otherwise everything top-level becomes public. */
typedef struct {
    int   any;
    int   n;
    char *locals[256];
    char *aliases[256];
} JsExports;

static void js_collect_exports(Node *prog, JsExports *out) {
    out->any = 0; out->n = 0;
    if (!prog || VAL_TAG(prog) != NODE_PROGRAM) return;
    for (int i = 0; i < prog->program.stmts.len; i++) {
        Node *st = prog->program.stmts.items[i];
        if (!st || VAL_TAG(st) != NODE_EXPORT) continue;
        out->any = 1;
        for (int k = 0; k < st->export_.nnames && out->n < 256; k++) {
            out->locals[out->n]  = st->export_.names[k];
            out->aliases[out->n] = st->export_.aliases[k] ? st->export_.aliases[k]
                                                          : st->export_.names[k];
            out->n++;
        }
    }
}

/* Top-level statements that bind a name in JS scope. We use this to
   decide what to expose when there is no `export { ... }` list. */
static const char *js_stmt_binds_name(Node *st) {
    if (!st) return NULL;
    switch (VAL_TAG(st)) {
    case NODE_LET:        return st->let.name;
    case NODE_VAR:        return st->let.name;
    case NODE_CONST:      return st->const_.name;
    case NODE_FN_DECL:    return st->fn_decl.name;
    case NODE_STRUCT_DECL: return st->struct_decl.name;
    case NODE_ENUM_DECL:  return st->enum_decl.name;
    case NODE_CLASS_DECL: return st->class_decl.name;
    default: return NULL;
    }
}
static int js_is_deleted_var(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < js_n_deleted_vars; i++)
        if (strcmp(js_deleted_vars[i], name) == 0) return 1;
    return 0;
}
static void js_mark_deleted_var(const char *name) {
    if (!name) return;
    if (js_is_deleted_var(name)) return;
    if (js_n_deleted_vars < MAX_JS_DELETED_VARS)
        js_deleted_vars[js_n_deleted_vars++] = name;
}

/* AST walker that flags constructs the JS target can't faithfully lower.
 * Refused at emit time so we get a clear error rather than runtime garbage.
 * Mirrors find_unsupported_for_c (different set of unsupporteds). */
static const char *find_unsupported_for_js(Node *n) {
    if (!n) return NULL;
    switch (VAL_TAG(n)) {
    case NODE_FN_DECL: {
        for (int i = 0; i < n->fn_decl.n_decorators; i++) {
            const char *dn = n->fn_decl.decorators[i].name;
            if (!dn) continue;
            if (strcmp(dn, "memoize") == 0 || strcmp(dn, "retry") == 0 ||
                strcmp(dn, "timed") == 0   || strcmp(dn, "trace") == 0 ||
                strcmp(dn, "throttle") == 0|| strcmp(dn, "debounce") == 0)
                return "wrapping decorators (@memoize/@retry/@timed/@trace/@throttle/@debounce)";
        }
        return find_unsupported_for_js(n->fn_decl.body);
    }
    case NODE_LAMBDA:   return find_unsupported_for_js(n->lambda.body);
    case NODE_BIND:     return "reactive `bind` declarations";
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++) {
            const char *r = find_unsupported_for_js(n->program.stmts.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++) {
            const char *r = find_unsupported_for_js(n->block.stmts.items[i]);
            if (r) return r;
        }
        return find_unsupported_for_js(n->block.expr);
    case NODE_BINOP:
        { const char *r = find_unsupported_for_js(n->binop.left); if (r) return r; }
        return find_unsupported_for_js(n->binop.right);
    case NODE_UNARY:    return find_unsupported_for_js(n->unary.expr);
    case NODE_CALL:
        { const char *r = find_unsupported_for_js(n->call.callee); if (r) return r; }
        for (int i = 0; i < n->call.args.len; i++) {
            const char *r = find_unsupported_for_js(n->call.args.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_METHOD_CALL:
        { const char *r = find_unsupported_for_js(n->method_call.obj); if (r) return r; }
        for (int i = 0; i < n->method_call.args.len; i++) {
            const char *r = find_unsupported_for_js(n->method_call.args.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_INDEX:
        { const char *r = find_unsupported_for_js(n->index.obj); if (r) return r; }
        return find_unsupported_for_js(n->index.index);
    case NODE_FIELD:    return find_unsupported_for_js(n->field.obj);
    case NODE_ASSIGN:
        { const char *r = find_unsupported_for_js(n->assign.target); if (r) return r; }
        return find_unsupported_for_js(n->assign.value);
    case NODE_IF:
        { const char *r = find_unsupported_for_js(n->if_expr.cond); if (r) return r; }
        { const char *r = find_unsupported_for_js(n->if_expr.then); if (r) return r; }
        return find_unsupported_for_js(n->if_expr.else_branch);
    case NODE_FOR:
        { const char *r = find_unsupported_for_js(n->for_loop.iter); if (r) return r; }
        return find_unsupported_for_js(n->for_loop.body);
    case NODE_WHILE:
        { const char *r = find_unsupported_for_js(n->while_loop.cond); if (r) return r; }
        return find_unsupported_for_js(n->while_loop.body);
    case NODE_RETURN:   return find_unsupported_for_js(n->ret.value);
    case NODE_LET:
    case NODE_VAR:      return find_unsupported_for_js(n->let.value);
    case NODE_CONST:    return find_unsupported_for_js(n->const_.value);
    case NODE_EXPR_STMT: return find_unsupported_for_js(n->expr_stmt.expr);
    case NODE_TRY:
        { const char *r = find_unsupported_for_js(n->try_.body); if (r) return r; }
        return find_unsupported_for_js(n->try_.finally_block);
    case NODE_THROW:    return find_unsupported_for_js(n->throw_.value);
    case NODE_MATCH:
        { const char *r = find_unsupported_for_js(n->match.subject); if (r) return r; }
        for (int i = 0; i < n->match.arms.len; i++) {
            { const char *r = find_unsupported_for_js(n->match.arms.items[i].guard); if (r) return r; }
            { const char *r = find_unsupported_for_js(n->match.arms.items[i].body); if (r) return r; }
        }
        return NULL;
    case NODE_LIT_ARRAY:
    case NODE_LIT_TUPLE:
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            const char *r = find_unsupported_for_js(n->lit_array.elems.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_LIT_MAP:
        for (int i = 0; i < n->lit_map.vals.len; i++) {
            const char *r = find_unsupported_for_js(n->lit_map.vals.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_IMPL_DECL:
        for (int i = 0; i < n->impl_decl.members.len; i++) {
            const char *r = find_unsupported_for_js(n->impl_decl.members.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_RANGE:
        { const char *r = find_unsupported_for_js(n->range.start); if (r) return r; }
        return find_unsupported_for_js(n->range.end);
    default: return NULL;
    }
}

/* state: are we inside a class method body? */
static int in_class_method = 0;

/* helpers */
static int is_callee_name(Node *callee, const char *name) {
    return callee && VAL_TAG(callee) == NODE_IDENT && strcmp(callee->ident.name, name) == 0;
}

/* Common JS reserved names that XS allows as parameter or variable names. */
static int is_js_reserved(const char *name) {
    if (!name) return 0;
    static const char *resv[] = {
        "default","class","const","let","var","function","return","if","else",
        "while","do","for","break","continue","switch","case","try","catch",
        "finally","throw","new","delete","typeof","instanceof","in","of",
        "this","super","null","true","false","undefined","void","with",
        "yield","async","await","import","export","from","as","static",
        "private","protected","public","interface","extends","implements",
        "enum","package","arguments","eval", NULL
    };
    for (int i = 0; resv[i]; i++) if (strcmp(name, resv[i]) == 0) return 1;
    return 0;
}
static void emit_param_name(SB *s, const char *name) {
    if (!name) { sb_add(s, "_"); return; }
    if (is_js_reserved(name)) sb_printf(s, "__xs_%s", name);
    else sb_add(s, name);
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
        emit_param_name(s, p->name);
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
        /* JS Number can't represent integers above 2^53 exactly, so
           emit anything outside Number.MAX_SAFE_INTEGER as a BigInt
           literal. The runtime helpers (`__xs_add`, `__xs_div`,
           `__xs_eq`) already handle the bigint+number mix. */
        if (n->lit_int.ival > 9007199254740991LL ||
            n->lit_int.ival < -9007199254740991LL) {
            sb_printf(s, "%" PRId64 "n", n->lit_int.ival);
        } else {
            sb_printf(s, "%" PRId64, n->lit_int.ival);
        }
        break;
    case NODE_LIT_BIGINT:
        sb_printf(s, "%sn", n->lit_bigint.bigint_str);
        break;
    case NODE_LIT_FLOAT: {
        /* shortest round-trippable representation; %g rounds at 6 digits
           and silently loses precision for things like Number.MAX_VALUE. */
        char buf[64];
        for (int prec = 1; prec <= 17; prec++) {
            snprintf(buf, sizeof buf, "%.*g", prec, n->lit_float.fval);
            double back; if (sscanf(buf, "%lf", &back) == 1 && back == n->lit_float.fval) break;
        }
        sb_add(s, buf);
        break;
    }
    case NODE_LIT_DURATION:
        sb_printf(s, "%lld", (long long)n->lit_duration.ns);
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
    case NODE_LIT_TUPLE: {
        int is_tuple = (VAL_TAG(n) == NODE_LIT_TUPLE);
        if (is_tuple) sb_add(s, "__xs_tuple([");
        else sb_addc(s, '[');
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            if (i) sb_add(s, ", ");
            emit_expr(s, n->lit_array.elems.items[i], depth);
        }
        if (is_tuple) sb_add(s, "])");
        else sb_addc(s, ']');
        break;
    }
    case NODE_LIT_MAP: {
        /* Spreads and bareword identifier keys both need special handling.
           An identifier key like `{ a: 1 }` is a string key "a" in the
           interpreter and VM, not a variable lookup. A spread `...m`
           merges the source map's entries in order. We build the Map
           imperatively so spreads can interleave with explicit entries. */
        int has_spread = 0;
        for (int i = 0; i < n->lit_map.keys.len; i++) {
            Node *k = n->lit_map.keys.items[i];
            if (k && VAL_TAG(k) == NODE_SPREAD) { has_spread = 1; break; }
        }
        if (!has_spread) {
            sb_add(s, "new Map([");
            for (int i = 0; i < n->lit_map.keys.len; i++) {
                Node *k = n->lit_map.keys.items[i];
                if (i) sb_add(s, ", ");
                sb_addc(s, '[');
                if (k && VAL_TAG(k) == NODE_IDENT) {
                    sb_addc(s, '"');
                    sb_add(s, k->ident.name);
                    sb_addc(s, '"');
                } else {
                    emit_expr(s, k, depth);
                }
                sb_add(s, ", ");
                emit_expr(s, n->lit_map.vals.items[i], depth);
                sb_addc(s, ']');
            }
            sb_add(s, "])");
        } else {
            sb_add(s, "(() => { const __m = new Map();");
            for (int i = 0; i < n->lit_map.keys.len; i++) {
                Node *k = n->lit_map.keys.items[i];
                if (k && VAL_TAG(k) == NODE_SPREAD) {
                    sb_add(s, " { const __src = ");
                    emit_expr(s, k->spread.expr, depth);
                    sb_add(s, "; if (__src instanceof Map) { for (const [__k, __v] of __src) __m.set(__k, __v); } else if (__src && typeof __src === 'object') { for (const __k of Object.keys(__src)) __m.set(__k, __src[__k]); } }");
                } else {
                    sb_add(s, " __m.set(");
                    if (k && VAL_TAG(k) == NODE_IDENT) {
                        sb_addc(s, '"');
                        sb_add(s, k->ident.name);
                        sb_addc(s, '"');
                    } else {
                        emit_expr(s, k, depth);
                    }
                    sb_add(s, ", ");
                    emit_expr(s, n->lit_map.vals.items[i], depth);
                    sb_add(s, ");");
                }
            }
            sb_add(s, " return __m; })()");
        }
        break;
    }
    case NODE_IDENT: {
        int wrap_del = js_is_deleted_var(n->ident.name);
        if (wrap_del) sb_add(s, "__xs_check_deleted(");
        if (in_class_method && n->ident.name && strcmp(n->ident.name, "self") == 0)
            sb_add(s, "this");
        else if (n->ident.name && is_js_reserved(n->ident.name))
            sb_printf(s, "__xs_%s", n->ident.name);
        else
            sb_add(s, n->ident.name);
        if (wrap_del) sb_printf(s, ", \"%s\")", n->ident.name);
        break;
    }
    case NODE_BINOP: {
        const char *op = n->binop.op;
        if (strcmp(op, "++") == 0) {
            /* ++ in XS is concat: array + array -> array, string +
               string -> string, map + map -> merge. JS's bare `+`
               only does string concat correctly; arrays under `+`
               coerce to comma-joined strings. Route through a helper
               that picks the right shape. */
            sb_add(s, "__xs_concat(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
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
            /* Route through __xs_pow so big-integer results don't
               silently fall to Number and lose precision. */
            sb_add(s, "__xs_pow(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "*") == 0 || strcmp(op, "-") == 0 ||
                   strcmp(op, "%") == 0) {
            /* Promote bigint when either operand is one. JS won't mix
               bigint and number under * / - / %. */
            sb_printf(s, "__xs_arith(\"%s\", ", op);
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
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
            /* Float-typed divide produces NaN on /0; XS's interp matches.
               If either operand is a float literal, skip the runtime
               helper's int-throw path and use plain JS divide. */
            int either_float =
                (n->binop.left  && VAL_TAG(n->binop.left)  == NODE_LIT_FLOAT) ||
                (n->binop.right && VAL_TAG(n->binop.right) == NODE_LIT_FLOAT);
            if (either_float) {
                sb_addc(s, '(');
                emit_expr(s, n->binop.left, depth);
                sb_add(s, " / ");
                emit_expr(s, n->binop.right, depth);
                sb_addc(s, ')');
            } else {
                sb_add(s, "__xs_div(");
                emit_expr(s, n->binop.left, depth);
                sb_add(s, ", ");
                emit_expr(s, n->binop.right, depth);
                sb_addc(s, ')');
            }
        } else if (strcmp(op, "==") == 0) {
            sb_add(s, "__xs_eq(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "!=") == 0) {
            sb_add(s, "(!__xs_eq(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "<=>") == 0) {
            sb_add(s, "__xs_cmp(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "is") == 0) {
            /* `value is TypeName` -> __xs_is_a(value, "TypeName"). The RHS
               is parsed as an identifier (or a chain of identifiers); we
               emit it as a string literal. */
            sb_add(s, "__xs_is_a(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", \"");
            if (n->binop.right && VAL_TAG(n->binop.right) == NODE_IDENT) {
                sb_add(s, n->binop.right->ident.name);
            } else if (n->binop.right && VAL_TAG(n->binop.right) == NODE_LIT_STRING) {
                if (n->binop.right->lit_string.sval) sb_add(s, n->binop.right->lit_string.sval);
            }
            sb_add(s, "\")");
        } else if (strcmp(op, "in") == 0) {
            /* x in collection -> __xs_contains(collection, x) */
            sb_add(s, "__xs_contains(");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.left, depth);
            sb_addc(s, ')');
        } else {
            /* default: -, *, %, <, >, <=, >=, &, |, ^, <<, >> */
            sb_addc(s, '(');
            emit_expr(s, n->binop.left, depth);
            sb_addc(s, ' ');
            sb_add(s, op);
            sb_addc(s, ' ');
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_UNARY: {
        const char *op = n->unary.op;
        if (strcmp(op, "not") == 0 || strcmp(op, "!") == 0) {
            /* XS truthiness differs from JS: [], {}, "" are falsy in
               XS but truthy in JS. Route through __xs_truthy. */
            sb_add(s, "(!__xs_truthy(");
            emit_expr(s, n->unary.expr, depth);
            sb_add(s, "))");
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
            sb_add(s, "__xs_repr(");
        } else if (is_callee_name(n->call.callee, "bool")) {
            sb_add(s, "__xs_truthy(");
        } else if (is_callee_name(n->call.callee, "int")) {
            sb_add(s, "__xs_to_int((");
        } else if (is_callee_name(n->call.callee, "float")) {
            sb_add(s, "__xs_to_float((");
        } else if (is_callee_name(n->call.callee, "type")) {
            /* XS type names differ from JS typeof: int / float / str /
               array / map / null / bool. Route through __xs_type. */
            sb_add(s, "__xs_type(");
        } else if (is_callee_name(n->call.callee, "__pure?")) {
            sb_add(s, "__xs_is_pure(");
        } else if (is_callee_name(n->call.callee, "len")) {
            sb_add(s, "__xs_len(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        } else if (is_callee_name(n->call.callee, "assert")) {
            int has_aw = 0;
            for (int j = 0; j < n->call.args.len; j++)
                if (node_has_await(n->call.args.items[j])) { has_aw = 1; break; }
            if (has_aw) sb_add(s, "await (async function(){ if (!(");
            else        sb_add(s, "(function(){ if (!(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ")) throw new Error(\"assertion failed\"); })()");
            break;
        } else if (is_callee_name(n->call.callee, "assert_eq")) {
            int has_aw = 0;
            for (int j = 0; j < n->call.args.len; j++)
                if (node_has_await(n->call.args.items[j])) { has_aw = 1; break; }
            if (has_aw) sb_add(s, "await (async function(){ if (!__xs_eq_assert(");
            else        sb_add(s, "(function(){ if (!__xs_eq_assert(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ", ");
            if (n->call.args.len > 1) emit_expr(s, n->call.args.items[1], depth);
            sb_add(s, ")) throw new Error(\"assertion failed: \" + __xs_repr(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            sb_add(s, ") + \" !== \" + __xs_repr(");
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
        } else if (is_callee_name(n->call.callee, "reduce") ||
                   is_callee_name(n->call.callee, "fold") ||
                   is_callee_name(n->call.callee, "map") ||
                   is_callee_name(n->call.callee, "filter") ||
                   is_callee_name(n->call.callee, "each") ||
                   is_callee_name(n->call.callee, "some") ||
                   is_callee_name(n->call.callee, "every") ||
                   is_callee_name(n->call.callee, "find") ||
                   is_callee_name(n->call.callee, "count") ||
                   is_callee_name(n->call.callee, "sum")) {
            /* Free-function forms of array methods. The pipe operator
               desugars `xs |> reduce(0, fn)` to `reduce(xs, 0, fn)`,
               and at top level there is no `reduce` to call. Route to
               the prelude polyfill so the same pattern works. fold is
               an alias for reduce. */
            const char *cname = n->call.callee->ident.name;
            if (strcmp(cname, "fold") == 0) sb_add(s, "__xs_reduce(");
            else { sb_add(s, "__xs_"); sb_add(s, cname); sb_addc(s, '('); }
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
        /* int() / float() wrap the args in an extra paren that the
           helper unwraps; everything else closes a single paren. */
        if (is_callee_name(n->call.callee, "int") ||
            is_callee_name(n->call.callee, "float")) {
            sb_add(s, "))");
        } else {
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_METHOD_CALL: {
        /* Map XS method names to their JS equivalents. The receiver type
           often isn't statically knowable (string vs array), so polymorphic
           ones (.contains, .reverse, .is_empty, .index_of, .len) route
           through __xs_* runtime helpers. Type-specific ones inline. */
        const char *m = n->method_call.method;
        int nargs = n->method_call.args.len;
        /* super.method(args) -- JS treats `super` as a keyword that
           can only appear in this exact position (not as a bare value
           passed to a helper). Emit directly. */
        if (m && n->method_call.obj &&
            VAL_TAG(n->method_call.obj) == NODE_IDENT &&
            n->method_call.obj->ident.name &&
            strcmp(n->method_call.obj->ident.name, "super") == 0) {
            /* in JS, super.init(args) inside a constructor must be super(args) */
            if (strcmp(m, "init") == 0 || strcmp(m, "new") == 0)
                sb_add(s, "super(");
            else {
                sb_add(s, "super.");
                sb_add(s, m);
                sb_addc(s, '(');
            }
            for (int i = 0; i < nargs; i++) {
                if (i) sb_add(s, ", ");
                emit_expr(s, n->method_call.args.items[i], depth);
            }
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "len") == 0 && nargs == 0) {
            sb_add(s, "__xs_len(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "is_empty") == 0 && nargs == 0) {
            sb_add(s, "__xs_is_empty(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "contains") == 0 && nargs == 1) {
            sb_add(s, "__xs_contains(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "index_of") == 0 && nargs == 1) {
            sb_add(s, "__xs_index_of(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "chars") == 0 && nargs == 0) {
            sb_add(s, "__xs_chars(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "enumerate") == 0 && nargs == 0) {
            sb_add(s, "__xs_enumerate(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "zip") == 0 && nargs == 1) {
            sb_add(s, "__xs_zip(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && (strcmp(m, "flat") == 0 || strcmp(m, "flatten") == 0) && nargs == 0) {
            sb_add(s, "__xs_flatten(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "flat_map") == 0 && nargs == 1) {
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ".flatMap(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "chunks") == 0 && nargs == 1) {
            sb_add(s, "__xs_chunks(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "clamp") == 0 && nargs == 2) {
            sb_add(s, "__xs_clamp(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[1], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "abs") == 0 && nargs == 0) {
            sb_add(s, "Math.abs(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "floor") == 0 && nargs == 0) {
            sb_add(s, "Math.floor(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "ceil") == 0 && nargs == 0) {
            sb_add(s, "Math.ceil(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "round") == 0 && nargs == 0) {
            sb_add(s, "Math.round(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "sqrt") == 0 && nargs == 0) {
            sb_add(s, "Math.sqrt(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "upper") == 0 && nargs == 0) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.upper === 'function') return __o.upper();");
            sb_add(s, " return String(__o).toUpperCase(); })()");
            break;
        }
        if (m && strcmp(m, "lower") == 0 && nargs == 0) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.lower === 'function') return __o.lower();");
            sb_add(s, " return String(__o).toLowerCase(); })()");
            break;
        }
        if (m && strcmp(m, "trim") == 0 && nargs == 0) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.trim === 'function') return __o.trim();");
            sb_add(s, " return String(__o).trim(); })()");
            break;
        }
        if (m && strcmp(m, "starts_with") == 0 && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.starts_with === 'function') return __o.starts_with(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); return String(__o).startsWith(String(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ")); })()");
            break;
        }
        if (m && strcmp(m, "ends_with") == 0 && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.ends_with === 'function') return __o.ends_with(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); return String(__o).endsWith(String(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ")); })()");
            break;
        }
        if (m && strcmp(m, "is_a") == 0 && nargs == 1) {
            sb_add(s, "__xs_is_a(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        /* Misc methods that map to a single JS expression. The JS Array
           prototype already defines .find / .map / .filter / .indexOf, etc.
           XS .replace returns a copy with the FIRST match replaced (matching
           Python's str.replace behaviour); JS String.prototype.replace does
           the same when called with a string needle. */
        if (m && strcmp(m, "split") == 0 && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.split === 'function') return __o.split(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); return String(__o).split(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); })()");
            break;
        }
        if (m && strcmp(m, "replace") == 0 && nargs == 2) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o !== 'string' && __o && typeof __o.replace === 'function') return __o.replace(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[1], depth);
            sb_add(s, "); return String(__o).split(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ").join(");
            emit_expr(s, n->method_call.args.items[1], depth);
            sb_add(s, "); })()");
            break;
        }
        if (m && strcmp(m, "repeat") == 0 && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; const __n = ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "; if (typeof __o === 'string') return __o.repeat(__n < 0 ? 0 : __n); if (Array.isArray(__o)) { const r = []; for (let i = 0; i < __n; i++) for (const x of __o) r.push(x); return r; }");
            sb_add(s, " if (__o && typeof __o.repeat === 'function') return __o.repeat(__n);");
            sb_add(s, " return __o; })()");
            break;
        }
        if (m && strcmp(m, "join") == 0 && nargs == 1) {
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ".join(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "reverse") == 0 && nargs == 0) {
            /* polymorphic: array .reverse() mutates and returns,
               string needs codepoint-aware reversal. Defer to a
               user-defined .reverse() method first. */
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (typeof __o === 'string') return __xs_str_reverse(__o);");
            sb_add(s, " if (Array.isArray(__o)) return [...__o].reverse();");
            sb_add(s, " if (__o && typeof __o.reverse === 'function') return __o.reverse();");
            sb_add(s, " return __o; })()");
            break;
        }
        /* Method calls forwarded as-is to the JS receiver. The argument
           list often differs (e.g. .any -> .some, .all -> .every,
           .find on arrays needs a predicate too). Skip the inline
           rewrite when the receiver is a known module (async / etc.)
           so async.all routes through the module polyfill. */
        int receiver_is_module = 0;
        if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT) {
            const char *rn = n->method_call.obj->ident.name;
            if (rn && (strcmp(rn, "async") == 0 || strcmp(rn, "math") == 0 ||
                       strcmp(rn, "json") == 0 || strcmp(rn, "time") == 0 ||
                       strcmp(rn, "random") == 0 || strcmp(rn, "re") == 0 ||
                       strcmp(rn, "collections") == 0 || strcmp(rn, "string") == 0))
                receiver_is_module = 1;
        }
        if (!receiver_is_module && m && strcmp(m, "any") == 0 && nargs == 1) {
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ".some(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        if (!receiver_is_module && m && strcmp(m, "all") == 0 && nargs == 1) {
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ".every(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        /* XS reduce can accept (fn, init) or (init, fn) -- the runtime
           picks based on which arg is callable. fold is always
           (init, fn). The two map onto JS's .reduce(fn, init) once we
           figure out which slot is the lambda. We inline a callable
           check at runtime so both orderings transpile correctly. */
        if (m && (strcmp(m, "reduce") == 0 || strcmp(m, "fold") == 0) && nargs == 2) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (!Array.isArray(__o) && __o && typeof __o.");
            sb_add(s, m);
            sb_add(s, " === 'function') return __o.");
            sb_add(s, m);
            sb_add(s, "(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[1], depth);
            sb_add(s, "); ");
            sb_add(s, "const __a0 = ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "; const __a1 = ");
            emit_expr(s, n->method_call.args.items[1], depth);
            sb_add(s, "; const __fn = typeof __a0 === 'function' ? __a0 : __a1; "
                      "const __init = typeof __a0 === 'function' ? __a1 : __a0; return __o.reduce(__fn, __init); })()");
            break;
        }
        /* arr.reduce(fn) (no init) -> JS .reduce(fn) */
        if (m && (strcmp(m, "reduce") == 0 || strcmp(m, "fold") == 0) && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (!Array.isArray(__o) && __o && typeof __o.");
            sb_add(s, m);
            sb_add(s, " === 'function') return __o.");
            sb_add(s, m);
            sb_add(s, "(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); return __o.reduce(");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "); })()");
            break;
        }
        /* sort with no args sorts numerically; JS default sort is
           lexicographic on stringified values, so [10, 2] sorts to
           [10, 2]. Force a numeric/string-aware comparator. */
        if (m && strcmp(m, "sort") == 0 && nargs == 0) {
            sb_add(s, "((__a) => { __a.sort((a, b) => __xs_cmp(a, b)); return __a; })(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        if (m && strcmp(m, "sort") == 0 && nargs == 1) {
            sb_add(s, "((__a, __c) => { __a.sort(__c); return __a; })(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_addc(s, ')');
            break;
        }
        /* `arr.keys()` on a Map -> [...m.keys()]; on an array, return
           the index array. Default JS .keys() returns an iterator, not
           an array, which doesn't compare equal to [1,2,3]. Defer to
           a user-defined .keys() if one exists (class instance with a
           method). */
        if (m && strcmp(m, "keys") == 0 && nargs == 0) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (__o instanceof Map) return [...__o.keys()]; if (Array.isArray(__o)) return [...__o.keys()]; ");
            sb_add(s, "if (typeof __o.keys === 'function') return __o.keys(); ");
            sb_add(s, "return Object.keys(__o); })()");
            break;
        }
        if (m && strcmp(m, "values") == 0 && nargs == 0) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; if (__o instanceof Map) return [...__o.values()]; if (Array.isArray(__o)) return [...__o]; ");
            sb_add(s, "if (typeof __o.values === 'function') return __o.values(); ");
            sb_add(s, "return Object.values(__o); })()");
            break;
        }
        if (m && strcmp(m, "get") == 0 && nargs >= 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; const __k = ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "; if (__o instanceof Map) return __o.has(__k) ? __o.get(__k) : ");
            if (nargs >= 2) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "null");
            /* Wrapper objects (Counter / OrderedMap polyfill) expose
               their own .get; defer to it before falling through to
               bare object index. */
            sb_add(s, "; if (typeof __o.get === 'function') return __o.get(__k");
            if (nargs >= 2) { sb_add(s, ", "); emit_expr(s, n->method_call.args.items[1], depth); }
            sb_add(s, ");\n");
            sb_add(s, "        if (Array.isArray(__o)) { const __v = __o[__k]; return __v !== undefined ? __v : ");
            if (nargs >= 2) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "null");
            sb_add(s, "; }");
            sb_add(s, " return __o[__k] !== undefined ? __o[__k] : ");
            if (nargs >= 2) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "null");
            sb_add(s, "; })()");
            break;
        }
        if (m && strcmp(m, "has") == 0 && nargs == 1) {
            sb_add(s, "(() => { const __o = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; const __k = ");
            emit_expr(s, n->method_call.args.items[0], depth);
            sb_add(s, "; if (__o instanceof Map) return __o.has(__k); ");
            sb_add(s, "if (typeof __o.has === 'function') return __o.has(__k); ");
            sb_add(s, "return Object.prototype.hasOwnProperty.call(__o, __k); })()");
            break;
        }
        /* `.to_str()` is the XS spelling. Route through __xs_repr so
           arrays/maps/structs get the same shape the VM emits, not
           Array.prototype.toString's "1,2,3" or Map's "[object Map]". */
        if (m && (strcmp(m, "to_str") == 0 || strcmp(m, "to_string") == 0) && nargs == 0) {
            sb_add(s, "__xs_repr(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        /* `.to_int()` -> Number(...) | 0 truncation */
        if (m && strcmp(m, "to_int") == 0 && nargs == 0) {
            sb_add(s, "Math.trunc(Number(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "))");
            break;
        }
        /* `.to_float()` -> Number(...) */
        if (m && strcmp(m, "to_float") == 0 && nargs == 0) {
            sb_add(s, "Number(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
            break;
        }
        /* Default: dispatch through __xs_call_method so a method call on a
           Map (whose value at the named key is the actual function) and a
           method call on a class instance both reach the right callable.
           Optional-call (?. before the call) bails on null receivers. */
        if (n->method_call.optional) {
            sb_add(s, "(((__o) => __o == null ? undefined : __xs_call_method(__o, \"");
            sb_add(s, n->method_call.method);
            sb_add(s, "\"");
            for (int i = 0; i < n->method_call.args.len; i++) {
                sb_add(s, ", ");
                emit_expr(s, n->method_call.args.items[i], depth);
            }
            sb_add(s, "))(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "))");
        } else {
            sb_add(s, "__xs_call_method(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", \"");
            sb_add(s, n->method_call.method);
            sb_addc(s, '"');
            for (int i = 0; i < n->method_call.args.len; i++) {
                sb_add(s, ", ");
                emit_expr(s, n->method_call.args.items[i], depth);
            }
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_INDEX:
        /* arr[a..b] / arr[a..=b] are range-slice operations, not key
           lookups. Lower them to .slice() directly so the JS Array
           method actually runs instead of __xs_idx receiving the
           materialised range Array (which then index-fails). */
        if (n->index.index && VAL_TAG(n->index.index) == NODE_RANGE) {
            Node *r = n->index.index;
            sb_add(s, "(");
            emit_expr(s, n->index.obj, depth);
            sb_add(s, ").slice(");
            if (r->range.start) emit_expr(s, r->range.start, depth);
            else sb_add(s, "0");
            sb_add(s, ", ");
            if (r->range.end) {
                emit_expr(s, r->range.end, depth);
                if (r->range.inclusive) sb_add(s, " + 1");
            } else {
                sb_add(s, "undefined");
            }
            sb_addc(s, ')');
            break;
        }
        sb_add(s, "__xs_idx(");
        emit_expr(s, n->index.obj, depth);
        sb_add(s, ", ");
        emit_expr(s, n->index.index, depth);
        sb_addc(s, ')');
        break;
    case NODE_FIELD:
        /* Field access has to handle plain JS objects, classes (where the
           field is a real property), and Maps (where the same dot syntax
           in XS retrieves a string key). __xs_field probes both. */
        sb_add(s, "__xs_field(");
        emit_expr(s, n->field.obj, depth);
        sb_printf(s, ", \"%s\"", n->field.name);
        if (n->field.optional) sb_add(s, ", true");
        sb_addc(s, ')');
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
        } else if (tgt && VAL_TAG(tgt) == NODE_FIELD && strcmp(n->assign.op, "=") == 0) {
            /* Field assignment: emit a JS dot-set rather than wrapping
               the LHS in __xs_field, which is a function call and not an
               assignable target. The runtime helper __xs_setfield does
               the same Map/object dispatch the read path does. */
            sb_add(s, "__xs_setfield(");
            emit_expr(s, tgt->field.obj, depth);
            sb_printf(s, ", \"%s\", ", tgt->field.name);
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
        /* Funnel through __xs_range so we can stash the original (start,
         * end, inclusive) on the materialised array. .start()/.end() pull
         * from those instead of items[0]/items[last]; without the marker
         * exclusive ranges' .end() drifts (1..5 ends at 5, not 4). */
        sb_add(s, "__xs_range(");
        emit_expr(s, n->range.start, depth);
        sb_add(s, ", ");
        emit_expr(s, n->range.end, depth);
        sb_printf(s, ", %d)", n->range.inclusive ? 1 : 0);
        break;
    }
    case NODE_LAMBDA: {
        int lam_is_gen = n->lambda.is_generator ||
                          node_has_perform(n->lambda.body);
        if (n->lambda.inferred_pure) sb_add(s, "__xs_mark_pure");
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
        sb_add(s, "__xs_truthy("); emit_expr(s, n->if_expr.cond, depth); sb_addc(s, ')');
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
            sb_add(s, "__xs_truthy("); emit_expr(s, n->if_expr.elif_conds.items[i], depth); sb_addc(s, ')');
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
        /* try as expression -> IIFE. The body's trailing expression
           needs an explicit `return` since emit_block_body only emits
           statements, and a try-expression like `try { 1 } catch ...`
           must return that 1 for the IIFE to produce it. */
        sb_add(s, "(() => { try {\n");
        if (n->try_.body) {
            emit_block_body(s, n->try_.body, depth + 1);
            if (VAL_TAG(n->try_.body) == NODE_BLOCK && n->try_.body->block.expr) {
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
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
            /* The block-trailing expression isn't a statement so
               emit_block_body skips it; without this, finally bodies
               that are a single expression (like `println("done")`)
               came out as empty {}. */
            if (VAL_TAG(n->try_.finally_block) == NODE_BLOCK &&
                n->try_.finally_block->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->try_.finally_block->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
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
           enclosing generator to yield into. We can't go through the
           usual NODE_BLOCK emit because that produces an arrow IIFE and
           `yield` inside an arrow is a SyntaxError -- the body has to
           live directly inside the function*(). For non-block bodies
           we still wrap with the gen-call check so a helper that itself
           performs gets `yield*`'d. */
        sb_add(s, "(() => {\n");
        sb_indent(s, depth + 1);
        Node *body = n->handle.expr;
        if (body && VAL_TAG(body) == NODE_BLOCK) {
            sb_add(s, "const __body = (function*() {\n");
            emit_block_body(s, body, depth + 2);
            if (body->block.expr) {
                sb_indent(s, depth + 2);
                sb_add(s, "return ");
                emit_expr(s, body->block.expr, depth + 2);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth + 1);
            sb_add(s, "});\n");
        } else {
            sb_add(s, "const __body = (function*() { const __r = ");
            emit_expr(s, n->handle.expr, depth + 1);
            sb_add(s, "; if (__r && typeof __r.next === \"function\") return yield* __r; return __r; });\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "const __gen = __body();\n");
        sb_indent(s, depth + 1);
        sb_add(s, "let __result = __gen.next();\n");
        sb_indent(s, depth + 1);
        sb_add(s, "while (!__result.done) {\n");
        sb_indent(s, depth + 2);
        sb_add(s, "const __eff = __result.value;\n");
        sb_indent(s, depth + 2);
        sb_add(s, "let __resumed = false;\n");
        sb_indent(s, depth + 2);
        sb_add(s, "let __arm_value = undefined;\n");
        sb_indent(s, depth + 2);
        sb_add(s, "let __matched = false;\n");
        for (int i = 0; i < n->handle.arms.len; i++) {
            EffectArm *earm = &n->handle.arms.items[i];
            sb_indent(s, depth + 2);
            if (i == 0) sb_add(s, "if (");
            else sb_add(s, "else if (");
            sb_printf(s, "__eff && __eff.__effect === \"%s\" && __eff.__op === \"%s\"",
                      earm->effect_name ? earm->effect_name : "",
                      earm->op_name ? earm->op_name : "");
            sb_add(s, ") {\n");
            sb_indent(s, depth + 3);
            sb_add(s, "__matched = true;\n");
            /* bind handler parameters from __eff.__args */
            for (int p = 0; p < earm->params.len; p++) {
                sb_indent(s, depth + 3);
                sb_add(s, "const ");
                if (earm->params.items[p].name) sb_add(s, earm->params.items[p].name);
                else sb_add(s, "_");
                sb_printf(s, " = __eff.__args[%d];\n", p);
            }
            sb_indent(s, depth + 3);
            sb_add(s, "const __xs_resume = (v) => { __resumed = true; __result = __gen.next(v); };\n");
            if (earm->body && VAL_TAG(earm->body) == NODE_BLOCK) {
                emit_block_body(s, earm->body, depth + 3);
                /* trailing block expr becomes the arm's value */
                if (earm->body->block.expr) {
                    sb_indent(s, depth + 3);
                    sb_add(s, "__arm_value = ");
                    emit_expr(s, earm->body->block.expr, depth + 3);
                    sb_add(s, ";\n");
                }
            } else if (earm->body) {
                sb_indent(s, depth + 3);
                sb_add(s, "__arm_value = ");
                emit_expr(s, earm->body, depth + 3);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
        }
        /* Unmatched effect: terminate to avoid infinite spin. */
        sb_indent(s, depth + 2);
        sb_add(s, "if (!__matched) { return __arm_value; }\n");
        /* Handler that didn't call resume returns its own value. */
        sb_indent(s, depth + 2);
        sb_add(s, "if (!__resumed) { return __arm_value; }\n");
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
        if (n->fn_decl.inferred_pure) sb_add(s, "__xs_mark_pure");
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
        /* while-as-expression -> IIFE (arrow preserves enclosing this) */
        sb_add(s, "(() => { ");
        if (n->while_loop.label) sb_printf(s, "%s: ", n->while_loop.label);
        sb_add(s, "while (");
        sb_add(s, "__xs_truthy("); emit_expr(s, n->while_loop.cond, depth); sb_addc(s, ')');
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
        /* for-as-expression -> IIFE. Use arrow so `this` is preserved
           in class methods and so super.method() can still resolve. */
        sb_add(s, "(() => { ");
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
        /* loop-as-expression -> IIFE (arrow preserves this) */
        sb_add(s, "(() => { ");
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
        /* Tuple patterns must reject plain arrays. __xs_tuple sets the
         * non-enumerable __xs_is_tuple flag; require it here so
         * `match (1,2) { [a,b] => ... }` doesn't fire on a tuple. */
        sb_printf(s, "(Array.isArray(%s) && %s.__xs_is_tuple && %s.length === %d",
                  subject, subject, subject, pat->pat_tuple.elems.len);
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
        /* Plain array pattern - reject tuples (which set __xs_is_tuple). */
        if (pat->pat_slice.rest) {
            sb_printf(s, "(Array.isArray(%s) && !%s.__xs_is_tuple && %s.length >= %d",
                      subject, subject, subject, pat->pat_slice.elems.len);
        } else {
            sb_printf(s, "(Array.isArray(%s) && !%s.__xs_is_tuple && %s.length === %d",
                      subject, subject, subject, pat->pat_slice.elems.len);
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
            const char *fkey = pat->pat_struct.fields.items[i].key;
            snprintf(sub, sizeof sub, "%s.%s", subject, fkey);
            if (pat->pat_struct.fields.items[i].val) {
                emit_pattern_bindings(s, pat->pat_struct.fields.items[i].val, sub, depth);
            } else if (fkey) {
                /* Shorthand: `Point { x, y }` binds x and y directly. */
                sb_indent(s, depth);
                sb_printf(s, "const %s = %s;\n", fkey, sub);
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

/* Mirrors node_has_perform but for await. Used to decide whether the
   assert/assert_eq IIFE wrapper needs to be async + awaited so its
   inner await isn't a SyntaxError under top-level-await context. */
static int node_has_await(Node *n) {
    if (!n) return 0;
    if (VAL_TAG(n) == NODE_AWAIT) return 1;
    switch (VAL_TAG(n)) {
    case NODE_FN_DECL: case NODE_LAMBDA: return 0;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            if (node_has_await(n->block.stmts.items[i])) return 1;
        if (n->block.expr && node_has_await(n->block.expr)) return 1;
        return 0;
    case NODE_EXPR_STMT:  return node_has_await(n->expr_stmt.expr);
    case NODE_RETURN:     return node_has_await(n->ret.value);
    case NODE_LET: case NODE_VAR: return node_has_await(n->let.value);
    case NODE_CONST:      return node_has_await(n->const_.value);
    case NODE_ASSIGN:     return node_has_await(n->assign.value) ||
                                 node_has_await(n->assign.target);
    case NODE_IF:
        if (node_has_await(n->if_expr.cond)) return 1;
        if (node_has_await(n->if_expr.then)) return 1;
        for (int j = 0; j < n->if_expr.elif_conds.len; j++)
            if (node_has_await(n->if_expr.elif_conds.items[j])) return 1;
        for (int j = 0; j < n->if_expr.elif_thens.len; j++)
            if (node_has_await(n->if_expr.elif_thens.items[j])) return 1;
        if (node_has_await(n->if_expr.else_branch)) return 1;
        return 0;
    case NODE_BINOP:      return node_has_await(n->binop.left) ||
                                 node_has_await(n->binop.right);
    case NODE_UNARY:      return node_has_await(n->unary.expr);
    case NODE_CALL:
        if (node_has_await(n->call.callee)) return 1;
        for (int j = 0; j < n->call.args.len; j++)
            if (node_has_await(n->call.args.items[j])) return 1;
        return 0;
    case NODE_METHOD_CALL:
        if (node_has_await(n->method_call.obj)) return 1;
        for (int j = 0; j < n->method_call.args.len; j++)
            if (node_has_await(n->method_call.args.items[j])) return 1;
        return 0;
    default:
        return 0;
    }
}

/* statement emitter */
static void emit_stmt(SB *s, Node *n, int depth) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_LET: {
        /* `let _ = expr` -- discard binding. JS would otherwise reject
           multiple `const _ = ...` declarations in the same block.
           Detection: name is "_", or pattern is a wildcard, or both
           are absent. Anything else is a real binding. */
        int is_wild = 0;
        if (n->let.name && strcmp(n->let.name, "_") == 0) is_wild = 1;
        if (!n->let.name && n->let.pattern &&
            VAL_TAG(n->let.pattern) == NODE_PAT_WILD) is_wild = 1;
        if (!n->let.name && !n->let.pattern) is_wild = 1;
        if (is_wild) {
            sb_indent(s, depth);
            if (n->let.value) emit_expr(s, n->let.value, depth);
            sb_add(s, ";\n");
            break;
        }
    }
        /* let {a, b} = m -- map pattern needs Map.get() lookups because
           the RHS is an XS map (JS Map), not a JS object. */
        if (!n->let.name && n->let.pattern &&
            (VAL_TAG(n->let.pattern) == NODE_PAT_MAP ||
             VAL_TAG(n->let.pattern) == NODE_PAT_STRUCT) && n->let.value) {
            static int pat_uid = 0;
            int mid = pat_uid++;
            sb_indent(s, depth);
            sb_printf(s, "const __pat_%d = ", mid);
            emit_expr(s, n->let.value, depth);
            sb_add(s, ";\n");
            if (VAL_TAG(n->let.pattern) == NODE_PAT_MAP) {
                for (int i = 0; i < n->let.pattern->pat_map.nfields; i++) {
                    const char *k = n->let.pattern->pat_map.keys[i];
                    if (!k) continue;
                    sb_indent(s, depth);
                    sb_printf(s, "let %s = __pat_%d instanceof Map ? __pat_%d.get(\"%s\") : __pat_%d[\"%s\"];\n",
                              k, mid, mid, k, mid, k);
                }
            } else {
                for (int i = 0; i < n->let.pattern->pat_struct.fields.len; i++) {
                    const char *k = n->let.pattern->pat_struct.fields.items[i].key;
                    sb_indent(s, depth);
                    sb_printf(s, "let %s = __pat_%d instanceof Map ? __pat_%d.get(\"%s\") : __pat_%d[\"%s\"];\n",
                              k, mid, mid, k, mid, k);
                }
            }
            break;
        }
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
        int __saved_n_deleted = js_n_deleted_vars;
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
        if (named && n->fn_decl.inferred_pure) {
            sb_indent(s, depth);
            sb_printf(s, "__xs_mark_pure(%s);\n\n", fn_name);
        }
        js_n_deleted_vars = __saved_n_deleted; /* unwind del scope on fn exit */
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
        sb_add(s, "__xs_truthy("); emit_expr(s, n->if_expr.cond, depth); sb_addc(s, ')');
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
            sb_add(s, "__xs_truthy("); emit_expr(s, n->if_expr.elif_conds.items[i], depth); sb_addc(s, ')');
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
        sb_add(s, "__xs_truthy("); emit_expr(s, n->while_loop.cond, depth); sb_addc(s, ')');
        sb_add(s, ") {\n");
        if (n->while_loop.body) {
            emit_block_body(s, n->while_loop.body, depth + 1);
            if (VAL_TAG(n->while_loop.body) == NODE_BLOCK && n->while_loop.body->block.expr) {
                Node *te = n->while_loop.body->block.expr;
                if (te && (VAL_TAG(te) == NODE_IF ||
                           VAL_TAG(te) == NODE_BREAK ||
                           VAL_TAG(te) == NODE_CONTINUE ||
                           VAL_TAG(te) == NODE_WHILE ||
                           VAL_TAG(te) == NODE_FOR ||
                           VAL_TAG(te) == NODE_LOOP ||
                           VAL_TAG(te) == NODE_MATCH)) {
                    emit_stmt(s, te, depth + 1);
                } else {
                    sb_indent(s, depth + 1);
                    emit_expr(s, te, depth + 1);
                    sb_add(s, ";\n");
                }
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
                Node *te = n->for_loop.body->block.expr;
                if (te && (VAL_TAG(te) == NODE_IF ||
                           VAL_TAG(te) == NODE_BREAK ||
                           VAL_TAG(te) == NODE_CONTINUE ||
                           VAL_TAG(te) == NODE_WHILE ||
                           VAL_TAG(te) == NODE_FOR ||
                           VAL_TAG(te) == NODE_LOOP ||
                           VAL_TAG(te) == NODE_MATCH)) {
                    emit_stmt(s, te, depth + 1);
                } else {
                    sb_indent(s, depth + 1);
                    emit_expr(s, te, depth + 1);
                    sb_add(s, ";\n");
                }
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
                /* trailing expression in loop body: emit as stmt so a
                   bare `break` / `continue` / `if cond { break }`
                   doesn't get wrapped in an IIFE -- arrow functions
                   create a new lexical context that JS rejects break
                   from. */
                Node *te = n->loop.body->block.expr;
                if (te && (VAL_TAG(te) == NODE_IF ||
                           VAL_TAG(te) == NODE_BREAK ||
                           VAL_TAG(te) == NODE_CONTINUE ||
                           VAL_TAG(te) == NODE_WHILE ||
                           VAL_TAG(te) == NODE_FOR ||
                           VAL_TAG(te) == NODE_LOOP ||
                           VAL_TAG(te) == NODE_MATCH)) {
                    emit_stmt(s, te, depth + 1);
                } else {
                    sb_indent(s, depth + 1);
                    emit_expr(s, te, depth + 1);
                    sb_add(s, ";\n");
                }
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
            /* The block-trailing expression isn't a statement so
               emit_block_body skips it; without this, finally bodies
               that are a single expression (like `println("done")`)
               came out as empty {}. */
            if (VAL_TAG(n->try_.finally_block) == NODE_BLOCK &&
                n->try_.finally_block->block.expr) {
                sb_indent(s, depth + 1);
                emit_expr(s, n->try_.finally_block->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
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
        /* Tag instances so struct match patterns
           (`Point { x, y }`) can recognise them by name. */
        sb_indent(s, depth + 2);
        sb_printf(s, "this.__type__ = \"%s\";\n", n->struct_decl.name);
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
            /* Tag is the path-qualified form (Enum::Variant) so the
               match-arm tag check (which uses the same path string)
               agrees. Stripping it to the bare variant name made
               match arms silently fail because pat_enum.path carries
               the full path. */
            if (v->fields.len == 0) {
                sb_printf(s, "%s: Object.freeze({tag: \"%s::%s\", data: []})",
                          v->name, n->enum_decl.name, v->name);
            } else {
                sb_printf(s, "%s: (", v->name);
                for (int j = 0; j < v->fields.len; j++) {
                    if (j) sb_add(s, ", ");
                    sb_printf(s, "v%d", j);
                }
                sb_printf(s, ") => Object.freeze({tag: \"%s::%s\", data: [",
                          n->enum_decl.name, v->name);
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
        /* Inherit trait default-method bodies for any method this impl
           didn't override. Without this, calling a default-only method
           on the impl type throws "no method on object". */
        if (n->impl_decl.trait_name) {
            Node *trait = js_find_trait(n->impl_decl.trait_name);
            if (trait) {
                for (int ti = 0; ti < trait->trait_decl.methods.len; ti++) {
                    Node *defm = trait->trait_decl.methods.items[ti];
                    if (!defm || VAL_TAG(defm) != NODE_FN_DECL) continue;
                    if (!defm->fn_decl.body) continue;  /* required, no default */
                    int overridden = 0;
                    for (int j = 0; j < n->impl_decl.members.len; j++) {
                        Node *im = n->impl_decl.members.items[j];
                        if (im && VAL_TAG(im) == NODE_FN_DECL && im->fn_decl.name &&
                            defm->fn_decl.name &&
                            strcmp(im->fn_decl.name, defm->fn_decl.name) == 0) {
                            overridden = 1; break;
                        }
                    }
                    if (overridden) continue;
                    sb_indent(s, depth);
                    sb_printf(s, "%s.prototype.%s = function",
                              n->impl_decl.type_name, defm->fn_decl.name);
                    emit_params_ex(s, &defm->fn_decl.params, 1);
                    sb_add(s, " {\n");
                    int save_cm = in_class_method;
                    in_class_method = 1;
                    if (VAL_TAG(defm->fn_decl.body) == NODE_BLOCK) {
                        emit_block_body(s, defm->fn_decl.body, depth + 1);
                        if (defm->fn_decl.body->block.expr) {
                            sb_indent(s, depth + 1);
                            sb_add(s, "return ");
                            emit_expr(s, defm->fn_decl.body->block.expr, depth + 1);
                            sb_add(s, ";\n");
                        }
                    } else {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, defm->fn_decl.body, depth + 1);
                        sb_add(s, ";\n");
                    }
                    in_class_method = save_cm;
                    sb_indent(s, depth);
                    sb_add(s, "};\n");
                }
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
        /* Record so impls can pick up default bodies for methods they
           don't override. */
        js_register_trait(n);
        break;
    }
    case NODE_TYPE_ALIAS:
        sb_indent(s, depth);
        sb_printf(s, "// type %s = %s\n", n->type_alias.name,
                  n->type_alias.target ? n->type_alias.target : "?");
        break;
    case NODE_IMPORT: {
        sb_indent(s, depth);
        /* stdlib modules don't ship as separate .js files; the runtime
           preamble already binds equivalents (e.g. JSON, Math). Emit a
           single-statement no-op so the bindings stay valid and the
           transpiled file stays runnable. */
        const char *first = (n->import.nparts > 0) ? n->import.path[0] : NULL;
        static const char *stdlib_names[] = {
            "json", "math", "time", "random", "fs", "os", "path",
            "process", "string", "io", "fmt", "log", "csv", "url",
            "toml", "base64", "hash", "uuid", "crypto", "re", "regex",
            "collections", "net", "http", "async", "thread", "buf",
            "encode", "db", "cli", "ffi", "reflect", "gc", "tracing",
            "test", "concurrent", "msgpack", NULL
        };
        int is_stdlib = 0;
        if (first) {
            for (int i = 0; stdlib_names[i]; i++) {
                if (strcmp(first, stdlib_names[i]) == 0) { is_stdlib = 1; break; }
            }
        }
        if (is_stdlib) {
            sb_printf(s, "// import %s (stdlib -- runtime polyfill)\n", first);
            break;
        }
        /* Emit as ES module import for user modules */
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
        /* Class field that lets struct-style match patterns recognise
           instances by name. Auto-runs before the user constructor body. */
        sb_indent(s, depth + 1);
        sb_printf(s, "__type__ = \"%s\";\n", n->class_decl.name);
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
            } else if (m && (VAL_TAG(m) == NODE_LET || VAL_TAG(m) == NODE_VAR)) {
                /* Class field decl: `let n = 0` inside a class body lowers
                   to a public field. JS class fields don't take const/let
                   keywords, just `name = expr;`. */
                sb_indent(s, depth + 1);
                if (m->let.name) sb_add(s, m->let.name);
                else sb_add(s, "_");
                if (m->let.value) {
                    sb_add(s, " = ");
                    emit_expr(s, m->let.value, depth + 1);
                }
                sb_add(s, ";\n");
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
    case NODE_BIND:
        sb_indent(s, depth);
        sb_printf(s, "let %s = ", n->bind_decl.name ? n->bind_decl.name : "_bind");
        emit_expr(s, n->bind_decl.expr, depth);
        sb_add(s, ";\n");
        break;
    case NODE_DEL:
        sb_indent(s, depth);
        if (n->del_.name) {
            sb_printf(s, "%s = __XS_DELETED;\n", n->del_.name);
            js_mark_deleted_var(n->del_.name);
        } else {
            sb_add(s, "; /* del without name */\n");
        }
        break;
    case NODE_USE: {
        if (n->use_.is_plugin) {
            sb_indent(s, depth);
            sb_printf(s, "// use plugin \"%s\" (no JS equivalent)\n",
                      n->use_.path ? n->use_.path : "?");
            break;
        }
        if (!n->use_.path) break;
        char resolved[2048];
        js_resolve_use_path(n->use_.path, resolved, sizeof(resolved));
        Node *modprog = js_load_use_module(resolved);
        if (!modprog) {
            sb_indent(s, depth);
            sb_printf(s, "// use \"%s\": load failed\n", n->use_.path);
            break;
        }
        JsExports exp;
        js_collect_exports(modprog, &exp);
        int is_selective = (n->use_.nnames > 0);
        /* Emit the inlined body + return object once. The header line
           differs depending on whether this is a namespace import
           (`const util = (() => {...})()`) or a selective destructure
           (`const { shout, public_const: pc } = (() => {...})()`). */
        sb_indent(s, depth);
        if (is_selective) {
            sb_add(s, "const { ");
            for (int i = 0; i < n->use_.nnames; i++) {
                if (i) sb_add(s, ", ");
                const char *src = n->use_.names[i];
                const char *dst = (n->use_.name_aliases && n->use_.name_aliases[i])
                                      ? n->use_.name_aliases[i] : src;
                if (src && dst && strcmp(src, dst) != 0)
                    sb_printf(s, "%s: %s", src, dst);
                else if (src)
                    sb_add(s, src);
            }
            sb_add(s, " } = (() => {\n");
        } else {
            char alias[256];
            if (n->use_.alias && n->use_.alias[0]) {
                snprintf(alias, sizeof(alias), "%s", n->use_.alias);
            } else {
                js_derive_use_alias(n->use_.path, alias, sizeof(alias));
            }
            sb_printf(s, "const %s = (() => {\n", alias);
        }
        for (int i = 0; i < modprog->program.stmts.len; i++) {
            Node *st = modprog->program.stmts.items[i];
            if (!st) continue;
            if (VAL_TAG(st) == NODE_EXPORT) continue;
            emit_node(s, st, depth + 1);
        }
        sb_indent(s, depth + 1);
        sb_add(s, "return {");
        if (exp.any) {
            for (int i = 0; i < exp.n; i++) {
                if (i) sb_add(s, ",");
                sb_addc(s, ' ');
                if (exp.aliases[i] && exp.locals[i] &&
                    strcmp(exp.aliases[i], exp.locals[i]) != 0) {
                    sb_printf(s, "%s: %s", exp.aliases[i], exp.locals[i]);
                } else {
                    sb_add(s, exp.locals[i]);
                }
            }
        } else {
            int first = 1;
            for (int i = 0; i < modprog->program.stmts.len; i++) {
                const char *nm = js_stmt_binds_name(modprog->program.stmts.items[i]);
                if (!nm) continue;
                if (!first) sb_add(s, ",");
                sb_addc(s, ' ');
                sb_add(s, nm);
                first = 0;
            }
        }
        sb_add(s, " };\n");
        sb_indent(s, depth);
        sb_add(s, "})();\n");
        break;
    }
    case NODE_EXPORT:
        /* Top-level `export { ... }` in the importer is a publish list,
           not runnable code. The IIFE wrapper for `use` already reads the
           list to build the namespace object; emit nothing here. */
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
    const char *unsupported = find_unsupported_for_js(program);
    if (unsupported) {
        fprintf(stderr, "xs --emit js: %s not supported on this target. "
                "Use --vm or --interp.\n", unsupported);
        return NULL;
    }

    purity_analyze(program);
    js_n_deleted_vars = 0;
    SB s;
    sb_init(&s);

    /* preamble */
    sb_add(&s, "// Generated by xs transpile --target js\n");
    if (filename) sb_printf(&s, "// Source: %s\n", filename);
    sb_add(&s, "const __xs_repr = (v) => {\n");
    sb_add(&s, "    if (v === null || v === undefined) return \"null\";\n");
    sb_add(&s, "    if (typeof v === \"string\") return v;\n");
    sb_add(&s, "    if (typeof v === \"number\" || typeof v === \"bigint\" || typeof v === \"boolean\") return String(v);\n");
    sb_add(&s, "    if (Array.isArray(v)) {\n");
    sb_add(&s, "        const open = v.__xs_is_tuple ? \"(\" : \"[\";\n");
    sb_add(&s, "        const close = v.__xs_is_tuple ? \")\" : \"]\";\n");
    sb_add(&s, "        return open + v.map(__xs_repr).join(\", \") + close;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (v instanceof Map) {\n");
    sb_add(&s, "        const parts = [];\n");
    sb_add(&s, "        for (const [k, val] of v) parts.push(__xs_repr(k) + \": \" + __xs_repr(val));\n");
    sb_add(&s, "        return \"{\" + parts.join(\", \") + \"}\";\n");
    sb_add(&s, "    }\n");
    /* Enum constructors emit { tag: \"Enum::Variant\", data: [...] }.
       The VM/interp render those as Enum::Variant or Enum::Variant(a, b);
       match the same shape so println of an enum value reads naturally
       instead of [object Object]. */
    sb_add(&s, "    if (v && typeof v === 'object' && typeof v.tag === 'string' && Array.isArray(v.data)) {\n");
    sb_add(&s, "        if (v.data.length === 0) return v.tag;\n");
    sb_add(&s, "        return v.tag + \"(\" + v.data.map(__xs_repr).join(\", \") + \")\";\n");
    sb_add(&s, "    }\n");
    /* Plain objects (class instances, struct literals) -> {k: v, ...}.
       Struct values get the type name prefix so the JS output matches
       the VM's `Point { x: 3, y: 4 }` repr. */
    sb_add(&s, "    if (v && typeof v === 'object') {\n");
    sb_add(&s, "        const ctor = v.constructor && v.constructor.name;\n");
    /* Collection wrappers (Set, Counter, Deque, Stack, OrderedMap, PriorityQueue)
       expose method properties on the same object as the data. Filter out
       functions so the repr shows only the data shape. */
    sb_add(&s, "        const keys = Object.keys(v).filter(k => typeof v[k] !== 'function');\n");
    sb_add(&s, "        if (keys.length > 0) {\n");
    sb_add(&s, "            const parts = keys.map(k => k + \": \" + __xs_repr(v[k]));\n");
    sb_add(&s, "            const body = \"{ \" + parts.join(\", \") + \" }\";\n");
    sb_add(&s, "            return ctor && ctor !== 'Object' ? ctor + \" \" + body : \"{\" + parts.join(\", \") + \"}\";\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "        if (ctor && ctor !== 'Object') return ctor + \" {}\";\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return String(v);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_print = (...args) => console.log(args.map(__xs_repr).join(\" \"));\n");
    sb_add(&s, "const __xs_write = (...args) => {\n");
    sb_add(&s, "    const s = args.map(__xs_repr).join(\" \");\n");
    sb_add(&s, "    if (typeof process !== 'undefined') process.stdout.write(s);\n");
    sb_add(&s, "    else console.log(s);\n");
    sb_add(&s, "};\n");
    /* Builtins also need bare-reference bindings (`let g = println`). The
     * direct-call path lowers to __xs_print; this aliases the bare name. */
    sb_add(&s, "const println = __xs_print;\n");
    sb_add(&s, "const print   = __xs_write;\n");
    sb_add(&s, "const __xs_add = (a, b) => {\n");
    sb_add(&s, "    if (Array.isArray(a) && Array.isArray(b)) return a.concat(b);\n");
    sb_add(&s, "    if (typeof a === \"string\" || typeof b === \"string\") return __xs_repr(a) + __xs_repr(b);\n");
    sb_add(&s, "    if (typeof a === \"bigint\" || typeof b === \"bigint\") {\n");
    sb_add(&s, "        return BigInt(typeof a === \"bigint\" ? a : Math.trunc(a)) +\n");
    sb_add(&s, "               BigInt(typeof b === \"bigint\" ? b : Math.trunc(b));\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return a + b;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_arith = (op, a, b) => {\n");
    /* string * int repeats (matches Python / XS semantics). */
    sb_add(&s, "    if (op === '*' && typeof a === 'string' && typeof b === 'number') return a.repeat(b < 0 ? 0 : b);\n");
    sb_add(&s, "    if (op === '*' && typeof a === 'number' && typeof b === 'string') return b.repeat(a < 0 ? 0 : a);\n");
    sb_add(&s, "    if (op === '*' && Array.isArray(a) && typeof b === 'number') {\n");
    sb_add(&s, "        const r = []; for (let i = 0; i < b; i++) for (const x of a) r.push(x); return r;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof a === \"bigint\" || typeof b === \"bigint\") {\n");
    sb_add(&s, "        const ba = typeof a === \"bigint\" ? a : BigInt(Math.trunc(a));\n");
    sb_add(&s, "        const bb = typeof b === \"bigint\" ? b : BigInt(Math.trunc(b));\n");
    sb_add(&s, "        switch (op) {\n");
    sb_add(&s, "            case '*': return ba * bb;\n");
    sb_add(&s, "            case '-': return ba - bb;\n");
    sb_add(&s, "            case '%': return ba % bb;\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (op === '*') {\n");
    sb_add(&s, "        if (Number.isInteger(a) && Number.isInteger(b)) {\n");
    sb_add(&s, "            const r = a * b;\n");
    sb_add(&s, "            if (r > Number.MAX_SAFE_INTEGER || r < -Number.MAX_SAFE_INTEGER)\n");
    sb_add(&s, "                return BigInt(a) * BigInt(b);\n");
    sb_add(&s, "            return r;\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "        return a * b;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (op === '-' || op === '%') {\n");
    sb_add(&s, "        if (typeof a !== 'number' || typeof b !== 'number')\n");
    sb_add(&s, "            throw new Error('type error: ' + (typeof a) + ' ' + op + ' ' + (typeof b));\n");
    sb_add(&s, "        return op === '-' ? a - b : a % b;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return a + b;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_pow = (a, b) => {\n");
    sb_add(&s, "    if (typeof a === \"bigint\" || typeof b === \"bigint\") {\n");
    sb_add(&s, "        return (typeof a === \"bigint\" ? a : BigInt(Math.trunc(a))) **\n");
    sb_add(&s, "               (typeof b === \"bigint\" ? b : BigInt(Math.trunc(b)));\n");
    sb_add(&s, "    }\n");
    /* Promote to BigInt when both ints and the result overflows
       Number.MAX_SAFE_INTEGER. Tests `huge = 10 ** 30` and
       `prod = huge * huge`. */
    sb_add(&s, "    if (Number.isInteger(a) && Number.isInteger(b) && b >= 0) {\n");
    sb_add(&s, "        const r = Math.pow(a, b);\n");
    sb_add(&s, "        if (r > Number.MAX_SAFE_INTEGER || !Number.isFinite(r))\n");
    sb_add(&s, "            return BigInt(a) ** BigInt(b);\n");
    sb_add(&s, "        return r;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return a ** b;\n");
    sb_add(&s, "};\n");
    /* Errors thrown from the runtime are structured maps so XS-side
       try/catch arms can pattern-match `e.kind == "division by zero"`,
       which is what the interp throws. */
    sb_add(&s, "const __xs_err = (kind, msg) => { const m = new Map(); m.set('kind', kind); m.set('message', msg || kind); return m; };\n");
    sb_add(&s, "const __xs_div = (a, b) => {\n");
    sb_add(&s, "    if (typeof b === 'bigint') {\n");
    sb_add(&s, "        if (b === 0n) throw __xs_err('division by zero', 'division by zero');\n");
    sb_add(&s, "        const ba = typeof a === 'bigint' ? a : BigInt(Math.trunc(a));\n");
    sb_add(&s, "        return ba / b;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (b === 0) {\n");
    sb_add(&s, "        if (Number.isInteger(a) && Number.isInteger(b)) throw __xs_err('division by zero', 'division by zero');\n");
    sb_add(&s, "        return a / b;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (Number.isInteger(a) && Number.isInteger(b)) return Math.trunc(a / b);\n");
    sb_add(&s, "    return a / b;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_idx = (o, i) => {\n");
    sb_add(&s, "    if (o == null) throw __xs_err('type error', 'cannot index null');\n");
    sb_add(&s, "    if (typeof o === 'number' || typeof o === 'bigint' || typeof o === 'boolean')\n");
    sb_add(&s, "        throw __xs_err('type error', 'cannot index ' + typeof o);\n");
    sb_add(&s, "    if ((Array.isArray(o) || typeof o === \"string\") && typeof i === \"number\" && i < 0) i = o.length + i;\n");
    sb_add(&s, "    if (o instanceof Map) { const r = o.get(i); return r === undefined ? null : r; }\n");
    sb_add(&s, "    const r = o[i]; return r === undefined ? null : r;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_setidx = (o, i, v) => {\n");
    sb_add(&s, "    if (Array.isArray(o) && typeof i === \"number\" && i < 0) i = o.length + i;\n");
    sb_add(&s, "    if (o instanceof Map) { o.set(i, v); return v; }\n");
    sb_add(&s, "    o[i] = v;\n");
    sb_add(&s, "    return v;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_cmp = (a, b) => {\n");
    sb_add(&s, "    if (typeof a === 'bigint' || typeof b === 'bigint') {\n");
    sb_add(&s, "        const ba = typeof a === 'bigint' ? a : BigInt(Math.trunc(a));\n");
    sb_add(&s, "        const bb = typeof b === 'bigint' ? b : BigInt(Math.trunc(b));\n");
    sb_add(&s, "        return ba < bb ? -1 : ba > bb ? 1 : 0;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return a < b ? -1 : a > b ? 1 : 0;\n");
    sb_add(&s, "};\n");
    /* Codepoint-aware len. JS string .length counts UTF-16 units, so
       "hi 😀".length is 5; the interp treats strings as codepoint
       sequences. Spread iteration walks codepoints and matches. */
    sb_add(&s, "const __xs_len = (x) => {\n");
    sb_add(&s, "    if (x === null || x === undefined) return 0;\n");
    sb_add(&s, "    if (Array.isArray(x)) return x.length;\n");
    sb_add(&s, "    if (x instanceof Map) return x.size;\n");
    sb_add(&s, "    if (x instanceof Set) return x.size;\n");
    sb_add(&s, "    if (typeof x === 'string') { let n = 0; for (const _ of x) n++; return n; }\n");
    /* Typed wrappers (collections.Set / Counter / Deque / Stack /
       OrderedMap / PriorityQueue) expose .len() as a method. Honour
       it before falling through to the bare length/size shapes. */
    sb_add(&s, "    if (x && typeof x.len === 'function') return x.len();\n");
    sb_add(&s, "    if (typeof x.length === 'number') return x.length;\n");
    sb_add(&s, "    if (typeof x.size === 'number') return x.size;\n");
    sb_add(&s, "    return 0;\n");
    sb_add(&s, "};\n");
    /* String helpers covering XS method names that don't have a 1:1 JS
       equivalent. Dispatched from emit NODE_METHOD_CALL when the receiver
       can't be statically narrowed. */
    /* XS truthiness: 0 / "" / [] / {} / null are all falsy. JS treats
       arrays / objects / "" differently, so route through this helper
       whenever the runtime needs the XS rules. */
    sb_add(&s, "const __xs_truthy = (v) => {\n");
    sb_add(&s, "    if (v === null || v === undefined || v === false) return false;\n");
    sb_add(&s, "    if (v === 0 || v === 0n) return false;\n");
    sb_add(&s, "    if (typeof v === 'string') return v.length > 0;\n");
    sb_add(&s, "    if (Array.isArray(v)) return v.length > 0;\n");
    sb_add(&s, "    if (v instanceof Map) return v.size > 0;\n");
    sb_add(&s, "    if (v instanceof Set) return v.size > 0;\n");
    sb_add(&s, "    if (typeof v === 'object') {\n");
    sb_add(&s, "        if (typeof v.tag === 'string') return true;\n");
    sb_add(&s, "        for (const _k in v) return true; return false;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return true;\n");
    sb_add(&s, "};\n");
    /* type(v) -- XS reports semantic names (int/float/str/array/map/
       null/bool), not JS typeof results. */
    sb_add(&s, "const __xs_type = (v) => {\n");
    sb_add(&s, "    if (v === null || v === undefined) return 'null';\n");
    sb_add(&s, "    if (typeof v === 'boolean') return 'bool';\n");
    sb_add(&s, "    if (typeof v === 'string') return 'str';\n");
    sb_add(&s, "    if (typeof v === 'bigint') return 'int';\n");
    sb_add(&s, "    if (typeof v === 'number') return Number.isInteger(v) ? 'int' : 'float';\n");
    sb_add(&s, "    if (Array.isArray(v)) return 'array';\n");
    sb_add(&s, "    if (v instanceof Map) return 'map';\n");
    sb_add(&s, "    if (v && typeof v === 'object' && typeof v.tag === 'string') return 'enum';\n");
    sb_add(&s, "    if (v && typeof v === 'object') {\n");
    sb_add(&s, "        const ctor = v.constructor && v.constructor.name;\n");
    sb_add(&s, "        return ctor && ctor !== 'Object' ? ctor : 'map';\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof v === 'function') return 'fn';\n");
    sb_add(&s, "    return 'unknown';\n");
    sb_add(&s, "};\n");
    /* purity introspection. Pure fns are tagged with __xs_pure = true
       at construction time so __pure?(f) is a property check. */
    sb_add(&s, "const __xs_mark_pure = (f) => { if (typeof f === 'function') f.__xs_pure = true; return f; };\n");
    sb_add(&s, "const __xs_is_pure = (f) => typeof f === 'function' && f.__xs_pure === true;\n");
    /* ++ concat: arrays -> concat array, strings -> string, plain
       maps -> merged map. Anything else -> stringified concat. */
    sb_add(&s, "const __xs_concat = (a, b) => {\n");
    sb_add(&s, "    if (Array.isArray(a) && Array.isArray(b)) return a.concat(b);\n");
    sb_add(&s, "    if (typeof a === 'string' || typeof b === 'string') return __xs_repr(a) + __xs_repr(b);\n");
    sb_add(&s, "    if (a instanceof Map && b instanceof Map) {\n");
    sb_add(&s, "        const out = new Map(a); for (const [k, v] of b) out.set(k, v); return out;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (a && b && typeof a === 'object' && typeof b === 'object' && \n");
    sb_add(&s, "        !Array.isArray(a) && !Array.isArray(b) && !a.tag && !b.tag) {\n");
    sb_add(&s, "        return Object.assign({}, a, b);\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return __xs_repr(a) + __xs_repr(b);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_chars = (s) => [...String(s)];\n");
    sb_add(&s, "const __xs_str_reverse = (s) => [...String(s)].reverse().join('');\n");
    /* Free-function forms of arr.reduce / map / filter / each / fold /
       some / every / find. The pipe operator desugars `xs |> reduce(0,
       fn)` to `reduce(xs, 0, fn)`, and a top-level `reduce` is not in
       scope by default, so without these helpers the JS errors with
       ReferenceError at the first pipe-to-reduce site. The helpers
       accept the array as the first arg and forward to the
       corresponding Array prototype method, picking the (init, fn) vs
       (fn, init) order at runtime so users don't have to remember. */
    sb_add(&s, "const __xs_reduce = (xs, a, b) => {\n");
    sb_add(&s, "    if (!Array.isArray(xs)) throw new Error('reduce: expected array');\n");
    sb_add(&s, "    if (b === undefined) return xs.reduce((acc, x) => a(acc, x));\n");
    sb_add(&s, "    const fn = typeof a === 'function' ? a : b;\n");
    sb_add(&s, "    const init = typeof a === 'function' ? b : a;\n");
    sb_add(&s, "    return xs.reduce((acc, x) => fn(acc, x), init);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_map = (xs, fn) => Array.isArray(xs) ? xs.map(x => fn(x)) : Array.from(xs, x => fn(x));\n");
    sb_add(&s, "const __xs_filter = (xs, fn) => Array.isArray(xs) ? xs.filter(x => fn(x)) : Array.from(xs).filter(x => fn(x));\n");
    sb_add(&s, "const __xs_each = (xs, fn) => { for (const x of xs) fn(x); return null; };\n");
    sb_add(&s, "const __xs_some = (xs, fn) => Array.isArray(xs) ? xs.some(x => fn(x)) : false;\n");
    sb_add(&s, "const __xs_every = (xs, fn) => Array.isArray(xs) ? xs.every(x => fn(x)) : false;\n");
    sb_add(&s, "const __xs_find = (xs, fn) => Array.isArray(xs) ? xs.find(x => fn(x)) : undefined;\n");
    sb_add(&s, "const __xs_count = (xs, fn) => {\n");
    sb_add(&s, "    if (typeof fn === 'function') {\n");
    sb_add(&s, "        let n = 0; for (const x of xs) if (fn(x)) n++; return n;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return Array.isArray(xs) || typeof xs === 'string' ? xs.length : 0;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_sum = (xs) => Array.isArray(xs) ? xs.reduce((a, b) => __xs_add(a, b), 0) : 0;\n");
    sb_add(&s, "const __xs_contains = (a, b) => {\n");
    sb_add(&s, "    if (typeof a === 'string') return a.includes(String(b));\n");
    /* Predicate form: arr.contains(|x| ...) -> any-truthy probe.
       Equality form falls through to __xs_eq. */
    sb_add(&s, "    if (Array.isArray(a)) {\n");
    sb_add(&s, "        if (typeof b === 'function') return a.some(x => b(x));\n");
    sb_add(&s, "        return a.some(x => __xs_eq(x, b));\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (a instanceof Map) return a.has(b);\n");
    sb_add(&s, "    if (a instanceof Set) return a.has(b);\n");
    /* Typed wrappers expose .contains() as a method; defer to it. */
    sb_add(&s, "    if (a && typeof a.contains === 'function') return a.contains(b);\n");
    sb_add(&s, "    if (a && typeof a.has === 'function') return a.has(b);\n");
    sb_add(&s, "    return false;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_index_of = (a, x) => {\n");
    /* Mirror the VM's index_of: a callable arg means \"first index
       where pred(item) is truthy\", anything else means equality. */
    sb_add(&s, "    if (Array.isArray(a)) {\n");
    sb_add(&s, "        if (typeof x === 'function') { for (let i = 0; i < a.length; i++) if (x(a[i])) return i; return -1; }\n");
    sb_add(&s, "        for (let i = 0; i < a.length; i++) if (__xs_eq(a[i], x)) return i; return -1;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof a === 'string') return a.indexOf(String(x));\n");
    sb_add(&s, "    return -1;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_is_empty = (x) => __xs_len(x) === 0;\n");
    /* x.is_a(\"type-name\") - the XS interp accepts both lowercase and
       capitalised forms (str / String, int / Int). Match its behaviour. */
    sb_add(&s, "const __xs_is_a = (v, t) => {\n");
    sb_add(&s, "    const tl = String(t).toLowerCase();\n");
    sb_add(&s, "    if (v === null || v === undefined) return tl === 'null';\n");
    sb_add(&s, "    if (typeof v === 'string') return tl === 'str' || tl === 'string';\n");
    sb_add(&s, "    if (typeof v === 'boolean') return tl === 'bool' || tl === 'boolean';\n");
    sb_add(&s, "    if (typeof v === 'bigint') return tl === 'bigint' || tl === 'int';\n");
    sb_add(&s, "    if (typeof v === 'number') {\n");
    sb_add(&s, "        if (Number.isInteger(v)) return tl === 'int' || tl === 'number' || tl === 'float';\n");
    sb_add(&s, "        return tl === 'float' || tl === 'number';\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (Array.isArray(v)) return tl === 'array' || tl === 'list';\n");
    sb_add(&s, "    if (v instanceof Map) return tl === 'map';\n");
    sb_add(&s, "    if (v instanceof Set) return tl === 'set';\n");
    sb_add(&s, "    if (typeof v === 'function') return tl === 'fn' || tl === 'function';\n");
    sb_add(&s, "    if (typeof v === 'object') {\n");
    sb_add(&s, "        const ctor = v.constructor && v.constructor.name;\n");
    sb_add(&s, "        return ctor && (ctor === t || ctor.toLowerCase() === tl);\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return false;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_clamp = (x, lo, hi) => x < lo ? lo : x > hi ? hi : x;\n");
    sb_add(&s, "const __xs_chunks = (a, n) => {\n");
    sb_add(&s, "    const r = []; for (let i = 0; i < a.length; i += n) r.push(a.slice(i, i + n)); return r;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_enumerate = (a) => {\n");
    sb_add(&s, "    if (typeof a === 'string') { const r = []; let i = 0; for (const c of a) r.push([i++, c]); return r; }\n");
    sb_add(&s, "    return Array.from(a, (v, i) => [i, v]);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_zip = (a, b) => {\n");
    sb_add(&s, "    const n = Math.min(a.length, b.length); const r = [];\n");
    sb_add(&s, "    for (let i = 0; i < n; i++) r.push(__xs_tuple([a[i], b[i]])); return r;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_flatten = (a) => {\n");
    sb_add(&s, "    if (!Array.isArray(a)) return a; const r = [];\n");
    sb_add(&s, "    for (const x of a) { if (Array.isArray(x)) for (const y of x) r.push(y); else r.push(x); }\n");
    sb_add(&s, "    return r;\n");
    sb_add(&s, "};\n");
    /* Field access. The interp looks up the name in instance fields,
       map entries, struct/enum payload, etc. JS dot-access works for
       classes / objects / actors, but not for Map entries -- a bareword
       key on a Map literal returns undefined under .foo. Probe both. */
    sb_add(&s, "const __xs_field = (o, k, optional) => {\n");
    sb_add(&s, "    if (o === null || o === undefined) return optional ? undefined : o;\n");
    sb_add(&s, "    let v;\n");
    sb_add(&s, "    if (o instanceof Map) {\n");
    sb_add(&s, "        v = o.has(k) ? o.get(k) : o[k];\n");
    sb_add(&s, "    } else {\n");
    sb_add(&s, "        v = o[k];\n");
    sb_add(&s, "    }\n");
    /* Normalise to null. XS programs treat a missing field as null, so
       leaking JS's `undefined` makes (m.k == null) false even though XS
       semantics say otherwise. */
    sb_add(&s, "    return v === undefined ? null : v;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_setfield = (o, k, v) => {\n");
    sb_add(&s, "    if (o instanceof Map) { o.set(k, v); return v; }\n");
    sb_add(&s, "    o[k] = v; return v;\n");
    sb_add(&s, "};\n");
    /* Method dispatch. JS dot-call binds `this` to the receiver; we keep
       that semantics for class instances and plain objects, but Maps don't
       expose their entries as properties, so a bareword-keyed lambda would
       resolve to undefined under .method(). The helper picks the right
       lookup site for each shape. */
    sb_add(&s, "const __xs_call_method = (o, m, ...args) => {\n");
    sb_add(&s, "    if (o == null) throw new TypeError(\"method '\" + m + \"' on null\");\n");
    sb_add(&s, "    if (o instanceof Map) {\n");
    sb_add(&s, "        if (o.has(m)) { const f = o.get(m); if (typeof f === 'function') return f(...args); }\n");
    sb_add(&s, "        if (typeof o[m] === 'function') return o[m](...args);\n");
    /* XS map methods that JS Map doesn't expose by the same name. We
       inline the common ones so a transpiled program doesn't crash on
       m.merge / m.keys / m.has() etc. */
    sb_add(&s, "        switch (m) {\n");
    sb_add(&s, "            case 'keys':    return [...o.keys()];\n");
    sb_add(&s, "            case 'values':  return [...o.values()];\n");
    sb_add(&s, "            case 'entries': case 'items': return [...o.entries()];\n");
    sb_add(&s, "            case 'has': case 'contains': case 'contains_key': case 'has_key':\n");
    sb_add(&s, "                return o.has(args[0]);\n");
    sb_add(&s, "            case 'get': case 'get_or': {\n");
    sb_add(&s, "                if (o.has(args[0])) return o.get(args[0]);\n");
    sb_add(&s, "                return args.length >= 2 ? args[1] : null;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'set': case 'put': o.set(args[0], args[1]); return o;\n");
    sb_add(&s, "            case 'delete': case 'remove': { const ok = o.has(args[0]); o.delete(args[0]); return ok; }\n");
    sb_add(&s, "            case 'merge': case 'update': {\n");
    sb_add(&s, "                const out = new Map(o);\n");
    sb_add(&s, "                const other = args[0];\n");
    sb_add(&s, "                if (other instanceof Map) for (const [k, v] of other) out.set(k, v);\n");
    sb_add(&s, "                else if (other && typeof other === 'object') for (const k of Object.keys(other)) out.set(k, other[k]);\n");
    sb_add(&s, "                return out;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'len': case 'size': case 'count': return o.size;\n");
    sb_add(&s, "            case 'is_empty': return o.size === 0;\n");
    sb_add(&s, "            case 'clear': o.clear(); return o;\n");
    sb_add(&s, "            case 'clone': case 'copy': return new Map(o);\n");
    sb_add(&s, "            case 'map': {\n");
    sb_add(&s, "                const out = new Map();\n");
    sb_add(&s, "                for (const [k, v] of o) out.set(k, args[0](k, v));\n");
    sb_add(&s, "                return out;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'filter': {\n");
    sb_add(&s, "                const out = new Map();\n");
    sb_add(&s, "                for (const [k, v] of o) if (args[0](k, v)) out.set(k, v);\n");
    sb_add(&s, "                return out;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'for_each': for (const [k, v] of o) args[0](k, v); return null;\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "        throw new TypeError(\"no method '\" + m + \"' on map\");\n");
    sb_add(&s, "    }\n");
    /* String methods that XS exposes but JS doesn't ship under the
       same name. JS String already has slice/split/trim/repeat/etc.
       so the typical-case dispatch below catches those; this branch
       fills the gaps. */
    /* Array methods that XS exposes but JS Arrays don't expose by
       the same name, plus the shapes that take both a value and a
       predicate (find_index / contains / count) and need to switch
       on the arg's callability. */
    sb_add(&s, "    if (Array.isArray(o)) {\n");
    sb_add(&s, "        switch (m) {\n");
    sb_add(&s, "            case 'len': case 'size': return o.length;\n");
    sb_add(&s, "            case 'is_empty': return o.length === 0;\n");
    /* Range methods - .start / .end pull from the metadata stashed by
     * __xs_range; for non-range arrays they collapse to first / last,
     * which keeps `[1,2,3].start()` reasonable too. */
    sb_add(&s, "            case 'start': return o.__xs_range_start !== undefined ? o.__xs_range_start : (o[0] !== undefined ? o[0] : null);\n");
    sb_add(&s, "            case 'end':   return o.__xs_range_end !== undefined ? o.__xs_range_end : (o.length ? o[o.length-1] : null);\n");
    sb_add(&s, "            case 'to_array': case 'toArray': case 'collect': return [...o];\n");
    sb_add(&s, "            case 'first': case 'head': return o[0] !== undefined ? o[0] : null;\n");
    sb_add(&s, "            case 'last': return o.length ? o[o.length-1] : null;\n");
    sb_add(&s, "            case 'tail': return o.slice(1);\n");
    sb_add(&s, "            case 'init': return o.slice(0, -1);\n");
    sb_add(&s, "            case 'find_index': case 'index_of': {\n");
    sb_add(&s, "                if (typeof args[0] === 'function') {\n");
    sb_add(&s, "                    for (let i = 0; i < o.length; i++) if (args[0](o[i])) return i;\n");
    sb_add(&s, "                    return -1;\n");
    sb_add(&s, "                }\n");
    sb_add(&s, "                for (let i = 0; i < o.length; i++) if (__xs_eq(o[i], args[0])) return i;\n");
    sb_add(&s, "                return -1;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'contains': case 'includes':\n");
    sb_add(&s, "                if (typeof args[0] === 'function') return o.some(x => args[0](x));\n");
    sb_add(&s, "                return o.some(x => __xs_eq(x, args[0]));\n");
    sb_add(&s, "            case 'count': {\n");
    sb_add(&s, "                if (args.length === 0) return o.length;\n");
    sb_add(&s, "                if (typeof args[0] === 'function') { let c = 0; for (const x of o) if (args[0](x)) c++; return c; }\n");
    sb_add(&s, "                let c = 0; for (const x of o) if (__xs_eq(x, args[0])) c++; return c;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'sum': { let t = 0; for (const x of o) t = __xs_add(t, x); return t; }\n");
    sb_add(&s, "            case 'avg': { if (!o.length) return 0; let t = 0; for (const x of o) t += Number(x); return t / o.length; }\n");
    sb_add(&s, "            case 'min': return o.length ? Math.min(...o.map(Number)) : null;\n");
    sb_add(&s, "            case 'max': return o.length ? Math.max(...o.map(Number)) : null;\n");
    sb_add(&s, "            case 'product': { let t = 1; for (const x of o) t *= Number(x); return t; }\n");
    sb_add(&s, "            case 'reverse': case 'reversed': return [...o].reverse();\n");
    sb_add(&s, "            case 'unique': { const seen = new Map(); const r = []; for (const x of o) { const k = __xs_repr(x); if (!seen.has(k)) { seen.set(k, true); r.push(x); } } return r; }\n");
    sb_add(&s, "            case 'sorted': return [...o].sort((a, b) => __xs_cmp(a, b));\n");
    sb_add(&s, "            case 'sort_by': return [...o].sort((a, b) => __xs_cmp(args[0](a), args[0](b)));\n");
    sb_add(&s, "            case 'take': return o.slice(0, Math.max(0, args[0]));\n");
    sb_add(&s, "            case 'drop': return o.slice(Math.max(0, args[0]));\n");
    sb_add(&s, "            case 'take_while': { const r = []; for (const x of o) { if (!args[0](x)) break; r.push(x); } return r; }\n");
    sb_add(&s, "            case 'drop_while': { let i = 0; while (i < o.length && args[0](o[i])) i++; return o.slice(i); }\n");
    sb_add(&s, "            case 'group_by': { const out = new Map(); for (const x of o) { const k = args[0](x); const arr = out.get(k) || []; arr.push(x); out.set(k, arr); } return out; }\n");
    sb_add(&s, "            case 'partition': { const t = [], f = []; for (const x of o) (args[0](x) ? t : f).push(x); return [t, f]; }\n");
    sb_add(&s, "            case 'for_each': case 'each': for (const x of o) args[0](x); return null;\n");
    sb_add(&s, "            case 'iter': return o.values();\n");
    sb_add(&s, "            case 'enumerate': return o.map((v, i) => __xs_tuple([i, v]));\n");
    sb_add(&s, "            case 'zip': { const n = Math.min(o.length, args[0].length); const r = []; for (let i = 0; i < n; i++) r.push(__xs_tuple([o[i], args[0][i]])); return r; }\n");
    sb_add(&s, "            case 'chunks': { const n = args[0]; const r = []; for (let i = 0; i < o.length; i += n) r.push(o.slice(i, i+n)); return r; }\n");
    sb_add(&s, "            case 'windows': { const n = args[0]; const r = []; for (let i = 0; i + n <= o.length; i++) r.push(o.slice(i, i+n)); return r; }\n");
    sb_add(&s, "            case 'step': case 'stride': { const n = args[0]; const r = []; for (let i = 0; i < o.length; i += n) r.push(o[i]); return r; }\n");
    sb_add(&s, "            case 'intersperse': { const r = []; for (let i = 0; i < o.length; i++) { if (i) r.push(args[0]); r.push(o[i]); } return r; }\n");
    sb_add(&s, "            case 'flat': case 'flatten': return __xs_flatten(o);\n");
    sb_add(&s, "            case 'flat_map': return o.flatMap(args[0]);\n");
    sb_add(&s, "            case 'any': return o.some(x => args[0](x));\n");
    sb_add(&s, "            case 'all': return o.every(x => args[0](x));\n");
    sb_add(&s, "            case 'find': return o.find(x => args[0](x)) ?? null;\n");
    sb_add(&s, "            case 'fold': { let acc = args[0]; for (const x of o) acc = args[1](acc, x); return acc; }\n");
    sb_add(&s, "            case 'reduce': {\n");
    sb_add(&s, "                if (typeof args[0] === 'function') return args.length >= 2 ? o.reduce(args[0], args[1]) : o.reduce(args[0]);\n");
    sb_add(&s, "                return o.reduce(args[1], args[0]);\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'scan': { const acc = [args[0]]; for (const x of o) acc.push(args[1](acc[acc.length-1], x)); return acc; }\n");
    sb_add(&s, "            case 'join': return o.join(String(args[0] ?? ''));\n");
    sb_add(&s, "            case 'concat': case 'extend': return o.concat(...args);\n");
    sb_add(&s, "            case 'push': { o.push(...args); return o; }\n");
    sb_add(&s, "            case 'pop': return o.pop();\n");
    sb_add(&s, "            case 'shift': return o.shift();\n");
    sb_add(&s, "            case 'unshift': { o.unshift(...args); return o; }\n");
    sb_add(&s, "            case 'slice': return o.slice(args[0], args[1]);\n");
    sb_add(&s, "            case 'get': return o[args[0]] !== undefined ? o[args[0]] : (args.length >= 2 ? args[1] : null);\n");
    sb_add(&s, "            case 'to_array': return [...o];\n");
    sb_add(&s, "            case 'clone': case 'copy': return [...o];\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof o === 'string') {\n");
    sb_add(&s, "        switch (m) {\n");
    sb_add(&s, "            case 'len': case 'length': case 'size': { let n=0; for (const _ of o) n++; return n; }\n");
    sb_add(&s, "            case 'is_empty': return o.length === 0;\n");
    sb_add(&s, "            case 'parse_int': case 'to_int': case 'as_int': {\n");
    sb_add(&s, "                const radix = (args.length >= 1 && typeof args[0] === 'number') ? args[0] : 10;\n");
    sb_add(&s, "                const v = parseInt(o, radix);\n");
    sb_add(&s, "                return Number.isNaN(v) ? 0 : v;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'parse_float': case 'to_float': case 'as_float': {\n");
    sb_add(&s, "                const v = parseFloat(o);\n");
    sb_add(&s, "                return Number.isNaN(v) ? 0.0 : v;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'upper': return o.toUpperCase();\n");
    sb_add(&s, "            case 'lower': return o.toLowerCase();\n");
    sb_add(&s, "            case 'capitalize': return o.length ? o[0].toUpperCase() + o.slice(1).toLowerCase() : o;\n");
    sb_add(&s, "            case 'swap_case': {\n");
    sb_add(&s, "                let out = ''; for (const c of o) {\n");
    sb_add(&s, "                    const u = c.toUpperCase(); const l = c.toLowerCase();\n");
    sb_add(&s, "                    out += c === u ? l : (c === l ? u : c);\n");
    sb_add(&s, "                } return out;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'title': {\n");
    sb_add(&s, "                return o.replace(/\\w\\S*/g, w => w[0].toUpperCase() + w.slice(1).toLowerCase());\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'is_upper': return o.length > 0 && o === o.toUpperCase() && /[A-Z]/.test(o);\n");
    sb_add(&s, "            case 'is_lower': return o.length > 0 && o === o.toLowerCase() && /[a-z]/.test(o);\n");
    sb_add(&s, "            case 'is_ascii': { for (let i = 0; i < o.length; i++) if (o.charCodeAt(i) >= 128) return false; return true; }\n");
    sb_add(&s, "            case 'pad_left': return o.padStart(args[0], args[1] ?? ' ');\n");
    sb_add(&s, "            case 'pad_right': return o.padEnd(args[0], args[1] ?? ' ');\n");
    sb_add(&s, "            case 'center': {\n");
    sb_add(&s, "                const w = args[0] | 0; if (o.length >= w) return o;\n");
    sb_add(&s, "                const fill = String(args[1] ?? ' ');\n");
    sb_add(&s, "                const total = w - o.length, l = (total / 2) | 0, r = total - l;\n");
    sb_add(&s, "                return fill.repeat(l) + o + fill.repeat(r);\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'find': case 'index_of': case 'indexOf': return o.indexOf(String(args[0]));\n");
    sb_add(&s, "            case 'last_index_of': case 'rfind': return o.lastIndexOf(String(args[0]));\n");
    sb_add(&s, "            case 'remove_prefix': return args[0] && o.startsWith(args[0]) ? o.slice(args[0].length) : o;\n");
    sb_add(&s, "            case 'remove_suffix': return args[0] && o.endsWith(args[0]) ? o.slice(0, -args[0].length) : o;\n");
    sb_add(&s, "            case 'count': {\n");
    sb_add(&s, "                if (args.length === 0) return o.length;\n");
    sb_add(&s, "                const sub = String(args[0]); if (sub.length === 0) return 0;\n");
    sb_add(&s, "                let c = 0, i = 0; while ((i = o.indexOf(sub, i)) !== -1) { c++; i += sub.length; } return c;\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'lines': return o.split(/\\r?\\n/);\n");
    sb_add(&s, "            case 'char_at': { const i = args[0] | 0; const j = i < 0 ? o.length + i : i; return o[j] ?? null; }\n");
    sb_add(&s, "            case 'reverse': return [...o].reverse().join('');\n");
    sb_add(&s, "            case 'chars': return [...o];\n");
    sb_add(&s, "            case 'bytes': { const r = []; for (let i = 0; i < o.length; i++) r.push(o.charCodeAt(i)); return r; }\n");
    sb_add(&s, "            case 'contains': case 'includes': return o.includes(String(args[0]));\n");
    sb_add(&s, "            case 'starts_with': return o.startsWith(String(args[0]));\n");
    sb_add(&s, "            case 'ends_with': return o.endsWith(String(args[0]));\n");
    sb_add(&s, "            case 'replace': case 'replace_all': {\n");
    sb_add(&s, "                const from = String(args[0]); const to = String(args[1] ?? '');\n");
    /* optional 3rd arg caps replacements, matching the VM/interp */
    sb_add(&s, "                if (args.length >= 3 && typeof args[2] === 'number') {\n");
    sb_add(&s, "                    let n = Math.max(0, args[2]); let r = o;\n");
    sb_add(&s, "                    while (n-- > 0) { const i = r.indexOf(from); if (i < 0) break; r = r.slice(0, i) + to + r.slice(i + from.length); }\n");
    sb_add(&s, "                    return r;\n");
    sb_add(&s, "                }\n");
    sb_add(&s, "                return o.split(from).join(to);\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'replace_first': case 'replace_one': {\n");
    sb_add(&s, "                const from = String(args[0]); const to = String(args[1] ?? '');\n");
    sb_add(&s, "                const idx = o.indexOf(from);\n");
    sb_add(&s, "                return idx < 0 ? o : o.slice(0, idx) + to + o.slice(idx + from.length);\n");
    sb_add(&s, "            }\n");
    sb_add(&s, "            case 'trim': return o.trim();\n");
    sb_add(&s, "            case 'trim_start': case 'trim_left': return o.trimStart();\n");
    sb_add(&s, "            case 'trim_end': case 'trim_right': return o.trimEnd();\n");
    sb_add(&s, "            case 'is_digit': case 'is_numeric': return o.length > 0 && /^[0-9]+$/.test(o);\n");
    sb_add(&s, "            case 'is_alpha': return o.length > 0 && /^[a-zA-Z]+$/.test(o);\n");
    sb_add(&s, "            case 'is_alnum': return o.length > 0 && /^[a-zA-Z0-9]+$/.test(o);\n");
    sb_add(&s, "            case 'is_space': case 'is_whitespace': return o.length > 0 && /^\\s+$/.test(o);\n");
    sb_add(&s, "            case 'split': return o.split(String(args[0] ?? ''));\n");
    sb_add(&s, "            case 'index_of': return o.indexOf(String(args[0]));\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    const f = o[m];\n");
    sb_add(&s, "    if (typeof f === 'function') return f.apply(o, args);\n");
    sb_add(&s, "    throw new TypeError(\"no method '\" + m + \"' on \" + typeof o);\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_resume = (v) => v;\n");
    /* f-string format spec: ${expr:spec} lowers to __xs_fmt(expr, spec).
       Handles the common shapes: '0>5' / '<5' / '>10' (fill+align+width),
       '.2f' (float precision), '#x' / 'x' (hex), 'b' (binary), 'o' (oct),
       'e' (scientific), 'd' (decimal). Any other spec falls through to
       String(v). */
    sb_add(&s, "const __xs_fmt = (v, spec) => {\n");
    sb_add(&s, "    if (!spec) return String(v);\n");
    sb_add(&s, "    let s = spec;\n");
    sb_add(&s, "    let fill = ' ', align = '';\n");
    sb_add(&s, "    if (s.length >= 2 && \"<>=^\".indexOf(s[1]) >= 0) { fill = s[0]; align = s[1]; s = s.slice(2); }\n");
    sb_add(&s, "    else if (s.length >= 1 && \"<>=^\".indexOf(s[0]) >= 0) { align = s[0]; s = s.slice(1); }\n");
    sb_add(&s, "    let alt = false;\n");
    sb_add(&s, "    if (s[0] === '#') { alt = true; s = s.slice(1); }\n");
    /* leading 0 means zero-fill (Python convention): treat as fill='0', align='>' */
    sb_add(&s, "    if (s[0] === '0' && s.length > 1 && /\\d/.test(s[1])) { if (!align) { fill = '0'; align = '>'; } s = s.slice(1); }\n");
    sb_add(&s, "    let width = 0; let m = s.match(/^(\\d+)/);\n");
    sb_add(&s, "    if (m) { width = parseInt(m[1], 10); s = s.slice(m[0].length); }\n");
    sb_add(&s, "    let prec = -1; m = s.match(/^\\.(\\d+)/);\n");
    sb_add(&s, "    if (m) { prec = parseInt(m[1], 10); s = s.slice(m[0].length); }\n");
    sb_add(&s, "    const ty = s;\n");
    sb_add(&s, "    let out;\n");
    sb_add(&s, "    if (ty === 'x') out = (alt ? '0x' : '') + Number(v).toString(16);\n");
    sb_add(&s, "    else if (ty === 'X') out = (alt ? '0X' : '') + Number(v).toString(16).toUpperCase();\n");
    sb_add(&s, "    else if (ty === 'b') out = (alt ? '0b' : '') + Number(v).toString(2);\n");
    sb_add(&s, "    else if (ty === 'o') out = (alt ? '0o' : '') + Number(v).toString(8);\n");
    sb_add(&s, "    else if (ty === 'd') out = String(Math.trunc(Number(v)));\n");
    sb_add(&s, "    else if (ty === 'e') out = Number(v).toExponential(prec >= 0 ? prec : 6);\n");
    sb_add(&s, "    else if (ty === 'f') out = Number(v).toFixed(prec >= 0 ? prec : 6);\n");
    sb_add(&s, "    else if (ty === 'g' || ty === '') {\n");
    /* match the VM: a precision on a numeric value with no type code means
       fixed decimals, not significant digits. */
    sb_add(&s, "        if (prec >= 0 && typeof v === 'number') out = Number(v).toFixed(prec);\n");
    sb_add(&s, "        else out = __xs_repr(v);\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    else out = String(v);\n");
    sb_add(&s, "    if (width > out.length) {\n");
    sb_add(&s, "        const pad = fill.repeat(width - out.length);\n");
    sb_add(&s, "        if (align === '<') out = out + pad;\n");
    sb_add(&s, "        else if (align === '^') { const l = Math.floor(pad.length / 2); out = pad.slice(0, l) + out + pad.slice(l); }\n");
    sb_add(&s, "        else out = pad + out; /* '>' or '=' default to right */\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return out;\n");
    sb_add(&s, "};\n");
    /* int() / float() coerce with the same VM semantics: invalid
       string input throws ValueError instead of yielding NaN. */
    sb_add(&s, "const __xs_to_int = (v) => {\n");
    sb_add(&s, "    if (typeof v === 'bigint') return v;\n");
    sb_add(&s, "    if (typeof v === 'number') {\n");
    sb_add(&s, "        if (Number.isNaN(v) || !Number.isFinite(v)) throw __xs_err('TypeError', \"int(): can't convert non-finite float\");\n");
    sb_add(&s, "        return Math.trunc(v);\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof v === 'boolean') return v ? 1 : 0;\n");
    sb_add(&s, "    if (typeof v === 'string') {\n");
    sb_add(&s, "        const trimmed = v.trim();\n");
    sb_add(&s, "        if (trimmed.length === 0) throw __xs_err('ValueError', \"int(): empty string\");\n");
    sb_add(&s, "        const n = Number(trimmed);\n");
    sb_add(&s, "        if (Number.isNaN(n)) throw __xs_err('ValueError', \"int(): invalid literal: '\" + v + \"'\");\n");
    sb_add(&s, "        return Math.trunc(n);\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    throw __xs_err('TypeError', 'int(): cannot convert from this type');\n");
    sb_add(&s, "};\n");
    /* Stdlib polyfills. Only the modules that map cleanly onto the
       host runtime are emitted; less-common modules (fs, os, ...)
       still need a Node shim and aren't covered here. */
    sb_add(&s, "const json = {\n");
    sb_add(&s, "    parse: (s) => JSON.parse(s),\n");
    sb_add(&s, "    stringify: (v) => JSON.stringify(v, (k, val) => val instanceof Map ? Object.fromEntries(val) : val),\n");
    sb_add(&s, "    pretty: (v) => JSON.stringify(v, (k, val) => val instanceof Map ? Object.fromEntries(val) : val, 2),\n");
    sb_add(&s, "    valid: (s) => { try { JSON.parse(s); return true; } catch { return false; } },\n");
    sb_add(&s, "    encode: (v) => JSON.stringify(v, (k, val) => val instanceof Map ? Object.fromEntries(val) : val),\n");
    sb_add(&s, "    decode: (s) => JSON.parse(s),\n");
    sb_add(&s, "    dumps:  (v) => JSON.stringify(v, (k, val) => val instanceof Map ? Object.fromEntries(val) : val),\n");
    sb_add(&s, "    loads:  (s) => JSON.parse(s),\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const math = {\n");
    sb_add(&s, "    pi: Math.PI, e: Math.E, tau: Math.PI * 2, inf: Infinity, nan: NaN,\n");
    sb_add(&s, "    abs: Math.abs, sqrt: Math.sqrt, pow: Math.pow,\n");
    sb_add(&s, "    floor: Math.floor, ceil: Math.ceil, round: Math.round, trunc: Math.trunc,\n");
    sb_add(&s, "    sin: Math.sin, cos: Math.cos, tan: Math.tan,\n");
    sb_add(&s, "    asin: Math.asin, acos: Math.acos, atan: Math.atan, atan2: Math.atan2,\n");
    sb_add(&s, "    sinh: Math.sinh, cosh: Math.cosh, tanh: Math.tanh,\n");
    sb_add(&s, "    log: Math.log, log2: Math.log2, log10: Math.log10, exp: Math.exp,\n");
    sb_add(&s, "    min: (...a) => Math.min(...a), max: (...a) => Math.max(...a),\n");
    sb_add(&s, "    sign: Math.sign,\n");
    sb_add(&s, "    is_nan: (x) => Number.isNaN(x), is_inf: (x) => !Number.isFinite(x) && !Number.isNaN(x),\n");
    sb_add(&s, "    is_finite: (x) => Number.isFinite(x),\n");
    sb_add(&s, "    degrees: (rad) => rad * 180 / Math.PI,\n");
    sb_add(&s, "    radians: (deg) => deg * Math.PI / 180,\n");
    sb_add(&s, "    gcd: (a, b) => { a = Math.abs(a); b = Math.abs(b); while (b) { [a, b] = [b, a % b]; } return a; },\n");
    sb_add(&s, "    lcm: (a, b) => { if (a === 0 || b === 0) return 0; const g = (function gcd(x, y) { return y ? gcd(y, x % y) : x; })(Math.abs(a), Math.abs(b)); return Math.abs(a * b) / g; },\n");
    sb_add(&s, "    cbrt: Math.cbrt, hypot: Math.hypot,\n");
    sb_add(&s, "    factorial: (n) => { let r = 1n; for (let i = 2n; i <= BigInt(n); i++) r *= i; return r; },\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const time = {\n");
    sb_add(&s, "    now: () => Date.now() / 1000,\n");
    sb_add(&s, "    now_ms: () => Date.now(),\n");
    sb_add(&s, "    now_ns: () => BigInt(Date.now()) * 1000000n,\n");
    sb_add(&s, "    sleep: (s) => new Promise(r => setTimeout(r, s * 1000)),\n");
    sb_add(&s, "    sleep_ms: (ms) => new Promise(r => setTimeout(r, ms)),\n");
    sb_add(&s, "    monotonic: () => (typeof performance !== 'undefined' ? performance.now() / 1000 : Date.now() / 1000),\n");
    sb_add(&s, "    clock: () => (typeof performance !== 'undefined' ? performance.now() / 1000 : Date.now() / 1000),\n");
    sb_add(&s, "    format: (ts, fmt) => {\n");
    sb_add(&s, "        const d = new Date((typeof ts === 'bigint' ? Number(ts) : ts) * 1000);\n");
    sb_add(&s, "        const pad = (n, w=2) => String(n).padStart(w, '0');\n");
    sb_add(&s, "        const Y = d.getUTCFullYear(), M = d.getUTCMonth()+1, D = d.getUTCDate();\n");
    sb_add(&s, "        const H = d.getUTCHours(), m = d.getUTCMinutes(), S = d.getUTCSeconds();\n");
    sb_add(&s, "        return (fmt || '%Y-%m-%d %H:%M:%S').replace(/%[YmdHMS]/g, t => ({\n");
    sb_add(&s, "            '%Y': String(Y), '%m': pad(M), '%d': pad(D),\n");
    sb_add(&s, "            '%H': pad(H), '%M': pad(m), '%S': pad(S),\n");
    sb_add(&s, "        }[t]));\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    parse: (s, fmt) => Math.floor(Date.parse(s) / 1000),\n");
    sb_add(&s, "};\n");
    /* collections module: typed wrappers built on Map / Array. The VM
       represents these as plain maps tagged with _type, but the JS
       host is more comfortable with class-like objects that expose
       methods. Match the VM's surface: Set / Counter / Deque / Stack
       / OrderedMap / PriorityQueue. */
    sb_add(&s, "const collections = {\n");
    sb_add(&s, "    Set: (init) => {\n");
    sb_add(&s, "        const data = new Map();\n");
    sb_add(&s, "        if (Array.isArray(init)) for (const v of init) data.set(__xs_repr(v), true);\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'Set', _data: data,\n");
    sb_add(&s, "            add(v) { data.set(__xs_repr(v), v); return this; },\n");
    sb_add(&s, "            contains(v) { return data.has(__xs_repr(v)); },\n");
    sb_add(&s, "            has(v) { return data.has(__xs_repr(v)); },\n");
    sb_add(&s, "            remove(v) { return data.delete(__xs_repr(v)); },\n");
    sb_add(&s, "            len() { return data.size; },\n            size() { return data.size; },\n");
    sb_add(&s, "            is_empty() { return data.size === 0; },\n");
    sb_add(&s, "            to_array() { return [...data.values()]; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    Counter: (init) => {\n");
    sb_add(&s, "        const data = new Map();\n");
    sb_add(&s, "        if (Array.isArray(init)) for (const v of init) {\n");
    sb_add(&s, "            const k = __xs_repr(v); data.set(k, (data.get(k) || 0) + 1);\n");
    sb_add(&s, "        }\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'Counter', _data: data,\n");
    sb_add(&s, "            get(k) { return data.get(__xs_repr(k)) || 0; },\n");
    sb_add(&s, "            inc(k, n) { const key = __xs_repr(k); data.set(key, (data.get(key) || 0) + (n||1)); return this; },\n");
    sb_add(&s, "            total() { let t=0; for (const v of data.values()) t+=v; return t; },\n");
    sb_add(&s, "            most_common(n) {\n");
    sb_add(&s, "                const arr = [...data.entries()].sort((a,b)=>b[1]-a[1]);\n");
    sb_add(&s, "                return n ? arr.slice(0, n) : arr;\n");
    sb_add(&s, "            },\n");
    sb_add(&s, "            len() { return data.size; },\n            size() { return data.size; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    Deque: (init) => {\n");
    sb_add(&s, "        const data = Array.isArray(init) ? [...init] : [];\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'Deque', _data: data,\n");
    sb_add(&s, "            push_back(v) { data.push(v); return this; },\n");
    sb_add(&s, "            push_front(v) { data.unshift(v); return this; },\n");
    sb_add(&s, "            pop_back() { return data.pop(); },\n");
    sb_add(&s, "            pop_front() { return data.shift(); },\n");
    sb_add(&s, "            front() { return data[0]; },\n");
    sb_add(&s, "            back() { return data[data.length-1]; },\n");
    sb_add(&s, "            len() { return data.length; },\n            size() { return data.length; },\n");
    sb_add(&s, "            is_empty() { return data.length === 0; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    Stack: (init) => {\n");
    sb_add(&s, "        const data = Array.isArray(init) ? [...init] : [];\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'Stack', _data: data,\n");
    sb_add(&s, "            push(v) { data.push(v); return this; },\n");
    sb_add(&s, "            pop() { return data.pop(); },\n");
    sb_add(&s, "            peek() { return data[data.length-1]; },\n");
    sb_add(&s, "            len() { return data.length; },\n            size() { return data.length; },\n");
    sb_add(&s, "            is_empty() { return data.length === 0; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    OrderedMap: (init) => {\n");
    sb_add(&s, "        const data = new Map();\n");
    sb_add(&s, "        if (init instanceof Map) for (const [k, v] of init) data.set(k, v);\n");
    sb_add(&s, "        else if (init && typeof init === 'object') for (const k of Object.keys(init)) data.set(k, init[k]);\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'OrderedMap', _data: data,\n");
    sb_add(&s, "            get(k) { return data.get(k); },\n");
    sb_add(&s, "            set(k, v) { data.set(k, v); return this; },\n");
    sb_add(&s, "            delete(k) { return data.delete(k); },\n");
    sb_add(&s, "            has(k) { return data.has(k); }, contains(k) { return data.has(k); },\n");
    sb_add(&s, "            keys() { return [...data.keys()]; },\n");
    sb_add(&s, "            values() { return [...data.values()]; },\n");
    sb_add(&s, "            len() { return data.size; },\n            size() { return data.size; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "    PriorityQueue: () => {\n");
    sb_add(&s, "        const data = []; /* [item, priority] sorted ascending by priority */\n");
    sb_add(&s, "        return {\n");
    sb_add(&s, "            _type: 'PriorityQueue', _data: data,\n");
    sb_add(&s, "            push(item, priority) {\n");
    sb_add(&s, "                let i = 0; while (i < data.length && data[i][1] <= priority) i++;\n");
    sb_add(&s, "                data.splice(i, 0, [item, priority]); return this;\n");
    sb_add(&s, "            },\n");
    sb_add(&s, "            pop() { const e = data.shift(); return e ? e[0] : null; },\n");
    sb_add(&s, "            peek() { return data.length ? data[0][0] : null; },\n");
    sb_add(&s, "            len() { return data.length; },\n            size() { return data.length; },\n");
    sb_add(&s, "            is_empty() { return data.length === 0; },\n");
    sb_add(&s, "        };\n");
    sb_add(&s, "    },\n");
    sb_add(&s, "};\n");
    /* Builtin globals from the XS runtime that don't exist in JS but
       are used unqualified (range, len already covered, etc.). range
       returns an Array so for-of, .map, .to_array all work. Bound on
       globalThis with `??=` so user code can shadow it with a local
       `fn range` declaration without hitting the JS double-decl error. */
    sb_add(&s, "globalThis.range ?\?= (a, b, step) => {\n");
    sb_add(&s, "    const start = (b === undefined) ? 0 : a;\n");
    sb_add(&s, "    const end   = (b === undefined) ? a : b;\n");
    sb_add(&s, "    const stp   = step === undefined ? 1 : step;\n");
    sb_add(&s, "    const r = [];\n");
    sb_add(&s, "    if (stp > 0) for (let i = start; i < end; i += stp) r.push(i);\n");
    sb_add(&s, "    else if (stp < 0) for (let i = start; i > end; i += stp) r.push(i);\n");
    sb_add(&s, "    r.to_array = () => [...r];\n");
    sb_add(&s, "    return r;\n");
    sb_add(&s, "};\n");
    /* Global helpers available unqualified in XS programs. Set via
       globalThis with a guard so user code can shadow these without
       hitting the JS \"already declared\" error. */
    sb_add(&s, "globalThis.sum ?\?= (arr) => { let t = 0; for (const v of arr) t = __xs_add(t, v); return t; };\n");
    sb_add(&s, "globalThis.product ?\?= (arr) => { let t = 1; for (const v of arr) t = (typeof t === 'bigint' || typeof v === 'bigint') ? BigInt(t) * BigInt(v) : t * v; return t; };\n");
    sb_add(&s, "globalThis.min ?\?= (...xs) => { const flat = xs.length === 1 && Array.isArray(xs[0]) ? xs[0] : xs; let r = flat[0]; for (let i = 1; i < flat.length; i++) if (__xs_cmp(flat[i], r) < 0) r = flat[i]; return r; };\n");
    sb_add(&s, "globalThis.max ?\?= (...xs) => { const flat = xs.length === 1 && Array.isArray(xs[0]) ? xs[0] : xs; let r = flat[0]; for (let i = 1; i < flat.length; i++) if (__xs_cmp(flat[i], r) > 0) r = flat[i]; return r; };\n");
    sb_add(&s, "globalThis.sorted ?\?= (arr, cmp) => { const a = [...arr]; a.sort(cmp || ((x, y) => __xs_cmp(x, y))); return a; };\n");
    sb_add(&s, "globalThis.reversed ?\?= (arr) => [...arr].reverse();\n");
    sb_add(&s, "globalThis.enumerate ?\?= (arr) => arr.map((v, i) => __xs_tuple([i, v]));\n");
    sb_add(&s, "globalThis.zip ?\?= (...arrs) => { const n = Math.min(...arrs.map(a => a.length)); const r = []; for (let i = 0; i < n; i++) r.push(__xs_tuple(arrs.map(a => a[i]))); return r; };\n");
    sb_add(&s, "globalThis.abs ?\?= (x) => typeof x === 'bigint' ? (x < 0n ? -x : x) : Math.abs(x);\n");
    /* Tuples are arrays with a marker so __xs_repr can render them with
       parens like the VM does. Defining as a property on the array is
       cheap; subclassing Array breaks too many JS interop paths. */
    sb_add(&s, "const __xs_tuple = (arr) => { Object.defineProperty(arr, '__xs_is_tuple', {value: true, enumerable: false}); return arr; };\n");
    /* del-tombstone sentinel. xs_check_deleted throws a catchable error\n     * on read of a deleted local, mirroring the C / VM behaviour. */
    sb_add(&s, "const __XS_DELETED = Symbol('__xs_deleted');\n");
    sb_add(&s, "const __xs_check_deleted = (v, name) => {\n");
    sb_add(&s, "    if (v === __XS_DELETED) throw new Error(\"name '\" + name + \"' is not defined (deleted)\");\n");
    sb_add(&s, "    return v;\n");
    sb_add(&s, "};\n");
    /* Materialise a range as an array but stash the original (start, end)
     * so r.end() returns the bound the user wrote rather than the last
     * materialised element. Mirrors what xs_arr.is_range does on the C
     * target. */
    sb_add(&s, "const __xs_range = (start, end, inclusive) => {\n");
    sb_add(&s, "    const s = Number(start), e = inclusive ? Number(end) + 1 : Number(end);\n");
    sb_add(&s, "    const step = s <= e ? 1 : -1;\n");
    sb_add(&s, "    const len = Math.max(0, (e - s) * step);\n");
    sb_add(&s, "    const a = new Array(len);\n");
    sb_add(&s, "    for (let i = 0; i < len; i++) a[i] = s + i * step;\n");
    sb_add(&s, "    Object.defineProperty(a, '__xs_range_start', {value: s, enumerable: false});\n");
    sb_add(&s, "    Object.defineProperty(a, '__xs_range_end',   {value: Number(end), enumerable: false});\n");
    sb_add(&s, "    Object.defineProperty(a, '__xs_range_inclusive', {value: !!inclusive, enumerable: false});\n");
    sb_add(&s, "    return a;\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const chr = (n) => String.fromCharCode(Number(n));\n");
    sb_add(&s, "const ord = (s) => typeof s === 'string' && s.length > 0 ? s.codePointAt(0) : 0;\n");
    sb_add(&s, "const random = {\n");
    sb_add(&s, "    random: () => Math.random(),\n");
    sb_add(&s, "    int: (lo, hi) => lo + Math.floor(Math.random() * (hi - lo + 1)),\n");
    sb_add(&s, "    float: (lo, hi) => lo + Math.random() * (hi - lo),\n");
    sb_add(&s, "    choice: (arr) => arr[Math.floor(Math.random() * arr.length)],\n");
    sb_add(&s, "    shuffle: (arr) => { const a = [...arr]; for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i+1)); [a[i],a[j]]=[a[j],a[i]]; } return a; },\n");
    sb_add(&s, "    sample: (arr, n) => { const a = [...arr]; const r = []; for (let i = 0; i < n && a.length; i++) { const j = Math.floor(Math.random() * a.length); r.push(a.splice(j, 1)[0]); } return r; },\n");
    sb_add(&s, "    seed: () => {},\n");
    sb_add(&s, "};\n");
    /* async module: thin polyfill over JS Promise. async is reserved
       in JS only at function-decl positions; using it as a variable
       name is fine. */
    sb_add(&s, "const __xs_async = {\n");
    sb_add(&s, "    all: (ps) => Promise.all(ps),\n");
    sb_add(&s, "    any: (ps) => Promise.any(ps),\n");
    sb_add(&s, "    race: (ps) => Promise.race(ps),\n");
    sb_add(&s, "    settle: (ps) => Promise.allSettled(ps),\n");
    sb_add(&s, "    sleep: (sec) => new Promise(r => setTimeout(r, sec * 1000)),\n");
    sb_add(&s, "    delay: (sec, v) => new Promise(r => setTimeout(() => r(v), sec * 1000)),\n");
    sb_add(&s, "    resolve: (v) => Promise.resolve(v),\n");
    sb_add(&s, "    reject: (e) => Promise.reject(e),\n");
    sb_add(&s, "};\n");
    /* re module: thin wrapper over JS RegExp. compile returns an object
       carrying the source pattern; match/find_all/replace/split take a
       string + pattern (string or compiled). */
    sb_add(&s, "const re = {\n");
    sb_add(&s, "    compile: (pat, flags) => ({ _type: 'Regex', _src: pat, _flags: flags || '',\n");
    sb_add(&s, "        match: (s) => { const m = new RegExp(pat, flags || '').exec(s); return m ? m[0] : null; },\n");
    sb_add(&s, "        find: (s) => { const m = new RegExp(pat, flags || '').exec(s); return m ? m[0] : null; },\n");
    sb_add(&s, "        find_all: (s) => [...s.matchAll(new RegExp(pat, (flags || '') + 'g'))].map(m => m[0]),\n");
    sb_add(&s, "        test: (s) => new RegExp(pat, flags || '').test(s),\n");
    sb_add(&s, "        replace: (s, r) => s.replace(new RegExp(pat, (flags || '') + 'g'), r),\n");
    sb_add(&s, "        split: (s) => s.split(new RegExp(pat, flags || '')),\n");
    sb_add(&s, "    }),\n");
    sb_add(&s, "    match: (s, pat) => { const m = new RegExp(pat).exec(s); return m ? m[0] : null; },\n");
    sb_add(&s, "    find: (s, pat) => { const m = new RegExp(pat).exec(s); return m ? m[0] : null; },\n");
    sb_add(&s, "    find_all: (s, pat) => [...s.matchAll(new RegExp(pat, 'g'))].map(m => m[0]),\n");
    sb_add(&s, "    test: (s, pat) => new RegExp(pat).test(s),\n");
    sb_add(&s, "    replace: (s, pat, r) => s.replace(new RegExp(pat, 'g'), r),\n");
    sb_add(&s, "    split: (s, pat) => s.split(new RegExp(pat)),\n");
    sb_add(&s, "};\n");
    sb_add(&s, "const __xs_to_float = (v) => {\n");
    sb_add(&s, "    if (typeof v === 'bigint') return Number(v);\n");
    sb_add(&s, "    if (typeof v === 'number') return v;\n");
    sb_add(&s, "    if (typeof v === 'boolean') return v ? 1.0 : 0.0;\n");
    sb_add(&s, "    if (typeof v === 'string') {\n");
    sb_add(&s, "        const trimmed = v.trim();\n");
    sb_add(&s, "        if (trimmed.length === 0) throw __xs_err('ValueError', \"float(): empty string\");\n");
    sb_add(&s, "        const n = Number(trimmed);\n");
    sb_add(&s, "        if (Number.isNaN(n)) throw __xs_err('ValueError', \"float(): invalid literal: '\" + v + \"'\");\n");
    sb_add(&s, "        return n;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    throw __xs_err('TypeError', 'float(): cannot convert from this type');\n");
    sb_add(&s, "};\n");
    /* Structural equality. JS '===' is reference-equal for arrays /
       maps / objects; XS callers expect deep comparison the same way
       the interp / VM compute it. */
    sb_add(&s, "const __xs_eq = (a, b) => {\n");
    sb_add(&s, "    if (a === b) return true;\n");
    sb_add(&s, "    if (a === null || b === null || a === undefined || b === undefined) return false;\n");
    sb_add(&s, "    if (typeof a !== typeof b) {\n");
    sb_add(&s, "        if ((typeof a === 'number' && typeof b === 'bigint') ||\n");
    sb_add(&s, "            (typeof a === 'bigint' && typeof b === 'number')) return Number(a) === Number(b);\n");
    sb_add(&s, "        return false;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (Array.isArray(a)) {\n");
    sb_add(&s, "        if (!Array.isArray(b) || a.length !== b.length) return false;\n");
    sb_add(&s, "        for (let i = 0; i < a.length; i++) if (!__xs_eq(a[i], b[i])) return false;\n");
    sb_add(&s, "        return true;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (a instanceof Map) {\n");
    sb_add(&s, "        if (!(b instanceof Map) || a.size !== b.size) return false;\n");
    sb_add(&s, "        for (const [k, v] of a) if (!__xs_eq(v, b.get(k))) return false;\n");
    sb_add(&s, "        return true;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    if (typeof a === 'object') {\n");
    sb_add(&s, "        const ak = Object.keys(a), bk = Object.keys(b);\n");
    sb_add(&s, "        if (ak.length !== bk.length) return false;\n");
    sb_add(&s, "        for (const k of ak) if (!__xs_eq(a[k], b[k])) return false;\n");
    sb_add(&s, "        return true;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return false;\n");
    sb_add(&s, "};\n");
    /* Tolerant equality for assert_eq: floats compare with the same
       relative+absolute slack as the native runtime so chained arithmetic
       (e.g. summing 3.14*r*r across shapes) still matches a literal. */
    sb_add(&s, "const __xs_eq_assert = (a, b) => {\n");
    sb_add(&s, "    if (__xs_eq(a, b)) return true;\n");
    sb_add(&s, "    if (typeof a === 'number' && typeof b === 'number'\n");
    sb_add(&s, "        && !Number.isNaN(a) && !Number.isNaN(b)) {\n");
    sb_add(&s, "        const diff = Math.abs(a - b);\n");
    sb_add(&s, "        const scale = Math.max(Math.abs(a), Math.abs(b));\n");
    sb_add(&s, "        if (diff <= 1e-9 + 1e-9 * scale) return true;\n");
    sb_add(&s, "    }\n");
    sb_add(&s, "    return false;\n");
    sb_add(&s, "};\n");
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
    js_n_traits = 0;
    /* free any leftover use-modules from a previous transpile call */
    for (int i = 0; i < js_n_use_mods; i++) {
        if (js_use_mods[i].prog) node_free(js_use_mods[i].prog);
        free(js_use_mods[i].path);
        free(js_use_mods[i].src);
        js_use_mods[i].prog = NULL;
        js_use_mods[i].path = NULL;
        js_use_mods[i].src  = NULL;
    }
    js_n_use_mods = 0;
    /* derive importer's directory so `use "./util.xs"` resolves */
    g_js_src_dir[0] = '\0';
    if (filename) {
        const char *slash = strrchr(filename, '/');
        if (slash && slash != filename) {
            size_t n = (size_t)(slash - filename);
            if (n >= sizeof(g_js_src_dir)) n = sizeof(g_js_src_dir) - 1;
            memcpy(g_js_src_dir, filename, n);
            g_js_src_dir[n] = '\0';
        }
    }

    int has_main = 0;
    /* Detect top-level await (outside any async fn) by walking the
       program statements. If found, wrap the body in an async IIFE
       so Node accepts it under CommonJS too. await inside an async
       fn declaration doesn't count -- only awaits at program scope. */
    int has_top_await = 0;
    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len && !has_top_await; i++) {
            Node *st = program->program.stmts.items[i];
            if (!st) continue;
            /* Skip fn decls -- their internal awaits stay inside the fn.
               Look only at expressions / let-bindings / etc. at top level. */
            if (VAL_TAG(st) == NODE_FN_DECL) continue;
            /* Tiny structural scan for an AWAIT node at top-level. */
            Node *probe = st;
            if (VAL_TAG(probe) == NODE_EXPR_STMT) probe = probe->expr_stmt.expr;
            else if (VAL_TAG(probe) == NODE_LET) probe = probe->let.value;
            else if (VAL_TAG(probe) == NODE_VAR) probe = probe->let.value;
            else if (VAL_TAG(probe) == NODE_CONST) probe = probe->const_.value;
            if (probe && VAL_TAG(probe) == NODE_AWAIT) has_top_await = 1;
            else if (probe && VAL_TAG(probe) == NODE_CALL) {
                for (int j = 0; j < probe->call.args.len && !has_top_await; j++)
                    if (probe->call.args.items[j] &&
                        VAL_TAG(probe->call.args.items[j]) == NODE_AWAIT)
                        has_top_await = 1;
            }
        }
    }

    if (has_top_await) sb_add(&s, "(async () => {\n");

    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            emit_node(&s, st, has_top_await ? 1 : 0);
            /* check if this is a main() function declaration */
            if (st && VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.name &&
                strcmp(st->fn_decl.name, "main") == 0) {
                has_main = 1;
            }
        }
    } else {
        emit_node(&s, program, has_top_await ? 1 : 0);
    }

    /* auto-call main if defined */
    if (has_main) {
        if (has_top_await) sb_add(&s, "    ");
        sb_add(&s, "main();\n");
    }

    if (has_top_await) sb_add(&s, "})();\n");

    /* source map reference */
    if (filename) {
        /* derive .xs.map name from source filename */
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        sb_printf(&s, "\n//# sourceMappingURL=%s.map\n", base);
    }

    return s.data;
}
