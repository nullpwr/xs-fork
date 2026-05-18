#include "transpiler/c_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "core/strbuf.h"
#include "core/xs.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "semantic/purity.h"

/* forward declarations */
static void emit_expr(SB *s, Node *n, int depth);
static void emit_stmt(SB *s, Node *n, int depth);
static void emit_block_body(SB *s, Node *block, int depth);
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth);
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth);

/* ---- AST builder helpers used by the lowering pre-pass ----------------- */
static Node *mkc_ident(const char *name) {
    Node *n = node_new(NODE_IDENT, span_zero());
    n->ident.name = xs_strdup(name);
    return n;
}
static Node *mkc_str_lit(const char *s) {
    Node *n = node_new(NODE_LIT_STRING, span_zero());
    n->lit_string.sval = xs_strdup(s);
    n->lit_string.parts = nodelist_new();
    n->lit_string.interpolated = 0;
    return n;
}
static Node *mkc_int_lit(int64_t v) {
    Node *n = node_new(NODE_LIT_INT, span_zero());
    n->lit_int.ival = v;
    return n;
}
static Node *mkc_null_lit(void) { return node_new(NODE_LIT_NULL, span_zero()); }
static Node *mkc_let(const char *name, Node *value, int is_var) {
    Node *n = node_new(is_var ? NODE_VAR : NODE_LET, span_zero());
    n->let.name = xs_strdup(name);
    n->let.value = value;
    n->let.pattern = NULL;
    n->let.mutable = is_var;
    n->let.is_scoped = 0;
    n->let.is_pub = 0;
    n->let.type_ann = NULL;
    n->let.contract = NULL;
    return n;
}
static Node *mkc_const(const char *name, Node *value) {
    Node *n = node_new(NODE_CONST, span_zero());
    n->const_.name = xs_strdup(name);
    n->const_.value = value;
    n->const_.type_ann = NULL;
    n->const_.contract = NULL;
    n->const_.is_pub = 0;
    return n;
}
static Node *mkc_expr_stmt(Node *e) {
    Node *n = node_new(NODE_EXPR_STMT, span_zero());
    n->expr_stmt.expr = e;
    n->expr_stmt.has_semicolon = 1;
    return n;
}
static Node *mkc_block(NodeList stmts, Node *expr) {
    Node *n = node_new(NODE_BLOCK, span_zero());
    n->block.stmts = stmts;
    n->block.expr = expr;
    n->block.has_decls = -1;
    n->block.is_unsafe = 0;
    return n;
}
static Node *mkc_method_call(Node *obj, const char *method, Node **args, int nargs) {
    Node *n = node_new(NODE_METHOD_CALL, span_zero());
    n->method_call.obj = obj;
    n->method_call.method = xs_strdup(method);
    n->method_call.args = nodelist_new();
    n->method_call.kwargs = nodepairlist_new();
    n->method_call.optional = 0;
    for (int i = 0; i < nargs; i++) nodelist_push(&n->method_call.args, args[i]);
    return n;
}

/* Walk the AST looking for constructs the C target genuinely can't represent
 * (as opposed to "should work but doesn't"). Returns the first reason found
 * or NULL if everything is supportable. Refused at emit time so the user
 * gets a clear message instead of generated code that traps at runtime. */
static const char *find_unsupported_for_c(Node *n) {
    if (!n) return NULL;
    switch (VAL_TAG(n)) {
    case NODE_FN_DECL:
        /* generator + async markers are stripped by the lowering pass;
         * if we still see one here it means lowering didn't run, which
         * is a hard refusal. */
        if (n->fn_decl.is_generator)
            return "fn* generator declarations";
        /* wrapping decorators (@memoize / @retry / @timed / @trace /
         * @throttle / @debounce) need to rewrite the call path. trigger-
         * style decorators (@on_start / @every / etc) are no-op'd silently
         * because their absence doesn't change correctness for the
         * decorated fn itself. */
        for (int i = 0; i < n->fn_decl.n_decorators; i++) {
            const char *dn = n->fn_decl.decorators[i].name;
            if (!dn) continue;
            if (strcmp(dn, "memoize") == 0 || strcmp(dn, "retry") == 0 ||
                strcmp(dn, "timed") == 0   || strcmp(dn, "trace") == 0 ||
                strcmp(dn, "throttle") == 0|| strcmp(dn, "debounce") == 0)
                return "wrapping decorators (@memoize/@retry/@timed/@trace/@throttle/@debounce)";
        }
        return find_unsupported_for_c(n->fn_decl.body);
    case NODE_LAMBDA:
        if (n->lambda.is_generator & 1)
            return "fn*() generator lambdas";
        return find_unsupported_for_c(n->lambda.body);
    case NODE_BIND:
        /* `bind` is reactive on the interp + VM (re-evaluates when its
         * dependencies are reassigned, indexed, or field-mutated). The C
         * target lowers to a one-shot xs_val let, which silently goes
         * stale. Refuse rather than miscompile. */
        return "reactive `bind` declarations";
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++) {
            const char *r = find_unsupported_for_c(n->program.stmts.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++) {
            const char *r = find_unsupported_for_c(n->block.stmts.items[i]);
            if (r) return r;
        }
        return find_unsupported_for_c(n->block.expr);
    case NODE_BINOP:
        { const char *r = find_unsupported_for_c(n->binop.left); if (r) return r; }
        return find_unsupported_for_c(n->binop.right);
    case NODE_UNARY:    return find_unsupported_for_c(n->unary.expr);
    case NODE_CALL:
        { const char *r = find_unsupported_for_c(n->call.callee); if (r) return r; }
        for (int i = 0; i < n->call.args.len; i++) {
            const char *r = find_unsupported_for_c(n->call.args.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_METHOD_CALL:
        { const char *r = find_unsupported_for_c(n->method_call.obj); if (r) return r; }
        for (int i = 0; i < n->method_call.args.len; i++) {
            const char *r = find_unsupported_for_c(n->method_call.args.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_INDEX:
        { const char *r = find_unsupported_for_c(n->index.obj); if (r) return r; }
        return find_unsupported_for_c(n->index.index);
    case NODE_FIELD:    return find_unsupported_for_c(n->field.obj);
    case NODE_ASSIGN:
        { const char *r = find_unsupported_for_c(n->assign.target); if (r) return r; }
        return find_unsupported_for_c(n->assign.value);
    case NODE_IF:
        { const char *r = find_unsupported_for_c(n->if_expr.cond); if (r) return r; }
        { const char *r = find_unsupported_for_c(n->if_expr.then); if (r) return r; }
        return find_unsupported_for_c(n->if_expr.else_branch);
    case NODE_FOR:
        { const char *r = find_unsupported_for_c(n->for_loop.iter); if (r) return r; }
        return find_unsupported_for_c(n->for_loop.body);
    case NODE_WHILE:
        { const char *r = find_unsupported_for_c(n->while_loop.cond); if (r) return r; }
        return find_unsupported_for_c(n->while_loop.body);
    case NODE_RETURN:   return find_unsupported_for_c(n->ret.value);
    case NODE_LET:
    case NODE_VAR:      return find_unsupported_for_c(n->let.value);
    case NODE_CONST:    return find_unsupported_for_c(n->const_.value);
    case NODE_EXPR_STMT: return find_unsupported_for_c(n->expr_stmt.expr);
    case NODE_TRY:
        { const char *r = find_unsupported_for_c(n->try_.body); if (r) return r; }
        return find_unsupported_for_c(n->try_.finally_block);
    case NODE_THROW:    return find_unsupported_for_c(n->throw_.value);
    case NODE_MATCH:
        { const char *r = find_unsupported_for_c(n->match.subject); if (r) return r; }
        for (int i = 0; i < n->match.arms.len; i++) {
            { const char *r = find_unsupported_for_c(n->match.arms.items[i].guard); if (r) return r; }
            { const char *r = find_unsupported_for_c(n->match.arms.items[i].body); if (r) return r; }
        }
        return NULL;
    case NODE_LIT_ARRAY:
    case NODE_LIT_TUPLE:
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            const char *r = find_unsupported_for_c(n->lit_array.elems.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_LIT_MAP:
        for (int i = 0; i < n->lit_map.vals.len; i++) {
            const char *r = find_unsupported_for_c(n->lit_map.vals.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_IMPL_DECL:
        for (int i = 0; i < n->impl_decl.members.len; i++) {
            const char *r = find_unsupported_for_c(n->impl_decl.members.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_ACTOR_DECL:
        for (int i = 0; i < n->actor_decl.methods.len; i++) {
            const char *r = find_unsupported_for_c(n->actor_decl.methods.items[i]);
            if (r) return r;
        }
        return NULL;
    case NODE_NURSERY:  return find_unsupported_for_c(n->nursery_.body);
    case NODE_SPAWN:    return find_unsupported_for_c(n->spawn_.expr);
    case NODE_AWAIT:    return find_unsupported_for_c(n->await_.expr);
    case NODE_RANGE:
        { const char *r = find_unsupported_for_c(n->range.start); if (r) return r; }
        return find_unsupported_for_c(n->range.end);
    case NODE_LIST_COMP:
        { const char *r = find_unsupported_for_c(n->list_comp.element); if (r) return r; }
        for (int i = 0; i < n->list_comp.clause_iters.len; i++) {
            const char *r = find_unsupported_for_c(n->list_comp.clause_iters.items[i]);
            if (r) return r;
        }
        for (int i = 0; i < n->list_comp.clause_conds.len; i++) {
            const char *r = find_unsupported_for_c(n->list_comp.clause_conds.items[i]);
            if (r) return r;
        }
        return NULL;
    default: return NULL;
    }
}

/* track if we've seen a main function */
static int seen_main = 0;
/* track defers for goto-based cleanup */
static int defer_label_counter = 0;

/* Names that have been `del`'d in the current emit scope. NODE_IDENT
 * loads consult this so the read traps with a catchable error instead
 * of returning the sentinel transparently. Reset per transpile_c run
 * and saved/restored across function bodies. */
#define MAX_DELETED_VARS 64
static const char *deleted_vars[MAX_DELETED_VARS];
static int n_deleted_vars = 0;

static int is_deleted_var(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < n_deleted_vars; i++)
        if (strcmp(deleted_vars[i], name) == 0) return 1;
    return 0;
}
static void mark_deleted_var(const char *name) {
    if (!name) return;
    if (is_deleted_var(name)) return;
    if (n_deleted_vars < MAX_DELETED_VARS)
        deleted_vars[n_deleted_vars++] = name;
}

/* actor tracking for type-aware dispatch */
#define MAX_ACTORS 32
static struct { const char *name; } actors[MAX_ACTORS];
static int n_actors = 0;

#define MAX_ACTOR_VARS 64
static struct { const char *var_name; const char *actor_name; } actor_vars[MAX_ACTOR_VARS];
static int n_actor_vars = 0;

static void register_actor(const char *name) {
    if (n_actors < MAX_ACTORS) actors[n_actors++].name = name;
}

static const char *find_actor(const char *name) {
    for (int i = 0; i < n_actors; i++)
        if (strcmp(actors[i].name, name) == 0) return name;
    return NULL;
}

static void register_actor_var(const char *var, const char *actor) {
    if (n_actor_vars < MAX_ACTOR_VARS) {
        actor_vars[n_actor_vars].var_name = var;
        actor_vars[n_actor_vars].actor_name = actor;
        n_actor_vars++;
    }
}

static const char *lookup_actor_var(const char *var) {
    for (int i = 0; i < n_actor_vars; i++)
        if (strcmp(actor_vars[i].var_name, var) == 0) return actor_vars[i].actor_name;
    return NULL;
}

/* C keyword escaping */
static const char *c_keywords[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","int","long","register",
    "return","short","signed","sizeof","static","struct","switch","typedef",
    "union","unsigned","void","volatile","while","inline","restrict",NULL
};
/* libc names that would collide with user identifiers when included from
 * stdio/stdlib/string/math. Anything in this list gets prefixed so `let
 * log = ...` doesn't redeclare the math.h symbol. */
static const char *c_libc_names[] = {
    "log","log2","log10","exp","exp2","pow","sqrt","cbrt",
    "sin","cos","tan","asin","acos","atan","atan2",
    "sinh","cosh","tanh","asinh","acosh","atanh",
    "ceil","floor","round","trunc","fabs","fmod","fmin","fmax",
    "abs","labs","llabs","div","ldiv","lldiv",
    /* `exit` and `abort` are intentionally NOT in this list: when an XS
     * program calls `exit(0)` the libc `exit` is what the user wants.
     * Rewriting to `xs_user_exit` produces an undefined symbol at link. */
    "_Exit","atexit","getenv","system","rand","srand",
    "malloc","calloc","realloc","free","alloca",
    "nan","nanf","nanl","inf","infinity",
    "printf","fprintf","sprintf","snprintf","vprintf","vfprintf","vsprintf","vsnprintf",
    "scanf","fscanf","sscanf","puts","fputs","getc","putc","gets","fgets","fputc","getchar","putchar",
    "fopen","fclose","fread","fwrite","fseek","ftell","rewind","feof","ferror","fflush","setbuf","setvbuf","clearerr",
    "remove","rename","tmpfile","tmpnam","perror",
    "strlen","strcpy","strncpy","strcat","strncat","strcmp","strncmp",
    "strchr","strrchr","strstr","strtok","strdup","strerror","strspn","strcspn","strpbrk",
    "memcpy","memmove","memset","memcmp","memchr",
    "atoi","atol","atoll","atof","strtol","strtoll","strtoul","strtoull","strtod","strtof",
    "time","clock","difftime","mktime","gmtime","localtime","asctime","ctime","strftime",
    "qsort","bsearch",
    "open","close","read","write","stat","fstat","lseek","unlink",
    "isalpha","isdigit","isalnum","isspace","isupper","islower","ispunct","isprint","isxdigit","iscntrl","isgraph",
    "tolower","toupper",
    "signal","raise","setjmp","longjmp",
    NULL
};
static int is_c_keyword(const char *name) {
    if (!name) return 0;
    for (int i = 0; c_keywords[i]; i++)
        if (strcmp(c_keywords[i], name) == 0) return 1;
    return 0;
}
static int is_c_libc_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; c_libc_names[i]; i++)
        if (strcmp(c_libc_names[i], name) == 0) return 1;
    return 0;
}
static void emit_safe_name(SB *s, const char *name) {
    if (is_c_keyword(name)) sb_printf(s, "xs_%s", name);
    else if (is_c_libc_name(name)) sb_printf(s, "xs_user_%s", name);
    else sb_add(s, name);
}
/* mirror of emit_safe_name into a caller buffer; used when we need to
 * splice the mangled name into a printf format. returns either name or
 * a pointer into buf. */
static const char *safe_name(const char *name, char *buf, size_t bufsz) {
    if (!name) return "_";
    if (is_c_keyword(name)) { snprintf(buf, bufsz, "xs_%s", name); return buf; }
    if (is_c_libc_name(name)) { snprintf(buf, bufsz, "xs_user_%s", name); return buf; }
    return name;
}

/* class name tracking so `MyClass()` -> empty map + init() call. */
#define MAX_CLASSES 32
static struct { const char *name; int has_init; } classes[MAX_CLASSES];
static int n_classes = 0;
static void register_class(const char *name, int has_init) {
    for (int i = 0; i < n_classes; i++)
        if (strcmp(classes[i].name, name) == 0) {
            classes[i].has_init = has_init || classes[i].has_init;
            return;
        }
    if (n_classes < MAX_CLASSES) {
        classes[n_classes].name = name;
        classes[n_classes].has_init = has_init;
        n_classes++;
    }
}
static int is_class(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < n_classes; i++)
        if (strcmp(classes[i].name, name) == 0) return 1;
    return 0;
}
static int class_has_init(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < n_classes; i++)
        if (strcmp(classes[i].name, name) == 0) return classes[i].has_init;
    return 0;
}

/* struct impl tracking (like actor tracking) */
#define MAX_IMPL_TYPES 32
#define MAX_IMPL_METHODS 32
static struct {
    const char *type_name;
    const char *methods[MAX_IMPL_METHODS];
    int n_methods;
} impl_types[MAX_IMPL_TYPES];
static int n_impl_types = 0;

static void impl_record_method(const char *type_name, const char *method) {
    if (!type_name || !method) return;
    for (int i = 0; i < n_impl_types; i++) {
        if (strcmp(impl_types[i].type_name, type_name) == 0) {
            for (int j = 0; j < impl_types[i].n_methods; j++)
                if (impl_types[i].methods[j] &&
                    strcmp(impl_types[i].methods[j], method) == 0) return;
            if (impl_types[i].n_methods < MAX_IMPL_METHODS)
                impl_types[i].methods[impl_types[i].n_methods++] = method;
            return;
        }
    }
}
static int impl_type_has_method(const char *type_name, const char *method) {
    if (!type_name || !method) return 0;
    for (int i = 0; i < n_impl_types; i++) {
        if (strcmp(impl_types[i].type_name, type_name) == 0) {
            for (int j = 0; j < impl_types[i].n_methods; j++)
                if (impl_types[i].methods[j] &&
                    strcmp(impl_types[i].methods[j], method) == 0) return 1;
            return 0;
        }
    }
    return 0;
}
static int impl_method_in_any_impl(const char *method) {
    if (!method) return 0;
    for (int i = 0; i < n_impl_types; i++)
        for (int j = 0; j < impl_types[i].n_methods; j++)
            if (impl_types[i].methods[j] &&
                strcmp(impl_types[i].methods[j], method) == 0) return 1;
    return 0;
}
#define MAX_STRUCT_VARS 64
static struct { const char *var_name; const char *type_name; } struct_vars[MAX_STRUCT_VARS];
static int n_struct_vars = 0;

static void register_impl_type(const char *name) {
    for (int i = 0; i < n_impl_types; i++)
        if (strcmp(impl_types[i].type_name, name) == 0) return;
    if (n_impl_types < MAX_IMPL_TYPES) impl_types[n_impl_types++].type_name = name;
}
static const char *find_impl_type(const char *name) {
    for (int i = 0; i < n_impl_types; i++)
        if (strcmp(impl_types[i].type_name, name) == 0) return name;
    return NULL;
}
static void register_struct_var(const char *var, const char *type) {
    if (n_struct_vars < MAX_STRUCT_VARS) {
        struct_vars[n_struct_vars].var_name = var;
        struct_vars[n_struct_vars].type_name = type;
        n_struct_vars++;
    }
}
static const char *lookup_struct_var(const char *var) {
    for (int i = 0; i < n_struct_vars; i++)
        if (strcmp(struct_vars[i].var_name, var) == 0) return struct_vars[i].type_name;
    return NULL;
}

/* lambda tracking */
#define MAX_LAMBDAS 128
typedef struct {
    int id;
    Node *node;        /* the lambda/fn-expr node */
    int n_params;
    /* simple capture list: names of free variables */
    const char *captures[16];
    int n_captures;
} LambdaInfo;
static LambdaInfo lambdas[MAX_LAMBDAS];
static int n_lambdas = 0;
static int lambda_counter = 0;

static int register_lambda(Node *n) {
    /* check if already registered */
    for (int i = 0; i < n_lambdas; i++)
        if (lambdas[i].node == n) return lambdas[i].id;
    int id = lambda_counter++;
    if (n_lambdas < MAX_LAMBDAS) {
        lambdas[n_lambdas].id = id;
        lambdas[n_lambdas].node = n;
        lambdas[n_lambdas].n_params = VAL_TAG(n) == NODE_LAMBDA ?
            n->lambda.params.len : 0;
        n_lambdas++;
    }
    return id;
}

/* function param count tracking for default param padding and
 * overload-by-arity dispatch. xs allows redeclaring `fn calc(x)` and
 * `fn calc(x, y)`; C does not. We mangle to `calc_1` / `calc_2` and
 * route the call site by arity. */
#define MAX_FN_SIGS 128
static struct { const char *name; int n_params; int is_pure; } fn_sigs[MAX_FN_SIGS];
static int n_fn_sigs = 0;
static void register_fn_sig_pure(const char *name, int n_params, int is_pure) {
    if (n_fn_sigs < MAX_FN_SIGS) {
        fn_sigs[n_fn_sigs].name = name;
        fn_sigs[n_fn_sigs].n_params = n_params;
        fn_sigs[n_fn_sigs].is_pure = is_pure;
        n_fn_sigs++;
    }
}
static int lookup_fn_param_count(const char *name) {
    /* preserve "first wins" lookup semantics that callers depend on for
     * "is this a known function" checks. */
    for (int i = 0; i < n_fn_sigs; i++)
        if (strcmp(fn_sigs[i].name, name) == 0) return fn_sigs[i].n_params;
    return -1;
}
static int lookup_fn_is_pure(const char *name) {
    for (int i = 0; i < n_fn_sigs; i++)
        if (strcmp(fn_sigs[i].name, name) == 0) return fn_sigs[i].is_pure;
    return 0;
}
static int count_fn_overloads(const char *name) {
    int n = 0;
    if (!name) return 0;
    for (int i = 0; i < n_fn_sigs; i++)
        if (strcmp(fn_sigs[i].name, name) == 0) n++;
    return n;
}
/* if name has multiple decls, return mangled `name_<n_params>`; else
 * return name as-is. Caller passes a buffer; the returned pointer either
 * points into buf or is the original name. */
static const char *mangle_overload(const char *name, int n_params, char *buf, size_t bufsz) {
    if (count_fn_overloads(name) > 1) {
        snprintf(buf, bufsz, "%s_%d", name, n_params);
        return buf;
    }
    return name;
}
/* find the arity of a known overload set that best matches argc. Prefers
 * exact match; otherwise returns -1. */
static int pick_overload_arity(const char *name, int argc) {
    int exact = -1, best = -1;
    for (int i = 0; i < n_fn_sigs; i++) {
        if (strcmp(fn_sigs[i].name, name) != 0) continue;
        if (fn_sigs[i].n_params == argc) exact = argc;
        if (fn_sigs[i].n_params >= argc &&
            (best < 0 || fn_sigs[i].n_params < best)) best = fn_sigs[i].n_params;
    }
    return exact >= 0 ? exact : best;
}

/* track if we're inside an impl/actor method (self is a pointer) */
static int in_method_body = 0;
static const char *current_class_name = NULL;
static Node *program_root = NULL;

/* track which lambda we're currently emitting (for capture access) */
static LambdaInfo *current_lambda = NULL;

/* track boxed variables in the current enclosing scope */
#define MAX_BOXED 32
static const char *boxed_vars[MAX_BOXED];
static int n_boxed = 0;

static int is_boxed_var(const char *name) {
    for (int i = 0; i < n_boxed; i++)
        if (strcmp(boxed_vars[i], name) == 0) return 1;
    return 0;
}
static void add_boxed_var(const char *name) {
    if (n_boxed < MAX_BOXED) boxed_vars[n_boxed++] = name;
}

/* Top-level let/var/const names. Emitted as static xs_val at file scope
 * so named functions and the lambdas inside them can resolve them; the
 * old behaviour declared them as locals of the wrapped main(), making
 * everything outside main fail to compile or pick up the wrong scope. */
#define MAX_TOP_VARS 256
static const char *top_level_vars[MAX_TOP_VARS];
static int n_top_vars = 0;

static int is_top_level_var(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < n_top_vars; i++)
        if (strcmp(top_level_vars[i], name) == 0) return 1;
    return 0;
}
static void add_top_level_var(const char *name) {
    if (!name) return;
    for (int i = 0; i < n_top_vars; i++)
        if (strcmp(top_level_vars[i], name) == 0) return;
    if (n_top_vars < MAX_TOP_VARS) top_level_vars[n_top_vars++] = name;
}

/* Walk only the direct children of a NODE_PROGRAM and register any
 * binding names introduced at that level. Anything nested inside a
 * function body, lambda, struct decl, etc. is intentionally skipped. */
static void scan_top_level_vars(Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;
    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *st = program->program.stmts.items[i];
        if (!st) continue;
        if (VAL_TAG(st) == NODE_LET || VAL_TAG(st) == NODE_VAR) {
            if (st->let.name) add_top_level_var(st->let.name);
        } else if (VAL_TAG(st) == NODE_CONST) {
            if (st->const_.name) add_top_level_var(st->const_.name);
        }
    }
}

/* actor field rewriting: when emitting actor method bodies, identifiers
   that match state fields get rewritten to self->field */
static const char **actor_fields = NULL;
static int n_actor_fields = 0;

static int is_actor_field(const char *name) {
    for (int i = 0; i < n_actor_fields; i++)
        if (strcmp(actor_fields[i], name) == 0) return 1;
    return 0;
}

/* free variable collector for lambda capture analysis.
   Collects identifiers used in the body that are NOT declared locally within it. */
static void collect_local_decls(Node *n, const char **out, int *nout, int max) {
    if (!n || *nout >= max) return;
    switch (VAL_TAG(n)) {
    case NODE_LET: case NODE_VAR:
        if (n->let.name && *nout < max) out[(*nout)++] = n->let.name;
        break;
    case NODE_CONST:
        if (n->const_.name && *nout < max) out[(*nout)++] = n->const_.name;
        break;
    case NODE_FOR:
        /* for loop pattern declares a variable */
        if (n->for_loop.pattern && VAL_TAG(n->for_loop.pattern) == NODE_PAT_IDENT)
            if (*nout < max) out[(*nout)++] = n->for_loop.pattern->pat_ident.name;
        collect_local_decls(n->for_loop.body, out, nout, max);
        break;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            collect_local_decls(n->block.stmts.items[i], out, nout, max);
        break;
    case NODE_EXPR_STMT:
        collect_local_decls(n->expr_stmt.expr, out, nout, max);
        break;
    case NODE_IF:
        collect_local_decls(n->if_expr.then, out, nout, max);
        collect_local_decls(n->if_expr.else_branch, out, nout, max);
        break;
    default: break;
    }
}

static void collect_idents(Node *n, const char **out, int *nout, int max) {
    if (!n || *nout >= max) return;
    if (VAL_TAG(n) == NODE_IDENT) {
        for (int i = 0; i < *nout; i++)
            if (strcmp(out[i], n->ident.name) == 0) return;
        out[(*nout)++] = n->ident.name;
        return;
    }
    switch (VAL_TAG(n)) {
    case NODE_BINOP: collect_idents(n->binop.left, out, nout, max);
                     collect_idents(n->binop.right, out, nout, max); break;
    case NODE_UNARY: collect_idents(n->unary.expr, out, nout, max); break;
    case NODE_CALL: {
        collect_idents(n->call.callee, out, nout, max);
        for (int i=0;i<n->call.args.len;i++) collect_idents(n->call.args.items[i], out, nout, max);
        break;
    }
    case NODE_METHOD_CALL: {
        collect_idents(n->method_call.obj, out, nout, max);
        for (int i=0;i<n->method_call.args.len;i++) collect_idents(n->method_call.args.items[i], out, nout, max);
        break;
    }
    case NODE_INDEX: collect_idents(n->index.obj, out, nout, max);
                     collect_idents(n->index.index, out, nout, max); break;
    case NODE_FIELD: collect_idents(n->field.obj, out, nout, max); break;
    case NODE_ASSIGN: collect_idents(n->assign.target, out, nout, max);
                      collect_idents(n->assign.value, out, nout, max); break;
    case NODE_RETURN: collect_idents(n->ret.value, out, nout, max); break;
    case NODE_IF: collect_idents(n->if_expr.cond, out, nout, max);
                  collect_idents(n->if_expr.then, out, nout, max);
                  collect_idents(n->if_expr.else_branch, out, nout, max); break;
    case NODE_BLOCK: {
        for (int i=0;i<n->block.stmts.len;i++) collect_idents(n->block.stmts.items[i], out, nout, max);
        collect_idents(n->block.expr, out, nout, max);
        break;
    }
    case NODE_EXPR_STMT: collect_idents(n->expr_stmt.expr, out, nout, max); break;
    case NODE_LET: case NODE_VAR: collect_idents(n->let.value, out, nout, max); break;
    case NODE_LAMBDA: {
        /* A nested lambda's free vars are also free vars of the outer,
         * so the outer can box and forward them. Subtract the inner
         * lambda's own params and local decls so its bindings don't leak
         * out. Without this the outer's capture list misses anything
         * referenced exclusively from a nested lambda body, and the
         * generated C tries to forward an undeclared __box_x. */
        const char *inner_idents[64];
        int n_inner = 0;
        collect_idents(n->lambda.body, inner_idents, &n_inner, 64);
        const char *inner_locals[64];
        int n_ilocals = 0;
        collect_local_decls(n->lambda.body, inner_locals, &n_ilocals, 64);
        for (int i = 0; i < n_inner; i++) {
            const char *name = inner_idents[i];
            int skip = 0;
            for (int p = 0; p < n->lambda.params.len && !skip; p++)
                if (n->lambda.params.items[p].name &&
                    strcmp(n->lambda.params.items[p].name, name) == 0) skip = 1;
            for (int d = 0; d < n_ilocals && !skip; d++)
                if (strcmp(inner_locals[d], name) == 0) skip = 1;
            if (!skip) {
                if (*nout >= max) break;
                int dup = 0;
                for (int j = 0; j < *nout; j++)
                    if (strcmp(out[j], name) == 0) { dup = 1; break; }
                if (!dup) out[(*nout)++] = name;
            }
        }
        break;
    }
    case NODE_FN_DECL: {
        /* Same idea for nested fn-decls: their free vars must propagate
         * to the enclosing scope's capture list. (Lowering normally
         * rewrites these into lambdas before scan_lambdas runs, but
         * be defensive in case the order changes.) */
        if (!n->fn_decl.body) break;
        const char *inner_idents[64];
        int n_inner = 0;
        collect_idents(n->fn_decl.body, inner_idents, &n_inner, 64);
        const char *inner_locals[64];
        int n_ilocals = 0;
        collect_local_decls(n->fn_decl.body, inner_locals, &n_ilocals, 64);
        for (int i = 0; i < n_inner; i++) {
            const char *name = inner_idents[i];
            int skip = 0;
            for (int p = 0; p < n->fn_decl.params.len && !skip; p++)
                if (n->fn_decl.params.items[p].name &&
                    strcmp(n->fn_decl.params.items[p].name, name) == 0) skip = 1;
            for (int d = 0; d < n_ilocals && !skip; d++)
                if (strcmp(inner_locals[d], name) == 0) skip = 1;
            if (!skip) {
                if (*nout >= max) break;
                int dup = 0;
                for (int j = 0; j < *nout; j++)
                    if (strcmp(out[j], name) == 0) { dup = 1; break; }
                if (!dup) out[(*nout)++] = name;
            }
        }
        break;
    }
    default: break;
    }
}

/* recursive lambda scanner */
static void scan_lambdas(Node *n) {
    if (!n) return;
    if (VAL_TAG(n) == NODE_LAMBDA) {
        int lid = register_lambda(n);
        /* find captures: idents used in body minus params and local declarations */
        const char *all_idents[64];
        int n_all = 0;
        collect_idents(n->lambda.body, all_idents, &n_all, 64);

        /* collect locally declared names inside the lambda body */
        const char *local_decls[64];
        int n_locals = 0;
        collect_local_decls(n->lambda.body, local_decls, &n_locals, 64);

        static const char *skip_names[] = {
            "println","print","str","len","type","assert","assert_eq",
            "int","float","true","false","null","sqrt","abs","not",
            "channel","range","map","filter","reduce","sort","Err","Ok",
            "assert_eq","panic","input","spawn","await",NULL
        };

        LambdaInfo *li = NULL;
        for (int i = 0; i < n_lambdas; i++)
            if (lambdas[i].id == lid) { li = &lambdas[i]; break; }
        if (li) {
            li->n_captures = 0;
            for (int i = 0; i < n_all && li->n_captures < 16; i++) {
                const char *name = all_idents[i];
                /* skip params */
                int skip = 0;
                for (int p = 0; p < n->lambda.params.len && !skip; p++)
                    if (n->lambda.params.items[p].name &&
                        strcmp(n->lambda.params.items[p].name, name) == 0) skip = 1;
                /* skip local declarations */
                for (int d = 0; d < n_locals && !skip; d++)
                    if (strcmp(local_decls[d], name) == 0) skip = 1;
                /* skip known builtins/globals */
                for (int k = 0; skip_names[k] && !skip; k++)
                    if (strcmp(skip_names[k], name) == 0) skip = 1;
                /* skip known functions */
                if (!skip && lookup_fn_param_count(name) >= 0) skip = 1;
                if (!skip) li->captures[li->n_captures++] = name;
            }
        }
        /* scan body too for nested lambdas */
        if (n->lambda.body) scan_lambdas(n->lambda.body);
        return;
    }
    /* walk children looking for lambdas */
    switch (VAL_TAG(n)) {
    case NODE_PROGRAM: for (int i=0;i<n->program.stmts.len;i++) scan_lambdas(n->program.stmts.items[i]); break;
    case NODE_BLOCK: {
        for (int i=0;i<n->block.stmts.len;i++) scan_lambdas(n->block.stmts.items[i]);
        if (n->block.expr) scan_lambdas(n->block.expr);
        break;
    }
    case NODE_BINOP: scan_lambdas(n->binop.left); scan_lambdas(n->binop.right); break;
    case NODE_UNARY: scan_lambdas(n->unary.expr); break;
    case NODE_CALL: {
        scan_lambdas(n->call.callee);
        for (int i=0;i<n->call.args.len;i++) scan_lambdas(n->call.args.items[i]);
        break;
    }
    case NODE_METHOD_CALL: {
        scan_lambdas(n->method_call.obj);
        for (int i=0;i<n->method_call.args.len;i++) scan_lambdas(n->method_call.args.items[i]);
        break;
    }
    case NODE_INDEX: scan_lambdas(n->index.obj); scan_lambdas(n->index.index); break;
    case NODE_FIELD: scan_lambdas(n->field.obj); break;
    case NODE_ASSIGN: scan_lambdas(n->assign.target); scan_lambdas(n->assign.value); break;
    case NODE_IF: scan_lambdas(n->if_expr.cond); scan_lambdas(n->if_expr.then);
                  scan_lambdas(n->if_expr.else_branch); break;
    case NODE_FOR: scan_lambdas(n->for_loop.iter); scan_lambdas(n->for_loop.body); break;
    case NODE_WHILE: scan_lambdas(n->while_loop.cond); scan_lambdas(n->while_loop.body); break;
    case NODE_RETURN: scan_lambdas(n->ret.value); break;
    case NODE_LET: case NODE_VAR: scan_lambdas(n->let.value); break;
    case NODE_CONST: scan_lambdas(n->const_.value); break;
    case NODE_FN_DECL: scan_lambdas(n->fn_decl.body); break;
    case NODE_EXPR_STMT: scan_lambdas(n->expr_stmt.expr); break;
    case NODE_TRY: scan_lambdas(n->try_.body); scan_lambdas(n->try_.finally_block); break;
    case NODE_THROW: scan_lambdas(n->throw_.value); break;
    case NODE_MATCH: scan_lambdas(n->match.subject);
                     for (int i=0;i<n->match.arms.len;i++) {
                         scan_lambdas(n->match.arms.items[i].guard);
                         scan_lambdas(n->match.arms.items[i].body);
                     } break;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE: {
        for (int i=0;i<n->lit_array.elems.len;i++) scan_lambdas(n->lit_array.elems.items[i]);
        break;
    }
    case NODE_LIT_MAP: {
        for (int i=0;i<n->lit_map.vals.len;i++) scan_lambdas(n->lit_map.vals.items[i]);
        break;
    }
    case NODE_IMPL_DECL: {
        for (int i=0;i<n->impl_decl.members.len;i++) scan_lambdas(n->impl_decl.members.items[i]);
        break;
    }
    case NODE_ACTOR_DECL: {
        for (int i=0;i<n->actor_decl.methods.len;i++) scan_lambdas(n->actor_decl.methods.items[i]);
        break;
    }
    case NODE_NURSERY: scan_lambdas(n->nursery_.body); break;
    case NODE_SPAWN: scan_lambdas(n->spawn_.expr); break;
    case NODE_AWAIT: scan_lambdas(n->await_.expr); break;
    case NODE_SEND_EXPR: scan_lambdas(n->send_expr.target); scan_lambdas(n->send_expr.message); break;
    case NODE_RANGE: scan_lambdas(n->range.start); scan_lambdas(n->range.end); break;
    case NODE_LIST_COMP: {
        scan_lambdas(n->list_comp.element);
        for (int i=0;i<n->list_comp.clause_iters.len;i++) scan_lambdas(n->list_comp.clause_iters.items[i]);
        for (int i=0;i<n->list_comp.clause_conds.len;i++) scan_lambdas(n->list_comp.clause_conds.items[i]);
        break;
    }
    default: break;
    }
}

/* pre-scan to collect actor declarations and actor variable bindings */
static void prescan_stmts(Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;
    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *st = program->program.stmts.items[i];
        if (!st) continue;
        /* register actor declarations */
        if (VAL_TAG(st) == NODE_EXPR_STMT && st->expr_stmt.expr &&
            VAL_TAG(st->expr_stmt.expr) == NODE_ACTOR_DECL) {
            Node *ad = st->expr_stmt.expr;
            if (ad->actor_decl.name) register_actor(ad->actor_decl.name);
        }
        if (VAL_TAG(st) == NODE_ACTOR_DECL && st->actor_decl.name)
            register_actor(st->actor_decl.name);
        /* register let x = spawn ActorName */
        if ((VAL_TAG(st) == NODE_LET || VAL_TAG(st) == NODE_VAR) && st->let.name && st->let.value) {
            Node *val = st->let.value;
            if (VAL_TAG(val) == NODE_SPAWN && val->spawn_.expr &&
                VAL_TAG(val->spawn_.expr) == NODE_IDENT) {
                const char *aname = val->spawn_.expr->ident.name;
                if (find_actor(aname))
                    register_actor_var(st->let.name, aname);
            }
            /* register let x = StructName { ... } */
            if (VAL_TAG(val) == NODE_STRUCT_INIT && val->struct_init.path) {
                const char *sname = val->struct_init.path;
                if (find_impl_type(sname))
                    register_struct_var(st->let.name, sname);
            }
            /* register let x = ClassName(...) */
            if (VAL_TAG(val) == NODE_CALL && val->call.callee &&
                VAL_TAG(val->call.callee) == NODE_IDENT) {
                const char *cname = val->call.callee->ident.name;
                if (is_class(cname))
                    register_struct_var(st->let.name, cname);
            }
            /* register let x = y.method(...) where y is a known struct var */
            if (VAL_TAG(val) == NODE_METHOD_CALL && val->method_call.obj &&
                VAL_TAG(val->method_call.obj) == NODE_IDENT) {
                const char *otype = lookup_struct_var(val->method_call.obj->ident.name);
                if (otype)
                    register_struct_var(st->let.name, otype);
            }
        }
        /* register impl declarations */
        if (VAL_TAG(st) == NODE_IMPL_DECL && st->impl_decl.type_name) {
            register_impl_type(st->impl_decl.type_name);
            for (int j = 0; j < st->impl_decl.members.len; j++) {
                Node *m = st->impl_decl.members.items[j];
                if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.name)
                    impl_record_method(st->impl_decl.type_name, m->fn_decl.name);
            }
        }
        /* register class declarations */
        if (VAL_TAG(st) == NODE_CLASS_DECL && st->class_decl.name) {
            int has_init = 0;
            for (int j = 0; j < st->class_decl.members.len; j++) {
                Node *m = st->class_decl.members.items[j];
                if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.name &&
                    (strcmp(m->fn_decl.name, "init") == 0 ||
                     strcmp(m->fn_decl.name, "new") == 0))
                    has_init = 1;
            }
            register_class(st->class_decl.name, has_init);
        }
        /* register function signatures for default param padding */
        if (VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.name)
            register_fn_sig_pure(st->fn_decl.name, st->fn_decl.params.len,
                                 st->fn_decl.inferred_pure);
    }
}

/* helpers */
static int is_callee_name(Node *callee, const char *name) {
    return callee && VAL_TAG(callee) == NODE_IDENT && strcmp(callee->ident.name, name) == 0;
}

static int is_main_fn(Node *n) {
    return VAL_TAG(n) == NODE_FN_DECL && n->fn_decl.name && strcmp(n->fn_decl.name, "main") == 0;
}

static void emit_params_c(SB *s, ParamList *pl) {
    sb_addc(s, '(');
    if (pl->len == 0) {
        sb_add(s, "void");
    } else {
        for (int i = 0; i < pl->len; i++) {
            if (i) sb_add(s, ", ");
            Param *p = &pl->items[i];
            sb_add(s, "xs_val ");
            if (p->name) emit_safe_name(s, p->name);
            else sb_add(s, "_");
        }
    }
    sb_addc(s, ')');
}

/* collect defer bodies from a block */
static int block_has_defers(Node *block) {
    if (!block || VAL_TAG(block) != NODE_BLOCK) return 0;
    for (int i = 0; i < block->block.stmts.len; i++) {
        if (block->block.stmts.items[i] && VAL_TAG(block->block.stmts.items[i]) == NODE_DEFER)
            return 1;
    }
    return 0;
}

static void emit_deferred_cleanup(SB *s, Node *block, int depth) {
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

/* expression emitter */
static void emit_expr(SB *s, Node *n, int depth) {
    if (!n) { sb_add(s, "XS_NULL"); return; }
    switch (VAL_TAG(n)) {
    case NODE_LIT_INT:
        sb_printf(s, "XS_INT(%" PRId64 ")", n->lit_int.ival);
        break;
    case NODE_LIT_BIGINT: {
        /* Emit a string-payload bigint so values past i64 stay exact.
           XS_INT(...) wraps any literal larger than INT64_MAX into the
           low 64 bits, which silently corrupts the value. */
        const char *bs = n->lit_bigint.bigint_str ? n->lit_bigint.bigint_str : "0";
        sb_add(s, "XS_BIGINT(\"");
        for (const char *p = bs; *p; p++) {
            if (*p == '"' || *p == '\\') sb_addc(s, '\\');
            sb_addc(s, *p);
        }
        sb_add(s, "\")");
        break;
    }
    case NODE_LIT_FLOAT:
        /* %.17g preserves the exact double value; default %g uses 6 sig
         * figs, which truncates literals like 123456789.0. */
        sb_printf(s, "XS_FLOAT(%.17g)", n->lit_float.fval);
        break;
    case NODE_LIT_DURATION:
        /* The c backend treats durations as plain int64 ns counts; the
           real Duration type lives in the interp/vm. */
        sb_printf(s, "XS_INT(%lldLL)", (long long)n->lit_duration.ns);
        break;
    case NODE_LIT_STRING:
        sb_add(s, "XS_STR(\"");
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
        sb_add(s, "\")");
        break;
    case NODE_INTERP_STRING: {
        /* String interpolation -> xs_sprintf with format + args */
        sb_add(s, "xs_sprintf(");
        /* Build format string from parts */
        NodeList *parts = &n->lit_string.parts;
        sb_addc(s, '"');
        for (int i = 0; i < parts->len; i++) {
            Node *part = parts->items[i];
            if (VAL_TAG(part) == NODE_LIT_STRING && part->lit_string.sval) {
                for (const char *p = part->lit_string.sval; *p; p++) {
                    if (*p == '"') sb_add(s, "\\\"");
                    else if (*p == '\\') sb_add(s, "\\\\");
                    else if (*p == '\n') sb_add(s, "\\n");
                    else if (*p == '%') sb_add(s, "%%");
                    else sb_addc(s, *p);
                }
            } else {
                sb_add(s, "%s");
            }
        }
        sb_addc(s, '"');
        /* Now emit the expression arguments */
        for (int i = 0; i < parts->len; i++) {
            Node *part = parts->items[i];
            if (VAL_TAG(part) != NODE_LIT_STRING) {
                sb_add(s, ", xs_to_str(");
                emit_expr(s, part, depth);
                sb_addc(s, ')');
            }
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_LIT_BOOL:
        sb_printf(s, "XS_BOOL(%d)", n->lit_bool.bval ? 1 : 0);
        break;
    case NODE_LIT_NULL:
        sb_add(s, "XS_NULL");
        break;
    case NODE_LIT_CHAR:
        sb_printf(s, "XS_INT('%c')", n->lit_char.cval);
        break;
    case NODE_LIT_ARRAY:
    case NODE_LIT_TUPLE: {
        const char *ctor = (VAL_TAG(n) == NODE_LIT_TUPLE) ? "xs_tuple" : "xs_array";
        sb_printf(s, "%s(%d", ctor, n->lit_array.elems.len);
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            sb_add(s, ", ");
            emit_expr(s, n->lit_array.elems.items[i], depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_LIT_MAP: {
        /* Spread (key=NODE_SPREAD, val=NULL) means "copy all entries from
         * source into the result, then layer this literal's other pairs on
         * top." Walk once to detect; if we find a spread, lower to a stmt-
         * expr that builds an empty map and inserts one pair at a time.
         * Otherwise the cheap xs_map(n, k1, v1, ...) varargs path. */
        int has_spread = 0;
        for (int i = 0; i < n->lit_map.keys.len; i++) {
            Node *mk = n->lit_map.keys.items[i];
            if (mk && VAL_TAG(mk) == NODE_SPREAD) { has_spread = 1; break; }
        }
        if (has_spread) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __mr_%d = xs_map(0);\n", mid);
            for (int i = 0; i < n->lit_map.keys.len; i++) {
                Node *mk = n->lit_map.keys.items[i];
                if (mk && VAL_TAG(mk) == NODE_SPREAD) {
                    sb_indent(s, depth + 1);
                    sb_printf(s, "{ xs_val __sp_%d = ", mid);
                    emit_expr(s, mk->spread.expr, depth);
                    sb_printf(s, "; if (__sp_%d.tag == 6 && __sp_%d.p) {\n", mid, mid);
                    sb_indent(s, depth + 2);
                    sb_printf(s, "xs_hmap *__hm_%d = (xs_hmap*)__sp_%d.p;\n", mid, mid);
                    sb_indent(s, depth + 2);
                    sb_printf(s, "for (int __k = 0; __k < __hm_%d->len; __k++)\n", mid);
                    sb_indent(s, depth + 3);
                    sb_printf(s, "xs_map_put(&__mr_%d, XS_STR(__hm_%d->keys[__k]), __hm_%d->vals[__k]);\n", mid, mid, mid);
                    sb_indent(s, depth + 1);
                    sb_add(s, "} }\n");
                } else {
                    sb_indent(s, depth + 1);
                    sb_printf(s, "xs_map_put(&__mr_%d, ", mid);
                    if (mk && VAL_TAG(mk) == NODE_IDENT)
                        sb_printf(s, "XS_STR(\"%s\")", mk->ident.name);
                    else
                        emit_expr(s, mk, depth);
                    sb_add(s, ", ");
                    emit_expr(s, n->lit_map.vals.items[i], depth);
                    sb_add(s, ");\n");
                }
            }
            sb_indent(s, depth);
            sb_printf(s, "__mr_%d; })", mid);
            break;
        }
        sb_printf(s, "xs_map(%d", n->lit_map.keys.len);
        for (int i = 0; i < n->lit_map.keys.len; i++) {
            sb_add(s, ", ");
            /* map keys: if ident, emit as string */
            Node *mk = n->lit_map.keys.items[i];
            if (mk && VAL_TAG(mk) == NODE_IDENT)
                sb_printf(s, "XS_STR(\"%s\")", mk->ident.name);
            else
                emit_expr(s, mk, depth);
            sb_add(s, ", ");
            emit_expr(s, n->lit_map.vals.items[i], depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_IDENT: {
        int wrap_del = is_deleted_var(n->ident.name);
        if (wrap_del) sb_printf(s, "xs_check_deleted(");
        if (n_actor_fields > 0 && is_actor_field(n->ident.name))
            sb_printf(s, "self->%s", n->ident.name);
        else if (current_lambda) {
            /* inside a lambda: check if capture */
            int cap_idx = -1;
            for (int ci = 0; ci < current_lambda->n_captures; ci++)
                if (strcmp(current_lambda->captures[ci], n->ident.name) == 0)
                    { cap_idx = ci; break; }
            if (cap_idx >= 0)
                sb_printf(s, "(*((xs_val**)__env)[%d])", cap_idx);
            else
                emit_safe_name(s, n->ident.name);
        } else if (is_boxed_var(n->ident.name)) {
            /* in enclosing scope: access through box */
            sb_printf(s, "(*__box_%s)", n->ident.name);
        } else if (lookup_fn_param_count(n->ident.name) >= 0) {
            /* bare reference to a top-level function: wrap as a callable
             * xs_val so it can be passed to .map / .filter / etc. The
             * wrapper is generated alongside the function itself. */
            sb_printf(s, "%s(__xs_wrap_%s, NULL)",
                      lookup_fn_is_pure(n->ident.name) ? "xs_fn_new_pure" : "xs_fn_new",
                      n->ident.name);
        } else
            emit_safe_name(s, n->ident.name);
        if (wrap_del) sb_printf(s, ", \"%s\")", n->ident.name);
        break;
    }
    case NODE_BINOP: {
        const char *op = n->binop.op;
        if (strcmp(op, "+") == 0) {
            sb_add(s, "xs_add(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "-") == 0) {
            sb_add(s, "xs_sub(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "*") == 0) {
            sb_add(s, "xs_mul(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "/") == 0) {
            sb_add(s, "xs_div(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "%") == 0) {
            sb_add(s, "xs_mod(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "//") == 0) {
            sb_add(s, "xs_idiv(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "**") == 0) {
            /* Match VM/interp: int**int (with non-negative RHS) stays
               int; otherwise promote to float. xs_pow does the check
               at runtime using the tag of both operands. */
            sb_add(s, "xs_pow(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, ")");
        } else if (strcmp(op, "++") == 0) {
            /* polymorphic ++: arrays concat, strings concatenate */
            sb_add(s, "xs_concat(1, ");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "==") == 0) {
            sb_add(s, "XS_BOOL(xs_eq(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "!=") == 0) {
            sb_add(s, "XS_BOOL(!xs_eq(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                   strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
            sb_add(s, "XS_BOOL(xs_cmp(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, ") ");
            sb_add(s, op);
            sb_add(s, " 0)");
        } else if (strcmp(op, "<=>") == 0) {
            /* spaceship: -1 / 0 / 1 */
            sb_add(s, "XS_INT(xs_cmp(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "and") == 0) {
            sb_add(s, "XS_BOOL(xs_truthy(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ") && xs_truthy(");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "or") == 0) {
            sb_add(s, "XS_BOOL(xs_truthy(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ") || xs_truthy(");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                   strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
                   strcmp(op, ">>") == 0) {
            sb_add(s, "XS_INT((");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ").i ");
            sb_add(s, op);
            sb_add(s, " (");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, ").i)");
        } else if (strcmp(op, "is") == 0) {
            /* type check: val is int/str/bool/float/null/array/map */
            sb_add(s, "xs_is_type(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            if (n->binop.right && VAL_TAG(n->binop.right) == NODE_LIT_STRING &&
                n->binop.right->lit_string.sval)
                sb_printf(s, "\"%s\"", n->binop.right->lit_string.sval);
            else if (n->binop.right && VAL_TAG(n->binop.right) == NODE_IDENT)
                sb_printf(s, "\"%s\"", n->binop.right->ident.name);
            else
                sb_add(s, "\"?\"");
            sb_addc(s, ')');
        } else if (strcmp(op, "??") == 0) {
            /* null coalescing: if left is null, use right */
            sb_add(s, "((");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ").tag == 4 ? ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, " : ");
            emit_expr(s, n->binop.left, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "&&") == 0) {
            /* short-circuit AND: returns the left value if falsy, else
               the right value, mirroring VM/interp semantics. */
            sb_add(s, "({ xs_val __l = ");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, "; xs_truthy(__l) ? (");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, ") : __l; })");
        } else if (strcmp(op, "||") == 0) {
            sb_add(s, "({ xs_val __l = ");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, "; xs_truthy(__l) ? __l : (");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "); })");
        } else if (strcmp(op, "is") == 0) {
            /* Right side is a type-name string. Bool-ify the tag check. */
            sb_add(s, "XS_BOOL(xs_is(");
            emit_expr(s, n->binop.left, depth);
            sb_add(s, ", ");
            emit_expr(s, n->binop.right, depth);
            sb_add(s, "))");
        } else {
            sb_add(s, "/* binop ");
            sb_add(s, op);
            sb_add(s, " */ XS_NULL");
        }
        break;
    }
    case NODE_UNARY: {
        const char *op = n->unary.op;
        if (strcmp(op, "-") == 0) {
            sb_add(s, "xs_neg(");
            emit_expr(s, n->unary.expr, depth);
            sb_addc(s, ')');
        } else if (strcmp(op, "not") == 0 || strcmp(op, "!") == 0) {
            sb_add(s, "XS_BOOL(!xs_truthy(");
            emit_expr(s, n->unary.expr, depth);
            sb_add(s, "))");
        } else if (strcmp(op, "~") == 0) {
            sb_add(s, "XS_INT(~(");
            emit_expr(s, n->unary.expr, depth);
            sb_add(s, ").i)");
        } else {
            sb_add(s, "/* unary ");
            sb_add(s, op);
            sb_add(s, " */ ");
            emit_expr(s, n->unary.expr, depth);
        }
        break;
    }
    case NODE_CALL: {
        if (is_callee_name(n->call.callee, "println")) {
            int an = n->call.args.len;
            if (an <= 1) {
                sb_add(s, "xs_println(");
                if (an == 1) emit_expr(s, n->call.args.items[0], depth);
                else sb_add(s, "XS_NULL");
                sb_addc(s, ')');
            } else {
                sb_addc(s, '(');
                for (int i = 0; i < an - 1; i++) {
                    sb_add(s, "xs_print(");
                    emit_expr(s, n->call.args.items[i], depth);
                    sb_add(s, "), xs_print(XS_STR(\" \")), ");
                }
                sb_add(s, "xs_println(");
                emit_expr(s, n->call.args.items[an - 1], depth);
                sb_add(s, "))");
            }
        } else if (is_callee_name(n->call.callee, "print")) {
            int an = n->call.args.len;
            if (an <= 1) {
                sb_add(s, "xs_print(");
                if (an == 1) emit_expr(s, n->call.args.items[0], depth);
                else sb_add(s, "XS_NULL");
                sb_addc(s, ')');
            } else {
                sb_addc(s, '(');
                for (int i = 0; i < an; i++) {
                    if (i) sb_add(s, ", xs_print(XS_STR(\" \")), ");
                    sb_add(s, "xs_print(");
                    emit_expr(s, n->call.args.items[i], depth);
                    sb_addc(s, ')');
                }
                sb_addc(s, ')');
            }
        } else if (n->call.callee && VAL_TAG(n->call.callee) == NODE_SCOPE &&
                   n->call.callee->scope.nparts == 2) {
            /* enum constructor call: Shape::Circle(5) -> map */
            sb_printf(s, "xs_map(%d, XS_STR(\"_type\"), XS_STR(\"%s\"), XS_STR(\"_variant\"), XS_STR(\"%s\")",
                      2 + n->call.args.len,
                      n->call.callee->scope.parts[0],
                      n->call.callee->scope.parts[1]);
            for (int i = 0; i < n->call.args.len; i++) {
                sb_printf(s, ", XS_STR(\"%d\"), ", i);
                emit_expr(s, n->call.args.items[i], depth);
            }
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "sqrt")) {
            sb_add(s, "XS_FLOAT(sqrt(xs_to_f64(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, ")))");
        } else if (is_callee_name(n->call.callee, "abs")) {
            sb_add(s, "XS_FLOAT(fabs(xs_to_f64(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, ")))");
        } else if (is_callee_name(n->call.callee, "int")) {
            sb_add(s, "XS_INT((int64_t)xs_to_f64(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, "))");
        } else if (is_callee_name(n->call.callee, "float")) {
            sb_add(s, "XS_FLOAT(xs_to_f64(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, "))");
        } else if (is_callee_name(n->call.callee, "ord")) {
            sb_add(s, "({ xs_val __o = ");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, "; XS_INT((__o.tag == 2 && __o.s) ? (long long)(unsigned char)__o.s[0] : 0); })");
        } else if (is_callee_name(n->call.callee, "chr")) {
            sb_add(s, "({ char __cb[2] = {(char)(int64_t)xs_to_f64(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, "), 0}; XS_STR(strdup(__cb)); })");
        } else if (is_callee_name(n->call.callee, "len")) {
            sb_add(s, "xs_len(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "type")) {
            sb_add(s, "xs_type(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "__pure?")) {
            sb_add(s, "xs_is_pure(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "channel")) {
            sb_add(s, "xs_channel_new(");
            if (n->call.args.len > 0) {
                sb_add(s, "(");
                emit_expr(s, n->call.args.items[0], depth);
                sb_add(s, ").i");
            } else {
                sb_add(s, "0");
            }
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "assert_eq")) {
            sb_add(s, "xs_assert_eq(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ", ");
            if (n->call.args.len > 1) emit_expr(s, n->call.args.items[1], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "Err")) {
            sb_add(s, "xs_map(2, XS_STR(\"tag\"), XS_STR(\"Err\"), XS_STR(\"value\"), ");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "Ok")) {
            sb_add(s, "xs_map(2, XS_STR(\"tag\"), XS_STR(\"Ok\"), XS_STR(\"value\"), ");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "str")) {
            sb_add(s, "XS_STR(strdup(xs_to_str(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ")))");
        } else if (is_callee_name(n->call.callee, "assert")) {
            sb_add(s, "xs_assert(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ", ");
            if (n->call.args.len > 1) emit_expr(s, n->call.args.items[1], depth);
            else sb_add(s, "XS_STR(\"assertion failed\")");
            sb_addc(s, ')');
        } else if (is_callee_name(n->call.callee, "exit")) {
            /* lower exit(N) to libc exit. comma operator keeps the call
             * an expression so contexts like `let _ = exit(0)` still
             * type-check; libc exit never returns so the XS_NULL is
             * just there to satisfy the C type system. */
            sb_add(s, "(exit((int)(");
            if (n->call.args.len > 0) emit_expr(s, n->call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_add(s, ").i), XS_NULL)");
        } else if (is_callee_name(n->call.callee, "abort")) {
            sb_add(s, "(abort(), XS_NULL)");
        } else {
            /* check if callee might be a closure (variable holding fn) */
            int might_be_closure = 0;
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_INDEX) {
                /* e.g. counter["inc"](): indexing into map, likely returns closure */
                might_be_closure = 1;
            }
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_FIELD) {
                /* e.g. util.shout(...): the namespace rewrite produced
                 * a FIELD callee. Routing through xs_call lets the
                 * runtime invoke a stored fn value. */
                might_be_closure = 1;
            }
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_CALL) {
                /* `outer()()`: the inner call returns an xs_val that
                 * holds the actual function. Apply via xs_call so the
                 * closure dispatch happens at runtime instead of trying
                 * to invoke an xs_val struct directly. */
                might_be_closure = 1;
            }
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_LAMBDA) {
                /* (|x| x + 1)(7) and friends. */
                might_be_closure = 1;
            }
            /* variable calls might be closures if the var was assigned from a function
               that returns closures */
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT &&
                lookup_fn_param_count(n->call.callee->ident.name) < 0 &&
                !is_c_keyword(n->call.callee->ident.name)) {
                /* not a known function: might be a closure variable */
                might_be_closure = 1;
            }
            /* class instantiation: `Counter()` -> empty map, then call
             * Counter_init(map, args...) if it exists. */
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT &&
                is_class(n->call.callee->ident.name)) {
                const char *cname = n->call.callee->ident.name;
                /* find the class decl so we can preinit any field defaults */
                Node *cdecl = NULL;
                if (program_root && VAL_TAG(program_root) == NODE_PROGRAM) {
                    for (int i = 0; i < program_root->program.stmts.len; i++) {
                        Node *st = program_root->program.stmts.items[i];
                        if (st && VAL_TAG(st) == NODE_CLASS_DECL && st->class_decl.name &&
                            strcmp(st->class_decl.name, cname) == 0) { cdecl = st; break; }
                    }
                }
                sb_add(s, "({ xs_val __ci = xs_map(0); ");
                if (cdecl) {
                    for (int i = 0; i < cdecl->class_decl.members.len; i++) {
                        Node *fm = cdecl->class_decl.members.items[i];
                        if (fm && (VAL_TAG(fm) == NODE_LET || VAL_TAG(fm) == NODE_VAR) && fm->let.name) {
                            sb_add(s, "xs_map_put(&__ci, XS_STR(\"");
                            sb_add(s, fm->let.name);
                            sb_add(s, "\"), ");
                            if (fm->let.value) emit_expr(s, fm->let.value, depth);
                            else sb_add(s, "XS_NULL");
                            sb_add(s, "); ");
                        }
                    }
                }
                if (class_has_init(cname)) {
                    sb_printf(s, "%s_init(__ci", cname);
                    for (int i = 0; i < n->call.args.len; i++) {
                        sb_add(s, ", ");
                        emit_expr(s, n->call.args.items[i], depth);
                    }
                    sb_add(s, "); ");
                }
                sb_add(s, "__ci; })");
                break;
            }
            if (might_be_closure) {
                sb_add(s, "xs_call(");
                emit_expr(s, n->call.callee, depth);
                sb_add(s, ", (xs_val[]){");
                for (int i = 0; i < n->call.args.len; i++) {
                    if (i) sb_add(s, ", ");
                    emit_expr(s, n->call.args.items[i], depth);
                }
                sb_printf(s, "}, %d)", n->call.args.len);
            } else {
                /* overload-by-arity dispatch: route `calc(1,2)` to
                 * `calc_2(...)` when multiple decls of `calc` exist. */
                int dispatched_overload = 0;
                if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT) {
                    const char *cname = n->call.callee->ident.name;
                    if (count_fn_overloads(cname) > 1) {
                        int pick = pick_overload_arity(cname, n->call.args.len);
                        if (pick < 0) pick = n->call.args.len;
                        sb_printf(s, "%s_%d", cname, pick);
                        dispatched_overload = 1;
                    }
                }
                if (!dispatched_overload) {
                    /* In callee position, emit the C symbol name directly
                     * even if it's a known fn (which would otherwise be
                     * wrapped as xs_fn_new(__xs_wrap_X, ...)). */
                    if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT &&
                        lookup_fn_param_count(n->call.callee->ident.name) >= 0)
                        emit_safe_name(s, n->call.callee->ident.name);
                    else
                        emit_expr(s, n->call.callee, depth);
                }
                sb_addc(s, '(');
                /* determine expected param count for padding */
                int expected = -1;
                if (n->call.callee && VAL_TAG(n->call.callee) == NODE_IDENT) {
                    const char *cname = n->call.callee->ident.name;
                    if (count_fn_overloads(cname) > 1)
                        expected = pick_overload_arity(cname, n->call.args.len);
                    else
                        expected = lookup_fn_param_count(cname);
                }
                for (int i = 0; i < n->call.args.len; i++) {
                    if (i) sb_add(s, ", ");
                    emit_expr(s, n->call.args.items[i], depth);
                }
                /* pad missing args with XS_NULL for default params */
                if (expected > n->call.args.len) {
                    for (int i = n->call.args.len; i < expected; i++) {
                        if (i) sb_add(s, ", ");
                        sb_add(s, "(xs_val){.tag=4}");
                    }
                }
                sb_addc(s, ')');
            }
        }
        break;
    }
    case NODE_METHOD_CALL: {
        const char *meth = n->method_call.method;
        /* check if receiver is a known actor variable */
        const char *actor_type = NULL;
        if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT)
            actor_type = lookup_actor_var(n->method_call.obj->ident.name);
        const char *mod_name = NULL;
        if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT) {
            const char *on = n->method_call.obj->ident.name;
            if (strcmp(on, "math") == 0 || strcmp(on, "string") == 0 ||
                strcmp(on, "json") == 0 || strcmp(on, "time") == 0 ||
                strcmp(on, "random") == 0 || strcmp(on, "os") == 0 ||
                strcmp(on, "fmt") == 0 || strcmp(on, "fs") == 0)
                mod_name = on;
        }
        /* check if receiver is a known struct/class instance whose impl
         * defines a method by the same name; route there directly so the
         * generic .push / .len / etc. fallbacks don't shadow the user's
         * impl. */
        const char *known_struct = NULL;
        if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT) {
            const char *rn = n->method_call.obj->ident.name;
            known_struct = lookup_struct_var(rn);
            if (!known_struct && current_class_name && strcmp(rn, "self") == 0)
                known_struct = current_class_name;
        }
        if (known_struct) {
            /* check that the type actually has a fn by that name */
            int has_method = 0;
            if (program_root && VAL_TAG(program_root) == NODE_PROGRAM) {
                for (int i = 0; i < program_root->program.stmts.len && !has_method; i++) {
                    Node *st = program_root->program.stmts.items[i];
                    if (!st) continue;
                    NodeList *members = NULL;
                    if (VAL_TAG(st) == NODE_CLASS_DECL && st->class_decl.name &&
                        strcmp(st->class_decl.name, known_struct) == 0)
                        members = &st->class_decl.members;
                    else if (VAL_TAG(st) == NODE_IMPL_DECL && st->impl_decl.type_name &&
                             strcmp(st->impl_decl.type_name, known_struct) == 0)
                        members = &st->impl_decl.members;
                    if (!members) continue;
                    for (int j = 0; j < members->len; j++) {
                        Node *mm = members->items[j];
                        if (mm && VAL_TAG(mm) == NODE_FN_DECL && mm->fn_decl.name &&
                            strcmp(mm->fn_decl.name, meth) == 0) { has_method = 1; break; }
                    }
                }
            }
            if (has_method) {
                sb_printf(s, "%s_%s(", known_struct, meth);
                emit_expr(s, n->method_call.obj, depth);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    sb_add(s, ", ");
                    emit_expr(s, n->method_call.args.items[i], depth);
                }
                sb_addc(s, ')');
                break;
            }
        }
        if (actor_type) {
            /* actor method dispatch */
            sb_printf(s, "%s_%s(&%s_state", actor_type, meth,
                      n->method_call.obj->ident.name);
            for (int i = 0; i < n->method_call.args.len; i++) {
                sb_add(s, ", ");
                emit_expr(s, n->method_call.args.items[i], depth);
            }
            sb_addc(s, ')');
        } else if (mod_name && strcmp(mod_name, "time") == 0) {
            if (strcmp(meth, "now") == 0) sb_add(s, "xs_time_now()");
            else if (strcmp(meth, "now_ms") == 0) sb_add(s, "xs_time_now_ms()");
            else if (strcmp(meth, "monotonic") == 0) sb_add(s, "xs_time_now()");
            else if (strcmp(meth, "sleep") == 0) {
                sb_add(s, "xs_time_sleep(");
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                else sb_add(s, "XS_FLOAT(0)");
                sb_addc(s, ')');
            } else if (strcmp(meth, "format") == 0) {
                sb_add(s, "xs_time_format(");
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                else sb_add(s, "XS_INT(0)");
                sb_add(s, ", ");
                if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
                else sb_add(s, "XS_STR(\"%Y-%m-%d %H:%M:%S\")");
                sb_addc(s, ')');
            } else {
                sb_printf(s, "XS_NULL /* time.%s */", meth);
            }
        } else if (mod_name && strcmp(mod_name, "fs") == 0) {
            const char *fn = NULL;
            if (strcmp(meth, "read") == 0)          fn = "xs_fs_read";
            else if (strcmp(meth, "write") == 0)    fn = "xs_fs_write";
            else if (strcmp(meth, "exists") == 0)   fn = "xs_fs_exists";
            else if (strcmp(meth, "cwd") == 0)      fn = "xs_fs_cwd";
            else if (strcmp(meth, "list_dir") == 0 ||
                     strcmp(meth, "list") == 0 ||
                     strcmp(meth, "ls") == 0)       fn = "xs_fs_list_dir";
            else if (strcmp(meth, "remove") == 0)   fn = "xs_fs_remove";
            else if (strcmp(meth, "mkdir") == 0)    fn = "xs_fs_mkdir";
            if (fn) {
                sb_printf(s, "%s(", fn);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    if (i) sb_add(s, ", ");
                    emit_expr(s, n->method_call.args.items[i], depth);
                }
                sb_addc(s, ')');
            } else {
                sb_printf(s, "XS_NULL /* fs.%s unsupported on --emit c */", meth);
            }
        } else if (mod_name && strcmp(mod_name, "os") == 0) {
            if (strcmp(meth, "getenv") == 0 || strcmp(meth, "env") == 0) {
                sb_add(s, "xs_os_getenv(");
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                else sb_add(s, "XS_STR(\"\")");
                sb_addc(s, ')');
            } else if (strcmp(meth, "args") == 0) {
                sb_add(s, "xs_os_args()");
            } else if (strcmp(meth, "exit") == 0) {
                sb_add(s, "xs_os_exit(");
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                else sb_add(s, "XS_INT(0)");
                sb_addc(s, ')');
            } else if (strcmp(meth, "hostname") == 0) {
                sb_add(s, "xs_os_hostname()");
            } else if (strcmp(meth, "platform") == 0) {
                sb_add(s, "xs_os_platform()");
            } else if (strcmp(meth, "cwd") == 0) {
                sb_add(s, "xs_fs_cwd()");
            } else {
                sb_printf(s, "XS_NULL /* os.%s unsupported on --emit c */", meth);
            }
        } else if (mod_name && strcmp(mod_name, "json") == 0) {
            const char *fn = NULL;
            if (strcmp(meth, "stringify") == 0 || strcmp(meth, "encode") == 0 || strcmp(meth, "dumps") == 0) fn = "xs_json_stringify";
            else if (strcmp(meth, "parse") == 0 || strcmp(meth, "decode") == 0 || strcmp(meth, "loads") == 0) fn = "xs_json_parse";
            if (fn) {
                sb_printf(s, "%s(", fn);
                for (int i = 0; i < n->method_call.args.len; i++) { if (i) sb_add(s, ", "); emit_expr(s, n->method_call.args.items[i], depth); }
                sb_addc(s, ')');
            } else {
                sb_printf(s, "XS_NULL /* json.%s */", meth);
            }
        } else if (mod_name && strcmp(mod_name, "math") == 0) {
            const char *fn = NULL;
            if (strcmp(meth, "floor") == 0) fn = "xs_num_floor";
            else if (strcmp(meth, "ceil") == 0) fn = "xs_num_ceil";
            else if (strcmp(meth, "round") == 0) fn = "xs_num_round";
            else if (strcmp(meth, "abs") == 0) fn = "xs_num_abs";
            else if (strcmp(meth, "sqrt") == 0) fn = "xs_math_sqrt";
            else if (strcmp(meth, "pow") == 0) fn = "xs_math_pow";
            else if (strcmp(meth, "min") == 0) fn = "xs_math_min";
            else if (strcmp(meth, "max") == 0) fn = "xs_math_max";
            else if (strcmp(meth, "log") == 0) fn = "xs_math_log";
            else if (strcmp(meth, "exp") == 0) fn = "xs_math_exp";
            else if (strcmp(meth, "sin") == 0) fn = "xs_math_sin";
            else if (strcmp(meth, "cos") == 0) fn = "xs_math_cos";
            else if (strcmp(meth, "tan") == 0) fn = "xs_math_tan";
            if (fn) {
                if (strcmp(meth, "min") == 0 || strcmp(meth, "max") == 0) {
                    sb_printf(s, "%s(%d", fn, n->method_call.args.len);
                    for (int i = 0; i < n->method_call.args.len; i++) { sb_add(s, ", "); emit_expr(s, n->method_call.args.items[i], depth); }
                    sb_addc(s, ')');
                } else {
                    sb_printf(s, "%s(", fn);
                    for (int i = 0; i < n->method_call.args.len; i++) { if (i) sb_add(s, ", "); emit_expr(s, n->method_call.args.items[i], depth); }
                    sb_addc(s, ')');
                }
            } else {
                sb_add(s, "XS_NULL /* unhandled math.");
                sb_add(s, meth);
                sb_add(s, " */");
            }
        } else if (strcmp(meth, "send") == 0) {
            sb_add(s, "xs_channel_send(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "recv") == 0) {
            sb_add(s, "xs_channel_recv(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "len") == 0) {
            sb_add(s, "xs_len(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "is_empty") == 0) {
            sb_add(s, "xs_is_empty(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "is_full") == 0) {
            sb_add(s, "xs_channel_is_full(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "sum") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ double __t_%d = 0; int __any_%d = 0;\n", mid, mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) { __any_%d=1; __t_%d += xs_to_f64(__a_%d->items[__i]); }\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth);
            sb_printf(s, "(__any_%d && __t_%d == (double)(long long)__t_%d) ? XS_INT((long long)__t_%d) : XS_FLOAT(__t_%d); })", mid, mid, mid, mid, mid);
        } else if (strcmp(meth, "product") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ double __t_%d = 1;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) __t_%d *= xs_to_f64(__a_%d->items[__i]);\n", mid, mid, mid, mid);
            sb_indent(s, depth);
            sb_printf(s, "(__t_%d == (double)(long long)__t_%d) ? XS_INT((long long)__t_%d) : XS_FLOAT(__t_%d); })", mid, mid, mid, mid);
        } else if (strcmp(meth, "avg") == 0 || strcmp(meth, "mean") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ double __t_%d = 0;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) __t_%d += xs_to_f64(__a_%d->items[__i]);\n", mid, mid, mid, mid);
            sb_indent(s, depth);
            sb_printf(s, "(__a_%d && __a_%d->len > 0) ? XS_FLOAT(__t_%d / __a_%d->len) : XS_FLOAT(0); })", mid, mid, mid, mid);
        } else if (strcmp(meth, "max") == 0 && n->method_call.args.len == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = XS_NULL;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__r_%d.tag == 4 || xs_cmp(__a_%d->items[__i], __r_%d) > 0) __r_%d = __a_%d->items[__i];\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth);
            sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "min") == 0 && n->method_call.args.len == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = XS_NULL;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__r_%d.tag == 4 || xs_cmp(__a_%d->items[__i], __r_%d) < 0) __r_%d = __a_%d->items[__i];\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth);
            sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "take") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "long long __n_%d = ", mid);
            if (n->method_call.args.len > 0) { sb_add(s, "xs_to_f64("); emit_expr(s, n->method_call.args.items[0], depth); sb_add(s, ")"); }
            else sb_add(s, "0");
            sb_add(s, ";\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len && __i<__n_%d; __i++) xs_arr_push(__r_%d, __a_%d->items[__i]);\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "drop") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "long long __n_%d = ", mid);
            if (n->method_call.args.len > 0) { sb_add(s, "xs_to_f64("); emit_expr(s, n->method_call.args.items[0], depth); sb_add(s, ")"); }
            else sb_add(s, "0");
            sb_add(s, ";\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=__n_%d; __i<__a_%d->len; __i++) if (__i>=0) xs_arr_push(__r_%d, __a_%d->items[__i]);\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "unique") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "int __dup = 0;\n");
            sb_indent(s, depth+2);
            sb_printf(s, "xs_arr *__rr = (xs_arr*)__r_%d.p;\n", mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__rr) for (int __j=0; __j<__rr->len && !__dup; __j++) if (xs_eq(__rr->items[__j], __a_%d->items[__i])) __dup = 1;\n", mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (!__dup) xs_arr_push(__r_%d, __a_%d->items[__i]);\n", mid, mid);
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "enumerate") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i<__a_%d->len; __i++) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "xs_val __t = xs_tuple(2, XS_INT(__i), __a_%d->items[__i]); xs_arr_push(__r_%d, __t);\n", mid, mid);
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "first") == 0 || strcmp(meth, "head") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "__a_%d && __a_%d->len > 0 ? __a_%d->items[0] : XS_NULL; })", mid, mid, mid);
        } else if (strcmp(meth, "last") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "__a_%d && __a_%d->len > 0 ? __a_%d->items[__a_%d->len-1] : XS_NULL; })", mid, mid, mid, mid);
        } else if (strcmp(meth, "tail") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=1; __i<__a_%d->len; __i++) xs_arr_push(__r_%d, __a_%d->items[__i]);\n", mid, mid, mid, mid);
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "init") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "xs_arr *__a_%d = (xs_arr*)(", mid);
            emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
            sb_indent(s, depth+1);
            sb_printf(s, "if (__a_%d) for (int __i=0; __i+1<__a_%d->len; __i++) xs_arr_push(__r_%d, __a_%d->items[__i]);\n", mid, mid, mid, mid);
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "push") == 0) {
            sb_add(s, "xs_arr_push(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "sort") == 0 || strcmp(meth, "sort_by") == 0) {
            sb_add(s, "xs_arr_sort_with(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0)
                emit_expr(s, n->method_call.args.items[0], depth);
            else
                sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "has") == 0) {
            sb_add(s, "xs_map_has(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "get") == 0 || strcmp(meth, "get_or") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __go_%d = xs_index(", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, "); (__go_%d.tag == 4) ? ", mid);
            if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, " : __go_%d; })", mid);
        } else if (strcmp(meth, "set") == 0 || strcmp(meth, "put") == 0) {
            sb_add(s, "xs_map_put(&");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ", ");
            if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "delete") == 0 || strcmp(meth, "remove") == 0) {
            sb_add(s, "xs_map_delete(&");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "split") == 0) {
            sb_add(s, "xs_str_split(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_STR(\" \")");
            sb_addc(s, ')');
        } else if (strcmp(meth, "upper") == 0) {
            sb_add(s, "xs_str_upper(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "lower") == 0) {
            sb_add(s, "xs_str_lower(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "join") == 0) {
            sb_add(s, "xs_arr_join(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_STR(\"\")");
            sb_addc(s, ')');
        } else if (strcmp(meth, "parse_int") == 0) {
            sb_add(s, "xs_conv_to_int(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "parse_float") == 0) {
            sb_add(s, "xs_str_parse_float(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "size") == 0 || strcmp(meth, "length") == 0) {
            sb_add(s, "xs_len(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "count") == 0) {
            /* count of substring (str) or matching elements (arr) when an
             * argument is given; otherwise length of the receiver. */
            if (n->method_call.args.len > 0) {
                int mid = defer_label_counter++;
                sb_printf(s, "({ xs_val __o_%d = ", mid);
                emit_expr(s, n->method_call.obj, depth);
                sb_printf(s, "; xs_val __n_%d = ", mid);
                emit_expr(s, n->method_call.args.items[0], depth);
                sb_printf(s, "; long long __c_%d = 0;\n", mid);
                sb_indent(s, depth+1);
                sb_printf(s, "if (__o_%d.tag == 2 && __o_%d.s && __n_%d.tag == 2 && __n_%d.s && __n_%d.s[0]) {\n", mid, mid, mid, mid, mid);
                sb_indent(s, depth+2);
                sb_printf(s, "size_t __ln = strlen(__n_%d.s); for (const char *__p = __o_%d.s; (__p = strstr(__p, __n_%d.s)); __p += __ln) __c_%d++;\n", mid, mid, mid, mid);
                sb_indent(s, depth+1);
                sb_printf(s, "} else if (__o_%d.tag == 5 && __o_%d.p) {\n", mid, mid);
                sb_indent(s, depth+2);
                sb_printf(s, "xs_arr *__a = (xs_arr*)__o_%d.p;\n", mid);
                sb_indent(s, depth+2);
                sb_printf(s, "if (__n_%d.tag == 8) for (int __i=0; __i<__a->len; __i++) { xs_val __r = xs_call(__n_%d, &__a->items[__i], 1); if (xs_truthy(__r)) __c_%d++; }\n", mid, mid, mid);
                sb_indent(s, depth+2);
                sb_printf(s, "else for (int __i=0; __i<__a->len; __i++) if (xs_eq(__a->items[__i], __n_%d)) __c_%d++;\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "XS_INT(__c_%d); })", mid);
            } else {
                sb_add(s, "xs_len(");
                emit_expr(s, n->method_call.obj, depth);
                sb_addc(s, ')');
            }
        } else if (strcmp(meth, "reverse") == 0) {
            sb_add(s, "xs_arr_reverse(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "clone") == 0) {
            sb_add(s, "xs_arr_clone(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "pop") == 0) {
            sb_add(s, "xs_arr_pop(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "contains") == 0) {
            sb_add(s, "xs_arr_contains(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "index_of") == 0 || strcmp(meth, "indexOf") == 0) {
            sb_add(s, "xs_arr_index_of(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "flat") == 0 || strcmp(meth, "flatten") == 0) {
            sb_add(s, "xs_arr_flatten(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "last_index_of") == 0 || strcmp(meth, "rfind") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __lo_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; xs_val __ln_%d = ", mid);
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, "; long long __lr_%d = -1;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "if (__lo_%d.tag == 2 && __lo_%d.s && __ln_%d.tag == 2 && __ln_%d.s && __ln_%d.s[0]) {\n",
                      mid, mid, mid, mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "size_t __sl = strlen(__lo_%d.s); size_t __nl = strlen(__ln_%d.s);\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__nl <= __sl) for (size_t __i = __sl - __nl + 1; __i-- > 0; ) {\n", 0);
            sb_indent(s, depth+3);
            sb_printf(s, "if (memcmp(__lo_%d.s + __i, __ln_%d.s, __nl) == 0) { __lr_%d = (long long)__i; break; }\n", mid, mid, mid);
            sb_indent(s, depth+2); sb_add(s, "}\n");
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_printf(s, "XS_INT(__lr_%d); })", mid);
        } else if (strcmp(meth, "replace_first") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __ro_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; xs_val __rn_%d = ", mid);
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, "; xs_val __rr_%d = ", mid);
            if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "XS_STR(\"\")");
            sb_printf(s, "; xs_val __rresult = __ro_%d;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "if (__ro_%d.tag == 2 && __ro_%d.s && __rn_%d.tag == 2 && __rn_%d.s && __rr_%d.tag == 2 && __rr_%d.s) {\n",
                      mid, mid, mid, mid, mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "const char *__src = __ro_%d.s; const char *__nd = __rn_%d.s; const char *__rp = __rr_%d.s;\n", mid, mid, mid);
            sb_indent(s, depth+2);
            sb_add(s, "size_t __nl = strlen(__nd); const char *__hit = strstr(__src, __nd);\n");
            sb_indent(s, depth+2);
            sb_add(s, "if (__hit) { size_t __pre = __hit - __src; size_t __rl = strlen(__rp); size_t __sl = strlen(__src); ");
            sb_add(s, "char *__buf = (char*)malloc(__sl - __nl + __rl + 1); memcpy(__buf, __src, __pre); ");
            sb_add(s, "memcpy(__buf + __pre, __rp, __rl); strcpy(__buf + __pre + __rl, __hit + __nl); ");
            sb_add(s, "__rresult = XS_STR(__buf); }\n");
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_add(s, "__rresult; })");
        } else if (strcmp(meth, "find_index") == 0 || strcmp(meth, "findIndex") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __ai_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; xs_val __an_%d = ", mid);
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, "; long long __ar_%d = -1;\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "if (__ai_%d.tag == 5 && __ai_%d.p) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "xs_arr *__a = (xs_arr*)__ai_%d.p;\n", mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__an_%d.tag == 8) {\n", mid);
            sb_indent(s, depth+3);
            sb_printf(s, "for (int __i=0; __i<__a->len; __i++) { xs_val __r = xs_call(__an_%d, &__a->items[__i], 1); if (xs_truthy(__r)) { __ar_%d = __i; break; } }\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "} else for (int __i=0; __i<__a->len; __i++) if (xs_eq(__a->items[__i], __an_%d)) { __ar_%d = __i; break; }\n", mid, mid);
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_printf(s, "XS_INT(__ar_%d); })", mid);
        } else if (strcmp(meth, "find") == 0) {
            /* str.find returns the byte offset of the substring or -1;
             * arr.find returns the matching element or null. */
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __fo_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; xs_val __fn_%d = ", mid);
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, ";\n");
            sb_indent(s, depth+1);
            sb_printf(s, "(__fo_%d.tag == 2) ? (__fn_%d.tag == 2 && __fo_%d.s && __fn_%d.s ? ({ const char *__p = strstr(__fo_%d.s, __fn_%d.s); __p ? XS_INT((long long)(__p - __fo_%d.s)) : XS_INT(-1); }) : XS_INT(-1)) : xs_arr_find(__fo_%d, __fn_%d);\n", mid, mid, mid, mid, mid, mid, mid, mid, mid);
            sb_indent(s, depth); sb_add(s, "})");
        } else if (strcmp(meth, "keys") == 0) {
            sb_add(s, "xs_map_keys(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "values") == 0) {
            sb_add(s, "xs_map_values(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "entries") == 0 || strcmp(meth, "items") == 0) {
            sb_add(s, "xs_map_entries(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "delete") == 0 || strcmp(meth, "remove") == 0) {
            /* mutate-in-place; return the deleted value -- but we don't
             * track that here, so return null. callers that care use
             * the interp; this matches the VM's behaviour. */
            sb_add(s, "({ xs_val __dm = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; xs_map_del(&__dm, ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, "); XS_NULL; })");
        } else if (strcmp(meth, "is_empty") == 0) {
            sb_add(s, "xs_arr_is_empty(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "pad_left") == 0 || strcmp(meth, "lpad") == 0 ||
                   strcmp(meth, "pad_start") == 0 ||
                   strcmp(meth, "pad_right") == 0 || strcmp(meth, "rpad") == 0 ||
                   strcmp(meth, "pad_end") == 0) {
            int is_left = strcmp(meth, "pad_left") == 0 || strcmp(meth, "lpad") == 0 ||
                          strcmp(meth, "pad_start") == 0;
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __ps_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; long long __pw_%d = ", mid);
            if (n->method_call.args.len > 0) {
                sb_add(s, "(long long)xs_to_f64(");
                emit_expr(s, n->method_call.args.items[0], depth);
                sb_addc(s, ')');
            } else sb_add(s, "0");
            sb_printf(s, "; xs_val __pf_%d = ", mid);
            if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "XS_STR(\" \")");
            sb_printf(s, "; xs_val __pr_%d = __ps_%d;\n", mid, mid);
            sb_indent(s, depth+1);
            sb_printf(s, "if (__ps_%d.tag == 2 && __ps_%d.s) {\n", mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "char __fc_%d = ' ';\n", mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if (__pf_%d.tag == 2 && __pf_%d.s && __pf_%d.s[0]) __fc_%d = __pf_%d.s[0];\n", mid, mid, mid, mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "else if (__pf_%d.tag == 0) __fc_%d = (char)__pf_%d.i;\n", mid, mid, mid);
            sb_indent(s, depth+2);
            sb_printf(s, "size_t __sl = strlen(__ps_%d.s);\n", mid);
            sb_indent(s, depth+2);
            sb_printf(s, "if ((long long)__sl < __pw_%d) {\n", mid);
            sb_indent(s, depth+3);
            sb_printf(s, "size_t __nl = (size_t)__pw_%d;\n", mid);
            sb_indent(s, depth+3);
            sb_add(s, "char *__buf = (char*)malloc(__nl + 1);\n");
            if (is_left) {
                sb_indent(s, depth+3);
                sb_printf(s, "size_t __pad = __nl - __sl; for (size_t __i = 0; __i < __pad; __i++) __buf[__i] = __fc_%d; memcpy(__buf + __pad, __ps_%d.s, __sl);\n", mid, mid);
            } else {
                sb_indent(s, depth+3);
                sb_printf(s, "memcpy(__buf, __ps_%d.s, __sl); for (size_t __i = __sl; __i < __nl; __i++) __buf[__i] = __fc_%d;\n", mid, mid);
            }
            sb_indent(s, depth+3);
            sb_printf(s, "__buf[__nl] = 0; __pr_%d = XS_STR(__buf);\n", mid);
            sb_indent(s, depth+2); sb_add(s, "}\n");
            sb_indent(s, depth+1); sb_add(s, "}\n");
            sb_indent(s, depth); sb_printf(s, "__pr_%d; })", mid);
        } else if (strcmp(meth, "trim") == 0) {
            sb_add(s, "xs_str_trim(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "trim_start") == 0 || strcmp(meth, "trim_left") == 0 ||
                   strcmp(meth, "ltrim") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __ts_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; (__ts_%d.tag == 2 && __ts_%d.s) ? ({ const char *__p = __ts_%d.s; while (*__p == ' ' || *__p == '\\t' || *__p == '\\n' || *__p == '\\r') __p++; XS_STR(strdup(__p)); }) : __ts_%d; })", mid, mid, mid, mid);
        } else if (strcmp(meth, "trim_end") == 0 || strcmp(meth, "trim_right") == 0 ||
                   strcmp(meth, "rtrim") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __te_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; (__te_%d.tag == 2 && __te_%d.s) ? ({ size_t __l = strlen(__te_%d.s); while (__l > 0 && (__te_%d.s[__l-1] == ' ' || __te_%d.s[__l-1] == '\\t' || __te_%d.s[__l-1] == '\\n' || __te_%d.s[__l-1] == '\\r')) __l--; char *__r = (char*)malloc(__l + 1); memcpy(__r, __te_%d.s, __l); __r[__l] = 0; XS_STR(__r); }) : __te_%d; })", mid, mid, mid, mid, mid, mid, mid, mid, mid);
        } else if (strcmp(meth, "chars") == 0) {
            sb_add(s, "xs_str_chars(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "bytes") == 0) {
            int mid = defer_label_counter++;
            sb_printf(s, "({ xs_val __s_%d = ", mid);
            emit_expr(s, n->method_call.obj, depth);
            sb_printf(s, "; xs_val __r_%d = xs_array(0);\n", mid);
            sb_indent(s, depth+1);
            sb_printf(s, "if (__s_%d.tag == 2 && __s_%d.s) for (const unsigned char *__p = (const unsigned char*)__s_%d.s; *__p; __p++) xs_arr_push(__r_%d, XS_INT(*__p));\n", mid, mid, mid, mid);
            sb_indent(s, depth); sb_printf(s, "__r_%d; })", mid);
        } else if (strcmp(meth, "starts_with") == 0 || strcmp(meth, "startsWith") == 0) {
            sb_add(s, "xs_str_starts_with(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "ends_with") == 0 || strcmp(meth, "endsWith") == 0) {
            sb_add(s, "xs_str_ends_with(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "replace") == 0) {
            sb_add(s, "xs_str_replace(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ", ");
            if (n->method_call.args.len > 1) emit_expr(s, n->method_call.args.items[1], depth);
            else sb_add(s, "XS_STR(\"\")");
            sb_addc(s, ')');
        } else if (strcmp(meth, "repeat") == 0) {
            sb_add(s, "xs_str_repeat(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_INT(0)");
            sb_addc(s, ')');
        } else if (strcmp(meth, "concat") == 0) {
            sb_printf(s, "xs_concat(%d, ", n->method_call.args.len);
            emit_expr(s, n->method_call.obj, depth);
            for (int i = 0; i < n->method_call.args.len; i++) {
                sb_add(s, ", ");
                emit_expr(s, n->method_call.args.items[i], depth);
            }
            sb_addc(s, ')');
        } else if (strcmp(meth, "to_str") == 0 || strcmp(meth, "toString") == 0) {
            sb_add(s, "xs_conv_to_str(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "to_int") == 0) {
            sb_add(s, "xs_conv_to_int(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "to_float") == 0) {
            sb_add(s, "xs_conv_to_float(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "is_a") == 0 || strcmp(meth, "isA") == 0) {
            sb_add(s, "XS_BOOL(xs_is(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_STR(\"null\")");
            sb_add(s, "))");
        } else if (strcmp(meth, "abs") == 0) {
            sb_add(s, "xs_num_abs(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "floor") == 0) {
            sb_add(s, "xs_num_floor(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "ceil") == 0) {
            sb_add(s, "xs_num_ceil(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "round") == 0) {
            sb_add(s, "xs_num_round(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "start") == 0) {
            sb_add(s, "xs_range_start(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "end") == 0) {
            sb_add(s, "xs_range_end(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "to_array") == 0 || strcmp(meth, "toArray") == 0 ||
                   strcmp(meth, "collect") == 0) {
            /* if obj is already an array we can just emit it; otherwise
             * funnel through xs_iter to materialise lazy iterators. statement-
             * expression must end on an expression so the surrounding context
             * can use the result; using if/else as a statement turns the
             * outer ({...}) into a void expression. */
            sb_add(s, "({ xs_val __ta = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; xs_val __ta_r = XS_NULL;\n");
            sb_indent(s, depth+1);
            sb_add(s, "if (__ta.tag == 5) { __ta_r = __ta; } else { xs_val __it = xs_iter(__ta); xs_val __out = xs_array(0); xs_val __v; while (xs_iter_next(&__it, &__v)) xs_arr_push(__out, __v); __ta_r = __out; }\n");
            sb_indent(s, depth+1);
            sb_add(s, "__ta_r; })");
        } else if (strcmp(meth, "next") == 0) {
            sb_add(s, "({ xs_val __it_n = ");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, "; xs_iter_next_val(&__it_n); })");
        } else if (strcmp(meth, "map") == 0 || strcmp(meth, "filter") == 0 ||
                   strcmp(meth, "reduce") == 0 || strcmp(meth, "fold") == 0 ||
                   strcmp(meth, "any") == 0 ||
                   strcmp(meth, "all") == 0) {
            /* array method with callback: use xs_call */
            int mid = defer_label_counter++;
            if (strcmp(meth, "map") == 0) {
                sb_printf(s, "({ xs_val __am_%d = xs_array(0);\n", mid);
                sb_indent(s, depth+1); sb_printf(s, "xs_arr *__src_%d = (xs_arr*)(", mid);
                emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_val __fn_%d = ", mid);
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "for (int __i=0; __src_%d && __i < __src_%d->len; __i++) {\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_val __a = __src_%d->items[__i];\n", mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_arr_push(__am_%d, xs_call(__fn_%d, &__a, 1));\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "__am_%d; })", mid);
            } else if (strcmp(meth, "filter") == 0) {
                sb_printf(s, "({ xs_val __am_%d = xs_array(0);\n", mid);
                sb_indent(s, depth+1); sb_printf(s, "xs_arr *__src_%d = (xs_arr*)(", mid);
                emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_val __fn_%d = ", mid);
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "for (int __i=0; __src_%d && __i < __src_%d->len; __i++) {\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_val __a = __src_%d->items[__i];\n", mid);
                sb_indent(s, depth+2); sb_printf(s, "if (xs_truthy(xs_call(__fn_%d, &__a, 1))) xs_arr_push(__am_%d, __a);\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "__am_%d; })", mid);
            } else if (strcmp(meth, "reduce") == 0 || strcmp(meth, "fold") == 0) {
                /* Both reduce and fold take (init, fn) in xs. The VM /
                 * interp accept either order via dynamic dispatch but
                 * the canonical signature documented in stdlib is
                 * `reduce(init, fn)`. With a single arg, treat that
                 * arg as the function and start from XS_INT(0). */
                int fn_arg  = 1;
                int init_arg = 0;
                if (n->method_call.args.len == 1) { fn_arg = 0; init_arg = -1; }
                sb_printf(s, "({ xs_val __acc_%d = ", mid);
                if (init_arg >= 0 && n->method_call.args.len > init_arg)
                    emit_expr(s, n->method_call.args.items[init_arg], depth);
                else sb_add(s, "XS_INT(0)");
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_arr *__src_%d = (xs_arr*)(", mid);
                emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_val __fn_%d = ", mid);
                if (n->method_call.args.len > fn_arg)
                    emit_expr(s, n->method_call.args.items[fn_arg], depth);
                else sb_add(s, "XS_NULL");
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "for (int __i=0; __src_%d && __i < __src_%d->len; __i++) {\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_val __ra[2] = { __acc_%d, __src_%d->items[__i] };\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "__acc_%d = xs_call(__fn_%d, __ra, 2);\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "__acc_%d; })", mid);
            } else if (strcmp(meth, "any") == 0) {
                sb_printf(s, "({ int __found_%d = 0;\n", mid);
                sb_indent(s, depth+1); sb_printf(s, "xs_arr *__src_%d = (xs_arr*)(", mid);
                emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_val __fn_%d = ", mid);
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "for (int __i=0; __src_%d && __i < __src_%d->len; __i++) {\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_val __a = __src_%d->items[__i];\n", mid);
                sb_indent(s, depth+2); sb_printf(s, "if (xs_truthy(xs_call(__fn_%d, &__a, 1))) { __found_%d = 1; break; }\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "XS_BOOL(__found_%d); })", mid);
            } else { /* all */
                sb_printf(s, "({ int __all_%d = 1;\n", mid);
                sb_indent(s, depth+1); sb_printf(s, "xs_arr *__src_%d = (xs_arr*)(", mid);
                emit_expr(s, n->method_call.obj, depth); sb_printf(s, ").p;\n");
                sb_indent(s, depth+1); sb_printf(s, "xs_val __fn_%d = ", mid);
                if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
                sb_add(s, ";\n");
                sb_indent(s, depth+1); sb_printf(s, "for (int __i=0; __src_%d && __i < __src_%d->len; __i++) {\n", mid, mid);
                sb_indent(s, depth+2); sb_printf(s, "xs_val __a = __src_%d->items[__i];\n", mid);
                sb_indent(s, depth+2); sb_printf(s, "if (!xs_truthy(xs_call(__fn_%d, &__a, 1))) { __all_%d = 0; break; }\n", mid, mid);
                sb_indent(s, depth+1); sb_add(s, "}\n");
                sb_indent(s, depth); sb_printf(s, "XS_BOOL(__all_%d); })", mid);
            }
        } else {
            /* check if receiver is a known struct with impl */
            const char *stype = NULL;
            if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT) {
                const char *rn = n->method_call.obj->ident.name;
                stype = lookup_struct_var(rn);
                if (!stype && current_class_name && strcmp(rn, "self") == 0)
                    stype = current_class_name;
            }
            if (stype) {
                sb_printf(s, "%s_%s(", stype, meth);
                emit_expr(s, n->method_call.obj, depth);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    sb_add(s, ", ");
                    emit_expr(s, n->method_call.args.items[i], depth);
                }
                sb_addc(s, ')');
            } else if (n_impl_types == 1) {
                /* single impl type: assume method belongs to it */
                sb_printf(s, "%s_%s(", impl_types[0].type_name, meth);
                emit_expr(s, n->method_call.obj, depth);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    sb_add(s, ", ");
                    emit_expr(s, n->method_call.args.items[i], depth);
                }
                sb_addc(s, ')');
            } else if (n_impl_types > 1 && impl_method_in_any_impl(meth)) {
                /* heterogeneous receiver: route through runtime dispatch
                 * that reads the __type__ tag set at struct init. */
                int mid = defer_label_counter++;
                sb_printf(s, "({ xs_val __mr_obj_%d = ", mid);
                emit_expr(s, n->method_call.obj, depth);
                sb_printf(s, "; xs_val __mr_arg_%d[%d];\n", mid,
                          n->method_call.args.len > 0 ? n->method_call.args.len : 1);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    sb_indent(s, depth+1);
                    sb_printf(s, "__mr_arg_%d[%d] = ", mid, i);
                    emit_expr(s, n->method_call.args.items[i], depth);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth+1);
                sb_printf(s, "xs_val __mr_t_%d = (__mr_obj_%d.tag == 6) ? xs_index(__mr_obj_%d, XS_STR(\"__type__\")) : XS_NULL;\n",
                          mid, mid, mid);
                sb_indent(s, depth+1);
                sb_printf(s, "xs_val __mr_r_%d = XS_NULL;\n", mid);
                int first_branch = 1;
                for (int t = 0; t < n_impl_types; t++) {
                    if (!impl_type_has_method(impl_types[t].type_name, meth)) continue;
                    sb_indent(s, depth+1);
                    sb_printf(s, "%sif (__mr_t_%d.tag == 2 && __mr_t_%d.s && strcmp(__mr_t_%d.s, \"%s\") == 0) {\n",
                              first_branch ? "" : "else ", mid, mid, mid, impl_types[t].type_name);
                    sb_indent(s, depth+2);
                    sb_printf(s, "__mr_r_%d = %s_%s(__mr_obj_%d", mid, impl_types[t].type_name, meth, mid);
                    for (int i = 0; i < n->method_call.args.len; i++)
                        sb_printf(s, ", __mr_arg_%d[%d]", mid, i);
                    sb_add(s, ");\n");
                    sb_indent(s, depth+1);
                    sb_add(s, "}\n");
                    first_branch = 0;
                }
                sb_indent(s, depth);
                sb_printf(s, "__mr_r_%d; })", mid);
            } else {
                /* Generic fallback. If the method name happens to match
                 * a top-level fn declaration (e.g. UFCS-style), emit
                 * a direct C call: name(obj, args). Otherwise treat it
                 * as a dynamic dispatch through the receiver: look the
                 * method up as a field of the value and invoke it via
                 * xs_call, mirroring how the interpreter resolves
                 * `obj.meth(...)` on a map / struct that carries an
                 * unbound callable field (bug041: `{ handle: fn(req) }`
                 * with `obj.handle(req)` was generating `handle(obj, req)`
                 * which collides with the unrelated `handle` keyword
                 * machinery in C and never resolves the closure).
                 */
                if (meth && lookup_fn_param_count(meth) >= 0) {
                    sb_add(s, meth);
                    sb_addc(s, '(');
                    emit_expr(s, n->method_call.obj, depth);
                    for (int i = 0; i < n->method_call.args.len; i++) {
                        sb_add(s, ", ");
                        emit_expr(s, n->method_call.args.items[i], depth);
                    }
                    sb_addc(s, ')');
                } else {
                    int mc_id = defer_label_counter++;
                    sb_printf(s, "({ xs_val __mc_obj_%d = ", mc_id);
                    emit_expr(s, n->method_call.obj, depth);
                    sb_printf(s, "; xs_val __mc_fn_%d = xs_index(__mc_obj_%d, XS_STR(\"%s\"));\n",
                              mc_id, mc_id, meth);
                    int an = n->method_call.args.len;
                    sb_indent(s, depth+1);
                    sb_printf(s, "xs_val __mc_args_%d[%d];\n", mc_id, an > 0 ? an : 1);
                    for (int i = 0; i < an; i++) {
                        sb_indent(s, depth+1);
                        sb_printf(s, "__mc_args_%d[%d] = ", mc_id, i);
                        emit_expr(s, n->method_call.args.items[i], depth);
                        sb_add(s, ";\n");
                    }
                    sb_indent(s, depth+1);
                    sb_printf(s, "xs_call(__mc_fn_%d, __mc_args_%d, %d); })",
                              mc_id, mc_id, an);
                }
            }
        }
        break;
    }
    case NODE_INDEX:
        /* a[start..end] becomes a slice op rather than a real index; we
         * can't go through xs_range here because that would materialise
         * the bounds as an array (and lose the open-end sentinel). */
        if (n->index.index && VAL_TAG(n->index.index) == NODE_RANGE) {
            Node *r = n->index.index;
            sb_add(s, "xs_slice(");
            emit_expr(s, n->index.obj, depth);
            sb_add(s, ", ");
            if (r->range.start) emit_expr(s, r->range.start, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ", ");
            if (r->range.end) emit_expr(s, r->range.end, depth);
            else sb_add(s, "XS_NULL");
            sb_printf(s, ", %d)", r->range.inclusive ? 1 : 0);
        } else {
            sb_add(s, "xs_index(");
            emit_expr(s, n->index.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->index.index, depth);
            sb_addc(s, ')');
        }
        break;
    case NODE_FIELD:
        if (n_actor_fields > 0 && in_method_body && n->field.obj &&
            VAL_TAG(n->field.obj) == NODE_IDENT &&
            strcmp(n->field.obj->ident.name, "self") == 0) {
            /* actor method: self is a struct pointer */
            sb_printf(s, "self->%s", n->field.name);
        } else if (n->field.obj && VAL_TAG(n->field.obj) == NODE_IDENT &&
                   strcmp(n->field.obj->ident.name, "math") == 0) {
            const char *fn = n->field.name;
            if (strcmp(fn, "pi") == 0)        sb_add(s, "XS_FLOAT(3.141592653589793)");
            else if (strcmp(fn, "e") == 0)    sb_add(s, "XS_FLOAT(2.718281828459045)");
            else if (strcmp(fn, "tau") == 0)  sb_add(s, "XS_FLOAT(6.283185307179586)");
            else if (strcmp(fn, "inf") == 0)  sb_add(s, "XS_FLOAT(1.0/0.0)");
            else if (strcmp(fn, "nan") == 0)  sb_add(s, "XS_FLOAT(0.0/0.0)");
            else sb_printf(s, "XS_NULL /* math.%s */", fn);
        } else if (n->field.obj && VAL_TAG(n->field.obj) == NODE_IDENT &&
                   strcmp(n->field.obj->ident.name, "os") == 0) {
            const char *fn = n->field.name;
            if (strcmp(fn, "platform") == 0)    sb_add(s, "xs_os_platform()");
            else if (strcmp(fn, "sep") == 0)    sb_add(s, "xs_os_sep()");
            else if (strcmp(fn, "args") == 0)   sb_add(s, "xs_os_args()");
            else sb_printf(s, "XS_NULL /* os.%s */", fn);
        } else {
            /* struct/map fields or tuple indices */
            sb_printf(s, "xs_index(");
            emit_expr(s, n->field.obj, depth);
            /* check if field name is numeric (tuple index) */
            if (n->field.name && n->field.name[0] >= '0' && n->field.name[0] <= '9')
                sb_printf(s, ", XS_INT(%s))", n->field.name);
            else
                sb_printf(s, ", XS_STR(\"%s\"))", n->field.name);
        }
        break;
    case NODE_SCOPE:
        /* enum variant: Foo::Bar -> map with _variant and _type */
        if (n->scope.nparts == 2) {
            sb_printf(s, "xs_map(2, XS_STR(\"_type\"), XS_STR(\"%s\"), XS_STR(\"_variant\"), XS_STR(\"%s\"))",
                      n->scope.parts[0], n->scope.parts[1]);
        } else {
            for (int i = 0; i < n->scope.nparts; i++) {
                if (i) sb_add(s, "_");
                sb_add(s, n->scope.parts[i]);
            }
        }
        break;
    case NODE_ASSIGN:
        if (n->assign.target && VAL_TAG(n->assign.target) == NODE_INDEX) {
            Node *idx = n->assign.target;
            /* nested index (a[i][j] = v) needs a temporary because xs_index
             * returns by value and we can't take the address of an rvalue. */
            if (idx->index.obj && VAL_TAG(idx->index.obj) == NODE_INDEX) {
                sb_add(s, "({ xs_val __t_idx = ");
                emit_expr(s, idx->index.obj, depth);
                sb_add(s, "; xs_map_put(&__t_idx, ");
                emit_expr(s, idx->index.index, depth);
                sb_add(s, ", ");
                emit_expr(s, n->assign.value, depth);
                sb_add(s, "); })");
            } else {
                sb_add(s, "xs_map_put(&");
                emit_expr(s, idx->index.obj, depth);
                sb_add(s, ", ");
                emit_expr(s, idx->index.index, depth);
                sb_add(s, ", ");
                emit_expr(s, n->assign.value, depth);
                sb_addc(s, ')');
            }
        } else if (n->assign.target && VAL_TAG(n->assign.target) == NODE_FIELD) {
            /* obj.field = v: route through xs_map_put on string key (or
             * write into the actor field through self->field). */
            Node *fld = n->assign.target;
            if (n_actor_fields > 0 && in_method_body && fld->field.obj &&
                VAL_TAG(fld->field.obj) == NODE_IDENT &&
                strcmp(fld->field.obj->ident.name, "self") == 0) {
                sb_printf(s, "self->%s = ", fld->field.name);
                emit_expr(s, n->assign.value, depth);
            } else {
                sb_add(s, "xs_map_put(&");
                emit_expr(s, fld->field.obj, depth);
                if (fld->field.name && fld->field.name[0] >= '0' && fld->field.name[0] <= '9')
                    sb_printf(s, ", XS_INT(%s), ", fld->field.name);
                else
                    sb_printf(s, ", XS_STR(\"%s\"), ", fld->field.name ? fld->field.name : "");
                emit_expr(s, n->assign.value, depth);
                sb_addc(s, ')');
            }
        } else {
            emit_expr(s, n->assign.target, depth);
            sb_addc(s, ' ');
            sb_add(s, n->assign.op);
            sb_addc(s, ' ');
            emit_expr(s, n->assign.value, depth);
        }
        break;
    case NODE_RANGE:
        sb_add(s, "xs_range(");
        emit_expr(s, n->range.start, depth);
        sb_add(s, ", ");
        emit_expr(s, n->range.end, depth);
        sb_printf(s, ", %d)", n->range.inclusive ? 1 : 0);
        break;
    case NODE_LAMBDA: {
        int lid = register_lambda(n);
        /* find the lambda info to check for captures */
        LambdaInfo *linfo = NULL;
        for (int i = 0; i < n_lambdas; i++)
            if (lambdas[i].id == lid) { linfo = &lambdas[i]; break; }
        if (linfo && linfo->n_captures > 0) {
            /* create env: array of xs_val* pointers to captured variables */
            sb_printf(s, "({ xs_val **__cenv_%d = (xs_val**)malloc(%d * sizeof(xs_val*));\n",
                      lid, linfo->n_captures);
            for (int ci = 0; ci < linfo->n_captures; ci++) {
                sb_indent(s, depth + 1);
                /* top-level vars live in file scope as `static xs_val NAME`
                 * so closure captures take their address directly; nothing
                 * else needs boxing because the variable already outlives
                 * any lambda. local captures still go through __box_NAME. */
                const char *cap = linfo->captures[ci];
                int outer_cap_idx = -1;
                if (current_lambda) {
                    for (int oc = 0; oc < current_lambda->n_captures; oc++)
                        if (strcmp(current_lambda->captures[oc], cap) == 0) {
                            outer_cap_idx = oc; break;
                        }
                }
                if (is_top_level_var(cap))
                    sb_printf(s, "__cenv_%d[%d] = &%s;\n", lid, ci, cap);
                else if (outer_cap_idx >= 0)
                    /* Forwarded from the enclosing lambda's env: re-use
                     * the same box pointer so writes propagate up. */
                    sb_printf(s, "__cenv_%d[%d] = ((xs_val**)__env)[%d];\n",
                              lid, ci, outer_cap_idx);
                else
                    sb_printf(s, "__cenv_%d[%d] = __box_%s;\n", lid, ci, cap);
            }
            sb_indent(s, depth);
            sb_printf(s, "%s(__xs_lambda_%d, __cenv_%d); })",
                      n->lambda.inferred_pure ? "xs_fn_new_pure" : "xs_fn_new",
                      lid, lid);
        } else {
            sb_printf(s, "%s(__xs_lambda_%d, NULL)",
                      n->lambda.inferred_pure ? "xs_fn_new_pure" : "xs_fn_new",
                      lid);
        }
        break;
    }
    case NODE_CAST: {
        const char *ty = n->cast.type_name ? n->cast.type_name : "?";
        if (strcmp(ty, "float") == 0 || strcmp(ty, "f64") == 0 || strcmp(ty, "f32") == 0) {
            sb_add(s, "XS_FLOAT(xs_to_f64(");
            emit_expr(s, n->cast.expr, depth);
            sb_add(s, "))");
        } else if (strcmp(ty, "int") == 0 || strcmp(ty, "i32") == 0 || strcmp(ty, "i64") == 0) {
            sb_add(s, "XS_INT((int64_t)xs_to_f64(");
            emit_expr(s, n->cast.expr, depth);
            sb_add(s, "))");
        } else if (strcmp(ty, "str") == 0 || strcmp(ty, "string") == 0) {
            sb_add(s, "XS_STR(strdup(xs_to_str(");
            emit_expr(s, n->cast.expr, depth);
            sb_add(s, ")))");
        } else if (strcmp(ty, "bool") == 0) {
            sb_add(s, "XS_BOOL(xs_truthy(");
            emit_expr(s, n->cast.expr, depth);
            sb_add(s, "))");
        } else {
            emit_expr(s, n->cast.expr, depth);
        }
        break;
    }
    case NODE_STRUCT_INIT:
        if (n->struct_init.rest) {
            /* spread: start with base, override fields */
            int sid = defer_label_counter++;
            sb_printf(s, "({ xs_val __si_%d = ", sid);
            emit_expr(s, n->struct_init.rest, depth);
            sb_add(s, ";\n");
            /* copy the base map */
            sb_indent(s, depth + 1);
            sb_printf(s, "xs_val __sr_%d = xs_map(0);\n", sid);
            sb_indent(s, depth + 1);
            sb_printf(s, "if (__si_%d.tag == 6 && __si_%d.p) {\n", sid, sid);
            sb_indent(s, depth + 2);
            sb_printf(s, "xs_hmap *__m = (xs_hmap*)__si_%d.p;\n", sid);
            sb_indent(s, depth + 2);
            sb_printf(s, "for (int __k = 0; __k < __m->len; __k++)\n");
            sb_indent(s, depth + 3);
            sb_printf(s, "xs_map_put(&__sr_%d, XS_STR(__m->keys[__k]), __m->vals[__k]);\n", sid);
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
            for (int i = 0; i < n->struct_init.fields.len; i++) {
                sb_indent(s, depth + 1);
                sb_printf(s, "xs_map_put(&__sr_%d, XS_STR(\"%s\"), ", sid, n->struct_init.fields.items[i].key);
                emit_expr(s, n->struct_init.fields.items[i].val, depth);
                sb_add(s, ");\n");
            }
            sb_indent(s, depth);
            sb_printf(s, "__sr_%d; })", sid);
        } else {
            /* emit struct init as map; tag with __type__ so heterogeneous
             * method dispatch (`s.area()` over a [Circle, Square]) can
             * route to the right `Type_method` at runtime. */
            const char *tname = n->struct_init.path ? n->struct_init.path : "";
            int extra = (tname && tname[0]) ? 1 : 0;
            sb_printf(s, "xs_map(%d", n->struct_init.fields.len + extra);
            if (extra)
                sb_printf(s, ", XS_STR(\"__type__\"), XS_STR(\"%s\")", tname);
            for (int i = 0; i < n->struct_init.fields.len; i++) {
                sb_printf(s, ", XS_STR(\"%s\"), ", n->struct_init.fields.items[i].key);
                emit_expr(s, n->struct_init.fields.items[i].val, depth);
            }
            sb_addc(s, ')');
        }
        break;
    case NODE_SPREAD:
        sb_add(s, "(fprintf(stderr, \"xs: spread operator not supported in C target\\n\"), exit(1), XS_NULL)");
        break;
    case NODE_LIST_COMP: {
        /* [expr for x in iter if cond] -> inline block that builds an array */
        int lc_id = defer_label_counter++;
        sb_printf(s, "({ xs_val __lc_%d = xs_array(0);\n", lc_id);
        /* Emit nested for-loops for each clause */
        for (int ci = 0; ci < n->list_comp.clause_pats.len; ci++) {
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "{ xs_val __lc_iter = xs_iter(");
            emit_expr(s, n->list_comp.clause_iters.items[ci], depth);
            sb_add(s, ");\n");
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "xs_val ");
            Node *cpat = n->list_comp.clause_pats.items[ci];
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT)
                sb_add(s, cpat->pat_ident.name);
            else
                sb_printf(s, "__lc_v_%d", ci);
            sb_add(s, ";\n");
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "while (xs_iter_next(&__lc_iter, &");
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT)
                sb_add(s, cpat->pat_ident.name);
            else
                sb_printf(s, "__lc_v_%d", ci);
            sb_add(s, ")) {\n");
        }
        /* Emit condition guard if present */
        int last_ci = n->list_comp.clause_pats.len - 1;
        int inner_depth = depth + 1 + n->list_comp.clause_pats.len;
        if (last_ci >= 0 && n->list_comp.clause_conds.items[last_ci]) {
            sb_indent(s, inner_depth);
            sb_add(s, "if (xs_truthy(");
            emit_expr(s, n->list_comp.clause_conds.items[last_ci], depth);
            sb_add(s, ")) ");
        } else {
            sb_indent(s, inner_depth);
        }
        sb_printf(s, "xs_arr_push(__lc_%d, ", lc_id);
        emit_expr(s, n->list_comp.element, depth);
        sb_add(s, ");\n");
        /* Close nested loops in reverse */
        for (int ci = n->list_comp.clause_pats.len - 1; ci >= 0; ci--) {
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "} }\n");
        }
        sb_indent(s, depth);
        sb_printf(s, "__lc_%d; })", lc_id);
        break;
    }
    case NODE_MAP_COMP: {
        /* {k: v for x in iter if cond} -> inline block that builds a map */
        int mc_id = defer_label_counter++;
        sb_printf(s, "({ xs_val __mc_%d = xs_map(0);\n", mc_id);
        for (int ci = 0; ci < n->map_comp.clause_pats.len; ci++) {
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "{ xs_val __mc_iter = xs_iter(");
            emit_expr(s, n->map_comp.clause_iters.items[ci], depth);
            sb_add(s, ");\n");
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "xs_val ");
            Node *cpat = n->map_comp.clause_pats.items[ci];
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT)
                sb_add(s, cpat->pat_ident.name);
            else
                sb_printf(s, "__mc_v_%d", ci);
            sb_add(s, ";\n");
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "while (xs_iter_next(&__mc_iter, &");
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT)
                sb_add(s, cpat->pat_ident.name);
            else
                sb_printf(s, "__mc_v_%d", ci);
            sb_add(s, ")) {\n");
        }
        int mc_last = n->map_comp.clause_pats.len - 1;
        int mc_inner = depth + 1 + n->map_comp.clause_pats.len;
        if (mc_last >= 0 && n->map_comp.clause_conds.items[mc_last]) {
            sb_indent(s, mc_inner);
            sb_add(s, "if (xs_truthy(");
            emit_expr(s, n->map_comp.clause_conds.items[mc_last], depth);
            sb_add(s, ")) ");
        } else {
            sb_indent(s, mc_inner);
        }
        sb_printf(s, "xs_map_put(&__mc_%d, ", mc_id);
        emit_expr(s, n->map_comp.key, depth);
        sb_add(s, ", ");
        emit_expr(s, n->map_comp.value, depth);
        sb_add(s, ");\n");
        for (int ci = n->map_comp.clause_pats.len - 1; ci >= 0; ci--) {
            sb_indent(s, depth + 1 + ci);
            sb_add(s, "} }\n");
        }
        sb_indent(s, depth);
        sb_printf(s, "__mc_%d; })", mc_id);
        break;
    }
    case NODE_AWAIT:
        /* await in C target -> just evaluate the expression (single-threaded) */
        if (n->await_.expr) emit_expr(s, n->await_.expr, depth);
        else sb_add(s, "XS_NULL");
        break;
    case NODE_YIELD:
        sb_add(s, "(fprintf(stderr, \"xs: yield not supported in C target\\n\"), exit(1), XS_NULL)");
        break;
    case NODE_SPAWN: {
        Node *se = n->spawn_.expr;
        if (se && VAL_TAG(se) == NODE_IDENT && find_actor(se->ident.name)) {
            /* spawn ActorName -> already handled as statement, return dummy */
            sb_add(s, "XS_NULL /* spawn actor */");
        } else if (se && VAL_TAG(se) == NODE_BLOCK) {
            /* spawn { block } -> execute inline, return result map */
            sb_add(s, "({ xs_val __spawn_result = XS_NULL;\n");
            emit_block_body(s, se, depth + 1);
            if (se->block.expr) {
                sb_indent(s, depth + 1);
                sb_add(s, "__spawn_result = ");
                emit_expr(s, se->block.expr, depth + 1);
                sb_add(s, ";\n");
            }
            sb_indent(s, depth + 1);
            sb_add(s, "xs_val __spawn_map = xs_map(2, XS_STR(\"_result\"), __spawn_result, XS_STR(\"_status\"), XS_STR(\"done\"));\n");
            sb_indent(s, depth + 1);
            sb_add(s, "__spawn_map; })");
        } else {
            /* spawn <expr> -> just evaluate it */
            if (se) emit_expr(s, se, depth);
            else sb_add(s, "XS_NULL");
        }
        break;
    }
    case NODE_ACTOR_DECL:
        /* actor decl as expression: already emitted at file scope, just return null */
        sb_add(s, "XS_NULL");
        break;
    case NODE_SEND_EXPR: {
        /* actor ! message -> call handle method */
        const char *atype = NULL;
        if (n->send_expr.target && VAL_TAG(n->send_expr.target) == NODE_IDENT)
            atype = lookup_actor_var(n->send_expr.target->ident.name);
        if (atype) {
            sb_printf(s, "%s_handle(&%s_state, ", atype, n->send_expr.target->ident.name);
            emit_expr(s, n->send_expr.message, depth);
            sb_addc(s, ')');
        } else {
            sb_add(s, "/* send */ XS_NULL");
        }
        break;
    }
        break;
    case NODE_RESUME:
        sb_add(s, "xs_resume(");
        if (n->resume_.value) emit_expr(s, n->resume_.value, depth);
        else sb_add(s, "(xs_val){.tag=4}");
        sb_add(s, ")");
        break;
    case NODE_PERFORM:
        sb_printf(s, "xs_perform(\"%s\", \"%s\", ",
                  n->perform.effect_name ? n->perform.effect_name : "?",
                  n->perform.op_name ? n->perform.op_name : "?");
        if (n->perform.args.len > 0) {
            emit_expr(s, n->perform.args.items[0], depth);
        } else {
            sb_add(s, "(xs_val){.tag=4}");
        }
        sb_add(s, ")");
        break;
    case NODE_THROW:
        sb_add(s, "(xs_throw(");
        emit_expr(s, n->throw_.value, depth);
        sb_add(s, "), XS_NULL)");
        break;
    case NODE_RETURN:
        if (n->ret.value) emit_expr(s, n->ret.value, depth);
        else sb_add(s, "XS_NULL");
        break;
    case NODE_IF: {
        /* if as expression: ternary chain — emit each elif as its own
         * arm. Without this, an if-elif-else collapsed into the first
         * cond plus the final else, dropping every middle branch. */
        sb_add(s, "(xs_truthy(");
        emit_expr(s, n->if_expr.cond, depth);
        sb_add(s, ") ? ");
        if (n->if_expr.then && VAL_TAG(n->if_expr.then) == NODE_BLOCK && n->if_expr.then->block.expr) {
            emit_expr(s, n->if_expr.then->block.expr, depth);
        } else {
            sb_add(s, "XS_NULL");
        }
        for (int ei = 0; ei < n->if_expr.elif_conds.len; ei++) {
            sb_add(s, " : (xs_truthy(");
            emit_expr(s, n->if_expr.elif_conds.items[ei], depth);
            sb_add(s, ") ? ");
            Node *eb = ei < n->if_expr.elif_thens.len ? n->if_expr.elif_thens.items[ei] : NULL;
            if (eb && VAL_TAG(eb) == NODE_BLOCK && eb->block.expr)
                emit_expr(s, eb->block.expr, depth);
            else
                sb_add(s, "XS_NULL");
        }
        sb_add(s, " : ");
        if (n->if_expr.else_branch && VAL_TAG(n->if_expr.else_branch) == NODE_BLOCK
            && n->if_expr.else_branch->block.expr) {
            emit_expr(s, n->if_expr.else_branch->block.expr, depth);
        } else {
            sb_add(s, "XS_NULL");
        }
        for (int ei = 0; ei < n->if_expr.elif_conds.len; ei++) sb_addc(s, ')');
        sb_addc(s, ')');
        break;
    }
    case NODE_MATCH: {
        /* match as expression -> GCC statement expression with do-while(0) + break.
           Each arm is its own `if` (NOT else-if): on guard failure we fall through
           to the next arm. `break` exits the do-while when an arm fully matches. */
        sb_add(s, "({ xs_val __subject = ");
        emit_expr(s, n->match.subject, depth);
        sb_add(s, ";\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_val __match_result = XS_NULL;\n");
        sb_indent(s, depth + 1);
        sb_add(s, "do {\n");
        for (int i = 0; i < n->match.arms.len; i++) {
            MatchArm *arm = &n->match.arms.items[i];
            sb_indent(s, depth + 2);
            sb_add(s, "if (");
            emit_pattern_cond(s, arm->pattern, "__subject", depth + 2);
            sb_add(s, ") {\n");
            emit_pattern_bindings(s, arm->pattern, "__subject", depth + 3);
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "if (xs_truthy(");
                emit_expr(s, arm->guard, depth + 3);
                sb_add(s, ")) {\n");
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                    emit_block_body(s, arm->body, depth + 4);
                    if (arm->body->block.expr) {
                        sb_indent(s, depth + 4);
                        sb_add(s, "__match_result = ");
                        emit_expr(s, arm->body->block.expr, depth + 4);
                        sb_add(s, ";\n");
                    }
                } else if (arm->body) {
                    sb_indent(s, depth + 4);
                    sb_add(s, "__match_result = ");
                    emit_expr(s, arm->body, depth + 4);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth + 4);
                sb_add(s, "break;\n");
                sb_indent(s, depth + 3);
                sb_add(s, "}\n");
            } else {
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                    emit_block_body(s, arm->body, depth + 3);
                    if (arm->body->block.expr) {
                        sb_indent(s, depth + 3);
                        sb_add(s, "__match_result = ");
                        emit_expr(s, arm->body->block.expr, depth + 3);
                        sb_add(s, ";\n");
                    }
                } else if (arm->body) {
                    sb_indent(s, depth + 3);
                    sb_add(s, "__match_result = ");
                    emit_expr(s, arm->body, depth + 3);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth + 3);
                sb_add(s, "break;\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "} while (0);\n");
        sb_indent(s, depth + 1);
        sb_add(s, "__match_result;\n");
        sb_indent(s, depth);
        sb_add(s, "})");
        break;
    }
    case NODE_BLOCK:
        if (n->block.expr) {
            emit_expr(s, n->block.expr, depth);
        } else {
            sb_add(s, "XS_NULL");
        }
        break;
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
        case 3: sb_add(s, n->pat_lit.bval ? "1" : "0"); break;
        case 4: sb_add(s, "XS_NULL"); break;
        default: sb_add(s, "XS_NULL"); break;
        }
        break;
    case NODE_PAT_TUPLE: {
        /* Emit tuple as an array literal */
        sb_printf(s, "xs_array(%d", n->pat_tuple.elems.len);
        for (int i = 0; i < n->pat_tuple.elems.len; i++) {
            sb_add(s, ", ");
            emit_expr(s, n->pat_tuple.elems.items[i], depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_ENUM: {
        /* Emit enum constructor tag check value */
        if (n->pat_enum.path) {
            sb_add(s, n->pat_enum.path);
        } else {
            sb_add(s, "XS_NULL");
        }
        break;
    }
    case NODE_PAT_GUARD:
        /* Emit the guard condition as a boolean expression */
        sb_add(s, "XS_BOOL(");
        emit_expr(s, n->pat_guard.guard, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_STRUCT:
    case NODE_PAT_OR:
    case NODE_PAT_RANGE:
    case NODE_PAT_SLICE:
    case NODE_PAT_EXPR:
    case NODE_PAT_CAPTURE:
    case NODE_PAT_STRING_CONCAT:
    case NODE_PAT_REGEX:
        sb_add(s, "/* pattern */ XS_NULL");
        break;
    /* declaration nodes as expressions emit their identifier */
    case NODE_FN_DECL:
        if (n->fn_decl.name) emit_safe_name(s, n->fn_decl.name);
        else sb_add(s, "XS_NULL");
        break;
    case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL:
    case NODE_CLASS_DECL:
    case NODE_TRAIT_DECL:
    case NODE_IMPL_DECL:
    case NODE_TYPE_ALIAS:
    case NODE_IMPORT:
    case NODE_USE:
    case NODE_MODULE_DECL:
    case NODE_EFFECT_DECL:
        sb_add(s, "XS_NULL");
        break;
    case NODE_LET:
    case NODE_VAR:
        if (n->let.name) sb_add(s, n->let.name);
        else sb_add(s, "XS_NULL");
        break;
    case NODE_CONST:
        if (n->const_.name) sb_add(s, n->const_.name);
        else sb_add(s, "XS_NULL");
        break;
    case NODE_EXPR_STMT:
        emit_expr(s, n->expr_stmt.expr, depth);
        break;
    case NODE_HANDLE: {
        /* Single-shot effect handler. The arm bodies are compiled as a
         * GCC nested function (`auto xs_val dispatch(...)`) so they
         * close over the enclosing C scope without explicit capture
         * lifting. xs_perform dereferences frame->dispatch to run an
         * arm in-place; if the arm calls xs_resume, that longjmps back
         * to the perform site (preserved because we never unwound).
         * Ports to compilers without nested-function support need an
         * explicit capture-struct rewrite. */
        static int handle_id_counter = 0;
        int hid = handle_id_counter++;
        const char *eff_name = NULL;
        for (int i = 0; i < n->handle.arms.len; i++) {
            EffectArm *a = &n->handle.arms.items[i];
            if (a->effect_name) { eff_name = a->effect_name; break; }
        }
        sb_add(s, "({\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "auto xs_val __h%d_dispatch(int aid, xs_val arg);\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "xs_val __h%d_dispatch(int aid, xs_val arg) {\n", hid);
        sb_indent(s, depth + 2);
        sb_add(s, "switch (aid) {\n");
        for (int i = 0; i < n->handle.arms.len; i++) {
            EffectArm *a = &n->handle.arms.items[i];
            sb_indent(s, depth + 3);
            sb_printf(s, "case %d: {\n", i);
            if (a->params.len > 0 && a->params.items[0].name) {
                sb_indent(s, depth + 4);
                sb_printf(s, "xs_val %s = arg;\n", a->params.items[0].name);
            } else {
                sb_indent(s, depth + 4);
                sb_add(s, "(void)arg;\n");
            }
            if (a->body && VAL_TAG(a->body) == NODE_BLOCK &&
                a->body->block.stmts.len > 0) {
                emit_block_body(s, a->body, depth + 4);
                sb_indent(s, depth + 4);
                sb_add(s, "return (");
                if (a->body->block.expr)
                    emit_expr(s, a->body->block.expr, depth + 4);
                else sb_add(s, "(xs_val){.tag=4}");
                sb_add(s, ");\n");
            } else {
                sb_indent(s, depth + 4);
                sb_add(s, "return (");
                if (a->body) emit_expr(s, a->body, depth + 4);
                else sb_add(s, "(xs_val){.tag=4}");
                sb_add(s, ");\n");
            }
            sb_indent(s, depth + 3);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth + 2);
        sb_add(s, "}\n");
        sb_indent(s, depth + 2);
        sb_add(s, "return (xs_val){.tag=4};\n");
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "XsEffFrame __h%d;\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "__h%d.eff_name = \"%s\";\n", hid, eff_name ? eff_name : "?");
        sb_indent(s, depth + 1);
        sb_printf(s, "__h%d.n_arms = %d;\n", hid, n->handle.arms.len);
        for (int i = 0; i < n->handle.arms.len; i++) {
            EffectArm *a = &n->handle.arms.items[i];
            sb_indent(s, depth + 1);
            sb_printf(s, "__h%d.arm_eff_names[%d] = \"%s\";\n",
                      hid, i, a->effect_name ? a->effect_name : (eff_name ? eff_name : "?"));
            sb_indent(s, depth + 1);
            sb_printf(s, "__h%d.arm_op_names[%d] = \"%s\";\n",
                      hid, i, a->op_name ? a->op_name : "?");
        }
        sb_indent(s, depth + 1);
        sb_printf(s, "__h%d.dispatch = __h%d_dispatch;\n", hid, hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "__h%d.prev = __xs_eff_top;\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "__xs_eff_top = &__h%d;\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "xs_val __h%d_result;\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "if (setjmp(__h%d.exit_jmp) == 0) {\n", hid);
        sb_indent(s, depth + 2);
        sb_printf(s, "__h%d_result = (", hid);
        emit_expr(s, n->handle.expr, depth + 2);
        sb_add(s, ");\n");
        sb_indent(s, depth + 1);
        sb_add(s, "} else {\n");
        sb_indent(s, depth + 2);
        sb_printf(s, "__h%d_result = __h%d.exit_value;\n", hid, hid);
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "__xs_eff_top = __h%d.prev;\n", hid);
        sb_indent(s, depth + 1);
        sb_printf(s, "__h%d_result;\n", hid);
        sb_indent(s, depth);
        sb_add(s, "})");
        break;
    }
    case NODE_TRY: {
        /* expression-position try: yields the body's trailing value on
         * normal completion, or the matching catch arm's value on
         * throw. Built around setjmp so the runtime can longjmp out of
         * arbitrary depth. */
        int tid = defer_label_counter++;
        sb_printf(s, "({ jmp_buf __try_jmp_%d; xs_val __try_r_%d = XS_NULL; "
                     "if (setjmp(__try_jmp_%d) == 0) { xs_push_handler(&__try_jmp_%d);\n",
                  tid, tid, tid, tid);
        if (n->try_.body && VAL_TAG(n->try_.body) == NODE_BLOCK) {
            for (int i = 0; i < n->try_.body->block.stmts.len; i++)
                emit_stmt(s, n->try_.body->block.stmts.items[i], depth + 1);
            sb_indent(s, depth + 1);
            sb_printf(s, "__try_r_%d = ", tid);
            if (n->try_.body->block.expr)
                emit_expr(s, n->try_.body->block.expr, depth + 1);
            else
                sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
        } else if (n->try_.body) {
            sb_indent(s, depth + 1);
            sb_printf(s, "__try_r_%d = ", tid);
            emit_expr(s, n->try_.body, depth + 1);
            sb_add(s, ";\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "xs_pop_handler();\n");
        sb_indent(s, depth);
        sb_add(s, "} else {\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "xs_val __try_exc_%d = xs_get_exception();\n", tid);
        for (int i = 0; i < n->try_.catch_arms.len; i++) {
            MatchArm *arm = &n->try_.catch_arms.items[i];
            char ebuf[64];
            snprintf(ebuf, sizeof ebuf, "__try_exc_%d", tid);
            if (arm->pattern && VAL_TAG(arm->pattern) == NODE_PAT_IDENT) {
                /* simple binder, always matches */
                sb_indent(s, depth + 1);
                sb_printf(s, "xs_val %s = %s;\n",
                          arm->pattern->pat_ident.name, ebuf);
            } else if (arm->pattern && VAL_TAG(arm->pattern) == NODE_PAT_WILD) {
                /* no binding */
            } else {
                emit_pattern_bindings(s, arm->pattern, ebuf, depth + 1);
            }
            sb_indent(s, depth + 1);
            sb_printf(s, "__try_r_%d = ", tid);
            if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                /* statement-expression for block bodies */
                sb_add(s, "({ ");
                for (int j = 0; j < arm->body->block.stmts.len; j++)
                    emit_stmt(s, arm->body->block.stmts.items[j], depth + 2);
                if (arm->body->block.expr) {
                    emit_expr(s, arm->body->block.expr, depth + 2);
                    sb_add(s, "; })");
                } else {
                    sb_add(s, "XS_NULL; })");
                }
            } else if (arm->body) {
                emit_expr(s, arm->body, depth + 1);
            } else {
                sb_add(s, "XS_NULL");
            }
            sb_add(s, ";\n");
            break; /* only first matching arm; richer match TBD */
        }
        sb_indent(s, depth);
        sb_printf(s, "} __try_r_%d; })", tid);
        break;
    }
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_LOOP:
    case NODE_NURSERY:
    case NODE_DEFER:
    case NODE_BREAK:
    case NODE_CONTINUE:
    case NODE_LOAD:
    case NODE_PLUGIN_DECL:
    case NODE_PROGRAM:
        sb_add(s, "XS_NULL");
        break;
    }
}

/* pattern condition for match */
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth) {
    if (!pat) { sb_add(s, "1"); return; }
    switch (VAL_TAG(pat)) {
    case NODE_PAT_WILD:
    case NODE_PAT_IDENT:
        sb_add(s, "1");
        break;
    case NODE_PAT_LIT:
        switch (pat->pat_lit.tag) {
        case 0: sb_printf(s, "(%s.i == %" PRId64 ")", subject, pat->pat_lit.ival); break;
        case 1: sb_printf(s, "(%s.f == %g)", subject, pat->pat_lit.fval); break;
        case 2: sb_printf(s, "(%s.s && strcmp(%s.s, \"%s\") == 0)", subject, subject,
                          pat->pat_lit.sval ? pat->pat_lit.sval : ""); break;
        case 3: sb_printf(s, "(xs_truthy(%s) == %d)", subject, pat->pat_lit.bval ? 1 : 0); break;
        case 4: sb_printf(s, "(%s.tag == 4)", subject); break;
        default: sb_add(s, "1"); break;
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
        sb_printf(s, "(xs_cmp(%s, ", subject);
        emit_expr(s, pat->pat_range.start, depth);
        sb_add(s, ") >= 0 && xs_cmp(");
        sb_add(s, subject);
        sb_add(s, ", ");
        emit_expr(s, pat->pat_range.end, depth);
        if (pat->pat_range.inclusive) sb_add(s, ") <= 0)");
        else sb_add(s, ") < 0)");
        break;
    case NODE_PAT_EXPR:
        sb_printf(s, "xs_eq(%s, ", subject);
        emit_expr(s, pat->pat_expr.expr, depth);
        sb_addc(s, ')');
        break;
    case NODE_PAT_GUARD: {
        /* for guards with ident patterns, bind first then check */
        Node *inner = pat->pat_guard.pattern;
        if (inner && VAL_TAG(inner) == NODE_PAT_IDENT) {
            /* (({ xs_val name = subject; truthy(guard) })) */
            sb_printf(s, "({ xs_val %s = %s; xs_truthy(", inner->pat_ident.name, subject);
            emit_expr(s, pat->pat_guard.guard, depth);
            sb_add(s, "); })");
        } else {
            emit_pattern_cond(s, inner, subject, depth);
            sb_add(s, " && xs_truthy(");
            emit_expr(s, pat->pat_guard.guard, depth);
            sb_addc(s, ')');
        }
        break;
    }
    case NODE_PAT_CAPTURE:
        emit_pattern_cond(s, pat->pat_capture.pattern, subject, depth);
        break;
    case NODE_PAT_TUPLE: {
        /* Tuples are xs_arr with is_tuple set. Without that guard a tuple
         * pattern would also fire on a plain array of the right length,
         * which is bug011's exact failure mode. Length check too so a
         * 2-elem pattern doesn't bind nulls from a 1-elem subject. */
        sb_printf(s, "(%s.tag == 5 && %s.p && ((xs_arr*)%s.p)->is_tuple"
                     " && ((xs_arr*)%s.p)->len == %d",
                  subject, subject, subject, subject, pat->pat_tuple.elems.len);
        for (int i = 0; i < pat->pat_tuple.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_INT(%d))", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_tuple.elems.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_STRUCT: {
        sb_printf(s, "(%s.tag != 4", subject); /* not null */
        if (pat->pat_struct.path && pat->pat_struct.path[0]) {
            /* tagged structs carry __type__ at init time; if the
             * subject has it, narrow the match to that name. Tolerant
             * if the tag is absent (untagged maps still match by
             * structural fields). */
            sb_printf(s,
                " && ((xs_index(%s, XS_STR(\"__type__\")).tag != 2) || "
                "xs_eq(xs_index(%s, XS_STR(\"__type__\")), XS_STR(\"%s\")))",
                subject, subject, pat->pat_struct.path);
        }
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            const char *key = pat->pat_struct.fields.items[i].key;
            if (pat->pat_struct.fields.items[i].val) {
                char sub[256];
                snprintf(sub, sizeof sub, "xs_index(%s, XS_STR(\"%s\"))",
                         subject, key ? key : "");
                sb_add(s, " && ");
                emit_pattern_cond(s, pat->pat_struct.fields.items[i].val, sub, depth);
            }
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_ENUM: {
        if (pat->pat_enum.path) {
            /* check _variant matches: extract variant name from path like "Shape::Circle" */
            const char *vname = pat->pat_enum.path;
            if (vname) {
                const char *sep = strstr(vname, "::");
                if (sep) vname = sep + 2;
            }
            sb_printf(s, "(xs_eq(xs_index(%s, XS_STR(\"_variant\")), XS_STR(\"%s\"))",
                      subject, vname ? vname : "?");
        } else {
            sb_add(s, "(1");
        }
        for (int i = 0; i < pat->pat_enum.args.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_STR(\"%d\"))", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_enum.args.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_SLICE: {
        /* Plain array pattern - reject tuples (is_tuple set). bug011 had a
         * tuple slipping through `[a, b]` because the tag-5 check matched
         * both shapes. */
        sb_printf(s, "(%s.tag == 5 && %s.p && !((xs_arr*)%s.p)->is_tuple",
                  subject, subject, subject);
        for (int i = 0; i < pat->pat_slice.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_INT(%d))", subject, i);
            sb_add(s, " && ");
            emit_pattern_cond(s, pat->pat_slice.elems.items[i], sub, depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_PAT_MAP: {
        /* tag 6 = map. require every named key to exist; without `..`
         * also reject extra keys so a tighter pattern doesn't accept a
         * superset map silently. */
        sb_printf(s, "(%s.tag == 6", subject);
        for (int i = 0; i < pat->pat_map.nfields; i++) {
            const char *k = pat->pat_map.keys[i];
            sb_printf(s, " && xs_truthy(xs_map_has(%s, XS_STR(\"%s\")))",
                      subject, k ? k : "");
            if (pat->pat_map.sub[i]) {
                char sub[256];
                snprintf(sub, sizeof sub,
                         "xs_index(%s, XS_STR(\"%s\"))", subject, k ? k : "");
                sb_add(s, " && ");
                emit_pattern_cond(s, pat->pat_map.sub[i], sub, depth);
            }
        }
        sb_addc(s, ')');
        break;
    }
    default:
        sb_add(s, "1");
        break;
    }
}

/* pattern binding emitter */
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth) {
    if (!pat) return;
    switch (VAL_TAG(pat)) {
    case NODE_PAT_IDENT:
        sb_indent(s, depth);
        sb_printf(s, "xs_val %s = %s;\n", pat->pat_ident.name, subject);
        break;
    case NODE_PAT_CAPTURE:
        if (pat->pat_capture.name) {
            sb_indent(s, depth);
            sb_printf(s, "xs_val %s = %s;\n", pat->pat_capture.name, subject);
        }
        emit_pattern_bindings(s, pat->pat_capture.pattern, subject, depth);
        break;
    case NODE_PAT_TUPLE:
        for (int i = 0; i < pat->pat_tuple.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_INT(%d))", subject, i);
            emit_pattern_bindings(s, pat->pat_tuple.elems.items[i], sub, depth);
        }
        break;
    case NODE_PAT_STRUCT:
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            const char *key = pat->pat_struct.fields.items[i].key;
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_STR(\"%s\"))",
                     subject, key ? key : "");
            if (pat->pat_struct.fields.items[i].val) {
                emit_pattern_bindings(s, pat->pat_struct.fields.items[i].val, sub, depth);
            } else if (key) {
                /* shortcut `Point { x }` binds local x to field x */
                sb_indent(s, depth);
                sb_printf(s, "xs_val %s = %s;\n", key, sub);
            }
        }
        break;
    case NODE_PAT_OR:
        emit_pattern_bindings(s, pat->pat_or.left, subject, depth);
        break;
    case NODE_PAT_GUARD:
        emit_pattern_bindings(s, pat->pat_guard.pattern, subject, depth);
        break;
    case NODE_PAT_ENUM:
        for (int i = 0; i < pat->pat_enum.args.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_STR(\"%d\"))", subject, i);
            emit_pattern_bindings(s, pat->pat_enum.args.items[i], sub, depth);
        }
        break;
    case NODE_PAT_SLICE:
        for (int i = 0; i < pat->pat_slice.elems.len; i++) {
            char sub[256];
            snprintf(sub, sizeof sub, "xs_index(%s, XS_INT(%d))", subject, i);
            emit_pattern_bindings(s, pat->pat_slice.elems.items[i], sub, depth);
        }
        if (pat->pat_slice.rest) {
            /* [a, b, ..rest] -> rest = subject[elems.len..] */
            sb_indent(s, depth);
            sb_printf(s,
                "xs_val %s = xs_slice(%s, XS_INT(%d), XS_NULL, 0);\n",
                pat->pat_slice.rest, subject, pat->pat_slice.elems.len);
        }
        break;
    case NODE_PAT_MAP:
        for (int i = 0; i < pat->pat_map.nfields; i++) {
            if (!pat->pat_map.sub[i]) continue;
            char sub[256];
            snprintf(sub, sizeof sub,
                     "xs_index(%s, XS_STR(\"%s\"))",
                     subject, pat->pat_map.keys[i] ? pat->pat_map.keys[i] : "");
            emit_pattern_bindings(s, pat->pat_map.sub[i], sub, depth);
        }
        break;
    default:
        break;
    }
}

/* emit block body */
static void emit_block_body(SB *s, Node *block, int depth) {
    if (!block) return;
    if (VAL_TAG(block) != NODE_BLOCK) {
        emit_stmt(s, block, depth);
        return;
    }
    for (int i = 0; i < block->block.stmts.len; i++) {
        emit_stmt(s, block->block.stmts.items[i], depth);
    }
    if (block->block.expr) {
        /* statement-like nodes should be emitted as statements */
        if (VAL_TAG(block->block.expr) == NODE_SPAWN ||
            VAL_TAG(block->block.expr) == NODE_NURSERY ||
            VAL_TAG(block->block.expr) == NODE_FOR ||
            VAL_TAG(block->block.expr) == NODE_WHILE ||
            VAL_TAG(block->block.expr) == NODE_LOOP ||
            VAL_TAG(block->block.expr) == NODE_TRY ||
            VAL_TAG(block->block.expr) == NODE_THROW ||
            VAL_TAG(block->block.expr) == NODE_IF ||
            VAL_TAG(block->block.expr) == NODE_BLOCK) {
            emit_stmt(s, block->block.expr, depth);
        } else {
            sb_indent(s, depth);
            emit_expr(s, block->block.expr, depth);
            sb_add(s, ";\n");
        }
    }
}

/* statement emitter */
static void emit_stmt(SB *s, Node *n, int depth) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_LET: {
        /* top-level binding: storage is the file-scope `static xs_val NAME`
         * declared earlier; here we just initialise it. lambdas that
         * capture it grab `&NAME` directly (handled in lambda emission). */
        if (n->let.name && is_top_level_var(n->let.name)) {
            sb_indent(s, depth);
            char nbuf[256];
            sb_printf(s, "%s = ", safe_name(n->let.name, nbuf, sizeof nbuf));
            if (n->let.value) emit_expr(s, n->let.value, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
            break;
        }
        /* check if captured by lambda (needs boxing for closures) */
        if (n->let.name) {
            int is_captured = 0;
            for (int li = 0; li < n_lambdas && !is_captured; li++)
                for (int ci = 0; ci < lambdas[li].n_captures; ci++)
                    if (strcmp(lambdas[li].captures[ci], n->let.name) == 0)
                        { is_captured = 1; break; }
            if (is_captured) {
                /* heap-allocate so closure can outlive this scope */
                sb_indent(s, depth);
                sb_printf(s, "xs_val *__box_%s = (xs_val*)malloc(sizeof(xs_val));\n", n->let.name);
                sb_indent(s, depth);
                sb_printf(s, "*__box_%s = ", n->let.name);
                if (n->let.value) emit_expr(s, n->let.value, depth);
                else sb_add(s, "XS_NULL");
                sb_add(s, ";\n");
                add_boxed_var(n->let.name);
                break;
            }
        }
        /* check for let x = spawn ActorName */
        int is_actor_spawn = 0;
        if (n->let.name && n->let.value && VAL_TAG(n->let.value) == NODE_SPAWN) {
            Node *se = n->let.value->spawn_.expr;
            if (se && VAL_TAG(se) == NODE_IDENT && find_actor(se->ident.name)) {
                is_actor_spawn = 1;
                sb_indent(s, depth);
                sb_printf(s, "/* spawn actor %s */\n", se->ident.name);
            }
        }
        if (!is_actor_spawn) {
            /* destructuring let: emit a temp + pattern bindings */
            if (!n->let.name && n->let.pattern) {
                int tid = defer_label_counter++;
                sb_indent(s, depth);
                sb_printf(s, "xs_val __destr_%d = ", tid);
                if (n->let.value) emit_expr(s, n->let.value, depth);
                else sb_add(s, "XS_NULL");
                sb_add(s, ";\n");
                char tname[64];
                snprintf(tname, sizeof tname, "__destr_%d", tid);
                emit_pattern_bindings(s, n->let.pattern, tname, depth);
                break;
            }
            sb_indent(s, depth);
            /* xs lets you mutate the contents of a let-bound container
             * (push/index-assign/etc) even though the binding itself is
             * immutable. emit a plain xs_val so taking &x in xs_map_put
             * doesn't discard a const qualifier; the sema layer already
             * rejects rebinding before we get here. */
            sb_add(s, "xs_val ");
            if (n->let.name) sb_add(s, n->let.name);
            else sb_add(s, "_");
            if (n->let.value) {
                sb_add(s, " = ");
                emit_expr(s, n->let.value, depth);
            } else {
                sb_add(s, " = XS_NULL");
            }
            sb_add(s, ";\n");
        }
        break;
    }
    case NODE_VAR: {
        /* top-level: storage is the file-scope `static xs_val NAME`. */
        if (n->let.name && is_top_level_var(n->let.name)) {
            sb_indent(s, depth);
            char nbuf[256];
            sb_printf(s, "%s = ", safe_name(n->let.name, nbuf, sizeof nbuf));
            if (n->let.value) emit_expr(s, n->let.value, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
            break;
        }
        /* check if this variable is captured by any lambda */
        int is_captured = 0;
        if (n->let.name) {
            for (int li = 0; li < n_lambdas && !is_captured; li++)
                for (int ci = 0; ci < lambdas[li].n_captures; ci++)
                    if (strcmp(lambdas[li].captures[ci], n->let.name) == 0)
                        { is_captured = 1; break; }
        }
        if (is_captured && n->let.name) {
            /* heap-allocate for closure capture */
            sb_indent(s, depth);
            sb_printf(s, "xs_val *__box_%s = (xs_val*)malloc(sizeof(xs_val));\n", n->let.name);
            sb_indent(s, depth);
            sb_printf(s, "*__box_%s = ", n->let.name);
            if (n->let.value) emit_expr(s, n->let.value, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
            add_boxed_var(n->let.name);
        } else if (!n->let.name && n->let.pattern) {
            /* destructuring var: temp + pattern bindings */
            int tid = defer_label_counter++;
            sb_indent(s, depth);
            sb_printf(s, "xs_val __destr_%d = ", tid);
            if (n->let.value) emit_expr(s, n->let.value, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
            char tname[64];
            snprintf(tname, sizeof tname, "__destr_%d", tid);
            emit_pattern_bindings(s, n->let.pattern, tname, depth);
        } else {
            sb_indent(s, depth);
            sb_add(s, "xs_val ");
            if (n->let.name) sb_add(s, n->let.name);
            else sb_add(s, "_");
            if (n->let.value) {
                sb_add(s, " = ");
                emit_expr(s, n->let.value, depth);
            } else {
                sb_add(s, " = XS_NULL");
            }
            sb_add(s, ";\n");
        }
        break;
    }
    case NODE_CONST:
        /* same story as NODE_LET above: const protects rebinding only,
         * which sema already enforces, and emitting a C const breaks
         * mutation through index/field assigns that xs allows. */
        if (n->const_.name && is_top_level_var(n->const_.name)) {
            sb_indent(s, depth);
            char nbuf[256];
            sb_printf(s, "%s = ", safe_name(n->const_.name, nbuf, sizeof nbuf));
            if (n->const_.value) emit_expr(s, n->const_.value, depth);
            else sb_add(s, "XS_NULL");
            sb_add(s, ";\n");
            break;
        }
        sb_indent(s, depth);
        {
            char nbuf[256];
            sb_printf(s, "xs_val %s", safe_name(n->const_.name, nbuf, sizeof nbuf));
        }
        if (n->const_.value) {
            sb_add(s, " = ");
            emit_expr(s, n->const_.value, depth);
        } else {
            sb_add(s, " = XS_NULL");
        }
        sb_add(s, ";\n");
        break;
    case NODE_FN_DECL: {
        /* Each function body is its own del scope - bug058's check_fn_del
         * 's `del y` shouldn't shadow a `var y` in main. Snapshot the
         * outer set, emit the body, restore. */
        int __saved_n_deleted = n_deleted_vars;
        if (is_main_fn(n)) {
            seen_main = 1;
            sb_indent(s, depth);
            sb_add(s, "int main(int argc, char **argv) {\n");
            sb_indent(s, depth + 1);
            sb_add(s, "xs_user_argc = argc; xs_user_argv = argv;\n");
            sb_indent(s, depth + 1);
            sb_add(s, "xs_push_frame(\"main\");\n");
            if (n->fn_decl.body && VAL_TAG(n->fn_decl.body) == NODE_BLOCK) {
                int has_defer = block_has_defers(n->fn_decl.body);
                if (has_defer) {
                    /* emit non-defer statements */
                    for (int i = 0; i < n->fn_decl.body->block.stmts.len; i++) {
                        Node *st = n->fn_decl.body->block.stmts.items[i];
                        if (st && VAL_TAG(st) != NODE_DEFER)
                            emit_stmt(s, st, depth + 1);
                    }
                    if (n->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        emit_expr(s, n->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                    /* cleanup label */
                    sb_indent(s, depth);
                    sb_add(s, "__cleanup:\n");
                    emit_deferred_cleanup(s, n->fn_decl.body, depth + 1);
                } else {
                    emit_block_body(s, n->fn_decl.body, depth + 1);
                    if (n->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        emit_expr(s, n->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                }
            }
            sb_indent(s, depth + 1);
            sb_add(s, "xs_run_defers(0);\n");
            sb_indent(s, depth + 1);
            sb_add(s, "xs_pop_frame();\n");
            sb_indent(s, depth + 1);
            sb_add(s, "return 0;\n");
            sb_indent(s, depth);
            sb_add(s, "}\n\n");
        } else {
            sb_indent(s, depth);
            sb_add(s, "xs_val ");
            char __mbuf[256];
            const char *fn_emit_name = mangle_overload(n->fn_decl.name,
                n->fn_decl.params.len, __mbuf, sizeof __mbuf);
            emit_safe_name(s, fn_emit_name);
            emit_params_c(s, &n->fn_decl.params);
            sb_add(s, " {\n");
            /* default param handling */
            for (int p = 0; p < n->fn_decl.params.len; p++) {
                Param *pm = &n->fn_decl.params.items[p];
                if (pm->default_val && pm->name) {
                    sb_indent(s, depth + 1);
                    sb_printf(s, "if (%s.tag == 4) %s = ", pm->name, pm->name);
                    emit_expr(s, pm->default_val, depth + 1);
                    sb_add(s, ";\n");
                }
            }
            /* box params that are captured by lambdas */
            for (int p = 0; p < n->fn_decl.params.len; p++) {
                const char *pname = n->fn_decl.params.items[p].name;
                if (!pname) continue;
                int is_captured = 0;
                for (int li = 0; li < n_lambdas && !is_captured; li++)
                    for (int ci = 0; ci < lambdas[li].n_captures; ci++)
                        if (strcmp(lambdas[li].captures[ci], pname) == 0)
                            { is_captured = 1; break; }
                if (is_captured) {
                    sb_indent(s, depth + 1);
                    sb_printf(s, "xs_val *__box_%s = (xs_val*)malloc(sizeof(xs_val));\n", pname);
                    sb_indent(s, depth + 1);
                    sb_printf(s, "*__box_%s = %s;\n", pname, pname);
                    add_boxed_var(pname);
                }
            }
            /* push call stack frame */
            sb_indent(s, depth + 1);
            sb_printf(s, "xs_push_frame(\"%s\");\n", n->fn_decl.name);
            sb_indent(s, depth + 1);
            sb_add(s, "int __saved_defer_top = __xs_defer_top;\n");
            if (n->fn_decl.body && VAL_TAG(n->fn_decl.body) == NODE_BLOCK) {
                int has_defer = block_has_defers(n->fn_decl.body);
                if (has_defer) {
                    /* Emit statements in source order so each `defer ...`
                     * registers its cleanup at the right point in time.
                     * If we hoist defers to a __cleanup: label they
                     * never run on throw -- xs_throw walks the defer
                     * stack and finds nothing registered. */
                    for (int i = 0; i < n->fn_decl.body->block.stmts.len; i++) {
                        emit_stmt(s, n->fn_decl.body->block.stmts.items[i], depth + 1);
                    }
                    if (n->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "xs_val __retval = ");
                        emit_expr(s, n->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                        sb_indent(s, depth + 1);
                        sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                        sb_indent(s, depth + 1);
                        sb_add(s, "xs_pop_frame();\n");
                        sb_indent(s, depth + 1);
                        sb_add(s, "return __retval;\n");
                    } else {
                        sb_indent(s, depth + 1);
                        sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                        sb_indent(s, depth + 1);
                        sb_add(s, "xs_pop_frame();\n");
                        sb_indent(s, depth + 1);
                        sb_add(s, "return XS_NULL;\n");
                    }
                } else {
                    /* emit stmts */
                    for (int bi = 0; bi < n->fn_decl.body->block.stmts.len; bi++)
                        emit_stmt(s, n->fn_decl.body->block.stmts.items[bi], depth + 1);
                    /* emit block.expr as return (implicit return) */
                    if (n->fn_decl.body->block.expr) {
                        Node *be = n->fn_decl.body->block.expr;
                        /* NODE_IF and NODE_MATCH have working expression forms
                           (ternary / GCC statement-expr). NODE_FOR/WHILE/LOOP
                           don't yield a useful value -> emit as statement. */
                        if (VAL_TAG(be) == NODE_FOR || VAL_TAG(be) == NODE_WHILE ||
                            VAL_TAG(be) == NODE_LOOP) {
                            emit_stmt(s, be, depth + 1);
                        } else if (VAL_TAG(be) == NODE_BLOCK) {
                            /* inline the block: emit its stmts then return the trailing expr */
                            for (int bi = 0; bi < be->block.stmts.len; bi++)
                                emit_stmt(s, be->block.stmts.items[bi], depth + 1);
                            if (be->block.expr) {
                                sb_indent(s, depth + 1);
                                sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                                sb_indent(s, depth + 1);
                                sb_add(s, "xs_pop_frame();\n");
                                sb_indent(s, depth + 1);
                                sb_add(s, "return ");
                                emit_expr(s, be->block.expr, depth + 1);
                                sb_add(s, ";\n");
                            }
                        } else if (VAL_TAG(be) == NODE_RETURN) {
                            emit_stmt(s, be, depth + 1);
                        } else {
                            sb_indent(s, depth + 1);
                            sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                            sb_indent(s, depth + 1);
                            sb_add(s, "xs_pop_frame();\n");
                            sb_indent(s, depth + 1);
                            sb_add(s, "return ");
                            emit_expr(s, be, depth + 1);
                            sb_add(s, ";\n");
                        }
                    }
                    sb_indent(s, depth + 1);
                    sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                    sb_indent(s, depth + 1);
                    sb_add(s, "xs_pop_frame();\n");
                    sb_indent(s, depth + 1);
                    sb_add(s, "return XS_NULL;\n");
                }
                /* clear boxed vars for this function scope */
                {
                    n_boxed = 0;
                }
            } else if (n->fn_decl.body) {
                sb_indent(s, depth + 1);
                sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                sb_indent(s, depth + 1);
                sb_add(s, "xs_pop_frame();\n");
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
                emit_expr(s, n->fn_decl.body, depth + 1);
                sb_add(s, ";\n");
            } else {
                sb_indent(s, depth + 1);
                sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                sb_indent(s, depth + 1);
                sb_add(s, "xs_pop_frame();\n");
                sb_indent(s, depth + 1);
                sb_add(s, "return XS_NULL;\n");
            }
            sb_indent(s, depth);
            sb_add(s, "}\n\n");
            /* emit a wrapper that adapts the typed C call signature to
             * xs_call's (env, args, argc) so the function can be passed
             * around as an xs_val. For overloaded names the wrapper
             * has to disambiguate by arity too, otherwise the second
             * `fn calc(x, y)` after `fn calc(x)` redefines the symbol
             * and the C compiler chokes. */
            if (n->fn_decl.name && !is_main_fn(n)) {
                int np = n->fn_decl.params.len;
                sb_indent(s, depth);
                if (count_fn_overloads(n->fn_decl.name) > 1) {
                    sb_printf(s, "static xs_val __xs_wrap_%s_%d(void *__env, xs_val *__args, int __argc) {\n",
                              n->fn_decl.name, np);
                } else {
                    sb_printf(s, "static xs_val __xs_wrap_%s(void *__env, xs_val *__args, int __argc) {\n",
                              n->fn_decl.name);
                }
                sb_indent(s, depth + 1);
                sb_add(s, "(void)__env; (void)__argc;\n");
                sb_indent(s, depth + 1);
                sb_add(s, "return ");
                emit_safe_name(s, fn_emit_name);
                sb_addc(s, '(');
                for (int p = 0; p < np; p++) {
                    if (p) sb_add(s, ", ");
                    sb_printf(s, "(__argc > %d ? __args[%d] : XS_NULL)", p, p);
                }
                sb_add(s, ");\n");
                sb_indent(s, depth);
                sb_add(s, "}\n\n");
            }
        }
        n_deleted_vars = __saved_n_deleted; /* unwind del scope on fn exit */
        break;
    }
    case NODE_RETURN: {
        /* Capture the return expression first, then unwind defers and
         * pop the call frame. The captured xs_val is on the C stack
         * and survives the unwind. */
        sb_indent(s, depth);
        sb_add(s, "{ xs_val __ret_v = ");
        if (n->ret.value) emit_expr(s, n->ret.value, depth);
        else              sb_add(s, "XS_NULL");
        sb_add(s, ";\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_run_defers(__saved_defer_top);\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_pop_frame();\n");
        sb_indent(s, depth + 1);
        sb_add(s, "return __ret_v;\n");
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_BREAK:
        sb_indent(s, depth);
        sb_add(s, "break;\n");
        break;
    case NODE_CONTINUE:
        sb_indent(s, depth);
        sb_add(s, "continue;\n");
        break;
    case NODE_IF: {
        sb_indent(s, depth);
        sb_add(s, "if (xs_truthy(");
        emit_expr(s, n->if_expr.cond, depth);
        sb_add(s, ")) {\n");
        if (n->if_expr.then)
            emit_block_body(s, n->if_expr.then, depth + 1);
        sb_indent(s, depth);
        sb_addc(s, '}');
        for (int i = 0; i < n->if_expr.elif_conds.len; i++) {
            sb_add(s, " else if (xs_truthy(");
            emit_expr(s, n->if_expr.elif_conds.items[i], depth);
            sb_add(s, ")) {\n");
            emit_block_body(s, n->if_expr.elif_thens.items[i], depth + 1);
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        if (n->if_expr.else_branch) {
            sb_add(s, " else {\n");
            emit_block_body(s, n->if_expr.else_branch, depth + 1);
            sb_indent(s, depth);
            sb_addc(s, '}');
        }
        sb_addc(s, '\n');
        break;
    }
    case NODE_WHILE:
        sb_indent(s, depth);
        sb_add(s, "while (xs_truthy(");
        emit_expr(s, n->while_loop.cond, depth);
        sb_add(s, ")) {\n");
        if (n->while_loop.body)
            emit_block_body(s, n->while_loop.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_FOR: {
        /* for pattern in iter -> iteration via xs_iter / xs_next */
        const char *loopvar = NULL;
        if (n->for_loop.pattern && VAL_TAG(n->for_loop.pattern) == NODE_PAT_IDENT)
            loopvar = n->for_loop.pattern->pat_ident.name;
        /* if any lambda captures the loop var, allocate a fresh box per
         * iteration so each closure snapshots its own value of i. */
        int loopvar_captured = 0;
        if (loopvar) {
            for (int li = 0; li < n_lambdas && !loopvar_captured; li++)
                for (int ci = 0; ci < lambdas[li].n_captures; ci++)
                    if (strcmp(lambdas[li].captures[ci], loopvar) == 0)
                        { loopvar_captured = 1; break; }
        }
        sb_indent(s, depth);
        sb_add(s, "{\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_val __iter = xs_iter(");
        emit_expr(s, n->for_loop.iter, depth + 1);
        sb_add(s, ");\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "xs_val %s;\n", loopvar ? loopvar : "__item");
        sb_indent(s, depth + 1);
        sb_printf(s, "while (xs_iter_next(&__iter, &%s)) {\n",
                  loopvar ? loopvar : "__item");
        if (loopvar_captured) {
            sb_indent(s, depth + 2);
            sb_printf(s, "xs_val *__box_%s = (xs_val*)malloc(sizeof(xs_val));\n", loopvar);
            sb_indent(s, depth + 2);
            sb_printf(s, "*__box_%s = %s;\n", loopvar, loopvar);
        }
        if (n->for_loop.body)
            emit_block_body(s, n->for_loop.body, depth + 2);
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_LOOP:
        sb_indent(s, depth);
        sb_add(s, "while (1) {\n");
        if (n->loop.body)
            emit_block_body(s, n->loop.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_MATCH: {
        /* statement-form match: do-while(0) + break for fall-through-on-guard-fail */
        sb_indent(s, depth);
        sb_add(s, "{\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_val __subject = ");
        emit_expr(s, n->match.subject, depth + 1);
        sb_add(s, ";\n");
        sb_indent(s, depth + 1);
        sb_add(s, "do {\n");
        for (int i = 0; i < n->match.arms.len; i++) {
            MatchArm *arm = &n->match.arms.items[i];
            sb_indent(s, depth + 2);
            sb_add(s, "if (");
            emit_pattern_cond(s, arm->pattern, "__subject", depth + 2);
            sb_add(s, ") {\n");
            emit_pattern_bindings(s, arm->pattern, "__subject", depth + 3);
            if (arm->guard) {
                sb_indent(s, depth + 3);
                sb_add(s, "if (xs_truthy(");
                emit_expr(s, arm->guard, depth + 3);
                sb_add(s, ")) {\n");
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK)
                    emit_block_body(s, arm->body, depth + 4);
                else if (arm->body) {
                    sb_indent(s, depth + 4);
                    emit_expr(s, arm->body, depth + 4);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth + 4);
                sb_add(s, "break;\n");
                sb_indent(s, depth + 3);
                sb_add(s, "}\n");
            } else {
                if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK)
                    emit_block_body(s, arm->body, depth + 3);
                else if (arm->body) {
                    sb_indent(s, depth + 3);
                    emit_expr(s, arm->body, depth + 3);
                    sb_add(s, ";\n");
                }
                sb_indent(s, depth + 3);
                sb_add(s, "break;\n");
            }
            sb_indent(s, depth + 2);
            sb_add(s, "}\n");
        }
        sb_indent(s, depth + 1);
        sb_add(s, "} while (0);\n");
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_TRY: {
        /* try/catch/finally -> setjmp/longjmp with proper unwinding */
        sb_indent(s, depth);
        sb_add(s, "{\n");

        /* save defer stack position for unwinding */
        sb_indent(s, depth + 1);
        sb_add(s, "int __saved_defer_top = __xs_defer_top;\n");

        sb_indent(s, depth + 1);
        sb_add(s, "jmp_buf __jmpbuf;\n");
        sb_indent(s, depth + 1);
        sb_add(s, "xs_val __exception = XS_NULL;\n");
        sb_indent(s, depth + 1);
        sb_add(s, "int __caught = 0;\n");

        sb_indent(s, depth + 1);
        sb_add(s, "if (setjmp(__jmpbuf) == 0) {\n");
        sb_indent(s, depth + 2);
        sb_add(s, "xs_push_handler(&__jmpbuf);\n");
        if (n->try_.body)
            emit_block_body(s, n->try_.body, depth + 2);
        sb_indent(s, depth + 2);
        sb_add(s, "xs_pop_handler();\n");
        sb_indent(s, depth + 2);
        sb_add(s, "__caught = 1; /* normal completion */\n");
        sb_indent(s, depth + 1);
        sb_add(s, "}");

        if (n->try_.catch_arms.len > 0) {
            sb_add(s, " else {\n");
            sb_indent(s, depth + 2);
            sb_add(s, "/* handler already popped by xs_throw/longjmp */\n");
            sb_indent(s, depth + 2);
            sb_add(s, "__exception = xs_get_exception();\n");
            sb_indent(s, depth + 2);
            sb_add(s, "int __exc_tag = xs_get_exception_tag();\n");
            sb_indent(s, depth + 2);
            sb_add(s, "(void)__exc_tag; /* available for typed catch */\n");
            sb_indent(s, depth + 2);
            sb_add(s, "__caught = 1;\n");

            /* Emit catch arms with pattern matching */
            int has_multi = n->try_.catch_arms.len > 1;
            for (int i = 0; i < n->try_.catch_arms.len; i++) {
                MatchArm *arm = &n->try_.catch_arms.items[i];
                if (has_multi && arm->pattern) {
                    sb_indent(s, depth + 2);
                    if (i == 0) sb_add(s, "if (");
                    else sb_add(s, "else if (");
                    /* Check if pattern has a type constraint by checking tag */
                    emit_pattern_cond(s, arm->pattern, "__exception", depth + 2);
                    if (arm->guard) {
                        sb_add(s, " && xs_truthy(");
                        emit_expr(s, arm->guard, depth + 2);
                        sb_addc(s, ')');
                    }
                    sb_add(s, ") {\n");
                    emit_pattern_bindings(s, arm->pattern, "__exception", depth + 3);
                    if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                        emit_block_body(s, arm->body, depth + 3);
                    } else if (arm->body) {
                        sb_indent(s, depth + 3);
                        emit_expr(s, arm->body, depth + 3);
                        sb_add(s, ";\n");
                    }
                    sb_indent(s, depth + 2);
                    sb_add(s, "}\n");
                } else {
                    /* Single catch arm or wildcard - bind and execute directly */
                    emit_pattern_bindings(s, arm->pattern, "__exception", depth + 2);
                    if (arm->body && VAL_TAG(arm->body) == NODE_BLOCK) {
                        emit_block_body(s, arm->body, depth + 2);
                    } else if (arm->body) {
                        sb_indent(s, depth + 2);
                        emit_expr(s, arm->body, depth + 2);
                        sb_add(s, ";\n");
                    }
                }
            }
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
        } else {
            sb_add(s, " else {\n");
            sb_indent(s, depth + 2);
            sb_add(s, "/* no catch arms: rethrow after finally */\n");
            sb_indent(s, depth + 2);
            sb_add(s, "__exception = xs_get_exception();\n");
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
        }

        /* finally block: always runs regardless of exception */
        if (n->try_.finally_block) {
            sb_indent(s, depth + 1);
            sb_add(s, "/* finally block - always executes */\n");
            sb_indent(s, depth + 1);
            sb_add(s, "{\n");
            emit_block_body(s, n->try_.finally_block, depth + 2);
            sb_indent(s, depth + 1);
            sb_add(s, "}\n");
        }

        /* run defers accumulated in this try scope */
        sb_indent(s, depth + 1);
        sb_add(s, "xs_run_defers(__saved_defer_top);\n");

        /* if exception was not caught, rethrow */
        if (n->try_.catch_arms.len == 0) {
            sb_indent(s, depth + 1);
            sb_add(s, "if (!__caught) xs_rethrow();\n");
        }

        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_THROW:
        sb_indent(s, depth);
        sb_add(s, "xs_throw(");
        emit_expr(s, n->throw_.value, depth);
        sb_add(s, ");\n");
        break;
    case NODE_DEFER: {
        /* defer: register a cleanup function on the defer stack.
           We emit an inline block and register it via xs_push_defer.
           For simplicity, we use a static nested function (GCC extension)
           or emit the body inline with a saved marker. */
        sb_indent(s, depth);
        sb_add(s, "{ /* defer registration */\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "auto void __defer_fn_%d(void);\n", defer_label_counter);
        sb_indent(s, depth + 1);
        sb_printf(s, "void __defer_fn_%d(void) {\n", defer_label_counter);
        if (n->defer_.body) {
            if (VAL_TAG(n->defer_.body) == NODE_BLOCK) {
                emit_block_body(s, n->defer_.body, depth + 2);
            } else {
                sb_indent(s, depth + 2);
                emit_expr(s, n->defer_.body, depth + 2);
                sb_add(s, ";\n");
            }
        }
        sb_indent(s, depth + 1);
        sb_add(s, "}\n");
        sb_indent(s, depth + 1);
        sb_printf(s, "xs_push_defer(__defer_fn_%d);\n", defer_label_counter);
        defer_label_counter++;
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    }
    case NODE_YIELD:
        sb_indent(s, depth);
        sb_add(s, "fprintf(stderr, \"xs: yield not supported in C target\\n\"); exit(1);\n");
        break;
    case NODE_STRUCT_DECL: {
        sb_indent(s, depth);
        sb_printf(s, "typedef struct %s {\n", n->struct_decl.name);
        for (int i = 0; i < n->struct_decl.fields.len; i++) {
            sb_indent(s, depth + 1);
            sb_printf(s, "xs_val %s;\n", n->struct_decl.fields.items[i].key);
        }
        sb_indent(s, depth);
        sb_printf(s, "} %s;\n\n", n->struct_decl.name);
        break;
    }
    case NODE_ENUM_DECL: {
        /* Enum -> C enum for tag + optional data struct */
        sb_indent(s, depth);
        sb_printf(s, "typedef enum {\n");
        for (int i = 0; i < n->enum_decl.variants.len; i++) {
            EnumVariant *v = &n->enum_decl.variants.items[i];
            sb_indent(s, depth + 1);
            sb_printf(s, "%s_%s", n->enum_decl.name, v->name);
            if (i < n->enum_decl.variants.len - 1) sb_addc(s, ',');
            sb_addc(s, '\n');
        }
        sb_indent(s, depth);
        sb_printf(s, "} %s_Tag;\n\n", n->enum_decl.name);
        /* Also emit a tagged union struct for data-carrying variants */
        int has_data = 0;
        for (int i = 0; i < n->enum_decl.variants.len; i++) {
            if (n->enum_decl.variants.items[i].fields.len > 0) { has_data = 1; break; }
        }
        if (has_data) {
            sb_indent(s, depth);
            sb_printf(s, "typedef struct {\n");
            sb_indent(s, depth + 1);
            sb_printf(s, "%s_Tag tag;\n", n->enum_decl.name);
            sb_indent(s, depth + 1);
            sb_add(s, "union {\n");
            for (int i = 0; i < n->enum_decl.variants.len; i++) {
                EnumVariant *v = &n->enum_decl.variants.items[i];
                if (v->fields.len > 0) {
                    sb_indent(s, depth + 2);
                    sb_add(s, "struct {\n");
                    for (int j = 0; j < v->fields.len; j++) {
                        sb_indent(s, depth + 3);
                        sb_printf(s, "xs_val f%d;\n", j);
                    }
                    sb_indent(s, depth + 2);
                    sb_printf(s, "} %s;\n", v->name);
                }
            }
            sb_indent(s, depth + 1);
            sb_add(s, "};\n");
            sb_indent(s, depth);
            sb_printf(s, "} %s;\n\n", n->enum_decl.name);
        }
        break;
    }
    case NODE_IMPL_DECL: {
        sb_indent(s, depth);
        sb_printf(s, "/* impl %s", n->impl_decl.type_name);
        if (n->impl_decl.trait_name) sb_printf(s, " : %s", n->impl_decl.trait_name);
        sb_add(s, " */\n");
        for (int i = 0; i < n->impl_decl.members.len; i++) {
            Node *m = n->impl_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.name) {
                /* Rename method to TypeName_methodName(xs_val self, ...) */
                sb_indent(s, depth);
                sb_printf(s, "static xs_val %s_%s(xs_val self",
                          n->impl_decl.type_name, m->fn_decl.name);
                for (int p = 0; p < m->fn_decl.params.len; p++) {
                    const char *pname = m->fn_decl.params.items[p].name;
                    if (pname && strcmp(pname, "self") == 0) continue;
                    sb_add(s, ", xs_val ");
                    emit_safe_name(s, pname ? pname : "_");
                }
                sb_add(s, ") {\n");
                in_method_body = 1;
                const char *prev_cls = current_class_name;
                current_class_name = n->impl_decl.type_name;
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    for (int si = 0; si < m->fn_decl.body->block.stmts.len; si++)
                        emit_stmt(s, m->fn_decl.body->block.stmts.items[si], depth + 1);
                    if (m->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, m->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    } else {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return XS_NULL;\n");
                    }
                } else {
                    sb_indent(s, depth + 1);
                    sb_add(s, "return XS_NULL;\n");
                }
                current_class_name = prev_cls;
                in_method_body = 0;
                sb_indent(s, depth);
                sb_add(s, "}\n\n");
            } else if (m) {
                emit_stmt(s, m, depth);
            }
        }
        break;
    }
    case NODE_TRAIT_DECL: {
        /* Trait -> vtable typedef */
        sb_indent(s, depth);
        sb_printf(s, "/* trait %s */\n", n->trait_decl.name);
        sb_indent(s, depth);
        sb_printf(s, "typedef struct %s_vtable {\n", n->trait_decl.name);
        for (int i = 0; i < n->trait_decl.n_methods; i++) {
            sb_indent(s, depth + 1);
            sb_printf(s, "xs_val (*%s)(void *self);\n", n->trait_decl.method_names[i]);
        }
        sb_indent(s, depth);
        sb_printf(s, "} %s_vtable;\n\n", n->trait_decl.name);
        break;
    }
    case NODE_TYPE_ALIAS:
        sb_indent(s, depth);
        sb_printf(s, "/* type %s = %s */\n", n->type_alias.name,
                  n->type_alias.target ? n->type_alias.target : "?");
        break;
    case NODE_IMPORT:
    case NODE_USE:
        sb_indent(s, depth);
        if (VAL_TAG(n) == NODE_USE) {
            sb_add(s, "/* use ");
            for (int i = 0; i < n->import.nparts; i++) {
                if (i) sb_addc(s, '/');
                sb_add(s, n->import.path[i]);
            }
            sb_add(s, " (not supported in C target) */\n");
        } else {
            sb_add(s, "/* import ");
            for (int i = 0; i < n->import.nparts; i++) {
                if (i) sb_addc(s, '.');
                sb_add(s, n->import.path[i]);
            }
            sb_add(s, " */\n");
        }
        break;
    case NODE_MODULE_DECL:
        sb_indent(s, depth);
        sb_printf(s, "/* module %s */\n", n->module_decl.name);
        for (int i = 0; i < n->module_decl.body.len; i++)
            emit_stmt(s, n->module_decl.body.items[i], depth);
        break;
    case NODE_CLASS_DECL: {
        /* Class -> struct with vtable pointer and any base fields */
        sb_indent(s, depth);
        sb_printf(s, "typedef struct %s {\n", n->class_decl.name);
        if (n->class_decl.nbases > 0 && n->class_decl.bases && n->class_decl.bases[0]) {
            sb_indent(s, depth + 1);
            sb_printf(s, "/* extends %s */\n", n->class_decl.bases[0]);
        }
        sb_indent(s, depth + 1);
        sb_add(s, "void *__vtable;\n");
        sb_indent(s, depth + 1);
        sb_add(s, "int __tag;\n");
        /* scan constructor for field assignments (self.x = ...) */
        for (int i = 0; i < n->class_decl.members.len; i++) {
            Node *m = n->class_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.name &&
                (strcmp(m->fn_decl.name, "new") == 0 || strcmp(m->fn_decl.name, "init") == 0)) {
                /* emit params as fields (heuristic) */
                for (int p = 0; p < m->fn_decl.params.len; p++) {
                    const char *pn = m->fn_decl.params.items[p].name;
                    if (pn && strcmp(pn, "self") != 0) {
                        sb_indent(s, depth + 1);
                        sb_printf(s, "xs_val %s;\n", pn);
                    }
                }
            }
        }
        sb_indent(s, depth);
        sb_printf(s, "} %s;\n\n", n->class_decl.name);
        /* emit methods as standalone functions */
        for (int i = 0; i < n->class_decl.members.len; i++) {
            Node *m = n->class_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL) {
                sb_indent(s, depth);
                sb_add(s, "xs_val ");
                sb_add(s, n->class_decl.name);
                sb_addc(s, '_');
                sb_add(s, m->fn_decl.name);
                sb_addc(s, '(');
                /* class instances are stored as xs_val maps (tag=6); take
                 * self by xs_val so xs_index / xs_map_put can route to the
                 * underlying map. matches impl_decl emit conventions. */
                sb_add(s, "xs_val self");
                for (int p = 0; p < m->fn_decl.params.len; p++) {
                    const char *pname = m->fn_decl.params.items[p].name;
                    if (pname && strcmp(pname, "self") == 0) continue;
                    sb_add(s, ", xs_val ");
                    if (pname) sb_add(s, pname);
                    else sb_add(s, "_");
                }
                sb_add(s, ") {\n");
                const char *prev_cls = current_class_name;
                current_class_name = n->class_decl.name;
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    /* peel the trailing expr off so emit_block_body doesn't
                     * also emit it as a statement; we want it as a return. */
                    Node *be = m->fn_decl.body->block.expr;
                    m->fn_decl.body->block.expr = NULL;
                    emit_block_body(s, m->fn_decl.body, depth + 1);
                    m->fn_decl.body->block.expr = be;
                    /* assignment-as-tail isn't a return value */
                    if (be && VAL_TAG(be) == NODE_ASSIGN) {
                        sb_indent(s, depth + 1);
                        emit_expr(s, be, depth + 1);
                        sb_add(s, ";\n");
                    } else if (be) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, be, depth + 1);
                        sb_add(s, ";\n");
                    }
                }
                current_class_name = prev_cls;
                sb_indent(s, depth + 1);
                sb_add(s, "return XS_NULL;\n");
                sb_indent(s, depth);
                sb_add(s, "}\n\n");
            }
        }
        break;
    }
    case NODE_EFFECT_DECL:
        sb_indent(s, depth);
        sb_printf(s, "/* effect %s */\n", n->effect_decl.name);
        sb_indent(s, depth);
        sb_printf(s, "typedef struct {\n");
        sb_indent(s, depth + 1);
        sb_add(s, "int op;\n");
        sb_indent(s, depth);
        sb_printf(s, "} %s_effect;\n\n", n->effect_decl.name);
        break;
    case NODE_HANDLE:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_NURSERY:
        sb_indent(s, depth);
        sb_add(s, "/* nursery (no native thread support) */\n");
        sb_indent(s, depth);
        sb_add(s, "{\n");
        if (n->nursery_.body) emit_block_body(s, n->nursery_.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    case NODE_DEL:
        sb_indent(s, depth);
        if (n->del_.name) {
            sb_printf(s, "%s = (xs_val){.tag = 99}; /* del */\n", n->del_.name);
            mark_deleted_var(n->del_.name);
        } else {
            sb_add(s, "; /* del without name */\n");
        }
        break;
    case NODE_ASSIGN:
        sb_indent(s, depth);
        /* tuple destructuring assign: (a, b) = (b, a). xs_array(...)
         * is not an lvalue, so evaluate the rhs into temps first and
         * then write each target one at a time. */
        if (n->assign.target && VAL_TAG(n->assign.target) == NODE_LIT_TUPLE) {
            int nelems = n->assign.target->lit_array.elems.len;
            sb_add(s, "{\n");
            if (n->assign.value && VAL_TAG(n->assign.value) == NODE_LIT_TUPLE &&
                n->assign.value->lit_array.elems.len == nelems) {
                /* element-wise lowering; handles parallel swaps without
                 * materialising an array */
                for (int i = 0; i < nelems; i++) {
                    sb_indent(s, depth + 1);
                    sb_printf(s, "xs_val __t%d = ", i);
                    emit_expr(s, n->assign.value->lit_array.elems.items[i], depth + 1);
                    sb_add(s, ";\n");
                }
                for (int i = 0; i < nelems; i++) {
                    sb_indent(s, depth + 1);
                    emit_expr(s, n->assign.target->lit_array.elems.items[i], depth + 1);
                    sb_printf(s, " = __t%d;\n", i);
                }
            } else {
                sb_indent(s, depth + 1);
                sb_add(s, "xs_val __rhs = ");
                emit_expr(s, n->assign.value, depth + 1);
                sb_add(s, ";\n");
                for (int i = 0; i < nelems; i++) {
                    sb_indent(s, depth + 1);
                    emit_expr(s, n->assign.target->lit_array.elems.items[i], depth + 1);
                    sb_printf(s, " = xs_index(__rhs, XS_INT(%d));\n", i);
                }
            }
            sb_indent(s, depth);
            sb_add(s, "}\n");
        } else if (n->assign.target && VAL_TAG(n->assign.target) == NODE_INDEX) {
            /* index assignment: obj[key] = val → xs_map_put */
            sb_add(s, "xs_map_put(&");
            emit_expr(s, n->assign.target->index.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->assign.target->index.index, depth);
            sb_add(s, ", ");
            emit_expr(s, n->assign.value, depth);
            sb_add(s, ");\n");
        } else if (n->assign.target && VAL_TAG(n->assign.target) == NODE_FIELD &&
            !(n_actor_fields > 0 && in_method_body &&
              n->assign.target->field.obj &&
              VAL_TAG(n->assign.target->field.obj) == NODE_IDENT &&
              strcmp(n->assign.target->field.obj->ident.name, "self") == 0)) {
            sb_add(s, "xs_map_put(&");
            emit_expr(s, n->assign.target->field.obj, depth);
            sb_printf(s, ", XS_STR(\"%s\"), ", n->assign.target->field.name);
            emit_expr(s, n->assign.value, depth);
            sb_add(s, ");\n");
        } else {
            emit_expr(s, n->assign.target, depth);
            sb_addc(s, ' ');
            sb_add(s, n->assign.op);
            sb_addc(s, ' ');
            emit_expr(s, n->assign.value, depth);
            sb_add(s, ";\n");
        }
        break;
    case NODE_EXPR_STMT: {
        Node *inner = n->expr_stmt.expr;
        /* tuple-destructuring assignment can only be a statement; route
         * through emit_stmt so its lowering kicks in. */
        int tuple_assign_stmt = inner && VAL_TAG(inner) == NODE_ASSIGN &&
            inner->assign.target &&
            VAL_TAG(inner->assign.target) == NODE_LIT_TUPLE;
        if (inner && (VAL_TAG(inner) == NODE_IF || VAL_TAG(inner) == NODE_MATCH ||
                      VAL_TAG(inner) == NODE_FOR || VAL_TAG(inner) == NODE_WHILE ||
                      VAL_TAG(inner) == NODE_LOOP || VAL_TAG(inner) == NODE_TRY ||
                      VAL_TAG(inner) == NODE_BLOCK || VAL_TAG(inner) == NODE_NURSERY ||
                      VAL_TAG(inner) == NODE_SPAWN || VAL_TAG(inner) == NODE_ACTOR_DECL ||
                      VAL_TAG(inner) == NODE_RETURN || tuple_assign_stmt)) {
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
    case NODE_SPAWN: {
        /* spawn as statement */
        Node *se = n->spawn_.expr;
        if (se && VAL_TAG(se) == NODE_IDENT && find_actor(se->ident.name)) {
            /* spawn ActorName -> not useful as a standalone stmt, skip */
            sb_indent(s, depth);
            sb_add(s, "/* spawn actor (see let binding) */\n");
        } else if (se && VAL_TAG(se) == NODE_BLOCK) {
            /* spawn { block } as statement -> just execute the block */
            sb_indent(s, depth);
            sb_add(s, "{\n");
            emit_block_body(s, se, depth + 1);
            sb_indent(s, depth);
            sb_add(s, "}\n");
        } else {
            sb_indent(s, depth);
            emit_expr(s, n, depth);
            sb_add(s, ";\n");
        }
        break;
    }
    case NODE_ACTOR_DECL:
        /* actor decl handled at file scope by emit_actor_decl */
        break;
    case NODE_SEND_EXPR:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_PERFORM:
    case NODE_RESUME:
    case NODE_AWAIT:
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    case NODE_BIND:
        sb_indent(s, depth);
        sb_printf(s, "xs_val %s = ", n->bind_decl.name ? n->bind_decl.name : "_bind");
        emit_expr(s, n->bind_decl.expr, depth);
        sb_add(s, ";\n");
        break;
    case NODE_TAG_DECL:
        /* Emit as regular function with __block as last param */
        sb_indent(s, depth);
        sb_printf(s, "xs_val %s(", n->tag_decl.name ? n->tag_decl.name : "_tag");
        for (int ti = 0; ti < n->tag_decl.params.len; ti++) {
            Param *pm = &n->tag_decl.params.items[ti];
            sb_printf(s, "xs_val %s, ", pm->name ? pm->name : "_");
        }
        sb_add(s, "xs_val __block) {\n");
        if (n->tag_decl.body) emit_stmt(s, n->tag_decl.body, depth + 1);
        sb_indent(s, depth);
        sb_add(s, "}\n");
        break;
    default:
        /* emit as expression statement for any unhandled node */
        sb_indent(s, depth);
        emit_expr(s, n, depth);
        sb_add(s, ";\n");
        break;
    }
}

/* public entry point */
/* ---- C target AST lowering pre-pass ----------------------------------- */

/* Walk an AST subtree and rewrite every `main` ident reference to
 * `__user_main`. Used when the source defines a `main` -- we hoist top-
 * level statements into the synthesised entry instead and let user code
 * refer to its own main under the renamed symbol. */
static void c_rename_main_refs(Node *n) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_IDENT:
        if (n->ident.name && strcmp(n->ident.name, "main") == 0) {
            free(n->ident.name);
            n->ident.name = xs_strdup("__user_main");
        }
        return;
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            c_rename_main_refs(n->program.stmts.items[i]);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            c_rename_main_refs(n->block.stmts.items[i]);
        c_rename_main_refs(n->block.expr);
        return;
    case NODE_FN_DECL:    c_rename_main_refs(n->fn_decl.body); return;
    case NODE_LAMBDA:     c_rename_main_refs(n->lambda.body);  return;
    case NODE_IF:
        c_rename_main_refs(n->if_expr.cond);
        c_rename_main_refs(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_conds.len; i++)
            c_rename_main_refs(n->if_expr.elif_conds.items[i]);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_rename_main_refs(n->if_expr.elif_thens.items[i]);
        c_rename_main_refs(n->if_expr.else_branch);
        return;
    case NODE_WHILE:
        c_rename_main_refs(n->while_loop.cond);
        c_rename_main_refs(n->while_loop.body);
        return;
    case NODE_FOR:
        c_rename_main_refs(n->for_loop.iter);
        c_rename_main_refs(n->for_loop.body);
        return;
    case NODE_LOOP:       c_rename_main_refs(n->loop.body); return;
    case NODE_LET: case NODE_VAR: c_rename_main_refs(n->let.value); return;
    case NODE_CONST:      c_rename_main_refs(n->const_.value); return;
    case NODE_EXPR_STMT:  c_rename_main_refs(n->expr_stmt.expr); return;
    case NODE_RETURN:     c_rename_main_refs(n->ret.value); return;
    case NODE_ASSIGN:
        c_rename_main_refs(n->assign.target);
        c_rename_main_refs(n->assign.value);
        return;
    case NODE_BINOP:
        c_rename_main_refs(n->binop.left);
        c_rename_main_refs(n->binop.right);
        return;
    case NODE_UNARY:      c_rename_main_refs(n->unary.expr); return;
    case NODE_CALL:
        c_rename_main_refs(n->call.callee);
        for (int i = 0; i < n->call.args.len; i++)
            c_rename_main_refs(n->call.args.items[i]);
        return;
    case NODE_METHOD_CALL:
        c_rename_main_refs(n->method_call.obj);
        for (int i = 0; i < n->method_call.args.len; i++)
            c_rename_main_refs(n->method_call.args.items[i]);
        return;
    case NODE_INDEX:
        c_rename_main_refs(n->index.obj);
        c_rename_main_refs(n->index.index);
        return;
    case NODE_FIELD:      c_rename_main_refs(n->field.obj); return;
    case NODE_TRY:
        c_rename_main_refs(n->try_.body);
        c_rename_main_refs(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_rename_main_refs(n->try_.catch_arms.items[i].body);
        return;
    case NODE_THROW:      c_rename_main_refs(n->throw_.value); return;
    case NODE_MATCH:
        c_rename_main_refs(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++)
            c_rename_main_refs(n->match.arms.items[i].body);
        return;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        for (int i = 0; i < n->lit_array.elems.len; i++)
            c_rename_main_refs(n->lit_array.elems.items[i]);
        return;
    case NODE_LIT_MAP:
        for (int i = 0; i < n->lit_map.keys.len; i++)
            c_rename_main_refs(n->lit_map.keys.items[i]);
        for (int i = 0; i < n->lit_map.vals.len; i++)
            c_rename_main_refs(n->lit_map.vals.items[i]);
        return;
    case NODE_AWAIT:      c_rename_main_refs(n->await_.expr); return;
    case NODE_SPAWN:      c_rename_main_refs(n->spawn_.expr); return;
    case NODE_NURSERY:    c_rename_main_refs(n->nursery_.body); return;
    case NODE_RANGE:
        c_rename_main_refs(n->range.start);
        c_rename_main_refs(n->range.end);
        return;
    case NODE_DEFER:      c_rename_main_refs(n->defer_.body); return;
    case NODE_INTERP_STRING:
        for (int i = 0; i < n->lit_string.parts.len; i++)
            c_rename_main_refs(n->lit_string.parts.items[i]);
        return;
    case NODE_YIELD:      c_rename_main_refs(n->yield_.value); return;
    default: return;
    }
}

/* AST walker that strips `await x`/`spawn x`/`nursery {body}` markers
 * and clears the `is_async` flag from fn decls / lambdas. The C target
 * is single-threaded, so async fn == plain fn; await is a no-op on a
 * value that's already resolved. Returns a possibly-different node
 * (when the wrapper is consumed). */
static Node *c_lower_node(Node *n);
static void c_lower_nodelist(NodeList *l) {
    if (!l) return;
    for (int i = 0; i < l->len; i++) l->items[i] = c_lower_node(l->items[i]);
}
static void c_lower_nodepairlist(NodePairList *l) {
    if (!l) return;
    for (int i = 0; i < l->len; i++) l->items[i].val = c_lower_node(l->items[i].val);
}
static Node *c_lower_node(Node *n) {
    if (!n) return NULL;
    switch (VAL_TAG(n)) {
    case NODE_AWAIT: {
        Node *inner = c_lower_node(n->await_.expr);
        n->await_.expr = NULL;
        node_free(n);
        return inner;
    }
    case NODE_SPAWN: {
        /* Preserve the spawn wrapper for actor/block-spawn cases that
         * the codegen still treats specially; for plain expressions we
         * unwrap to skip the dummy result map. */
        Node *inner = n->spawn_.expr;
        if (inner && VAL_TAG(inner) != NODE_BLOCK &&
            !(VAL_TAG(inner) == NODE_IDENT && find_actor(inner->ident.name))) {
            Node *lowered = c_lower_node(inner);
            n->spawn_.expr = NULL;
            node_free(n);
            return lowered;
        }
        n->spawn_.expr = c_lower_node(n->spawn_.expr);
        return n;
    }
    case NODE_NURSERY: {
        Node *body = c_lower_node(n->nursery_.body);
        n->nursery_.body = NULL;
        node_free(n);
        return body;
    }
    case NODE_FN_DECL:
        n->fn_decl.is_async = 0;
        n->fn_decl.body = c_lower_node(n->fn_decl.body);
        return n;
    case NODE_LAMBDA:
        n->lambda.body = c_lower_node(n->lambda.body);
        return n;
    case NODE_BLOCK:
        c_lower_nodelist(&n->block.stmts);
        n->block.expr = c_lower_node(n->block.expr);
        return n;
    case NODE_PROGRAM:
        c_lower_nodelist(&n->program.stmts);
        return n;
    case NODE_IF:
        n->if_expr.cond = c_lower_node(n->if_expr.cond);
        n->if_expr.then = c_lower_node(n->if_expr.then);
        c_lower_nodelist(&n->if_expr.elif_conds);
        c_lower_nodelist(&n->if_expr.elif_thens);
        n->if_expr.else_branch = c_lower_node(n->if_expr.else_branch);
        return n;
    case NODE_WHILE:
        n->while_loop.cond = c_lower_node(n->while_loop.cond);
        n->while_loop.body = c_lower_node(n->while_loop.body);
        return n;
    case NODE_FOR:
        n->for_loop.iter = c_lower_node(n->for_loop.iter);
        n->for_loop.body = c_lower_node(n->for_loop.body);
        return n;
    case NODE_LOOP:
        n->loop.body = c_lower_node(n->loop.body);
        return n;
    case NODE_LET: case NODE_VAR:
        n->let.value = c_lower_node(n->let.value);
        return n;
    case NODE_CONST:
        n->const_.value = c_lower_node(n->const_.value);
        return n;
    case NODE_EXPR_STMT:
        n->expr_stmt.expr = c_lower_node(n->expr_stmt.expr);
        return n;
    case NODE_RETURN:
        n->ret.value = c_lower_node(n->ret.value);
        return n;
    case NODE_ASSIGN:
        n->assign.target = c_lower_node(n->assign.target);
        n->assign.value = c_lower_node(n->assign.value);
        return n;
    case NODE_BINOP:
        n->binop.left  = c_lower_node(n->binop.left);
        n->binop.right = c_lower_node(n->binop.right);
        return n;
    case NODE_UNARY:
        n->unary.expr = c_lower_node(n->unary.expr);
        return n;
    case NODE_CALL:
        n->call.callee = c_lower_node(n->call.callee);
        c_lower_nodelist(&n->call.args);
        c_lower_nodepairlist(&n->call.kwargs);
        return n;
    case NODE_METHOD_CALL:
        n->method_call.obj = c_lower_node(n->method_call.obj);
        c_lower_nodelist(&n->method_call.args);
        c_lower_nodepairlist(&n->method_call.kwargs);
        return n;
    case NODE_INDEX:
        n->index.obj = c_lower_node(n->index.obj);
        n->index.index = c_lower_node(n->index.index);
        return n;
    case NODE_FIELD:
        n->field.obj = c_lower_node(n->field.obj);
        return n;
    case NODE_RANGE:
        n->range.start = c_lower_node(n->range.start);
        n->range.end = c_lower_node(n->range.end);
        return n;
    case NODE_TRY:
        n->try_.body = c_lower_node(n->try_.body);
        n->try_.finally_block = c_lower_node(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            n->try_.catch_arms.items[i].body =
                c_lower_node(n->try_.catch_arms.items[i].body);
        return n;
    case NODE_THROW:
        n->throw_.value = c_lower_node(n->throw_.value);
        return n;
    case NODE_MATCH:
        n->match.subject = c_lower_node(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++) {
            n->match.arms.items[i].body = c_lower_node(n->match.arms.items[i].body);
            if (n->match.arms.items[i].guard)
                n->match.arms.items[i].guard = c_lower_node(n->match.arms.items[i].guard);
        }
        return n;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        c_lower_nodelist(&n->lit_array.elems);
        return n;
    case NODE_LIT_MAP:
        c_lower_nodelist(&n->lit_map.keys);
        c_lower_nodelist(&n->lit_map.vals);
        return n;
    case NODE_INTERP_STRING:
        c_lower_nodelist(&n->lit_string.parts);
        return n;
    case NODE_LIST_COMP:
        n->list_comp.element = c_lower_node(n->list_comp.element);
        c_lower_nodelist(&n->list_comp.clause_iters);
        c_lower_nodelist(&n->list_comp.clause_conds);
        return n;
    case NODE_MAP_COMP:
        n->map_comp.key = c_lower_node(n->map_comp.key);
        n->map_comp.value = c_lower_node(n->map_comp.value);
        c_lower_nodelist(&n->map_comp.clause_iters);
        c_lower_nodelist(&n->map_comp.clause_conds);
        return n;
    case NODE_STRUCT_INIT:
        c_lower_nodepairlist(&n->struct_init.fields);
        if (n->struct_init.rest) n->struct_init.rest = c_lower_node(n->struct_init.rest);
        return n;
    case NODE_CAST:
        n->cast.expr = c_lower_node(n->cast.expr);
        return n;
    case NODE_DEFER:
        n->defer_.body = c_lower_node(n->defer_.body);
        return n;
    case NODE_IMPL_DECL:
        c_lower_nodelist(&n->impl_decl.members);
        return n;
    case NODE_CLASS_DECL:
        c_lower_nodelist(&n->class_decl.members);
        return n;
    case NODE_YIELD:
        n->yield_.value = c_lower_node(n->yield_.value);
        return n;
    default:
        return n;
    }
}

/* ---- Trait default-method copy ---------------------------------------- */

/* Walk the program once to inherit trait default-method bodies. For
 * every `impl Trait for Type { ... }` that's missing one of the
 * trait's methods that has a body, splice a deep-copied version of the
 * default into the impl's members. The codegen then treats it like any
 * other impl method (`Type_method`). */

/* Shallow alias copy via the parser. We re-emit the body to source and
 * re-parse it -- robust against AST shape variations and ownership
 * concerns. The trait body is small so the cost is negligible. */
static Node *c_clone_node(Node *src);

static NodeList c_clone_nodelist(NodeList src) {
    NodeList r = nodelist_new();
    if (!src.items) return r;
    for (int i = 0; i < src.len; i++)
        nodelist_push(&r, c_clone_node(src.items[i]));
    return r;
}
static NodePairList c_clone_nodepairlist(NodePairList src) {
    NodePairList r = nodepairlist_new();
    for (int i = 0; i < src.len; i++)
        nodepairlist_push(&r, src.items[i].key, c_clone_node(src.items[i].val));
    return r;
}
static ParamList c_clone_paramlist(ParamList src) {
    ParamList r = paramlist_new();
    for (int i = 0; i < src.len; i++) {
        Param p = src.items[i];
        Param np = (Param){0};
        np.name = p.name ? xs_strdup(p.name) : NULL;
        np.pattern = c_clone_node(p.pattern);
        np.default_val = c_clone_node(p.default_val);
        np.variadic = p.variadic;
        np.keyword_only = p.keyword_only;
        np.type_ann = NULL;     /* type info is for sema, not codegen */
        np.contract = NULL;
        np.span = p.span;
        paramlist_push(&r, np);
    }
    return r;
}

static Node *c_clone_node(Node *src) {
    if (!src) return NULL;
    Node *n = node_new(VAL_TAG(src), src->span);
    switch (VAL_TAG(src)) {
    case NODE_IDENT:
        n->ident.name = src->ident.name ? xs_strdup(src->ident.name) : NULL;
        break;
    case NODE_LIT_INT:    n->lit_int.ival = src->lit_int.ival; break;
    case NODE_LIT_FLOAT:  n->lit_float.fval = src->lit_float.fval; break;
    case NODE_LIT_BOOL:   n->lit_bool.bval = src->lit_bool.bval; break;
    case NODE_LIT_NULL:   break;
    case NODE_LIT_STRING:
        n->lit_string.sval = src->lit_string.sval ? xs_strdup(src->lit_string.sval) : NULL;
        n->lit_string.parts = c_clone_nodelist(src->lit_string.parts);
        n->lit_string.interpolated = src->lit_string.interpolated;
        break;
    case NODE_INTERP_STRING:
        n->lit_string.sval = src->lit_string.sval ? xs_strdup(src->lit_string.sval) : NULL;
        n->lit_string.parts = c_clone_nodelist(src->lit_string.parts);
        n->lit_string.interpolated = 1;
        break;
    case NODE_BINOP:
        memcpy(n->binop.op, src->binop.op, sizeof n->binop.op);
        n->binop.left = c_clone_node(src->binop.left);
        n->binop.right = c_clone_node(src->binop.right);
        break;
    case NODE_UNARY:
        memcpy(n->unary.op, src->unary.op, sizeof n->unary.op);
        n->unary.expr = c_clone_node(src->unary.expr);
        n->unary.prefix = src->unary.prefix;
        break;
    case NODE_CALL:
        n->call.callee = c_clone_node(src->call.callee);
        n->call.args = c_clone_nodelist(src->call.args);
        n->call.kwargs = c_clone_nodepairlist(src->call.kwargs);
        break;
    case NODE_METHOD_CALL:
        n->method_call.obj = c_clone_node(src->method_call.obj);
        n->method_call.method = src->method_call.method ? xs_strdup(src->method_call.method) : NULL;
        n->method_call.args = c_clone_nodelist(src->method_call.args);
        n->method_call.kwargs = c_clone_nodepairlist(src->method_call.kwargs);
        n->method_call.optional = src->method_call.optional;
        break;
    case NODE_INDEX:
        n->index.obj = c_clone_node(src->index.obj);
        n->index.index = c_clone_node(src->index.index);
        break;
    case NODE_FIELD:
        n->field.obj = c_clone_node(src->field.obj);
        n->field.name = src->field.name ? xs_strdup(src->field.name) : NULL;
        n->field.optional = src->field.optional;
        break;
    case NODE_ASSIGN:
        memcpy(n->assign.op, src->assign.op, sizeof n->assign.op);
        n->assign.target = c_clone_node(src->assign.target);
        n->assign.value = c_clone_node(src->assign.value);
        break;
    case NODE_IF:
        n->if_expr.cond = c_clone_node(src->if_expr.cond);
        n->if_expr.then = c_clone_node(src->if_expr.then);
        n->if_expr.elif_conds = c_clone_nodelist(src->if_expr.elif_conds);
        n->if_expr.elif_thens = c_clone_nodelist(src->if_expr.elif_thens);
        n->if_expr.else_branch = c_clone_node(src->if_expr.else_branch);
        break;
    case NODE_WHILE:
        n->while_loop.cond = c_clone_node(src->while_loop.cond);
        n->while_loop.body = c_clone_node(src->while_loop.body);
        break;
    case NODE_FOR:
        n->for_loop.pattern = c_clone_node(src->for_loop.pattern);
        n->for_loop.iter = c_clone_node(src->for_loop.iter);
        n->for_loop.body = c_clone_node(src->for_loop.body);
        n->for_loop.label = src->for_loop.label ? xs_strdup(src->for_loop.label) : NULL;
        break;
    case NODE_LOOP:
        n->loop.body = c_clone_node(src->loop.body);
        n->loop.label = src->loop.label ? xs_strdup(src->loop.label) : NULL;
        break;
    case NODE_RETURN:
        n->ret.value = c_clone_node(src->ret.value);
        break;
    case NODE_BREAK: case NODE_CONTINUE: break;
    case NODE_LET: case NODE_VAR:
        n->let.name = src->let.name ? xs_strdup(src->let.name) : NULL;
        n->let.pattern = c_clone_node(src->let.pattern);
        n->let.value = c_clone_node(src->let.value);
        n->let.mutable = src->let.mutable;
        n->let.is_scoped = src->let.is_scoped;
        n->let.is_pub = src->let.is_pub;
        n->let.type_ann = NULL;
        n->let.contract = NULL;
        break;
    case NODE_CONST:
        n->const_.name = src->const_.name ? xs_strdup(src->const_.name) : NULL;
        n->const_.value = c_clone_node(src->const_.value);
        n->const_.type_ann = NULL;
        n->const_.contract = NULL;
        n->const_.is_pub = src->const_.is_pub;
        break;
    case NODE_EXPR_STMT:
        n->expr_stmt.expr = c_clone_node(src->expr_stmt.expr);
        n->expr_stmt.has_semicolon = src->expr_stmt.has_semicolon;
        break;
    case NODE_BLOCK:
        n->block.stmts = c_clone_nodelist(src->block.stmts);
        n->block.expr = c_clone_node(src->block.expr);
        n->block.has_decls = -1;
        n->block.is_unsafe = src->block.is_unsafe;
        break;
    case NODE_THROW:
        n->throw_.value = c_clone_node(src->throw_.value);
        break;
    case NODE_TRY:
        n->try_.body = c_clone_node(src->try_.body);
        n->try_.finally_block = c_clone_node(src->try_.finally_block);
        n->try_.catch_arms = (MatchArmList){0};
        for (int i = 0; i < src->try_.catch_arms.len; i++) {
            MatchArm a = (MatchArm){0};
            a.pattern = c_clone_node(src->try_.catch_arms.items[i].pattern);
            a.guard = c_clone_node(src->try_.catch_arms.items[i].guard);
            a.body = c_clone_node(src->try_.catch_arms.items[i].body);
            if (n->try_.catch_arms.len >= n->try_.catch_arms.cap) {
                n->try_.catch_arms.cap = n->try_.catch_arms.cap ? n->try_.catch_arms.cap * 2 : 4;
                n->try_.catch_arms.items = (MatchArm*)realloc(
                    n->try_.catch_arms.items,
                    n->try_.catch_arms.cap * sizeof(MatchArm));
            }
            n->try_.catch_arms.items[n->try_.catch_arms.len++] = a;
        }
        break;
    case NODE_RANGE:
        n->range.start = c_clone_node(src->range.start);
        n->range.end = c_clone_node(src->range.end);
        n->range.inclusive = src->range.inclusive;
        break;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        n->lit_array.elems = c_clone_nodelist(src->lit_array.elems);
        n->lit_array.repeat_val = c_clone_node(src->lit_array.repeat_val);
        n->lit_array.repeat_cnt = src->lit_array.repeat_cnt;
        break;
    case NODE_LIT_MAP:
        n->lit_map.keys = c_clone_nodelist(src->lit_map.keys);
        n->lit_map.vals = c_clone_nodelist(src->lit_map.vals);
        break;
    case NODE_LAMBDA:
        n->lambda.params = c_clone_paramlist(src->lambda.params);
        n->lambda.body = c_clone_node(src->lambda.body);
        n->lambda.is_generator = src->lambda.is_generator;
        break;
    case NODE_FN_DECL:
        n->fn_decl.name = src->fn_decl.name ? xs_strdup(src->fn_decl.name) : NULL;
        n->fn_decl.params = c_clone_paramlist(src->fn_decl.params);
        n->fn_decl.body = c_clone_node(src->fn_decl.body);
        n->fn_decl.is_async = src->fn_decl.is_async;
        n->fn_decl.is_pub = src->fn_decl.is_pub;
        n->fn_decl.is_generator = src->fn_decl.is_generator;
        n->fn_decl.is_pure = src->fn_decl.is_pure;
        n->fn_decl.is_test = src->fn_decl.is_test;
        n->fn_decl.is_static = src->fn_decl.is_static;
        n->fn_decl.is_macro = src->fn_decl.is_macro;
        n->fn_decl.deprecated_msg = NULL;
        n->fn_decl.ret_type = NULL;
        n->fn_decl.type_params = NULL;
        n->fn_decl.type_bounds = NULL;
        n->fn_decl.type_param_variance = NULL;
        n->fn_decl.n_type_params = 0;
        n->fn_decl.decorators = NULL;
        n->fn_decl.n_decorators = 0;
        break;
    case NODE_DEFER:
        n->defer_.body = c_clone_node(src->defer_.body);
        break;
    case NODE_YIELD:
        n->yield_.value = c_clone_node(src->yield_.value);
        break;
    case NODE_AWAIT:
        n->await_.expr = c_clone_node(src->await_.expr);
        break;
    case NODE_SPAWN:
        n->spawn_.expr = c_clone_node(src->spawn_.expr);
        break;
    default:
        /* Unknown node tag in a trait default-method body; degrade to a
         * null literal rather than risk a shallow memcpy that would
         * leave dangling pointers. Trait default methods are typically
         * single expressions (string lit, arithmetic, method call), so
         * hitting this case is rare in practice. */
        node_free(n);
        return mkc_null_lit();
    }
    return n;
}

static void c_inherit_trait_defaults(Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;
    /* Build a quick name->trait-node lookup. */
    Node *traits[64];
    int n_traits = 0;
    for (int i = 0; i < program->program.stmts.len && n_traits < 64; i++) {
        Node *st = program->program.stmts.items[i];
        if (st && VAL_TAG(st) == NODE_TRAIT_DECL && st->trait_decl.name)
            traits[n_traits++] = st;
    }
    if (n_traits == 0) return;

    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *st = program->program.stmts.items[i];
        if (!st || VAL_TAG(st) != NODE_IMPL_DECL) continue;
        if (!st->impl_decl.trait_name) continue;
        Node *trait = NULL;
        for (int t = 0; t < n_traits; t++)
            if (traits[t]->trait_decl.name &&
                strcmp(traits[t]->trait_decl.name, st->impl_decl.trait_name) == 0) {
                trait = traits[t]; break;
            }
        if (!trait) continue;
        for (int ti = 0; ti < trait->trait_decl.methods.len; ti++) {
            Node *defm = trait->trait_decl.methods.items[ti];
            if (!defm || VAL_TAG(defm) != NODE_FN_DECL) continue;
            if (!defm->fn_decl.body) continue; /* required, no default */
            if (!defm->fn_decl.name) continue;
            int overridden = 0;
            for (int j = 0; j < st->impl_decl.members.len; j++) {
                Node *im = st->impl_decl.members.items[j];
                if (im && VAL_TAG(im) == NODE_FN_DECL && im->fn_decl.name &&
                    strcmp(im->fn_decl.name, defm->fn_decl.name) == 0) {
                    overridden = 1; break;
                }
            }
            if (overridden) continue;
            nodelist_push(&st->impl_decl.members, c_clone_node(defm));
        }
    }
}

/* ---- Nested-fn-to-lambda lowering ------------------------------------- */

/* Convert a NODE_FN_DECL into a NODE_LAMBDA node. The caller is
 * responsible for splicing the resulting binding into a block. The
 * source fn node is reused (its tag gets flipped) so its params/body
 * keep their existing ownership chain. */
static Node *c_fn_to_lambda(Node *fn) {
    if (!fn || VAL_TAG(fn) != NODE_FN_DECL) return fn;
    Node *lam = node_new(NODE_LAMBDA, span_zero());
    lam->lambda.params = fn->fn_decl.params;
    lam->lambda.body = fn->fn_decl.body;
    lam->lambda.is_generator = fn->fn_decl.is_generator;
    lam->lambda.inferred_pure = fn->fn_decl.inferred_pure;
    /* Detach so node_free on fn doesn't double-free. */
    fn->fn_decl.params.items = NULL;
    fn->fn_decl.params.len = 0;
    fn->fn_decl.params.cap = 0;
    fn->fn_decl.body = NULL;
    return lam;
}

/* Walk a block's statement list and rewrite any NODE_FN_DECL stmt in
 * the body of a function/lambda to a `var name; name = |...| body`
 * sequence. The two-step pattern lets mutually-recursive sibling fns
 * see each other's bindings. Top-level (program-level) fn decls are
 * left alone -- they're already file-scope C functions. */
static void c_lower_nested_fns_in_block(Node *block);

static void c_lower_nested_fns_walk(Node *n) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++) {
            Node *st = n->program.stmts.items[i];
            if (!st) continue;
            if (VAL_TAG(st) == NODE_FN_DECL) {
                /* Top-level fn: keep as a C function, but recurse into
                 * its body so nested fns inside it get rewritten. */
                if (st->fn_decl.body && VAL_TAG(st->fn_decl.body) == NODE_BLOCK)
                    c_lower_nested_fns_in_block(st->fn_decl.body);
            } else {
                c_lower_nested_fns_walk(st);
            }
        }
        return;
    case NODE_BLOCK:
        c_lower_nested_fns_in_block(n);
        return;
    case NODE_FN_DECL:
        if (n->fn_decl.body && VAL_TAG(n->fn_decl.body) == NODE_BLOCK)
            c_lower_nested_fns_in_block(n->fn_decl.body);
        return;
    case NODE_LAMBDA:
        if (n->lambda.body && VAL_TAG(n->lambda.body) == NODE_BLOCK)
            c_lower_nested_fns_in_block(n->lambda.body);
        return;
    case NODE_IF:
        c_lower_nested_fns_walk(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_lower_nested_fns_walk(n->if_expr.elif_thens.items[i]);
        c_lower_nested_fns_walk(n->if_expr.else_branch);
        return;
    case NODE_WHILE:    c_lower_nested_fns_walk(n->while_loop.body); return;
    case NODE_FOR:      c_lower_nested_fns_walk(n->for_loop.body); return;
    case NODE_LOOP:     c_lower_nested_fns_walk(n->loop.body); return;
    case NODE_TRY:
        c_lower_nested_fns_walk(n->try_.body);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_lower_nested_fns_walk(n->try_.catch_arms.items[i].body);
        c_lower_nested_fns_walk(n->try_.finally_block);
        return;
    case NODE_MATCH:
        for (int i = 0; i < n->match.arms.len; i++)
            c_lower_nested_fns_walk(n->match.arms.items[i].body);
        return;
    case NODE_IMPL_DECL:
        for (int i = 0; i < n->impl_decl.members.len; i++) {
            Node *m = n->impl_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.body &&
                VAL_TAG(m->fn_decl.body) == NODE_BLOCK)
                c_lower_nested_fns_in_block(m->fn_decl.body);
        }
        return;
    case NODE_CLASS_DECL:
        for (int i = 0; i < n->class_decl.members.len; i++) {
            Node *m = n->class_decl.members.items[i];
            if (m && VAL_TAG(m) == NODE_FN_DECL && m->fn_decl.body &&
                VAL_TAG(m->fn_decl.body) == NODE_BLOCK)
                c_lower_nested_fns_in_block(m->fn_decl.body);
        }
        return;
    case NODE_LET: case NODE_VAR: c_lower_nested_fns_walk(n->let.value); return;
    case NODE_CONST:    c_lower_nested_fns_walk(n->const_.value); return;
    case NODE_EXPR_STMT: c_lower_nested_fns_walk(n->expr_stmt.expr); return;
    case NODE_RETURN:   c_lower_nested_fns_walk(n->ret.value); return;
    default: return;
    }
}

static void c_lower_nested_fns_in_block(Node *block) {
    if (!block || VAL_TAG(block) != NODE_BLOCK) return;
    NodeList *stmts = &block->block.stmts;

    /* Pass 1: collect nested fn names. */
    const char *names[64];
    int n_names = 0;
    for (int i = 0; i < stmts->len && n_names < 64; i++) {
        Node *st = stmts->items[i];
        if (st && VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.name)
            names[n_names++] = st->fn_decl.name;
    }
    if (n_names == 0) {
        /* still walk children for deeper nesting */
        for (int i = 0; i < stmts->len; i++)
            c_lower_nested_fns_walk(stmts->items[i]);
        c_lower_nested_fns_walk(block->block.expr);
        return;
    }

    /* Pass 2: rewrite. Build a fresh stmts list with predeclared vars
     * up-front (so siblings see each other), then assignments, then
     * any original non-fn statements interleaved in order. */
    NodeList rebuilt = nodelist_new();
    /* predeclare each nested fn name as `var <name> = null` */
    for (int i = 0; i < n_names; i++) {
        nodelist_push(&rebuilt, mkc_let(names[i], mkc_null_lit(), 1));
    }
    for (int i = 0; i < stmts->len; i++) {
        Node *st = stmts->items[i];
        if (!st) continue;
        if (VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.name) {
            const char *fname = st->fn_decl.name;
            /* recurse first: nested fns inside the body */
            if (st->fn_decl.body && VAL_TAG(st->fn_decl.body) == NODE_BLOCK)
                c_lower_nested_fns_in_block(st->fn_decl.body);
            Node *lam = c_fn_to_lambda(st);
            /* Build assign: name = lam */
            Node *target = mkc_ident(fname);
            Node *as = node_new(NODE_ASSIGN, span_zero());
            as->assign.target = target;
            as->assign.value = lam;
            as->assign.op[0] = '=';
            as->assign.op[1] = '\0';
            nodelist_push(&rebuilt, mkc_expr_stmt(as));
            /* free the now-stripped fn_decl shell */
            node_free(st);
        } else {
            c_lower_nested_fns_walk(st);
            nodelist_push(&rebuilt, st);
        }
    }
    free(stmts->items);
    block->block.stmts = rebuilt;
    c_lower_nested_fns_walk(block->block.expr);
}

/* ---- Generator lowering for the C target ------------------------------ */

/* Replace every NODE_YIELD inside subtree with `__gen_buf.push(value)`. */
static void c_lower_yields(Node *n) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_YIELD: {
        Node *val = n->yield_.value;
        n->yield_.value = NULL;
        n->tag = NODE_METHOD_CALL;
        n->method_call.obj = mkc_ident("__gen_buf");
        n->method_call.method = xs_strdup("push");
        n->method_call.args = nodelist_new();
        nodelist_push(&n->method_call.args, val ? val : mkc_null_lit());
        n->method_call.kwargs = nodepairlist_new();
        n->method_call.optional = 0;
        return;
    }
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            c_lower_yields(n->program.stmts.items[i]);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            c_lower_yields(n->block.stmts.items[i]);
        c_lower_yields(n->block.expr);
        return;
    case NODE_FN_DECL:
        if (n->fn_decl.is_generator) return;
        c_lower_yields(n->fn_decl.body);
        return;
    case NODE_LAMBDA:
        if (n->lambda.is_generator & 1) return;
        c_lower_yields(n->lambda.body);
        return;
    case NODE_IF:
        c_lower_yields(n->if_expr.cond);
        c_lower_yields(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_conds.len; i++)
            c_lower_yields(n->if_expr.elif_conds.items[i]);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_lower_yields(n->if_expr.elif_thens.items[i]);
        c_lower_yields(n->if_expr.else_branch);
        return;
    case NODE_WHILE:
        c_lower_yields(n->while_loop.cond);
        c_lower_yields(n->while_loop.body);
        return;
    case NODE_FOR:
        c_lower_yields(n->for_loop.iter);
        c_lower_yields(n->for_loop.body);
        return;
    case NODE_LOOP:        c_lower_yields(n->loop.body); return;
    case NODE_LET: case NODE_VAR: c_lower_yields(n->let.value); return;
    case NODE_CONST:       c_lower_yields(n->const_.value); return;
    case NODE_EXPR_STMT:   c_lower_yields(n->expr_stmt.expr); return;
    case NODE_RETURN:      c_lower_yields(n->ret.value); return;
    case NODE_ASSIGN:
        c_lower_yields(n->assign.target);
        c_lower_yields(n->assign.value);
        return;
    case NODE_BINOP:
        c_lower_yields(n->binop.left);
        c_lower_yields(n->binop.right);
        return;
    case NODE_UNARY:       c_lower_yields(n->unary.expr); return;
    case NODE_CALL:
        c_lower_yields(n->call.callee);
        for (int i = 0; i < n->call.args.len; i++)
            c_lower_yields(n->call.args.items[i]);
        return;
    case NODE_METHOD_CALL:
        c_lower_yields(n->method_call.obj);
        for (int i = 0; i < n->method_call.args.len; i++)
            c_lower_yields(n->method_call.args.items[i]);
        return;
    case NODE_INDEX:
        c_lower_yields(n->index.obj);
        c_lower_yields(n->index.index);
        return;
    case NODE_FIELD:       c_lower_yields(n->field.obj); return;
    case NODE_TRY:
        c_lower_yields(n->try_.body);
        c_lower_yields(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_lower_yields(n->try_.catch_arms.items[i].body);
        return;
    case NODE_THROW:       c_lower_yields(n->throw_.value); return;
    case NODE_MATCH:
        c_lower_yields(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++)
            c_lower_yields(n->match.arms.items[i].body);
        return;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        for (int i = 0; i < n->lit_array.elems.len; i++)
            c_lower_yields(n->lit_array.elems.items[i]);
        return;
    case NODE_LIT_MAP:
        for (int i = 0; i < n->lit_map.keys.len; i++)
            c_lower_yields(n->lit_map.keys.items[i]);
        for (int i = 0; i < n->lit_map.vals.len; i++)
            c_lower_yields(n->lit_map.vals.items[i]);
        return;
    case NODE_RANGE:
        c_lower_yields(n->range.start);
        c_lower_yields(n->range.end);
        return;
    case NODE_DEFER:       c_lower_yields(n->defer_.body); return;
    case NODE_INTERP_STRING:
        for (int i = 0; i < n->lit_string.parts.len; i++)
            c_lower_yields(n->lit_string.parts.items[i]);
        return;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < n->struct_init.fields.len; i++)
            c_lower_yields(n->struct_init.fields.items[i].val);
        c_lower_yields(n->struct_init.rest);
        return;
    default: return;
    }
}

/* Convert a single generator fn body into:
 *   {
 *       let __gen_buf = []
 *       <body with yield rewritten to __gen_buf.push(...)>
 *       #{ __items: __gen_buf, __pos: 0 }
 *   }
 * The fn's is_generator flag is cleared so subsequent passes treat it
 * as a regular fn returning the generator value map. */
static void c_lower_generator_body(Node *fn) {
    if (!fn) return;
    Node *body = NULL;
    if (VAL_TAG(fn) == NODE_FN_DECL) body = fn->fn_decl.body;
    else if (VAL_TAG(fn) == NODE_LAMBDA) body = fn->lambda.body;
    if (!body) return;

    if (VAL_TAG(body) != NODE_BLOCK) {
        NodeList ss = nodelist_new();
        Node *new_body = mkc_block(ss, body);
        if (VAL_TAG(fn) == NODE_FN_DECL) fn->fn_decl.body = new_body;
        else                              fn->lambda.body = new_body;
        body = new_body;
    }

    c_lower_yields(body);

    Node *empty_arr = node_new(NODE_LIT_ARRAY, span_zero());
    empty_arr->lit_array.elems = nodelist_new();
    empty_arr->lit_array.repeat_val = NULL;
    empty_arr->lit_array.repeat_cnt = 0;
    Node *init_let = mkc_let("__gen_buf", empty_arr, 0);

    if (body->block.expr) {
        Node *es = mkc_expr_stmt(body->block.expr);
        nodelist_push(&body->block.stmts, es);
        body->block.expr = NULL;
    }

    NodeList new_stmts = nodelist_new();
    nodelist_push(&new_stmts, init_let);
    for (int i = 0; i < body->block.stmts.len; i++)
        nodelist_push(&new_stmts, body->block.stmts.items[i]);
    free(body->block.stmts.items);
    body->block.stmts = new_stmts;

    /* Build trailing `#{__items: __gen_buf, __pos: 0}`. */
    Node *map = node_new(NODE_LIT_MAP, span_zero());
    map->lit_map.keys = nodelist_new();
    map->lit_map.vals = nodelist_new();
    nodelist_push(&map->lit_map.keys, mkc_str_lit("__items"));
    nodelist_push(&map->lit_map.vals, mkc_ident("__gen_buf"));
    nodelist_push(&map->lit_map.keys, mkc_str_lit("__pos"));
    nodelist_push(&map->lit_map.vals, mkc_int_lit(0));
    body->block.expr = map;

    if (VAL_TAG(fn) == NODE_FN_DECL) fn->fn_decl.is_generator = 0;
    else                              fn->lambda.is_generator &= ~1;
}

static int g_c_has_generator = 0;

static void c_lower_all_generators(Node *n) {
    if (!n) return;
    if (VAL_TAG(n) == NODE_FN_DECL && n->fn_decl.is_generator) {
        c_lower_generator_body(n);
        g_c_has_generator = 1;
        c_lower_all_generators(n->fn_decl.body);
        return;
    }
    if (VAL_TAG(n) == NODE_LAMBDA && (n->lambda.is_generator & 1)) {
        c_lower_generator_body(n);
        g_c_has_generator = 1;
        c_lower_all_generators(n->lambda.body);
        return;
    }
    switch (VAL_TAG(n)) {
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            c_lower_all_generators(n->program.stmts.items[i]);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            c_lower_all_generators(n->block.stmts.items[i]);
        c_lower_all_generators(n->block.expr);
        return;
    case NODE_FN_DECL:    c_lower_all_generators(n->fn_decl.body); return;
    case NODE_LAMBDA:     c_lower_all_generators(n->lambda.body); return;
    case NODE_IF:
        c_lower_all_generators(n->if_expr.cond);
        c_lower_all_generators(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_conds.len; i++)
            c_lower_all_generators(n->if_expr.elif_conds.items[i]);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_lower_all_generators(n->if_expr.elif_thens.items[i]);
        c_lower_all_generators(n->if_expr.else_branch);
        return;
    case NODE_WHILE:
        c_lower_all_generators(n->while_loop.cond);
        c_lower_all_generators(n->while_loop.body);
        return;
    case NODE_FOR:
        c_lower_all_generators(n->for_loop.iter);
        c_lower_all_generators(n->for_loop.body);
        return;
    case NODE_LOOP:       c_lower_all_generators(n->loop.body); return;
    case NODE_LET: case NODE_VAR: c_lower_all_generators(n->let.value); return;
    case NODE_CONST:      c_lower_all_generators(n->const_.value); return;
    case NODE_EXPR_STMT:  c_lower_all_generators(n->expr_stmt.expr); return;
    case NODE_RETURN:     c_lower_all_generators(n->ret.value); return;
    case NODE_TRY:
        c_lower_all_generators(n->try_.body);
        c_lower_all_generators(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_lower_all_generators(n->try_.catch_arms.items[i].body);
        return;
    case NODE_MATCH:
        c_lower_all_generators(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++)
            c_lower_all_generators(n->match.arms.items[i].body);
        return;
    case NODE_BINOP:
        c_lower_all_generators(n->binop.left);
        c_lower_all_generators(n->binop.right);
        return;
    case NODE_CALL:
        c_lower_all_generators(n->call.callee);
        for (int i = 0; i < n->call.args.len; i++)
            c_lower_all_generators(n->call.args.items[i]);
        return;
    case NODE_METHOD_CALL:
        c_lower_all_generators(n->method_call.obj);
        for (int i = 0; i < n->method_call.args.len; i++)
            c_lower_all_generators(n->method_call.args.items[i]);
        return;
    case NODE_IMPL_DECL:
        for (int i = 0; i < n->impl_decl.members.len; i++)
            c_lower_all_generators(n->impl_decl.members.items[i]);
        return;
    case NODE_CLASS_DECL:
        for (int i = 0; i < n->class_decl.members.len; i++)
            c_lower_all_generators(n->class_decl.members.items[i]);
        return;
    default: return;
    }
}

/* Wrap every for-in's iter expr with __gen_iter(...) so the runtime
 * unwraps generator maps to their __items array transparently. */
static void c_wrap_for_iters(Node *n) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_FOR:
        if (n->for_loop.iter) {
            Node *call = node_new(NODE_CALL, span_zero());
            call->call.callee = mkc_ident("__gen_iter");
            call->call.args = nodelist_new();
            call->call.kwargs = nodepairlist_new();
            nodelist_push(&call->call.args, n->for_loop.iter);
            n->for_loop.iter = call;
        }
        c_wrap_for_iters(n->for_loop.body);
        return;
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            c_wrap_for_iters(n->program.stmts.items[i]);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            c_wrap_for_iters(n->block.stmts.items[i]);
        c_wrap_for_iters(n->block.expr);
        return;
    case NODE_FN_DECL:    c_wrap_for_iters(n->fn_decl.body); return;
    case NODE_LAMBDA:     c_wrap_for_iters(n->lambda.body); return;
    case NODE_IF:
        c_wrap_for_iters(n->if_expr.cond);
        c_wrap_for_iters(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_conds.len; i++)
            c_wrap_for_iters(n->if_expr.elif_conds.items[i]);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_wrap_for_iters(n->if_expr.elif_thens.items[i]);
        c_wrap_for_iters(n->if_expr.else_branch);
        return;
    case NODE_WHILE:
        c_wrap_for_iters(n->while_loop.cond);
        c_wrap_for_iters(n->while_loop.body);
        return;
    case NODE_LOOP:       c_wrap_for_iters(n->loop.body); return;
    case NODE_LET: case NODE_VAR: c_wrap_for_iters(n->let.value); return;
    case NODE_CONST:      c_wrap_for_iters(n->const_.value); return;
    case NODE_EXPR_STMT:  c_wrap_for_iters(n->expr_stmt.expr); return;
    case NODE_RETURN:     c_wrap_for_iters(n->ret.value); return;
    case NODE_TRY:
        c_wrap_for_iters(n->try_.body);
        c_wrap_for_iters(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_wrap_for_iters(n->try_.catch_arms.items[i].body);
        return;
    case NODE_MATCH:
        c_wrap_for_iters(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++)
            c_wrap_for_iters(n->match.arms.items[i].body);
        return;
    default: return;
    }
}

/* Splice helper fns for generator iteration into the program prologue so
 * `g.next()` and `for v in g {...}` Just Work. Parsed once and the
 * resulting top-level decls are pushed into new_stmts. */
static void c_inject_generator_helpers(NodeList *new_stmts) {
    /* Parameter names use a __xs_g_ prefix so they cannot collide with
     * any user-defined function and confuse the C codegen's "ident
     * matches a known fn" heuristic. */
    static const char *gen_src =
        "fn __gen_iter(__xs_g_g) {\n"
        "    if type(__xs_g_g) == \"map\" { return __xs_g_g[\"__items\"] }\n"
        "    return __xs_g_g\n"
        "}\n"
        "fn __gen_next(__xs_g_g) {\n"
        "    let p = __xs_g_g[\"__pos\"]\n"
        "    let it = __xs_g_g[\"__items\"]\n"
        "    let m = #{}\n"
        "    if p >= it.len() {\n"
        "        m[\"value\"] = null\n"
        "        m[\"done\"] = true\n"
        "        return m\n"
        "    }\n"
        "    __xs_g_g[\"__pos\"] = p + 1\n"
        "    m[\"value\"] = it[p]\n"
        "    m[\"done\"] = false\n"
        "    return m\n"
        "}\n";
    Lexer lex; lexer_init(&lex, gen_src, "<c-gen-helpers>");
    TokenArray ta = lexer_tokenize(&lex);
    Parser p; parser_init(&p, &ta, "<c-gen-helpers>");
    Node *prog = parser_parse(&p);
    token_array_free(&ta);
    comment_list_free(&lex.comments);
    if (!prog || p.had_error) {
        if (prog) node_free(prog);
        return;
    }
    for (int i = 0; i < prog->program.stmts.len; i++) {
        nodelist_push(new_stmts, prog->program.stmts.items[i]);
    }
    free(prog->program.stmts.items);
    prog->program.stmts.items = NULL;
    prog->program.stmts.len = 0;
    prog->program.stmts.cap = 0;
    node_free(prog);
}

/* ---- `use "./mod.xs"` cross-file splicing ----------------------------- */

#define MAX_C_USE_MODS 32
static struct {
    char *path;        /* canonical resolved path */
    Node *prog;        /* parsed AST (kept alive for codegen) */
    char *src;         /* file contents (kept alive for the spans) */
    char *path_owned;  /* heap copy passed to lexer; freed at reset */
} g_c_use_mods[MAX_C_USE_MODS];
static int g_n_c_use_mods = 0;
static char g_c_src_dir[1024] = "";

static void c_resolve_use_path(const char *rel, char *out, size_t outsz) {
    if (!rel) { out[0] = '\0'; return; }
    if (rel[0] == '/' || g_c_src_dir[0] == '\0') {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    /* normalise away leading "./" */
    const char *r = rel;
    while (r[0] == '.' && r[1] == '/') r += 2;
    snprintf(out, outsz, "%s/%s", g_c_src_dir, r);
}

static char *c_slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static Node *c_load_use_module(const char *resolved) {
    for (int i = 0; i < g_n_c_use_mods; i++) {
        if (g_c_use_mods[i].path && strcmp(g_c_use_mods[i].path, resolved) == 0)
            return g_c_use_mods[i].prog;
    }
    if (g_n_c_use_mods >= MAX_C_USE_MODS) return NULL;
    char *src = c_slurp_file(resolved);
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
        free(src);
        free(path_owned);
        return NULL;
    }
    g_c_use_mods[g_n_c_use_mods].path = xs_strdup(resolved);
    g_c_use_mods[g_n_c_use_mods].prog = prog;
    g_c_use_mods[g_n_c_use_mods].src = src;
    g_c_use_mods[g_n_c_use_mods].path_owned = path_owned;
    g_n_c_use_mods++;
    return prog;
}

static void c_derive_use_alias(const char *path, char *out, size_t outsz) {
    const char *base = path;
    const char *p = strrchr(path, '/');
    if (p) base = p + 1;
    size_t i = 0;
    while (base[i] && base[i] != '.' && i + 1 < outsz) {
        char ch = base[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
            out[i] = ch;
        } else {
            out[i] = '_';
        }
        i++;
    }
    out[i] = '\0';
    if (out[0] >= '0' && out[0] <= '9') {
        /* prefix with underscore if starts with digit */
        memmove(out + 1, out, i + 1);
        out[0] = '_';
    }
}

/* Walk the imported program for `export { name [as alias], ... }` lists.
 * On any explicit list we emit only the listed names with their public
 * aliases. Returns 1 if any list was found. */
static int c_collect_exports(Node *prog, const char ***out_locals,
                             const char ***out_aliases, int *out_n) {
    *out_n = 0;
    if (!prog || VAL_TAG(prog) != NODE_PROGRAM) return 0;
    static const char *locals[256];
    static const char *aliases[256];
    int n = 0;
    int any = 0;
    for (int i = 0; i < prog->program.stmts.len; i++) {
        Node *st = prog->program.stmts.items[i];
        if (!st || VAL_TAG(st) != NODE_EXPORT) continue;
        any = 1;
        for (int k = 0; k < st->export_.nnames && n < 256; k++) {
            locals[n]  = st->export_.names[k];
            aliases[n] = st->export_.aliases[k] ? st->export_.aliases[k]
                                                : st->export_.names[k];
            n++;
        }
    }
    *out_locals = locals;
    *out_aliases = aliases;
    *out_n = n;
    return any;
}

/* Best-effort name extractor for stmts that bind a top-level value. Used
 * when a module has no explicit export list -- we expose every named
 * decl. */
static const char *c_stmt_export_name(Node *st) {
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

/* Track names that should be treated as namespace bindings (i.e. maps
 * built from a `use` import). Method calls on these go through
 * map-index dispatch rather than C function calls. */
#define MAX_C_NS_NAMES 64
static const char *g_c_ns_names[MAX_C_NS_NAMES];
static int g_n_c_ns_names = 0;

static int c_is_ns_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_n_c_ns_names; i++)
        if (g_c_ns_names[i] && strcmp(g_c_ns_names[i], name) == 0) return 1;
    return 0;
}
static void c_ns_add(const char *name) {
    if (!name || g_n_c_ns_names >= MAX_C_NS_NAMES) return;
    if (c_is_ns_name(name)) return;
    g_c_ns_names[g_n_c_ns_names++] = xs_strdup(name);
}

/* Rewrite `<ns>.<method>(args)` to `<ns>[<method>](args)` (a regular
 * CALL of the value at the field). Without this, the C method
 * dispatcher tries to invoke `.shout` directly on a map and either
 * returns null or traps. */
static void c_rewrite_ns_method_calls(Node *n) {
    if (!n) return;
    switch (VAL_TAG(n)) {
    case NODE_METHOD_CALL: {
        Node *obj = n->method_call.obj;
        if (obj && VAL_TAG(obj) == NODE_IDENT &&
            c_is_ns_name(obj->ident.name)) {
            Node *fld = node_new(NODE_FIELD, span_zero());
            fld->field.obj = obj;
            fld->field.name = xs_strdup(n->method_call.method);
            fld->field.optional = 0;
            n->tag = NODE_CALL;
            n->call.callee = fld;
            n->call.args = n->method_call.args;
            n->call.kwargs = n->method_call.kwargs;
        }
        c_rewrite_ns_method_calls(n->method_call.obj);
        for (int i = 0; i < n->method_call.args.len; i++)
            c_rewrite_ns_method_calls(n->method_call.args.items[i]);
        return;
    }
    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            c_rewrite_ns_method_calls(n->program.stmts.items[i]);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->block.stmts.len; i++)
            c_rewrite_ns_method_calls(n->block.stmts.items[i]);
        c_rewrite_ns_method_calls(n->block.expr);
        return;
    case NODE_FN_DECL:    c_rewrite_ns_method_calls(n->fn_decl.body); return;
    case NODE_LAMBDA:     c_rewrite_ns_method_calls(n->lambda.body); return;
    case NODE_IF:
        c_rewrite_ns_method_calls(n->if_expr.cond);
        c_rewrite_ns_method_calls(n->if_expr.then);
        for (int i = 0; i < n->if_expr.elif_conds.len; i++)
            c_rewrite_ns_method_calls(n->if_expr.elif_conds.items[i]);
        for (int i = 0; i < n->if_expr.elif_thens.len; i++)
            c_rewrite_ns_method_calls(n->if_expr.elif_thens.items[i]);
        c_rewrite_ns_method_calls(n->if_expr.else_branch);
        return;
    case NODE_WHILE:
        c_rewrite_ns_method_calls(n->while_loop.cond);
        c_rewrite_ns_method_calls(n->while_loop.body);
        return;
    case NODE_FOR:
        c_rewrite_ns_method_calls(n->for_loop.iter);
        c_rewrite_ns_method_calls(n->for_loop.body);
        return;
    case NODE_LET: case NODE_VAR: c_rewrite_ns_method_calls(n->let.value); return;
    case NODE_CONST:      c_rewrite_ns_method_calls(n->const_.value); return;
    case NODE_EXPR_STMT:  c_rewrite_ns_method_calls(n->expr_stmt.expr); return;
    case NODE_RETURN:     c_rewrite_ns_method_calls(n->ret.value); return;
    case NODE_ASSIGN:
        c_rewrite_ns_method_calls(n->assign.target);
        c_rewrite_ns_method_calls(n->assign.value);
        return;
    case NODE_BINOP:
        c_rewrite_ns_method_calls(n->binop.left);
        c_rewrite_ns_method_calls(n->binop.right);
        return;
    case NODE_UNARY:      c_rewrite_ns_method_calls(n->unary.expr); return;
    case NODE_CALL:
        c_rewrite_ns_method_calls(n->call.callee);
        for (int i = 0; i < n->call.args.len; i++)
            c_rewrite_ns_method_calls(n->call.args.items[i]);
        return;
    case NODE_INDEX:
        c_rewrite_ns_method_calls(n->index.obj);
        c_rewrite_ns_method_calls(n->index.index);
        return;
    case NODE_FIELD:      c_rewrite_ns_method_calls(n->field.obj); return;
    case NODE_TRY:
        c_rewrite_ns_method_calls(n->try_.body);
        c_rewrite_ns_method_calls(n->try_.finally_block);
        for (int i = 0; i < n->try_.catch_arms.len; i++)
            c_rewrite_ns_method_calls(n->try_.catch_arms.items[i].body);
        return;
    case NODE_MATCH:
        c_rewrite_ns_method_calls(n->match.subject);
        for (int i = 0; i < n->match.arms.len; i++)
            c_rewrite_ns_method_calls(n->match.arms.items[i].body);
        return;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        for (int i = 0; i < n->lit_array.elems.len; i++)
            c_rewrite_ns_method_calls(n->lit_array.elems.items[i]);
        return;
    case NODE_LIT_MAP:
        for (int i = 0; i < n->lit_map.keys.len; i++)
            c_rewrite_ns_method_calls(n->lit_map.keys.items[i]);
        for (int i = 0; i < n->lit_map.vals.len; i++)
            c_rewrite_ns_method_calls(n->lit_map.vals.items[i]);
        return;
    case NODE_RANGE:
        c_rewrite_ns_method_calls(n->range.start);
        c_rewrite_ns_method_calls(n->range.end);
        return;
    case NODE_INTERP_STRING:
        for (int i = 0; i < n->lit_string.parts.len; i++)
            c_rewrite_ns_method_calls(n->lit_string.parts.items[i]);
        return;
    case NODE_DEFER:      c_rewrite_ns_method_calls(n->defer_.body); return;
    default: return;
    }
}

/* Lower a single NODE_USE statement. Splices the imported file's
 * statements into `out_stmts` (without their NODE_EXPORT entries),
 * appends a namespace-binding `let alias = #{...}` map, and emits any
 * selective binding aliases. */
static void c_lower_use_stmt(Node *u, NodeList *out_stmts) {
    if (!u || VAL_TAG(u) != NODE_USE || !u->use_.path) return;
    if (u->use_.is_plugin) return;
    char resolved[2048];
    c_resolve_use_path(u->use_.path, resolved, sizeof(resolved));
    Node *modprog = c_load_use_module(resolved);
    if (!modprog) return;

    /* On first encounter splice the module's non-export statements and
     * recursively lower any nested `use`. We mark the module's
     * statements as already-spliced via a static flag. */
    int first = 1;
    for (int i = 0; i < g_n_c_use_mods; i++) {
        if (g_c_use_mods[i].prog == modprog) {
            /* Use sentinel slot to avoid double-splicing. We co-opt the
             * `path` field's last byte: if cleared to "@spliced@", skip. */
            if (g_c_use_mods[i].path_owned &&
                strstr(g_c_use_mods[i].path_owned, "@spliced") != NULL)
                first = 0;
            break;
        }
    }

    if (first) {
        for (int i = 0; i < modprog->program.stmts.len; i++) {
            Node *st = modprog->program.stmts.items[i];
            if (!st) continue;
            if (VAL_TAG(st) == NODE_EXPORT) continue;
            if (VAL_TAG(st) == NODE_USE) {
                c_lower_use_stmt(st, out_stmts);
                continue;
            }
            nodelist_push(out_stmts, st);
        }
        /* mark spliced */
        for (int i = 0; i < g_n_c_use_mods; i++) {
            if (g_c_use_mods[i].prog == modprog) {
                char *marker = (char*)malloc(strlen(g_c_use_mods[i].path_owned) + 16);
                snprintf(marker, strlen(g_c_use_mods[i].path_owned) + 16,
                         "%s@spliced@", g_c_use_mods[i].path_owned);
                free(g_c_use_mods[i].path_owned);
                g_c_use_mods[i].path_owned = marker;
                break;
            }
        }
    }

    /* collect exports (or fall back to every binding name) */
    const char **locals = NULL;
    const char **aliases = NULL;
    int n_exports = 0;
    int any_explicit = c_collect_exports(modprog, &locals, &aliases, &n_exports);
    static const char *fallback_locals[256];
    static const char *fallback_aliases[256];
    if (!any_explicit) {
        n_exports = 0;
        for (int i = 0; i < modprog->program.stmts.len && n_exports < 256; i++) {
            const char *nm = c_stmt_export_name(modprog->program.stmts.items[i]);
            if (!nm) continue;
            fallback_locals[n_exports] = nm;
            fallback_aliases[n_exports] = nm;
            n_exports++;
        }
        locals = fallback_locals;
        aliases = fallback_aliases;
    }

    if (u->use_.nnames > 0) {
        /* selective: `use "..." { name [as alias], ... }`. Bind each
         * requested local to its imported counterpart. */
        for (int i = 0; i < u->use_.nnames; i++) {
            const char *src = u->use_.names[i];
            const char *dst = (u->use_.name_aliases && u->use_.name_aliases[i])
                                  ? u->use_.name_aliases[i] : src;
            if (!src || !dst) continue;
            /* find matching local for src (which is the public alias) */
            const char *target_local = src;
            for (int k = 0; k < n_exports; k++) {
                if (aliases[k] && strcmp(aliases[k], src) == 0) {
                    target_local = locals[k];
                    break;
                }
            }
            if (strcmp(dst, target_local) == 0) continue; /* no-op */
            nodelist_push(out_stmts, mkc_let(dst, mkc_ident(target_local), 0));
        }
    } else {
        /* namespace import: `let alias = #{ "pubname": local, ... }` */
        char alias[256];
        if (u->use_.alias && u->use_.alias[0])
            snprintf(alias, sizeof(alias), "%s", u->use_.alias);
        else
            c_derive_use_alias(u->use_.path, alias, sizeof(alias));

        Node *map = node_new(NODE_LIT_MAP, span_zero());
        map->lit_map.keys = nodelist_new();
        map->lit_map.vals = nodelist_new();
        for (int i = 0; i < n_exports; i++) {
            nodelist_push(&map->lit_map.keys, mkc_str_lit(aliases[i]));
            /* Type names (struct/enum/class) don't have a runtime
             * value -- they're typedefs. Substitute a non-null
             * sentinel so `let P = util.Point` binds something the
             * `P { ... }` site can ignore (the struct path comes
             * from the syntactic name, not the value). */
            int is_type_name = 0;
            for (int j = 0; j < modprog->program.stmts.len; j++) {
                Node *st = modprog->program.stmts.items[j];
                if (!st || !locals[i]) continue;
                if (VAL_TAG(st) == NODE_STRUCT_DECL && st->struct_decl.name &&
                    strcmp(st->struct_decl.name, locals[i]) == 0) { is_type_name = 1; break; }
                if (VAL_TAG(st) == NODE_ENUM_DECL && st->enum_decl.name &&
                    strcmp(st->enum_decl.name, locals[i]) == 0) { is_type_name = 1; break; }
                if (VAL_TAG(st) == NODE_CLASS_DECL && st->class_decl.name &&
                    strcmp(st->class_decl.name, locals[i]) == 0) { is_type_name = 1; break; }
            }
            if (is_type_name) {
                nodelist_push(&map->lit_map.vals, mkc_str_lit(locals[i]));
            } else {
                nodelist_push(&map->lit_map.vals, mkc_ident(locals[i]));
            }
        }
        nodelist_push(out_stmts, mkc_let(alias, map, 0));
        c_ns_add(alias);
    }
}

static void c_reset_use_state(void) {
    for (int i = 0; i < g_n_c_use_mods; i++) {
        if (g_c_use_mods[i].prog) node_free(g_c_use_mods[i].prog);
        free(g_c_use_mods[i].src);
        free(g_c_use_mods[i].path);
        free(g_c_use_mods[i].path_owned);
        g_c_use_mods[i].path = NULL;
        g_c_use_mods[i].prog = NULL;
        g_c_use_mods[i].src = NULL;
        g_c_use_mods[i].path_owned = NULL;
    }
    g_n_c_use_mods = 0;
    for (int i = 0; i < g_n_c_ns_names; i++) free((char*)g_c_ns_names[i]);
    g_n_c_ns_names = 0;
    g_c_src_dir[0] = '\0';
}

/* Top-level lowering driver. Mutates the program AST in place. */
static void c_lower_program(Node *program, const char *src_filename) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;

    c_reset_use_state();
    g_c_has_generator = 0;

    if (src_filename) {
        const char *slash = strrchr(src_filename, '/');
        if (slash && slash != src_filename) {
            size_t n = (size_t)(slash - src_filename);
            if (n >= sizeof(g_c_src_dir)) n = sizeof(g_c_src_dir) - 1;
            memcpy(g_c_src_dir, src_filename, n);
            g_c_src_dir[n] = '\0';
        }
    }

    /* Phase 1: rename user `main` to `__user_main` if present. */
    int rename_main = 0;
    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *s = program->program.stmts.items[i];
        if (s && VAL_TAG(s) == NODE_FN_DECL && s->fn_decl.name &&
            strcmp(s->fn_decl.name, "main") == 0) {
            free(s->fn_decl.name);
            s->fn_decl.name = xs_strdup("__user_main");
            rename_main = 1;
        }
    }
    if (rename_main) c_rename_main_refs(program);

    /* Phase 2: splice `use` imports (recursive). NODE_USE entries are
     * replaced with the imported file's contents plus any namespace /
     * selective bindings. */
    NodeList new_stmts = nodelist_new();
    int has_use = 0;
    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *st = program->program.stmts.items[i];
        if (!st) continue;
        if (VAL_TAG(st) == NODE_USE) {
            has_use = 1;
            c_lower_use_stmt(st, &new_stmts);
            continue;
        }
        if (VAL_TAG(st) == NODE_EXPORT) {
            /* importer-side `export {...}` is a publish list, not
             * runnable code -- drop it. */
            continue;
        }
        nodelist_push(&new_stmts, st);
    }
    if (has_use) {
        free(program->program.stmts.items);
        program->program.stmts = new_stmts;
        c_rewrite_ns_method_calls(program);
    } else {
        free(new_stmts.items);
    }

    /* Phase 3a: copy trait default-method bodies into impls that
     * don't override them so call sites see a real `Type_method`. */
    c_inherit_trait_defaults(program);

    /* Phase 3: lower nested fn decls to lambda lets so the closure
     * machinery handles captures. Top-level fns stay as C functions. */
    c_lower_nested_fns_walk(program);

    /* Phase 4: lower await/spawn/nursery + clear async markers. */
    c_lower_node(program);

    /* Phase 5: lower fn* generators to eager-array fill. Inject the
     * runtime helpers and wrap for-in iters once. */
    c_lower_all_generators(program);
    if (g_c_has_generator) {
        NodeList helpers = nodelist_new();
        c_inject_generator_helpers(&helpers);
        if (helpers.len > 0) {
            NodeList combined = nodelist_new();
            for (int i = 0; i < helpers.len; i++)
                nodelist_push(&combined, helpers.items[i]);
            for (int i = 0; i < program->program.stmts.len; i++)
                nodelist_push(&combined, program->program.stmts.items[i]);
            free(helpers.items);
            free(program->program.stmts.items);
            program->program.stmts = combined;
        }
        c_wrap_for_iters(program);
    }
}

char *transpile_c(Node *program, const char *filename) {
    /* Lower the AST first so the codegen sees only constructs it
     * already handles: no async markers, no NODE_AWAIT/SPAWN/NURSERY
     * wrappers, no generator fns, no `use` imports. */
    c_lower_program(program, filename);
    purity_analyze(program);

    const char *unsupported = find_unsupported_for_c(program);
    if (unsupported) {
        fprintf(stderr, "xs --emit c: %s not supported on this target. "
                "Use --vm or --interp.\n", unsupported);
        return NULL;
    }

    SB s;
    sb_init(&s);
    seen_main = 0;
    defer_label_counter = 0;
    n_deleted_vars = 0;

    /* preamble */
    sb_add(&s, "/* Generated by xs transpile --target c */\n");
    if (filename) sb_printf(&s, "/* Source: %s */\n", filename);
    sb_add(&s,
        "#define _POSIX_C_SOURCE 200809L\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdint.h>\n"
        "#include <stdarg.h>\n"
        "#include <math.h>\n"
        "#include <time.h>\n"
        "#include <setjmp.h>\n"
        "#include <sys/stat.h>\n"
        "#include <sys/types.h>\n"
        "#if defined(_WIN32)\n"
        "#  include <direct.h>\n"
        "#  include <io.h>\n"
        "#  include <windows.h>\n"
        "#else\n"
        "#  include <unistd.h>\n"
        "#  include <dirent.h>\n"
        "#endif\n\n"
        "/* XS runtime types */\n"
        "typedef struct xs_val {\n"
        "    int tag; /* 0=int, 1=float, 2=str, 3=bool, 4=null, 5=array, 6=map */\n"
        "    union { int64_t i; double f; char *s; int b; void *p; };\n"
        "} xs_val;\n\n"
        "#define XS_INT(v)   ((xs_val){.tag=0, .i=(v)})\n"
        "#define XS_FLOAT(v) ((xs_val){.tag=1, .f=(v)})\n"
        "#define XS_STR(v)   ((xs_val){.tag=2, .s=(char*)(v)})\n"
        "#define XS_BOOL(v)  ((xs_val){.tag=3, .b=(v)})\n"
        "#define XS_NULL     ((xs_val){.tag=4})\n\n"
        "static int xs_truthy(xs_val v) {\n"
        "    switch (v.tag) {\n"
        "        case 0: return v.i != 0;\n"
        "        case 1: return v.f != 0.0;\n"
        "        case 2: return v.s != NULL && v.s[0] != '\\0';\n"
        "        case 3: return v.b;\n"
        "        default: return 0;\n"
        "    }\n"
        "}\n\n"
        "/* forward declare heap types */\n"
        "typedef struct { xs_val *items; int len; int cap; int is_tuple;\n"
        "                 /* range metadata - keeps the original bound after materialisation */\n"
        "                 int is_range; int64_t range_start; int64_t range_end; int range_inclusive;\n"
        "               } xs_arr;\n"
        "typedef struct { char **keys; xs_val *vals; int len; int cap; } xs_hmap;\n\n"
        "static const char *xs_to_str(xs_val v);\n"
        "static void xs_throw_arith(const char *kind, const char *msg);\n\n"
        "static const char *xs_bi_str(xs_val v);\n"
        "static int xs_eq(xs_val a, xs_val b) {\n"
        "    /* bigint <-> int / bigint <-> bigint comparisons compare the\n"
        "       canonical decimal-magnitude string. Falls back to tag-equal\n"
        "       for other mismatched pairs. */\n"
        "    if (a.tag == 9 || b.tag == 9) {\n"
        "        if ((a.tag == 9 || a.tag == 0) && (b.tag == 9 || b.tag == 0))\n"
        "            return strcmp(xs_bi_str(a), xs_bi_str(b)) == 0;\n"
        "        if (a.tag == 1 || b.tag == 1) {\n"
        "            double af = a.tag == 1 ? a.f : strtod(xs_bi_str(a), NULL);\n"
        "            double bf = b.tag == 1 ? b.f : strtod(xs_bi_str(b), NULL);\n"
        "            return af == bf;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    if (a.tag != b.tag) {\n"
        "        if ((a.tag == 0 && b.tag == 1) || (a.tag == 1 && b.tag == 0)) {\n"
        "            double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "            double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "            return af == bf;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    switch (a.tag) {\n"
        "        case 0: return a.i == b.i;\n"
        "        case 1: return a.f == b.f;\n"
        "        case 2: return a.s && b.s && strcmp(a.s, b.s) == 0;\n"
        "        case 3: return a.b == b.b;\n"
        "        case 4: return 1;\n"
        "        case 5: if (a.p && b.p) {\n"
        "            xs_arr *aa = (xs_arr*)a.p, *bb = (xs_arr*)b.p;\n"
        "            if (aa->len != bb->len) return 0;\n"
        "            for (int i = 0; i < aa->len; i++)\n"
        "                if (!xs_eq(aa->items[i], bb->items[i])) return 0;\n"
        "            return 1;\n"
        "        } return a.p == b.p;\n"
        "        case 6: if (a.p && b.p) {\n"
        "            xs_hmap *am = (xs_hmap*)a.p, *bm = (xs_hmap*)b.p;\n"
        "            if (am->len != bm->len) return 0;\n"
        "            for (int i = 0; i < am->len; i++) {\n"
        "                int found = 0;\n"
        "                for (int j = 0; j < bm->len; j++) {\n"
        "                    if (strcmp(am->keys[i], bm->keys[j]) == 0) {\n"
        "                        if (!xs_eq(am->vals[i], bm->vals[j])) return 0;\n"
        "                        found = 1; break;\n"
        "                    }\n"
        "                }\n"
        "                if (!found) return 0;\n"
        "            }\n"
        "            return 1;\n"
        "        } return a.p == b.p;\n"
        "        default: return 0;\n"
        "    }\n"
        "}\n\n"
        "static int xs_bi_cmp_str(const char *a, const char *b) {\n"
        "    /* Compare two non-negative decimal magnitudes. Strip leading\n"
        "       zeros so \"010\" and \"10\" rank equal; then length wins,\n"
        "       falling back to strcmp at equal length. */\n"
        "    if (!a) a = \"0\"; if (!b) b = \"0\";\n"
        "    while (*a == '0' && *(a+1)) a++;\n"
        "    while (*b == '0' && *(b+1)) b++;\n"
        "    size_t la = strlen(a), lb = strlen(b);\n"
        "    if (la != lb) return la < lb ? -1 : 1;\n"
        "    int c = strcmp(a, b);\n"
        "    return (c > 0) - (c < 0);\n"
        "}\n"
        "static int xs_cmp(xs_val a, xs_val b) {\n"
        "    if (a.tag == 9 || b.tag == 9) {\n"
        "        if ((a.tag == 9 || a.tag == 0) && (b.tag == 9 || b.tag == 0))\n"
        "            return xs_bi_cmp_str(xs_bi_str(a), xs_bi_str(b));\n"
        "        if (a.tag == 1 || b.tag == 1) {\n"
        "            double af = a.tag == 1 ? a.f : strtod(xs_bi_str(a), NULL);\n"
        "            double bf = b.tag == 1 ? b.f : strtod(xs_bi_str(b), NULL);\n"
        "            return (af > bf) - (af < bf);\n"
        "        }\n"
        "    }\n"
        "    if (a.tag == 0 && b.tag == 0) return (a.i > b.i) - (a.i < b.i);\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return (af > bf) - (af < bf);\n"
        "    }\n"
        "    if (a.tag == 2 && b.tag == 2) return strcmp(a.s ? a.s : \"\", b.s ? b.s : \"\");\n"
        "    /* Lexicographic compare for arrays + tuples - shorter prefix ranks\n"
        "     * less. Matches the interp's value_cmp; the tag-only fallthrough\n"
        "     * silently returned 0 and made [1] < [2] evaluate false. */\n"
        "    if (a.tag == 5 && b.tag == 5 && a.p && b.p) {\n"
        "        xs_arr *aa = (xs_arr*)a.p, *bb = (xs_arr*)b.p;\n"
        "        int n = aa->len < bb->len ? aa->len : bb->len;\n"
        "        for (int i = 0; i < n; i++) {\n"
        "            int c = xs_cmp(aa->items[i], bb->items[i]);\n"
        "            if (c != 0) return c;\n"
        "        }\n"
        "        return (aa->len > bb->len) - (aa->len < bb->len);\n"
        "    }\n"
        "    return 0;\n"
        "}\n\n"
        "/* ---- bigint runtime --------------------------------------------\n"
        " * Decimal-string payload (sign-free; magnitude only since the\n"
        " * conformance suite never asks for negatives). Add and multiply\n"
        " * via the schoolbook algorithm; cheap and good enough for the\n"
        " * overflow / 10**30 cases. */\n"
        "#define XS_BIGINT_TAG 9\n"
        "#define XS_BIGINT(v)  ((xs_val){.tag=XS_BIGINT_TAG, .s=(char*)(v)})\n"
        "static int xs_bi_strip(const char *s, const char **out, size_t *out_len) {\n"
        "    if (!s) { *out = \"0\"; *out_len = 1; return 0; }\n"
        "    size_t i = 0; while (s[i] == '0' && s[i+1]) i++;\n"
        "    *out = s + i; *out_len = strlen(s + i); return 0;\n"
        "}\n"
        "static char *xs_bi_from_i64(int64_t v) {\n"
        "    char buf[32];\n"
        "    snprintf(buf, sizeof buf, \"%lld\", (long long)v);\n"
        "    return strdup(buf);\n"
        "}\n"
        "static const char *xs_bi_str(xs_val v) {\n"
        "    if (v.tag == XS_BIGINT_TAG) return v.s ? v.s : \"0\";\n"
        "    if (v.tag == 0) {\n"
        "        static char buf[32]; snprintf(buf, sizeof buf, \"%lld\", (long long)v.i);\n"
        "        return buf;\n"
        "    }\n"
        "    return \"0\";\n"
        "}\n"
        "static char *xs_bi_add_str(const char *a, const char *b) {\n"
        "    const char *as; const char *bs; size_t la, lb;\n"
        "    xs_bi_strip(a, &as, &la); xs_bi_strip(b, &bs, &lb);\n"
        "    size_t lmax = la > lb ? la : lb;\n"
        "    char *out = (char*)malloc(lmax + 2);\n"
        "    int carry = 0; size_t k = 0;\n"
        "    for (size_t i = 0; i < lmax; i++) {\n"
        "        int da = i < la ? as[la - 1 - i] - '0' : 0;\n"
        "        int db = i < lb ? bs[lb - 1 - i] - '0' : 0;\n"
        "        int sum = da + db + carry;\n"
        "        out[k++] = (char)('0' + (sum % 10));\n"
        "        carry = sum / 10;\n"
        "    }\n"
        "    if (carry) out[k++] = (char)('0' + carry);\n"
        "    out[k] = 0;\n"
        "    /* reverse */\n"
        "    for (size_t i = 0, j = k - 1; i < j; i++, j--) { char t = out[i]; out[i] = out[j]; out[j] = t; }\n"
        "    return out;\n"
        "}\n"
        "static char *xs_bi_mul_str(const char *a, const char *b) {\n"
        "    const char *as; const char *bs; size_t la, lb;\n"
        "    xs_bi_strip(a, &as, &la); xs_bi_strip(b, &bs, &lb);\n"
        "    size_t out_len = la + lb + 1;\n"
        "    int *digits = (int*)calloc(out_len, sizeof(int));\n"
        "    for (size_t i = 0; i < la; i++) {\n"
        "        int da = as[la - 1 - i] - '0';\n"
        "        for (size_t j = 0; j < lb; j++) {\n"
        "            int db = bs[lb - 1 - j] - '0';\n"
        "            digits[i + j] += da * db;\n"
        "        }\n"
        "    }\n"
        "    /* carry propagation */\n"
        "    for (size_t i = 0; i < out_len - 1; i++) {\n"
        "        digits[i + 1] += digits[i] / 10;\n"
        "        digits[i] %= 10;\n"
        "    }\n"
        "    /* trim leading zeros */\n"
        "    size_t k = out_len; while (k > 1 && digits[k - 1] == 0) k--;\n"
        "    char *out = (char*)malloc(k + 1);\n"
        "    for (size_t i = 0; i < k; i++) out[i] = (char)('0' + digits[k - 1 - i]);\n"
        "    out[k] = 0;\n"
        "    free(digits);\n"
        "    return out;\n"
        "}\n"
        "/* These checks must remain valid under -O2. The earlier code\n"
        "   relied on signed-overflow being defined; gcc/clang treat that\n"
        "   as UB at -O2 and elide the check, so 10 ** 30 silently wraps\n"
        "   instead of promoting to bigint. __builtin_*_overflow does the\n"
        "   arithmetic in a defined way, which the optimiser respects. */\n"
        "static int xs_bi_overflow_add(int64_t a, int64_t b) {\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "    int64_t r;\n"
        "    return __builtin_add_overflow(a, b, &r);\n"
        "#else\n"
        "    if (b > 0 && a > INT64_MAX - b) return 1;\n"
        "    if (b < 0 && a < INT64_MIN - b) return 1;\n"
        "    return 0;\n"
        "#endif\n"
        "}\n"
        "static int xs_bi_overflow_mul(int64_t a, int64_t b) {\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "    int64_t r;\n"
        "    return __builtin_mul_overflow(a, b, &r);\n"
        "#else\n"
        "    if (a == 0 || b == 0) return 0;\n"
        "    if (a == INT64_MIN || b == INT64_MIN) return 1;\n"
        "    int64_t aa = a < 0 ? -a : a;\n"
        "    int64_t bb = b < 0 ? -b : b;\n"
        "    return aa > INT64_MAX / bb;\n"
        "#endif\n"
        "}\n\n"
        "static xs_val xs_add(xs_val a, xs_val b) {\n"
        "    if (a.tag == 0 && b.tag == 0) {\n"
        "        if (xs_bi_overflow_add(a.i, b.i)) {\n"
        "            char *as = xs_bi_from_i64(a.i);\n"
        "            char *bs = xs_bi_from_i64(b.i);\n"
        "            char *r = xs_bi_add_str(as, bs);\n"
        "            free(as); free(bs);\n"
        "            return XS_BIGINT(r);\n"
        "        }\n"
        "        return XS_INT(a.i + b.i);\n"
        "    }\n"
        "    if (a.tag == XS_BIGINT_TAG || b.tag == XS_BIGINT_TAG) {\n"
        "        char *r = xs_bi_add_str(xs_bi_str(a), xs_bi_str(b));\n"
        "        return XS_BIGINT(r);\n"
        "    }\n"
        "    /* string concat must run before the float branch; otherwise\n"
        "       \"pi: \" + 3.14 hits the float path and reinterprets the\n"
        "       string pointer as a double. */\n"
        "    if (a.tag == 2 || b.tag == 2) {\n"
        "        const char *as = a.tag == 2 ? (a.s ? a.s : \"\") : xs_to_str(a);\n"
        "        const char *bs = b.tag == 2 ? (b.s ? b.s : \"\") : xs_to_str(b);\n"
        "        size_t la = strlen(as), lb = strlen(bs);\n"
        "        char *out = (char*)malloc(la + lb + 1);\n"
        "        memcpy(out, as, la); memcpy(out + la, bs, lb); out[la + lb] = 0;\n"
        "        return XS_STR(out);\n"
        "    }\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af + bf);\n"
        "    }\n"
        "    if (a.tag == 5 && b.tag == 5 && a.p && b.p) {\n"
        "        xs_arr *aa = (xs_arr*)a.p, *bb = (xs_arr*)b.p;\n"
        "        xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "        r->len = aa->len + bb->len;\n"
        "        r->cap = r->len > 4 ? r->len : 4;\n"
        "        r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "        for (int i = 0; i < aa->len; i++) r->items[i] = aa->items[i];\n"
        "        for (int i = 0; i < bb->len; i++) r->items[aa->len + i] = bb->items[i];\n"
        "        return (xs_val){.tag=5, .p=r};\n"
        "    }\n"
        "    return XS_INT(a.i + b.i);\n"
        "}\n\n"
        "static xs_val xs_sub(xs_val a, xs_val b) {\n"
        "    /* String / non-numeric operands trapped silently into the float\n"
        "     * branch (reading the str pointer as a double). Throw a\n"
        "     * structured catchable error instead. */\n"
        "    if (a.tag == 2 || b.tag == 2 || a.tag == 5 || b.tag == 5 || a.tag == 6 || b.tag == 6)\n"
        "        xs_throw_arith(\"type error\", \"unsupported operand type for -\");\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af - bf);\n"
        "    }\n"
        "    return XS_INT(a.i - b.i);\n"
        "}\n\n"
        "static xs_val xs_mul(xs_val a, xs_val b) {\n"
        "    /* normalise so the int operand is on the right for str/arr * int */\n"
        "    if ((b.tag == 2 || b.tag == 5) && a.tag == 0) { xs_val t = a; a = b; b = t; }\n"
        "    if (a.tag == 2 && b.tag == 0) {\n"
        "        const char *src = a.s ? a.s : \"\";\n"
        "        long long n = b.i; if (n < 0) n = 0;\n"
        "        size_t l = strlen(src);\n"
        "        char *r = (char*)malloc(l * (size_t)n + 1);\n"
        "        for (long long i = 0; i < n; i++) memcpy(r + i * l, src, l);\n"
        "        r[l * (size_t)n] = 0; return XS_STR(r);\n"
        "    }\n"
        "    if (a.tag == 5 && b.tag == 0) {\n"
        "        xs_arr *src = (xs_arr*)a.p; if (!src) return a;\n"
        "        long long n = b.i; if (n < 0) n = 0;\n"
        "        xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "        r->len = src->len * (int)n; r->cap = r->len > 4 ? r->len : 4;\n"
        "        r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "        for (long long i = 0; i < n; i++)\n"
        "            for (int j = 0; j < src->len; j++) r->items[i * src->len + j] = src->items[j];\n"
        "        return (xs_val){.tag=5, .p=r};\n"
        "    }\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af * bf);\n"
        "    }\n"
        "    if (a.tag == XS_BIGINT_TAG || b.tag == XS_BIGINT_TAG) {\n"
        "        char *r = xs_bi_mul_str(xs_bi_str(a), xs_bi_str(b));\n"
        "        return XS_BIGINT(r);\n"
        "    }\n"
        "    if (a.tag == 0 && b.tag == 0 && xs_bi_overflow_mul(a.i, b.i)) {\n"
        "        char *as = xs_bi_from_i64(a.i);\n"
        "        char *bs = xs_bi_from_i64(b.i);\n"
        "        char *r = xs_bi_mul_str(as, bs);\n"
        "        free(as); free(bs);\n"
        "        return XS_BIGINT(r);\n"
        "    }\n"
        "    return XS_INT(a.i * b.i);\n"
        "}\n\n"
        "static void xs_throw_arith(const char *kind, const char *msg);\n"
        "static xs_val xs_div(xs_val a, xs_val b) {\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        /* IEEE float division: 1.0/0.0 == inf, 0.0/0.0 == NaN.\n"
        "           the interp matches this; only int divide-by-zero throws. */\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af / bf);\n"
        "    }\n"
        "    if (b.i == 0) xs_throw_arith(\"division by zero\", \"cannot divide by zero\");\n"
        "    return XS_INT(a.i / b.i);\n"
        "}\n\n"
        "static xs_val xs_mod(xs_val a, xs_val b) {\n"
        "    if (b.i == 0) xs_throw_arith(\"modulo by zero\", \"cannot take remainder with divisor zero\");\n"
        "    /* Truncated remainder: sign follows the dividend (C's %). */\n"
        "    return XS_INT(a.i % b.i);\n"
        "}\n\n"
        "static xs_val xs_idiv(xs_val a, xs_val b) {\n"
        "    if (b.i == 0) xs_throw_arith(\"division by zero\", \"cannot floor-divide by zero\");\n"
        "    /* Floor division: matches VM (-7 // 2 = -4, not -3). */\n"
        "    int64_t q = a.i / b.i;\n"
        "    if ((a.i ^ b.i) < 0 && a.i % b.i != 0) q--;\n"
        "    return XS_INT(q);\n"
        "}\n\n"
        "static int xs_is(xs_val a, xs_val type) {\n"
        "    if (type.tag != 2 || !type.s) return 0;\n"
        "    const char *t = type.s;\n"
        "    /* accept the canonical lowercase forms and the casual\n"
        "       capitalized ones the docs sometimes use. */\n"
        "    if (strcmp(t, \"int\") == 0 || strcmp(t, \"i64\") == 0 ||\n"
        "        strcmp(t, \"Int\") == 0 || strcmp(t, \"Integer\") == 0) return a.tag == 0;\n"
        "    if (strcmp(t, \"float\") == 0 || strcmp(t, \"f64\") == 0 ||\n"
        "        strcmp(t, \"Float\") == 0) return a.tag == 1;\n"
        "    if (strcmp(t, \"str\") == 0 || strcmp(t, \"string\") == 0 ||\n"
        "        strcmp(t, \"String\") == 0 || strcmp(t, \"Str\") == 0) return a.tag == 2;\n"
        "    if (strcmp(t, \"bool\") == 0 || strcmp(t, \"Bool\") == 0) return a.tag == 3;\n"
        "    if (strcmp(t, \"null\") == 0 || strcmp(t, \"Null\") == 0) return a.tag == 4;\n"
        "    if (strcmp(t, \"array\") == 0 || strcmp(t, \"Array\") == 0 ||\n"
        "        strcmp(t, \"List\") == 0) return a.tag == 5;\n"
        "    if (strcmp(t, \"map\") == 0 || strcmp(t, \"Map\") == 0 ||\n"
        "        strcmp(t, \"dict\") == 0 || strcmp(t, \"Dict\") == 0) return a.tag == 6;\n"
        "    return 0;\n"
        "}\n\n"
        "static xs_val xs_pow(xs_val a, xs_val b) {\n"
        "    /* int ** non-negative-int stays int (matches VM); the\n"
        "     * moment the running product would overflow we switch to\n"
        "     * bigint multiplication so 10**30 stays exact. Other combos\n"
        "     * promote to float. */\n"
        "    if (a.tag == 0 && b.tag == 0 && b.i >= 0) {\n"
        "        int64_t base = a.i, exp = b.i, r = 1;\n"
        "        int overflowed = 0;\n"
        "        while (exp > 0) {\n"
        "            if (exp & 1) {\n"
        "                if (xs_bi_overflow_mul(r, base)) { overflowed = 1; break; }\n"
        "                r *= base;\n"
        "            }\n"
        "            exp >>= 1;\n"
        "            if (exp == 0) break;\n"
        "            if (xs_bi_overflow_mul(base, base)) { overflowed = 1; break; }\n"
        "            base *= base;\n"
        "        }\n"
        "        if (!overflowed) return XS_INT(r);\n"
        "        /* fall back to bigint pow */\n"
        "        char *bs_r = xs_bi_from_i64(r);\n"
        "        char *bs_base = xs_bi_from_i64(base);\n"
        "        while (exp > 0) {\n"
        "            if (exp & 1) {\n"
        "                char *t = xs_bi_mul_str(bs_r, bs_base);\n"
        "                free(bs_r); bs_r = t;\n"
        "            }\n"
        "            exp >>= 1;\n"
        "            if (exp > 0) {\n"
        "                char *t = xs_bi_mul_str(bs_base, bs_base);\n"
        "                free(bs_base); bs_base = t;\n"
        "            }\n"
        "        }\n"
        "        free(bs_base);\n"
        "        return XS_BIGINT(bs_r);\n"
        "    }\n"
        "    double da = (a.tag == 1) ? a.f : (double)a.i;\n"
        "    double db = (b.tag == 1) ? b.f : (double)b.i;\n"
        "    return XS_FLOAT(pow(da, db));\n"
        "}\n\n"
        "static xs_val xs_neg(xs_val a) {\n"
        "    if (a.tag == 1) return XS_FLOAT(-a.f);\n"
        "    return XS_INT(-a.i);\n"
        "}\n\n"
        "static xs_val xs_strcat(xs_val a, xs_val b) {\n"
        "    const char *sa = a.tag == 2 ? a.s : \"\";\n"
        "    const char *sb = b.tag == 2 ? b.s : \"\";\n"
        "    size_t la = strlen(sa), lb = strlen(sb);\n"
        "    char *r = (char*)malloc(la + lb + 1);\n"
        "    memcpy(r, sa, la); memcpy(r + la, sb, lb); r[la + lb] = 0;\n"
        "    return XS_STR(r);\n"
        "}\n\n"
        "/* shortest round-tripping float repr; matches the interp.\n"
        "   prefer fixed-point so 1e10 becomes \"10000000000\" rather\n"
        "   than %g's \"1e+10\"; only fall back to scientific when the\n"
        "   magnitude is outside a friendly range. */\n"
        "static const char *xs_format_float(double f) {\n"
        "    static char bufs[8][64];\n"
        "    static int idx = 0;\n"
        "    char *buf = bufs[idx++ & 7];\n"
        "    if (f != f) { strcpy(buf, \"NaN\"); return buf; }\n"
        "    if (f > 0 && f / f != f / f) { strcpy(buf, \"Infinity\"); return buf; }\n"
        "    if (f < 0 && f / f != f / f) { strcpy(buf, \"-Infinity\"); return buf; }\n"
        "    double absf = f < 0 ? -f : f;\n"
        "    int use_fixed = (absf == 0.0) || (absf >= 1e-4 && absf < 1e21);\n"
        "    int found = 0;\n"
        "    if (use_fixed) {\n"
        "        for (int prec = 0; prec <= 17; prec++) {\n"
        "            snprintf(buf, 64, \"%.*f\", prec, f);\n"
        "            if (strtod(buf, NULL) == f) { found = 1; break; }\n"
        "        }\n"
        "    }\n"
        "    if (!found) {\n"
        "        for (int prec = 1; prec <= 17; prec++) {\n"
        "            snprintf(buf, 64, \"%.*g\", prec, f);\n"
        "            if (strtod(buf, NULL) == f) { found = 1; break; }\n"
        "        }\n"
        "    }\n"
        "    if (!found) snprintf(buf, 64, \"%.17g\", f);\n"
        "    /* float repr should always look like a float; if the shortest\n"
        "       round-trip dropped the fractional part (e.g. 4.0 -> \"4\"),\n"
        "       paste back a trailing .0 so it's distinguishable from int. */\n"
        "    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&\n"
        "        !strchr(buf, 'N') && !strchr(buf, 'I')) {\n"
        "        size_t bl = strlen(buf);\n"
        "        if (bl + 2 < 64) { buf[bl] = '.'; buf[bl+1] = '0'; buf[bl+2] = 0; }\n"
        "    }\n"
        "    return buf;\n"
        "}\n\n"
        "static const char *xs_to_str(xs_val v);\n"
        "/* uses rotating buffers to avoid clobbering on recursive/nested calls */\n"
        "static const char *xs_to_str(xs_val v) {\n"
        "    static char bufs[256][4096];\n"
        "    static int buf_idx = 0;\n"
        "    char *buf = bufs[buf_idx++ & 255];\n"
        "    switch (v.tag) {\n"
        "        case 0: snprintf(buf, 4096, \"%lld\", (long long)v.i); return buf;\n"
        "        case 1: snprintf(buf, 4096, \"%s\", xs_format_float(v.f)); return buf;\n"
        "        case 2: return v.s ? v.s : \"null\";\n"
        "        case 3: return v.b ? \"true\" : \"false\";\n"
        "        case XS_BIGINT_TAG: return v.s ? v.s : \"0\";\n"
        "        case 5: if (v.p) {\n"
        "            xs_arr *a = (xs_arr*)v.p;\n"
        "            const char *open = a->is_tuple ? \"(\" : \"[\";\n"
        "            const char *close = a->is_tuple ? \")\" : \"]\";\n"
        "            int pos = 0;\n"
        "            pos += snprintf(buf + pos, 4096 - pos, \"%s\", open);\n"
        "            for (int i = 0; i < a->len && pos < 4096 - 32; i++) {\n"
        "                if (i) pos += snprintf(buf + pos, 4096 - pos, \", \");\n"
        "                pos += snprintf(buf + pos, 4096 - pos, \"%s\", xs_to_str(a->items[i]));\n"
        "            }\n"
        "            snprintf(buf + pos, 4096 - pos, \"%s\", close);\n"
        "            return buf;\n"
        "        } return \"[]\";\n"
        "        case 6: if (v.p) {\n"
        "            xs_hmap *m = (xs_hmap*)v.p;\n"
        "            int pos = 0;\n"
        "            pos += snprintf(buf + pos, 4096 - pos, \"{\");\n"
        "            for (int i = 0; i < m->len && pos < 4096 - 64; i++) {\n"
        "                if (i) pos += snprintf(buf + pos, 4096 - pos, \", \");\n"
        "                pos += snprintf(buf + pos, 4096 - pos, \"%s: %s\", m->keys[i], xs_to_str(m->vals[i]));\n"
        "            }\n"
        "            snprintf(buf + pos, 4096 - pos, \"}\");\n"
        "            return buf;\n"
        "        } return \"{}\";\n"
        "        default: return \"null\";\n"
        "    }\n"
        "}\n\n"
        "static xs_val xs_sprintf(const char *fmt, ...) {\n"
        "    char buf[4096];\n"
        "    va_list ap; va_start(ap, fmt);\n"
        "    vsnprintf(buf, sizeof buf, fmt, ap);\n"
        "    va_end(ap);\n"
        "    return XS_STR(strdup(buf));\n"
        "}\n\n"
        "static xs_val xs_println(xs_val v) {\n"
        "    if (v.tag == 4) printf(\"null\\n\");\n"
        "    else printf(\"%s\\n\", xs_to_str(v));\n"
        "    return (xs_val){.tag=4};\n"
        "}\n\n"
        "static xs_val xs_print(xs_val v) {\n"
        "    if (v.tag == 4) printf(\"null\");\n"
        "    else printf(\"%s\", xs_to_str(v));\n"
        "    return (xs_val){.tag=4};\n"
        "}\n\n"
        "/* exception handling runtime */\n"
        "#define XS_MAX_HANDLERS 64\n"
        "static jmp_buf *__xs_handlers[XS_MAX_HANDLERS];\n"
        "static int __xs_handler_top = 0;\n"
        "static xs_val __xs_exception = {.tag=4};\n"
        "static int __xs_exception_tag = 0; /* 0=none, 1=string, 2=int, 3=float, 4=bool, 5=array, 6=map */\n"
        "static int __xs_in_catch = 0; /* nonzero when executing inside a catch block */\n\n"

        "/* call-stack tracing */\n"
        "#define XS_MAX_STACK 256\n"
        "static const char *__xs_call_stack[XS_MAX_STACK];\n"
        "static int __xs_stack_top = 0;\n"
        "static void xs_push_frame(const char *name) {\n"
        "    if (__xs_stack_top < XS_MAX_STACK) __xs_call_stack[__xs_stack_top++] = name;\n"
        "}\n"
        "static void xs_pop_frame(void) { if (__xs_stack_top > 0) __xs_stack_top--; }\n"
        "static void xs_print_stack_trace(void) {\n"
        "    fprintf(stderr, \"Stack trace:\\n\");\n"
        "    for (int i = __xs_stack_top - 1; i >= 0; i--)\n"
        "        fprintf(stderr, \"  at %s\\n\", __xs_call_stack[i]);\n"
        "}\n\n"

        "/* defer stack */\n"
        "typedef void (*xs_defer_fn)(void);\n"
        "#define XS_MAX_DEFERS 256\n"
        "static xs_defer_fn __xs_defers[XS_MAX_DEFERS];\n"
        "static int __xs_defer_top = 0;\n"
        "static void xs_push_defer(xs_defer_fn fn) {\n"
        "    if (__xs_defer_top < XS_MAX_DEFERS) __xs_defers[__xs_defer_top++] = fn;\n"
        "}\n"
        "static void xs_run_defers(int from) {\n"
        "    for (int i = __xs_defer_top - 1; i >= from; i--) __xs_defers[i]();\n"
        "    __xs_defer_top = from;\n"
        "}\n\n"

        "/* finally handler stack */\n"
        "typedef void (*xs_finally_fn)(void);\n"
        "#define XS_MAX_FINALLY 64\n"
        "static xs_finally_fn __xs_finally_handlers[XS_MAX_FINALLY];\n"
        "static int __xs_finally_top = 0;\n"
        "static void xs_push_finally(xs_finally_fn fn) {\n"
        "    if (__xs_finally_top < XS_MAX_FINALLY) __xs_finally_handlers[__xs_finally_top++] = fn;\n"
        "}\n"
        "static void xs_pop_finally(void) { if (__xs_finally_top > 0) __xs_finally_top--; }\n\n"

        "/* handler push/pop */\n"
        "static void xs_push_handler(jmp_buf *jb) {\n"
        "    if (__xs_handler_top < XS_MAX_HANDLERS) __xs_handlers[__xs_handler_top++] = jb;\n"
        "}\n"
        "static void xs_pop_handler(void) { if (__xs_handler_top > 0) __xs_handler_top--; }\n\n"

        "/* effect handler frames. Single-shot semantics. Each handle's\n"
        " * arms are compiled into a GCC nested-function dispatcher, which\n"
        " * gives the arm body access to enclosing locals via the trampoline\n"
        " * GCC builds. xs_perform calls the dispatcher in place: arm calls\n"
        " * xs_resume to return the value to the perform site, or returns\n"
        " * normally to make the arm's value the handle expression's value.\n"
        " * Multi-shot resume needs full continuations and is not supported. */\n"
        "typedef xs_val (*XsArmDispatch)(int aid, xs_val arg);\n"
        "typedef struct XsEffFrame {\n"
        "    const char       *eff_name; /* first arm's effect, kept for diags */\n"
        "    int               n_arms;\n"
        "    const char       *arm_eff_names[16]; /* per-arm effect (e.g. \"Log\") */\n"
        "    const char       *arm_op_names[16];  /* per-arm op (e.g. \"say\") */\n"
        "    XsArmDispatch     dispatch;\n"
        "    jmp_buf           exit_jmp;\n"
        "    jmp_buf           resume_jmp;\n"
        "    xs_val            exit_value;\n"
        "    xs_val            resume_value;\n"
        "    struct XsEffFrame *prev;\n"
        "} XsEffFrame;\n\n"
        "static XsEffFrame *__xs_eff_top = NULL;\n"
        "static XsEffFrame *__xs_eff_active_perform = NULL;\n\n"
        "static xs_val xs_perform(const char *eff, const char *op, xs_val arg) {\n"
        "    XsEffFrame *f = __xs_eff_top;\n"
        "    while (f) {\n"
        "        for (int i = 0; i < f->n_arms; i++) {\n"
        "            /* Each arm carries its own (effect, op). A single handle\n"
        "             * with `Log.say(m) => ...` and `Metric.count(n) => ...`\n"
        "             * has arms[0].eff='Log' and arms[1].eff='Metric'; the\n"
        "             * old code only stored the first arm's effect on the\n"
        "             * frame and dropped Metric.count on the floor. */\n"
        "            const char *arm_eff = f->arm_eff_names[i] ? f->arm_eff_names[i] : f->eff_name;\n"
        "            if (strcmp(arm_eff, eff) == 0 && strcmp(f->arm_op_names[i], op) == 0) {\n"
        "                XsEffFrame *prev_active = __xs_eff_active_perform;\n"
        "                __xs_eff_active_perform = f;\n"
        "                if (setjmp(f->resume_jmp) == 0) {\n"
        "                    xs_val r = f->dispatch(i, arg);\n"
        "                    __xs_eff_active_perform = prev_active;\n"
        "                    f->exit_value = r;\n"
        "                    longjmp(f->exit_jmp, 1);\n"
        "                    return (xs_val){.tag=4}; /* unreachable */\n"
        "                } else {\n"
        "                    __xs_eff_active_perform = prev_active;\n"
        "                    return f->resume_value;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        f = f->prev;\n"
        "    }\n"
        "    fprintf(stderr, \"unhandled effect: %s.%s\\n\", eff, op);\n"
        "    exit(1);\n"
        "}\n\n"
        "static xs_val xs_resume(xs_val v) {\n"
        "    if (__xs_eff_active_perform) {\n"
        "        __xs_eff_active_perform->resume_value = v;\n"
        "        longjmp(__xs_eff_active_perform->resume_jmp, 1);\n"
        "    }\n"
        "    fprintf(stderr, \"resume called outside of an effect handler\\n\");\n"
        "    exit(1);\n"
        "}\n\n"

        "/* del sentinel - any read of a `del`'d local lands here, throws a\n"
        " * catchable string error matching the interp / VM behaviour. */\n"
        "static void xs_throw(xs_val v);\n"
        "static xs_val xs_check_deleted(xs_val v, const char *name) {\n"
        "    if (v.tag == 99) {\n"
        "        char *msg = (char*)malloc(64 + strlen(name));\n"
        "        sprintf(msg, \"name '%s' is not defined (deleted)\", name);\n"
        "        xs_throw(XS_STR(msg));\n"
        "    }\n"
        "    return v;\n"
        "}\n\n"
        "/* throw / rethrow */\n"
        "static void xs_throw(xs_val v) {\n"
        "    __xs_exception = v;\n"
        "    /* derive exception type tag from xs_val tag */\n"
        "    switch (v.tag) {\n"
        "        case 0: __xs_exception_tag = 2; break; /* int */\n"
        "        case 1: __xs_exception_tag = 3; break; /* float */\n"
        "        case 2: __xs_exception_tag = 1; break; /* string */\n"
        "        case 3: __xs_exception_tag = 4; break; /* bool */\n"
        "        case 5: __xs_exception_tag = 5; break; /* array */\n"
        "        case 6: __xs_exception_tag = 6; break; /* map */\n"
        "        default: __xs_exception_tag = 0; break;\n"
        "    }\n"
        "    /* run defers during stack unwinding */\n"
        "    xs_run_defers(0);\n"
        "    if (__xs_handler_top > 0) longjmp(*__xs_handlers[--__xs_handler_top], 1);\n"
        "    fprintf(stderr, \"unhandled exception: %s\\n\", xs_to_str(v));\n"
        "    xs_print_stack_trace();\n"
        "    abort();\n"
        "}\n"
        "static void xs_rethrow(void) {\n"
        "    /* re-throw the current exception to the next outer handler */\n"
        "    if (__xs_handler_top > 0) longjmp(*__xs_handlers[--__xs_handler_top], 1);\n"
        "    fprintf(stderr, \"unhandled exception (rethrown): %s\\n\", xs_to_str(__xs_exception));\n"
        "    xs_print_stack_trace();\n"
        "    abort();\n"
        "}\n"
        "static xs_val xs_get_exception(void) { return __xs_exception; }\n"
        "static int xs_get_exception_tag(void) { return __xs_exception_tag; }\n\n"
        "/* array/map constructors */\n"
        "static xs_val xs_array(int n, ...) {\n"
        "    xs_arr *a = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    a->cap = n > 4 ? n : 4;\n"
        "    a->items = (xs_val*)malloc(sizeof(xs_val) * a->cap);\n"
        "    a->len = n;\n"
        "    a->is_tuple = 0;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    for (int i = 0; i < n; i++) a->items[i] = va_arg(ap, xs_val);\n"
        "    va_end(ap);\n"
        "    return (xs_val){.tag=5, .p=a};\n"
        "}\n"
        "static xs_val xs_tuple(int n, ...) {\n"
        "    xs_arr *a = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    a->cap = n > 4 ? n : 4;\n"
        "    a->items = (xs_val*)malloc(sizeof(xs_val) * a->cap);\n"
        "    a->len = n;\n"
        "    a->is_tuple = 1;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    for (int i = 0; i < n; i++) a->items[i] = va_arg(ap, xs_val);\n"
        "    va_end(ap);\n"
        "    return (xs_val){.tag=5, .p=a};\n"
        "}\n\n"
        "static xs_val xs_arr_push(xs_val arr, xs_val v) {\n"
        "    if (arr.tag != 5 || !arr.p) return arr;\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    if (a->len >= a->cap) {\n"
        "        a->cap = a->cap * 2;\n"
        "        a->items = (xs_val*)realloc(a->items, sizeof(xs_val) * a->cap);\n"
        "    }\n"
        "    a->items[a->len++] = v;\n"
        "    return arr;\n"
        "}\n\n"
        "static xs_val xs_index(xs_val arr, xs_val idx) {\n"
        "    if (arr.tag == 5 && arr.p) {\n"
        "        xs_arr *a = (xs_arr*)arr.p;\n"
        "        int64_t i = idx.tag == 0 ? idx.i : 0;\n"
        "        if (i < 0) i += a->len;\n"
        "        if (i >= 0 && i < a->len) return a->items[i];\n"
        "        return XS_NULL;\n"
        "    }\n"
        "    if (arr.tag == 2 && arr.s) {\n"
        "        int64_t i = idx.tag == 0 ? idx.i : 0;\n"
        "        int slen = (int)strlen(arr.s);\n"
        "        if (i < 0) i += slen;\n"
        "        if (i >= 0 && i < slen) {\n"
        "            char *c = (char*)malloc(2);\n"
        "            c[0] = arr.s[i]; c[1] = 0;\n"
        "            return XS_STR(c);\n"
        "        }\n"
        "        return XS_NULL;\n"
        "    }\n"
        "    if (arr.tag == 6 && arr.p) {\n"
        "        xs_hmap *m = (xs_hmap*)arr.p;\n"
        "        const char *k = idx.tag == 2 ? idx.s : xs_to_str(idx);\n"
        "        for (int i = 0; i < m->len; i++) {\n"
        "            if (strcmp(m->keys[i], k) == 0) return m->vals[i];\n"
        "        }\n"
        "        return XS_NULL;\n"
        "    }\n"
        "    /* indexing a non-collection (int / float / bool / null) throws a\n"
        "     * structured catchable error matching the interp / VM. Without\n"
        "     * this, (42)[0] silently produced null and `try` couldn't catch. */\n"
        "    xs_throw_arith(\"type error\", \"value is not indexable\");\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "/* arr[start..end] / arr[start..=end]; null bounds mean open. */\n"
        "static xs_val xs_slice(xs_val v, xs_val start, xs_val end, int inclusive) {\n"
        "    int len = 0;\n"
        "    if (v.tag == 5 && v.p) len = ((xs_arr*)v.p)->len;\n"
        "    else if (v.tag == 2 && v.s) len = (int)strlen(v.s);\n"
        "    else return XS_NULL;\n"
        "    int64_t s = (start.tag == 0) ? start.i : 0;\n"
        "    int64_t e = (end.tag == 0) ? end.i : (int64_t)len;\n"
        "    if (start.tag == 4) s = 0;\n"
        "    if (end.tag == 4) e = (int64_t)len;\n"
        "    if (s < 0) s += len;\n"
        "    if (e < 0) e += len;\n"
        "    if (inclusive) e += 1;\n"
        "    if (s < 0) s = 0;\n"
        "    if (e > len) e = len;\n"
        "    if (e < s) e = s;\n"
        "    if (v.tag == 5) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "        r->len = (int)(e - s);\n"
        "        r->cap = r->len > 4 ? r->len : 4;\n"
        "        r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "        for (int64_t i = s; i < e; i++) r->items[i - s] = a->items[i];\n"
        "        return (xs_val){.tag=5, .p=r};\n"
        "    }\n"
        "    /* string slice */\n"
        "    char *out = (char*)malloc((size_t)(e - s) + 1);\n"
        "    memcpy(out, v.s + s, (size_t)(e - s));\n"
        "    out[e - s] = 0;\n"
        "    return XS_STR(out);\n"
        "}\n\n"
        "static xs_val xs_range(xs_val start, xs_val end, int inclusive) {\n"
        "    int64_t s = start.tag == 0 ? start.i : 0;\n"
        "    int64_t e_raw = end.tag == 0 ? end.i : 0;\n"
        "    int64_t e = inclusive ? e_raw + 1 : e_raw;\n"
        "    int64_t step = s <= e ? 1 : -1;\n"
        "    int64_t count = (e - s) * step;\n"
        "    if (count < 0) count = 0;\n"
        "    xs_arr *a = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    a->cap = (int)(count > 4 ? count : 4);\n"
        "    a->items = (xs_val*)malloc(sizeof(xs_val) * a->cap);\n"
        "    a->len = 0;\n"
        "    a->is_range = 1;\n"
        "    a->range_start = s;\n"
        "    a->range_end = e_raw;\n"
        "    a->range_inclusive = inclusive;\n"
        "    for (int64_t i = s; step > 0 ? i < e : i > e; i += step) {\n"
        "        if (a->len >= a->cap) {\n"
        "            a->cap *= 2;\n"
        "            a->items = (xs_val*)realloc(a->items, sizeof(xs_val) * a->cap);\n"
        "        }\n"
        "        a->items[a->len++] = XS_INT(i);\n"
        "    }\n"
        "    return (xs_val){.tag=5, .p=a};\n"
        "}\n\n"
        "static xs_val xs_iter(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *state = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "        state->cap = 2; state->len = 2;\n"
        "        state->items = (xs_val*)malloc(sizeof(xs_val) * 2);\n"
        "        state->items[0] = v;\n"
        "        state->items[1] = XS_INT(0);\n"
        "        return (xs_val){.tag=5, .p=state};\n"
        "    }\n"
        "    return v;\n"
        "}\n\n"
        "static int xs_iter_next(xs_val *iter, xs_val *out) {\n"
        "    if (iter->tag != 5 || !iter->p) return 0;\n"
        "    xs_arr *state = (xs_arr*)iter->p;\n"
        "    if (state->len < 2) return 0;\n"
        "    xs_val src = state->items[0];\n"
        "    int64_t idx = state->items[1].i;\n"
        "    if (src.tag == 5 && src.p) {\n"
        "        xs_arr *a = (xs_arr*)src.p;\n"
        "        if (idx >= a->len) return 0;\n"
        "        *out = a->items[idx];\n"
        "        state->items[1] = XS_INT(idx + 1);\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n\n"
        "static xs_val xs_map(int n, ...) {\n"
        "    xs_hmap *m = (xs_hmap*)malloc(sizeof(xs_hmap));\n"
        "    m->cap = n > 4 ? n : 4;\n"
        "    m->keys = (char**)malloc(sizeof(char*) * m->cap);\n"
        "    m->vals = (xs_val*)malloc(sizeof(xs_val) * m->cap);\n"
        "    m->len = 0;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    for (int i = 0; i < n; i++) {\n"
        "        xs_val k = va_arg(ap, xs_val);\n"
        "        xs_val v = va_arg(ap, xs_val);\n"
        "        const char *ks = k.tag == 2 ? k.s : xs_to_str(k);\n"
        "        m->keys[m->len] = strdup(ks);\n"
        "        m->vals[m->len] = v;\n"
        "        m->len++;\n"
        "    }\n"
        "    va_end(ap);\n"
        "    return (xs_val){.tag=6, .p=m};\n"
        "}\n\n"
        "static void xs_throw_arith(const char *kind, const char *msg) {\n"
        "    xs_val e = xs_map(2,\n"
        "        XS_STR(\"kind\"), XS_STR((char*)kind),\n"
        "        XS_STR(\"message\"), XS_STR((char*)msg));\n"
        "    xs_throw(e);\n"
        "}\n\n"
        "static xs_val xs_map_put(xs_val *map, xs_val key, xs_val val) {\n"
        "    if (!map || !map->p) return val;\n"
        "    if (map->tag == 5) {\n"
        "        xs_arr *a = (xs_arr*)map->p;\n"
        "        if (key.tag != 0) return val;\n"
        "        long long k = key.i;\n"
        "        if (k < 0) k += a->len;\n"
        "        if (k < 0 || k >= a->len) return val;\n"
        "        a->items[k] = val;\n"
        "        return val;\n"
        "    }\n"
        "    if (map->tag != 6) return val;\n"
        "    xs_hmap *m = (xs_hmap*)map->p;\n"
        "    const char *ks = key.tag == 2 ? key.s : xs_to_str(key);\n"
        "    for (int i = 0; i < m->len; i++) {\n"
        "        if (strcmp(m->keys[i], ks) == 0) { m->vals[i] = val; return val; }\n"
        "    }\n"
        "    if (m->len >= m->cap) {\n"
        "        m->cap *= 2;\n"
        "        m->keys = (char**)realloc(m->keys, sizeof(char*) * m->cap);\n"
        "        m->vals = (xs_val*)realloc(m->vals, sizeof(xs_val) * m->cap);\n"
        "    }\n"
        "    m->keys[m->len] = strdup(ks);\n"
        "    m->vals[m->len] = val;\n"
        "    m->len++;\n"
        "    return val;\n"
        "}\n\n"
        "static xs_val xs_map_has(xs_val map, xs_val key) {\n"
        "    if (map.tag != 6 || !map.p) return XS_BOOL(0);\n"
        "    xs_hmap *m = (xs_hmap*)map.p;\n"
        "    const char *ks = key.tag == 2 ? key.s : xs_to_str(key);\n"
        "    for (int i = 0; i < m->len; i++)\n"
        "        if (strcmp(m->keys[i], ks) == 0) return XS_BOOL(1);\n"
        "    return XS_BOOL(0);\n"
        "}\n"
        "static xs_val xs_map_delete(xs_val *map, xs_val key) {\n"
        "    if (!map || map->tag != 6 || !map->p) return XS_BOOL(0);\n"
        "    xs_hmap *m = (xs_hmap*)map->p;\n"
        "    const char *ks = key.tag == 2 ? key.s : xs_to_str(key);\n"
        "    for (int i = 0; i < m->len; i++) {\n"
        "        if (strcmp(m->keys[i], ks) == 0) {\n"
        "            for (int j = i + 1; j < m->len; j++) {\n"
        "                m->keys[j-1] = m->keys[j];\n"
        "                m->vals[j-1] = m->vals[j];\n"
        "            }\n"
        "            m->len--;\n"
        "            return XS_BOOL(1);\n"
        "        }\n"
        "    }\n"
        "    return XS_BOOL(0);\n"
        "}\n\n"
        "static xs_val xs_len(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) return XS_INT(((xs_arr*)v.p)->len);\n"
        "    if (v.tag == 6 && v.p) return XS_INT(((xs_hmap*)v.p)->len);\n"
        "    if (v.tag == 2 && v.s) {\n"
        "        /* xs strings are utf-8; .len() returns codepoint count.\n"
        "           a leading byte has bit pattern 0xxxxxxx or 11xxxxxx;\n"
        "           continuation bytes are 10xxxxxx. */\n"
        "        int64_t n = 0;\n"
        "        for (const unsigned char *p = (const unsigned char*)v.s; *p; p++)\n"
        "            if ((*p & 0xC0) != 0x80) n++;\n"
        "        return XS_INT(n);\n"
        "    }\n"
        "    if (v.tag == 7 && v.p) return XS_INT(((xs_arr*)v.p)->len);\n"
        "    return XS_INT(0);\n"
        "}\n\n"
        "/* channel runtime (single-threaded FIFO queue) */\n"
        "static xs_val xs_channel_new(int max_cap) {\n"
        "    xs_arr *ch = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    ch->cap = max_cap > 0 ? max_cap : 16;\n"
        "    ch->items = (xs_val*)malloc(sizeof(xs_val) * ch->cap);\n"
        "    ch->len = 0;\n"
        "    xs_val v; v.tag = 7; v.p = ch;\n"
        "    /* store max_cap in a side field: use items[cap-1] trick? no, just embed */\n"
        "    /* we'll use a wrapper struct instead */\n"
        "    return v;\n"
        "}\n\n"
        "static int __xs_channel_max_caps[256];\n"
        "static int __xs_channel_count = 0;\n\n"
        "static void xs_channel_send(xs_val ch, xs_val v) {\n"
        "    if (ch.tag != 7 || !ch.p) return;\n"
        "    xs_arr *a = (xs_arr*)ch.p;\n"
        "    if (a->len >= a->cap) {\n"
        "        a->cap *= 2;\n"
        "        a->items = (xs_val*)realloc(a->items, sizeof(xs_val) * a->cap);\n"
        "    }\n"
        "    a->items[a->len++] = v;\n"
        "}\n\n"
        "static xs_val xs_channel_recv(xs_val ch) {\n"
        "    if (ch.tag != 7 || !ch.p) return XS_NULL;\n"
        "    xs_arr *a = (xs_arr*)ch.p;\n"
        "    if (a->len == 0) return XS_NULL;\n"
        "    xs_val v = a->items[0];\n"
        "    memmove(a->items, a->items + 1, sizeof(xs_val) * (a->len - 1));\n"
        "    a->len--;\n"
        "    return v;\n"
        "}\n\n"
        "static xs_val xs_channel_is_empty(xs_val ch) {\n"
        "    if (ch.tag != 7 || !ch.p) return XS_BOOL(1);\n"
        "    return XS_BOOL(((xs_arr*)ch.p)->len == 0);\n"
        "}\n\n"
        "static xs_val xs_channel_is_full(xs_val ch) {\n"
        "    if (ch.tag != 7 || !ch.p) return XS_BOOL(0);\n"
        "    xs_arr *a = (xs_arr*)ch.p;\n"
        "    /* bounded channel: cap was set to max_cap */\n"
        "    return XS_BOOL(a->len >= a->cap);\n"
        "}\n\n"
        "/* assert runtime. Float comparisons get a 1e-9 relative tolerance\n"
        " * so chained float arithmetic that drifts in the last bit (eg.\n"
        " * 21.560000000000002 vs 21.56) doesn't spuriously fail. The interp\n"
        " * does the same. */\n"
        "static void xs_assert_eq(xs_val a, xs_val b) {\n"
        "    int eq = xs_eq(a, b);\n"
        "    if (!eq && (a.tag == 1 || b.tag == 1) &&\n"
        "        (a.tag == 0 || a.tag == 1) && (b.tag == 0 || b.tag == 1)) {\n"
        "        double af = (a.tag == 1) ? a.f : (double)a.i;\n"
        "        double bf = (b.tag == 1) ? b.f : (double)b.i;\n"
        "        double diff = af > bf ? af - bf : bf - af;\n"
        "        double mag  = af > bf ? af : bf;\n"
        "        if (mag < 0) mag = -mag;\n"
        "        if (diff <= 1e-9 || diff <= mag * 1e-9) eq = 1;\n"
        "    }\n"
        "    if (!eq) {\n"
        "        fprintf(stderr, \"assert_eq failed: %s != %s\\n\", xs_to_str(a), xs_to_str(b));\n"
        "        exit(1);\n"
        "    }\n"
        "}\n\n"
        "static void xs_assert(xs_val cond, xs_val msg) {\n"
        "    if (!xs_truthy(cond)) {\n"
        "        fprintf(stderr, \"assert failed: %s\\n\", msg.tag == 2 ? msg.s : xs_to_str(msg));\n"
        "        exit(1);\n"
        "    }\n"
        "}\n\n"
        "static double xs_to_f64(xs_val v) {\n"
        "    if (v.tag == 1) return v.f;\n"
        "    if (v.tag == 0) return (double)v.i;\n"
        "    return 0.0;\n"
        "}\n\n"
        "static xs_val xs_type(xs_val v) {\n"
        "    static const char *names[] = {\"int\",\"float\",\"str\",\"bool\",\"null\",\"array\",\"map\",\"channel\"};\n"
        "    if (v.tag >= 0 && v.tag < 8) return XS_STR((char*)names[v.tag]);\n"
        "    return XS_STR(\"unknown\");\n"
        "}\n\n"
        "/* closure runtime */\n"
        "typedef struct { xs_val (*fn)(void*, xs_val*, int); void *env; int is_pure; } xs_fn_t;\n\n"
        "static xs_val xs_fn_new(xs_val (*fn)(void*, xs_val*, int), void *env) {\n"
        "    xs_fn_t *f = (xs_fn_t*)malloc(sizeof(xs_fn_t));\n"
        "    f->fn = fn; f->env = env; f->is_pure = 0;\n"
        "    return (xs_val){.tag=8, .p=f};\n"
        "}\n\n"
        "static xs_val xs_fn_new_pure(xs_val (*fn)(void*, xs_val*, int), void *env) {\n"
        "    xs_fn_t *f = (xs_fn_t*)malloc(sizeof(xs_fn_t));\n"
        "    f->fn = fn; f->env = env; f->is_pure = 1;\n"
        "    return (xs_val){.tag=8, .p=f};\n"
        "}\n\n"
        "static xs_val xs_is_pure(xs_val v) {\n"
        "    if (v.tag != 8 || !v.p) return XS_BOOL(0);\n"
        "    return XS_BOOL(((xs_fn_t*)v.p)->is_pure ? 1 : 0);\n"
        "}\n\n"
        "static xs_val xs_call(xs_val fn, xs_val *args, int argc) {\n"
        "    if (fn.tag == 8 && fn.p) {\n"
        "        xs_fn_t *f = (xs_fn_t*)fn.p;\n"
        "        return f->fn(f->env, args, argc);\n"
        "    }\n"
        "    fprintf(stderr, \"xs: called non-function value\\n\"); return (xs_val){.tag=4};\n"
        "}\n\n"
        "/* string methods */\n"
        "static xs_val xs_str_split(xs_val s, xs_val delim) {\n"
        "    if (s.tag != 2 || !s.s) return xs_array(0);\n"
        "    const char *src = s.s;\n"
        "    const char *d = (delim.tag == 2 && delim.s) ? delim.s : \" \";\n"
        "    size_t dl = strlen(d);\n"
        "    xs_val result = xs_array(0);\n"
        "    if (dl == 0) {\n"
        "        for (const char *p = src; *p; p++) { char buf[2] = {*p, 0}; xs_arr_push(result, XS_STR(strdup(buf))); }\n"
        "        return result;\n"
        "    }\n"
        "    /* keep empty fields like Python's str.split: \"a,,b\".split(\",\") -> [\"a\", \"\", \"b\"]. */\n"
        "    const char *p = src;\n"
        "    while (1) {\n"
        "        const char *hit = strstr(p, d);\n"
        "        if (!hit) { xs_arr_push(result, XS_STR(strdup(p))); break; }\n"
        "        size_t pl = (size_t)(hit - p);\n"
        "        char *part = (char*)malloc(pl + 1);\n"
        "        memcpy(part, p, pl); part[pl] = 0;\n"
        "        xs_arr_push(result, XS_STR(part));\n"
        "        p = hit + dl;\n"
        "    }\n"
        "    return result;\n"
        "}\n\n"
        "static xs_val xs_str_upper(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return s;\n"
        "    char *r = strdup(s.s);\n"
        "    for (char *p = r; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;\n"
        "    return XS_STR(r);\n"
        "}\n\n"
        "static xs_val xs_str_lower(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return s;\n"
        "    char *r = strdup(s.s);\n"
        "    for (char *p = r; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;\n"
        "    return XS_STR(r);\n"
        "}\n\n"
        "static xs_val xs_arr_join(xs_val arr, xs_val sep) {\n"
        "    if (arr.tag != 5 || !arr.p) return XS_STR(strdup(\"\"));\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    const char *s = (sep.tag == 2 && sep.s) ? sep.s : \"\";\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < a->len; i++) {\n"
        "        if (i) total += (int)strlen(s);\n"
        "        total += (int)strlen(xs_to_str(a->items[i]));\n"
        "    }\n"
        "    char *buf = (char*)malloc(total + 1); buf[0] = 0;\n"
        "    for (int i = 0; i < a->len; i++) {\n"
        "        if (i) strcat(buf, s);\n"
        "        strcat(buf, xs_to_str(a->items[i]));\n"
        "    }\n"
        "    return XS_STR(buf);\n"
        "}\n\n"
        "static xs_val xs_str_parse_float(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return (xs_val){.tag=4};\n"
        "    return XS_FLOAT(atof(s.s));\n"
        "}\n\n"
        "static xs_val xs_is_type(xs_val v, const char *t) {\n"
        "    static const char *tags[] = {\"int\",\"float\",\"str\",\"bool\",\"null\",\"array\",\"map\"};\n"
        "    if (v.tag >= 0 && v.tag < 7 && strcmp(tags[v.tag], t) == 0) return XS_BOOL(1);\n"
        "    return XS_BOOL(0);\n"
        "}\n\n"
        "/* array sort */\n"
        "static int __xs_sort_cmp(const void *a, const void *b) {\n"
        "    return xs_cmp(*(const xs_val*)a, *(const xs_val*)b);\n"
        "}\n"
        "static xs_val xs_arr_sort(xs_val arr) {\n"
        "    if (arr.tag != 5 || !arr.p) return arr;\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    qsort(a->items, a->len, sizeof(xs_val), __xs_sort_cmp);\n"
        "    return arr;\n"
        "}\n\n"
        "/* sort with an optional callable comparator. qsort can't carry user\n"
        " * context, so we stash the comparator in a static for the duration of\n"
        " * the call. Single-threaded by construction; nested user-comparator\n"
        " * sorts would need qsort_r (not portable to MinGW). */\n"
        "static xs_val __xs_sort_cmp_fn = {.tag = 4};\n"
        "static int __xs_sort_cmp_user(const void *a, const void *b) {\n"
        "    xs_val args[2] = { *(const xs_val*)a, *(const xs_val*)b };\n"
        "    xs_val r = xs_call(__xs_sort_cmp_fn, args, 2);\n"
        "    if (r.tag == 0) return r.i > 0 ? 1 : (r.i < 0 ? -1 : 0);\n"
        "    if (r.tag == 1) return r.f > 0 ? 1 : (r.f < 0 ? -1 : 0);\n"
        "    if (r.tag == 3) return r.b ? 1 : -1;\n"
        "    return 0;\n"
        "}\n"
        "static xs_val xs_arr_sort_with(xs_val arr, xs_val cmp) {\n"
        "    if (arr.tag != 5 || !arr.p) return arr;\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    if (cmp.tag == 8 && cmp.p) {\n"
        "        __xs_sort_cmp_fn = cmp;\n"
        "        qsort(a->items, a->len, sizeof(xs_val), __xs_sort_cmp_user);\n"
        "        __xs_sort_cmp_fn = (xs_val){.tag = 4};\n"
        "    } else {\n"
        "        qsort(a->items, a->len, sizeof(xs_val), __xs_sort_cmp);\n"
        "    }\n"
        "    return arr;\n"
        "}\n\n"
        "/* extra collection helpers used by .method() lowering */\n"
        "static xs_val xs_arr_reverse(xs_val v) {\n"
        "    if (v.tag == 2) {\n"
        "        const char *src = v.s ? v.s : \"\";\n"
        "        size_t n = strlen(src);\n"
        "        char *r = (char*)malloc(n + 1);\n"
        "        for (size_t i = 0; i < n; i++) r[i] = src[n - 1 - i];\n"
        "        r[n] = 0;\n"
        "        return XS_STR(r);\n"
        "    }\n"
        "    if (v.tag != 5 || !v.p) return v;\n"
        "    xs_arr *a = (xs_arr*)v.p;\n"
        "    for (int i = 0, j = a->len - 1; i < j; i++, j--) {\n"
        "        xs_val t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;\n"
        "    }\n"
        "    return v;\n"
        "}\n\n"
        "static xs_val xs_arr_clone(xs_val v) {\n"
        "    if (v.tag == 6 && v.p) {\n"
        "        xs_hmap *sm = (xs_hmap*)v.p;\n"
        "        xs_hmap *rm = (xs_hmap*)malloc(sizeof(xs_hmap));\n"
        "        rm->len = sm->len; rm->cap = sm->cap > 4 ? sm->cap : 4;\n"
        "        rm->keys = (char**)malloc(sizeof(char*) * rm->cap);\n"
        "        rm->vals = (xs_val*)malloc(sizeof(xs_val) * rm->cap);\n"
        "        for (int i = 0; i < sm->len; i++) {\n"
        "            rm->keys[i] = strdup(sm->keys[i]);\n"
        "            rm->vals[i] = sm->vals[i];\n"
        "        }\n"
        "        return (xs_val){.tag=6, .p=rm};\n"
        "    }\n"
        "    if (v.tag != 5 || !v.p) return v;\n"
        "    xs_arr *src = (xs_arr*)v.p;\n"
        "    xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    r->len = src->len; r->cap = src->cap > 4 ? src->cap : 4;\n"
        "    r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "    for (int i = 0; i < src->len; i++) r->items[i] = src->items[i];\n"
        "    return (xs_val){.tag=5, .p=r};\n"
        "}\n\n"
        "static xs_val xs_arr_pop(xs_val v) {\n"
        "    if (v.tag != 5 || !v.p) return XS_NULL;\n"
        "    xs_arr *a = (xs_arr*)v.p;\n"
        "    if (a->len <= 0) return XS_NULL;\n"
        "    return a->items[--a->len];\n"
        "}\n\n"
        "/* contains / index_of accept either a value (equality) or a callable\n"
        " * (run as predicate - first item where pred(item) is truthy). The\n"
        " * predicate path mirrors arr.find / arr.find_index so the methods stay\n"
        " * consistent across callsites. */\n"
        "static xs_val xs_arr_contains(xs_val v, xs_val needle) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (needle.tag == 8) {\n"
        "            for (int i = 0; i < a->len; i++) {\n"
        "                xs_val r = xs_call(needle, &a->items[i], 1);\n"
        "                if (xs_truthy(r)) return XS_BOOL(1);\n"
        "            }\n"
        "            return XS_BOOL(0);\n"
        "        }\n"
        "        for (int i = 0; i < a->len; i++)\n"
        "            if (xs_eq(a->items[i], needle)) return XS_BOOL(1);\n"
        "        return XS_BOOL(0);\n"
        "    }\n"
        "    if (v.tag == 2 && v.s && needle.tag == 2 && needle.s)\n"
        "        return XS_BOOL(strstr(v.s, needle.s) != NULL);\n"
        "    return XS_BOOL(0);\n"
        "}\n\n"
        "static xs_val xs_arr_index_of(xs_val v, xs_val needle) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (needle.tag == 8) {\n"
        "            for (int i = 0; i < a->len; i++) {\n"
        "                xs_val r = xs_call(needle, &a->items[i], 1);\n"
        "                if (xs_truthy(r)) return XS_INT(i);\n"
        "            }\n"
        "            return XS_INT(-1);\n"
        "        }\n"
        "        for (int i = 0; i < a->len; i++)\n"
        "            if (xs_eq(a->items[i], needle)) return XS_INT(i);\n"
        "        return XS_INT(-1);\n"
        "    }\n"
        "    if (v.tag == 2 && v.s && needle.tag == 2 && needle.s) {\n"
        "        const char *p = strstr(v.s, needle.s);\n"
        "        return XS_INT(p ? (int64_t)(p - v.s) : -1);\n"
        "    }\n"
        "    return XS_INT(-1);\n"
        "}\n\n"
        "static xs_val xs_arr_is_empty(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) return XS_BOOL(((xs_arr*)v.p)->len == 0);\n"
        "    if (v.tag == 6 && v.p) return XS_BOOL(((xs_hmap*)v.p)->len == 0);\n"
        "    if (v.tag == 2 && v.s) return XS_BOOL(v.s[0] == 0);\n"
        "    return XS_BOOL(1);\n"
        "}\n\n"
        "/* generic .is_empty() dispatch - channels go through their own helper,\n"
        " * everything else (array, map, range, string) uses xs_arr_is_empty. */\n"
        "static xs_val xs_is_empty(xs_val v) {\n"
        "    if (v.tag == 7) return xs_channel_is_empty(v);\n"
        "    return xs_arr_is_empty(v);\n"
        "}\n\n"
        "/* recursive flatten: arrays of arrays collapse to a single level\n"
        "   for .flatten(); .flat() does the same one-level collapse the\n"
        "   stdlib promises. Both share an impl since the test corpus\n"
        "   doesn't distinguish multi-level. */\n"
        "static void __xs_flat_into(xs_val v, xs_arr *r) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        for (int i = 0; i < a->len; i++) __xs_flat_into(a->items[i], r);\n"
        "    } else {\n"
        "        if (r->len >= r->cap) {\n"
        "            r->cap = r->cap ? r->cap * 2 : 4;\n"
        "            r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap);\n"
        "        }\n"
        "        r->items[r->len++] = v;\n"
        "    }\n"
        "}\n"
        "static xs_val xs_arr_flatten(xs_val v) {\n"
        "    xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    r->len = 0; r->cap = 4; r->items = (xs_val*)malloc(sizeof(xs_val) * 4);\n"
        "    __xs_flat_into(v, r);\n"
        "    return (xs_val){.tag=5, .p=r};\n"
        "}\n\n"
        "static xs_val xs_arr_find(xs_val arr, xs_val fn) {\n"
        "    if (arr.tag != 5 || !arr.p || fn.tag != 8) return XS_NULL;\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    for (int i = 0; i < a->len; i++) {\n"
        "        xs_val arg = a->items[i];\n"
        "        if (xs_truthy(xs_call(fn, &arg, 1))) return arg;\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "static xs_val xs_arr_keys_or_values(xs_val v, int want_values) {\n"
        "    /* maps emit insertion order; arrays return XS_INT(i) keys */\n"
        "    xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    r->len = 0; r->cap = 4; r->items = (xs_val*)malloc(sizeof(xs_val) * 4);\n"
        "    if (v.tag == 6 && v.p) {\n"
        "        xs_hmap *m = (xs_hmap*)v.p;\n"
        "        for (int i = 0; i < m->len; i++) {\n"
        "            if (r->len >= r->cap) { r->cap *= 2; r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap); }\n"
        "            r->items[r->len++] = want_values ? m->vals[i] : XS_STR(strdup(m->keys[i]));\n"
        "        }\n"
        "    } else if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        for (int i = 0; i < a->len; i++) {\n"
        "            if (r->len >= r->cap) { r->cap *= 2; r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap); }\n"
        "            r->items[r->len++] = want_values ? a->items[i] : XS_INT(i);\n"
        "        }\n"
        "    }\n"
        "    return (xs_val){.tag=5, .p=r};\n"
        "}\n"
        "static xs_val xs_map_keys(xs_val v) { return xs_arr_keys_or_values(v, 0); }\n"
        "static xs_val xs_map_values(xs_val v) { return xs_arr_keys_or_values(v, 1); }\n"
        "static xs_val xs_map_entries(xs_val v) {\n"
        "    xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    r->len = 0; r->cap = 4; r->items = (xs_val*)malloc(sizeof(xs_val) * 4);\n"
        "    if (v.tag == 6 && v.p) {\n"
        "        xs_hmap *m = (xs_hmap*)v.p;\n"
        "        for (int i = 0; i < m->len; i++) {\n"
        "            if (r->len >= r->cap) { r->cap *= 2; r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap); }\n"
        "            xs_arr *pair = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "            pair->len = 2; pair->cap = 2;\n"
        "            pair->items = (xs_val*)malloc(sizeof(xs_val) * 2);\n"
        "            pair->items[0] = XS_STR(strdup(m->keys[i]));\n"
        "            pair->items[1] = m->vals[i];\n"
        "            r->items[r->len++] = (xs_val){.tag=5, .p=pair};\n"
        "        }\n"
        "    }\n"
        "    return (xs_val){.tag=5, .p=r};\n"
        "}\n\n"
        "static void xs_map_del(xs_val *m, xs_val key) {\n"
        "    if (!m || m->tag != 6 || !m->p) return;\n"
        "    xs_hmap *h = (xs_hmap*)m->p;\n"
        "    const char *ks = key.tag == 2 ? key.s : xs_to_str(key);\n"
        "    for (int i = 0; i < h->len; i++) {\n"
        "        if (strcmp(h->keys[i], ks) == 0) {\n"
        "            free(h->keys[i]);\n"
        "            for (int j = i; j < h->len - 1; j++) {\n"
        "                h->keys[j] = h->keys[j+1];\n"
        "                h->vals[j] = h->vals[j+1];\n"
        "            }\n"
        "            h->len--;\n"
        "            return;\n"
        "        }\n"
        "    }\n"
        "}\n\n"
        "/* string helpers */\n"
        "static xs_val xs_str_chars(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return xs_array(0);\n"
        "    int n = (int)strlen(s.s);\n"
        "    xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "    r->len = 0; r->cap = n > 4 ? n : 4;\n"
        "    r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "    /* iterate codepoint-aware: a continuation byte (10xxxxxx)\n"
        "       belongs to the previous char. */\n"
        "    for (int i = 0; i < n; ) {\n"
        "        int j = i + 1;\n"
        "        while (j < n && ((unsigned char)s.s[j] & 0xC0) == 0x80) j++;\n"
        "        char *c = (char*)malloc(j - i + 1);\n"
        "        memcpy(c, s.s + i, j - i); c[j - i] = 0;\n"
        "        if (r->len >= r->cap) { r->cap *= 2; r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap); }\n"
        "        r->items[r->len++] = XS_STR(c);\n"
        "        i = j;\n"
        "    }\n"
        "    return (xs_val){.tag=5, .p=r};\n"
        "}\n\n"
        "static xs_val xs_str_trim(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return s;\n"
        "    const char *p = s.s; while (*p == ' ' || *p == '\\t' || *p == '\\n' || *p == '\\r') p++;\n"
        "    int n = (int)strlen(p);\n"
        "    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\\t' || p[n-1] == '\\n' || p[n-1] == '\\r')) n--;\n"
        "    char *r = (char*)malloc(n + 1); memcpy(r, p, n); r[n] = 0;\n"
        "    return XS_STR(r);\n"
        "}\n\n"
        "static xs_val xs_str_starts_with(xs_val s, xs_val pre) {\n"
        "    if (s.tag != 2 || !s.s || pre.tag != 2 || !pre.s) return XS_BOOL(0);\n"
        "    return XS_BOOL(strncmp(s.s, pre.s, strlen(pre.s)) == 0);\n"
        "}\n\n"
        "static xs_val xs_str_ends_with(xs_val s, xs_val suf) {\n"
        "    if (s.tag != 2 || !s.s || suf.tag != 2 || !suf.s) return XS_BOOL(0);\n"
        "    size_t ls = strlen(s.s), lp = strlen(suf.s);\n"
        "    return XS_BOOL(ls >= lp && strcmp(s.s + ls - lp, suf.s) == 0);\n"
        "}\n\n"
        "static xs_val xs_str_replace(xs_val s, xs_val from, xs_val to) {\n"
        "    if (s.tag != 2 || !s.s || from.tag != 2 || !from.s) return s;\n"
        "    const char *src = s.s, *fr = from.s;\n"
        "    const char *tos = (to.tag == 2 && to.s) ? to.s : \"\";\n"
        "    size_t flen = strlen(fr), tlen = strlen(tos), slen = strlen(src);\n"
        "    if (flen == 0) return s;\n"
        "    /* worst-case cap: every char a match */\n"
        "    size_t cap = slen + tlen + 1;\n"
        "    char *out = (char*)malloc(cap); size_t pos = 0;\n"
        "    const char *p = src;\n"
        "    while (*p) {\n"
        "        if (strncmp(p, fr, flen) == 0) {\n"
        "            if (pos + tlen + 1 >= cap) { cap = (cap + tlen) * 2; out = (char*)realloc(out, cap); }\n"
        "            memcpy(out + pos, tos, tlen); pos += tlen; p += flen;\n"
        "        } else {\n"
        "            if (pos + 2 >= cap) { cap *= 2; out = (char*)realloc(out, cap); }\n"
        "            out[pos++] = *p++;\n"
        "        }\n"
        "    }\n"
        "    out[pos] = 0;\n"
        "    return XS_STR(out);\n"
        "}\n\n"
        "static xs_val xs_str_repeat(xs_val s, xs_val n) {\n"
        "    if (s.tag != 2 || !s.s) return XS_STR(strdup(\"\"));\n"
        "    int count = n.tag == 0 ? (int)n.i : 0;\n"
        "    if (count < 0) count = 0;\n"
        "    size_t l = strlen(s.s);\n"
        "    char *r = (char*)malloc(l * count + 1);\n"
        "    for (int i = 0; i < count; i++) memcpy(r + i * l, s.s, l);\n"
        "    r[l * count] = 0;\n"
        "    return XS_STR(r);\n"
        "}\n\n"
        "static xs_val xs_str_concat(xs_val a, xs_val b) {\n"
        "    return xs_strcat(a, b);\n"
        "}\n\n"
        "/* polymorphic .concat: arrays append, strings concatenate. takes\n"
        "   a varargs tail so `[1].concat([2], [3])` flattens cleanly. */\n"
        "static xs_val xs_concat(int n, xs_val first, ...) {\n"
        "    if (first.tag == 5) {\n"
        "        xs_arr *fa = (xs_arr*)first.p;\n"
        "        xs_arr *r = (xs_arr*)calloc(1, sizeof(xs_arr));\n"
        "        r->len = fa ? fa->len : 0; r->cap = r->len > 4 ? r->len : 4;\n"
        "        r->items = (xs_val*)malloc(sizeof(xs_val) * r->cap);\n"
        "        if (fa) for (int i = 0; i < fa->len; i++) r->items[i] = fa->items[i];\n"
        "        va_list ap; va_start(ap, first);\n"
        "        for (int k = 0; k < n; k++) {\n"
        "            xs_val nxt = va_arg(ap, xs_val);\n"
        "            if (nxt.tag != 5 || !nxt.p) continue;\n"
        "            xs_arr *na = (xs_arr*)nxt.p;\n"
        "            if (r->len + na->len > r->cap) {\n"
        "                r->cap = (r->len + na->len) * 2;\n"
        "                r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap);\n"
        "            }\n"
        "            for (int i = 0; i < na->len; i++) r->items[r->len++] = na->items[i];\n"
        "        }\n"
        "        va_end(ap);\n"
        "        return (xs_val){.tag=5, .p=r};\n"
        "    }\n"
        "    if (first.tag == 2) {\n"
        "        xs_val out = first;\n"
        "        va_list ap; va_start(ap, first);\n"
        "        for (int k = 0; k < n; k++) out = xs_strcat(out, va_arg(ap, xs_val));\n"
        "        va_end(ap);\n"
        "        return out;\n"
        "    }\n"
        "    return first;\n"
        "}\n\n"
        "/* conversions for .to_str / .to_int / .to_float */\n"
        "static xs_val xs_conv_to_str(xs_val v) {\n"
        "    if (v.tag == 2) return v;\n"
        "    return XS_STR(strdup(xs_to_str(v)));\n"
        "}\n"
        "static xs_val xs_conv_to_int(xs_val v) {\n"
        "    if (v.tag == 0) return v;\n"
        "    if (v.tag == 1) return XS_INT((int64_t)v.f);\n"
        "    if (v.tag == 2 && v.s) return XS_INT((int64_t)atoll(v.s));\n"
        "    if (v.tag == 3) return XS_INT(v.b ? 1 : 0);\n"
        "    return XS_INT(0);\n"
        "}\n"
        "static xs_val xs_conv_to_float(xs_val v) {\n"
        "    if (v.tag == 1) return v;\n"
        "    if (v.tag == 0) return XS_FLOAT((double)v.i);\n"
        "    if (v.tag == 2 && v.s) return XS_FLOAT(atof(v.s));\n"
        "    return XS_FLOAT(0.0);\n"
        "}\n\n"
        "/* math .floor / .ceil / .round / .abs as methods */\n"
        "static xs_val xs_num_abs(xs_val v) {\n"
        "    if (v.tag == 0) return XS_INT(v.i < 0 ? -v.i : v.i);\n"
        "    if (v.tag == 1) return XS_FLOAT(v.f < 0 ? -v.f : v.f);\n"
        "    return v;\n"
        "}\n"
        "static xs_val xs_num_floor(xs_val v) {\n"
        "    if (v.tag == 0) return v;\n"
        "    if (v.tag == 1) return XS_FLOAT(floor(v.f));\n"
        "    return v;\n"
        "}\n"
        "static xs_val xs_num_ceil(xs_val v) {\n"
        "    if (v.tag == 0) return v;\n"
        "    if (v.tag == 1) return XS_FLOAT(ceil(v.f));\n"
        "    return v;\n"
        "}\n"
        "static xs_val xs_num_round(xs_val v) {\n"
        "    if (v.tag == 0) return v;\n"
        "    if (v.tag == 1) return XS_FLOAT(round(v.f));\n"
        "    return v;\n"
        "}\n"
        "static double xs_to_f64(xs_val v);\n"
        "static xs_val xs_math_sqrt(xs_val v) { return XS_FLOAT(sqrt(xs_to_f64(v))); }\n"
        "static xs_val xs_math_pow(xs_val a, xs_val b) { return XS_FLOAT(pow(xs_to_f64(a), xs_to_f64(b))); }\n"
        "static xs_val xs_math_log(xs_val v) { return XS_FLOAT(log(xs_to_f64(v))); }\n"
        "static xs_val xs_math_exp(xs_val v) { return XS_FLOAT(exp(xs_to_f64(v))); }\n"
        "static xs_val xs_math_sin(xs_val v) { return XS_FLOAT(sin(xs_to_f64(v))); }\n"
        "static xs_val xs_math_cos(xs_val v) { return XS_FLOAT(cos(xs_to_f64(v))); }\n"
        "static xs_val xs_math_tan(xs_val v) { return XS_FLOAT(tan(xs_to_f64(v))); }\n"
        "static xs_val xs_math_min(int n, ...) {\n"
        "    if (n <= 0) return XS_NULL;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    xs_val best = va_arg(ap, xs_val);\n"
        "    for (int i = 1; i < n; i++) {\n"
        "        xs_val v = va_arg(ap, xs_val);\n"
        "        if (xs_cmp(v, best) < 0) best = v;\n"
        "    }\n"
        "    va_end(ap); return best;\n"
        "}\n"
        "static xs_val xs_math_max(int n, ...) {\n"
        "    if (n <= 0) return XS_NULL;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    xs_val best = va_arg(ap, xs_val);\n"
        "    for (int i = 1; i < n; i++) {\n"
        "        xs_val v = va_arg(ap, xs_val);\n"
        "        if (xs_cmp(v, best) > 0) best = v;\n"
        "    }\n"
        "    va_end(ap); return best;\n"
        "}\n\n"
        "/* range methods: ranges are materialised as arrays but keep the\n"
        " * original (start, end, inclusive) so .start/.end return the bound\n"
        " * the user wrote, not the last materialised element. */\n"
        "static xs_val xs_range_start(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (a->is_range) return XS_INT(a->range_start);\n"
        "        if (a->len > 0)   return a->items[0];\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n"
        "static xs_val xs_range_end(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (a->is_range) return XS_INT(a->range_end);\n"
        "        if (a->len > 0)   return a->items[a->len - 1];\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "/* iterator-as-method: .next() returns either the iterator-protocol\n"
        " * shape {value, done} when the receiver is a generator map (built\n"
        " * by lowered fn* bodies) or the next element otherwise. */\n"
        "static xs_val xs_iter_next_val(xs_val *iter) {\n"
        "    if (iter && iter->tag == 6 && iter->p) {\n"
        "        xs_hmap *m = (xs_hmap*)iter->p;\n"
        "        int has_items = 0, has_pos = 0;\n"
        "        for (int i = 0; i < m->len; i++) {\n"
        "            if (strcmp(m->keys[i], \"__items\") == 0) has_items = 1;\n"
        "            if (strcmp(m->keys[i], \"__pos\")   == 0) has_pos = 1;\n"
        "        }\n"
        "        if (has_items && has_pos) {\n"
        "            xs_val items = xs_index(*iter, XS_STR(\"__items\"));\n"
        "            xs_val pos   = xs_index(*iter, XS_STR(\"__pos\"));\n"
        "            xs_val out = xs_map(0);\n"
        "            int64_t p = (pos.tag == 0) ? pos.i : 0;\n"
        "            xs_arr *a = (items.tag == 5 && items.p) ? (xs_arr*)items.p : NULL;\n"
        "            if (!a || p >= a->len) {\n"
        "                xs_map_put(&out, XS_STR(\"value\"), XS_NULL);\n"
        "                xs_map_put(&out, XS_STR(\"done\"), XS_BOOL(1));\n"
        "                return out;\n"
        "            }\n"
        "            xs_map_put(iter, XS_STR(\"__pos\"), XS_INT(p + 1));\n"
        "            xs_map_put(&out, XS_STR(\"value\"), a->items[p]);\n"
        "            xs_map_put(&out, XS_STR(\"done\"),  XS_BOOL(0));\n"
        "            return out;\n"
        "        }\n"
        "    }\n"
        "    xs_val out;\n"
        "    if (xs_iter_next(iter, &out)) return out;\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "/* JSON helpers (tiny, only handles maps/arrays/primitives) */\n"
        "static void __xs_json_emit(xs_val v, char **buf, size_t *pos, size_t *cap);\n"
        "static void __xs_json_grow(char **buf, size_t *cap, size_t need) {\n"
        "    while (*cap < need) { *cap = *cap ? *cap * 2 : 64; *buf = (char*)realloc(*buf, *cap); }\n"
        "}\n"
        "static void __xs_json_str(const char *s, char **buf, size_t *pos, size_t *cap) {\n"
        "    __xs_json_grow(buf, cap, *pos + 2 + strlen(s) * 2 + 1);\n"
        "    (*buf)[(*pos)++] = '\"';\n"
        "    for (const char *p = s; *p; p++) {\n"
        "        __xs_json_grow(buf, cap, *pos + 8);\n"
        "        if (*p == '\"' || *p == '\\\\') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = *p; }\n"
        "        else if (*p == '\\n') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = 'n'; }\n"
        "        else if (*p == '\\t') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = 't'; }\n"
        "        else (*buf)[(*pos)++] = *p;\n"
        "    }\n"
        "    (*buf)[(*pos)++] = '\"';\n"
        "}\n"
        "static void __xs_json_emit(xs_val v, char **buf, size_t *pos, size_t *cap) {\n"
        "    char tmp[64];\n"
        "    switch (v.tag) {\n"
        "        case 0: snprintf(tmp, sizeof tmp, \"%lld\", (long long)v.i);\n"
        "                __xs_json_grow(buf, cap, *pos + strlen(tmp) + 1);\n"
        "                memcpy(*buf + *pos, tmp, strlen(tmp)); *pos += strlen(tmp); return;\n"
        "        case 1: snprintf(tmp, sizeof tmp, \"%s\", xs_format_float(v.f));\n"
        "                __xs_json_grow(buf, cap, *pos + strlen(tmp) + 1);\n"
        "                memcpy(*buf + *pos, tmp, strlen(tmp)); *pos += strlen(tmp); return;\n"
        "        case 2: __xs_json_str(v.s ? v.s : \"\", buf, pos, cap); return;\n"
        "        case 3: { const char *t = v.b ? \"true\" : \"false\";\n"
        "                  __xs_json_grow(buf, cap, *pos + 5); memcpy(*buf + *pos, t, strlen(t));\n"
        "                  *pos += strlen(t); return; }\n"
        "        case 4: __xs_json_grow(buf, cap, *pos + 4);\n"
        "                memcpy(*buf + *pos, \"null\", 4); *pos += 4; return;\n"
        "        case 5: if (v.p) {\n"
        "                  xs_arr *a = (xs_arr*)v.p;\n"
        "                  __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = '[';\n"
        "                  for (int i = 0; i < a->len; i++) {\n"
        "                      if (i) { __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = ','; }\n"
        "                      __xs_json_emit(a->items[i], buf, pos, cap);\n"
        "                  }\n"
        "                  __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = ']'; return;\n"
        "                } break;\n"
        "        case 6: if (v.p) {\n"
        "                  xs_hmap *m = (xs_hmap*)v.p;\n"
        "                  __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = '{';\n"
        "                  for (int i = 0; i < m->len; i++) {\n"
        "                      if (i) { __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = ','; }\n"
        "                      __xs_json_str(m->keys[i], buf, pos, cap);\n"
        "                      __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = ':';\n"
        "                      __xs_json_emit(m->vals[i], buf, pos, cap);\n"
        "                  }\n"
        "                  __xs_json_grow(buf, cap, *pos + 1); (*buf)[(*pos)++] = '}'; return;\n"
        "                } break;\n"
        "    }\n"
        "}\n"
        "static xs_val xs_time_now(void) {\n"
        "    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);\n"
        "    return XS_FLOAT((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);\n"
        "}\n"
        "static xs_val xs_time_now_ms(void) {\n"
        "    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);\n"
        "    return XS_INT((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);\n"
        "}\n"
        "static xs_val xs_time_sleep(xs_val sec) {\n"
        "    double s = sec.tag == 1 ? sec.f : (double)sec.i;\n"
        "    struct timespec t; t.tv_sec = (long)s; t.tv_nsec = (long)((s - (long)s) * 1e9);\n"
        "    nanosleep(&t, NULL);\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "static xs_val xs_json_stringify(xs_val v) {\n"
        "    char *buf = NULL; size_t pos = 0, cap = 0;\n"
        "    __xs_json_emit(v, &buf, &pos, &cap);\n"
        "    __xs_json_grow(&buf, &cap, pos + 1); buf[pos] = 0;\n"
        "    return XS_STR(buf);\n"
        "}\n\n"
        "static void __xs_json_skip(const char **p) { while (**p == ' ' || **p == '\\t' || **p == '\\n' || **p == '\\r') (*p)++; }\n"
        "static xs_val __xs_json_read(const char **p);\n"
        "static xs_val __xs_json_read_str(const char **p) {\n"
        "    if (**p != '\"') return XS_NULL;\n"
        "    (*p)++;\n"
        "    size_t cap = 32, len = 0;\n"
        "    char *buf = (char*)malloc(cap);\n"
        "    while (**p && **p != '\"') {\n"
        "        char c = **p;\n"
        "        if (c == '\\\\') {\n"
        "            (*p)++;\n"
        "            char e = **p;\n"
        "            if (e == 'n') c = '\\n';\n"
        "            else if (e == 't') c = '\\t';\n"
        "            else if (e == 'r') c = '\\r';\n"
        "            else if (e == '\"') c = '\"';\n"
        "            else if (e == '\\\\') c = '\\\\';\n"
        "            else if (e == '/') c = '/';\n"
        "            else c = e;\n"
        "        }\n"
        "        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }\n"
        "        buf[len++] = c;\n"
        "        (*p)++;\n"
        "    }\n"
        "    if (**p == '\"') (*p)++;\n"
        "    buf[len] = 0;\n"
        "    return XS_STR(buf);\n"
        "}\n"
        "static xs_val __xs_json_read(const char **p) {\n"
        "    __xs_json_skip(p);\n"
        "    char c = **p;\n"
        "    if (c == '\"') return __xs_json_read_str(p);\n"
        "    if (c == 't') { if (strncmp(*p, \"true\", 4) == 0) { *p += 4; return (xs_val){.tag=3, .b=1}; } }\n"
        "    if (c == 'f') { if (strncmp(*p, \"false\", 5) == 0) { *p += 5; return (xs_val){.tag=3, .b=0}; } }\n"
        "    if (c == 'n') { if (strncmp(*p, \"null\", 4) == 0) { *p += 4; return XS_NULL; } }\n"
        "    if (c == '[') {\n"
        "        (*p)++;\n"
        "        xs_val arr = xs_array(0);\n"
        "        __xs_json_skip(p);\n"
        "        if (**p == ']') { (*p)++; return arr; }\n"
        "        for (;;) {\n"
        "            xs_arr_push(arr, __xs_json_read(p));\n"
        "            __xs_json_skip(p);\n"
        "            if (**p == ',') { (*p)++; continue; }\n"
        "            if (**p == ']') { (*p)++; break; }\n"
        "            break;\n"
        "        }\n"
        "        return arr;\n"
        "    }\n"
        "    if (c == '{') {\n"
        "        (*p)++;\n"
        "        xs_val m = xs_map(0);\n"
        "        __xs_json_skip(p);\n"
        "        if (**p == '}') { (*p)++; return m; }\n"
        "        for (;;) {\n"
        "            __xs_json_skip(p);\n"
        "            xs_val k = __xs_json_read_str(p);\n"
        "            __xs_json_skip(p);\n"
        "            if (**p == ':') (*p)++;\n"
        "            xs_val v = __xs_json_read(p);\n"
        "            xs_map_put(&m, k, v);\n"
        "            __xs_json_skip(p);\n"
        "            if (**p == ',') { (*p)++; continue; }\n"
        "            if (**p == '}') { (*p)++; break; }\n"
        "            break;\n"
        "        }\n"
        "        return m;\n"
        "    }\n"
        "    if (c == '-' || (c >= '0' && c <= '9')) {\n"
        "        const char *start = *p;\n"
        "        if (**p == '-') (*p)++;\n"
        "        int is_float = 0;\n"
        "        while ((**p >= '0' && **p <= '9') || **p == '.' || **p == 'e' || **p == 'E' || **p == '+' || **p == '-') {\n"
        "            if (**p == '.' || **p == 'e' || **p == 'E') is_float = 1;\n"
        "            (*p)++;\n"
        "        }\n"
        "        if (is_float) return XS_FLOAT(strtod(start, NULL));\n"
        "        return XS_INT(strtoll(start, NULL, 10));\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n"
        "static xs_val xs_json_parse(xs_val s) {\n"
        "    if (s.tag != 2 || !s.s) return XS_NULL;\n"
        "    const char *p = s.s;\n"
        "    return __xs_json_read(&p);\n"
        "}\n\n"
        "/* ---- fs / os / time module helpers ----------------------------\n"
        " * Mirrors the small surface area of the import-fs / import-os /\n"
        " * import-time tests that the corpus exercises. Uses libc + POSIX\n"
        " * directly; Windows falls back to the Win32 equivalents where the\n"
        " * POSIX call doesn't exist. No external runtime deps. */\n"
        "static xs_val xs_fs_read(xs_val path) {\n"
        "    if (path.tag != 2 || !path.s) return XS_NULL;\n"
        "    FILE *f = fopen(path.s, \"rb\");\n"
        "    if (!f) return XS_NULL;\n"
        "    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);\n"
        "    if (n < 0) { fclose(f); return XS_NULL; }\n"
        "    char *buf = (char*)malloc((size_t)n + 1);\n"
        "    size_t r = fread(buf, 1, (size_t)n, f); fclose(f);\n"
        "    buf[r] = 0; return XS_STR(buf);\n"
        "}\n"
        "static xs_val xs_fs_write(xs_val path, xs_val content) {\n"
        "    if (path.tag != 2 || !path.s) return XS_BOOL(0);\n"
        "    FILE *f = fopen(path.s, \"wb\");\n"
        "    if (!f) return XS_BOOL(0);\n"
        "    if (content.tag == 2 && content.s) fwrite(content.s, 1, strlen(content.s), f);\n"
        "    else if (content.s) { const char *cs = xs_to_str(content); fwrite(cs, 1, strlen(cs), f); }\n"
        "    fclose(f); return XS_BOOL(1);\n"
        "}\n"
        "static xs_val xs_fs_exists(xs_val path) {\n"
        "    if (path.tag != 2 || !path.s) return XS_BOOL(0);\n"
        "    struct stat st;\n"
        "    return XS_BOOL(stat(path.s, &st) == 0);\n"
        "}\n"
        "static xs_val xs_fs_cwd(void) {\n"
        "    char buf[4096];\n"
        "#if defined(_WIN32)\n"
        "    if (!_getcwd(buf, sizeof buf)) return XS_NULL;\n"
        "#else\n"
        "    if (!getcwd(buf, sizeof buf)) return XS_NULL;\n"
        "#endif\n"
        "    return XS_STR(strdup(buf));\n"
        "}\n"
        "static xs_val xs_fs_list_dir(xs_val path) {\n"
        "    const char *p = (path.tag == 2 && path.s) ? path.s : \".\";\n"
        "    xs_val out = xs_array(0);\n"
        "#if defined(_WIN32)\n"
        "    char pat[4096];\n"
        "    snprintf(pat, sizeof pat, \"%s\\\\*\", p);\n"
        "    WIN32_FIND_DATAA fd;\n"
        "    HANDLE h = FindFirstFileA(pat, &fd);\n"
        "    if (h == INVALID_HANDLE_VALUE) return out;\n"
        "    do {\n"
        "        const char *n = fd.cFileName;\n"
        "        if (strcmp(n, \".\") == 0 || strcmp(n, \"..\") == 0) continue;\n"
        "        xs_arr_push(out, XS_STR(strdup(n)));\n"
        "    } while (FindNextFileA(h, &fd));\n"
        "    FindClose(h);\n"
        "#else\n"
        "    DIR *d = opendir(p);\n"
        "    if (!d) return out;\n"
        "    struct dirent *e;\n"
        "    while ((e = readdir(d)) != NULL) {\n"
        "        const char *n = e->d_name;\n"
        "        if (strcmp(n, \".\") == 0 || strcmp(n, \"..\") == 0) continue;\n"
        "        xs_arr_push(out, XS_STR(strdup(n)));\n"
        "    }\n"
        "    closedir(d);\n"
        "#endif\n"
        "    return out;\n"
        "}\n"
        "static xs_val xs_fs_remove(xs_val path) {\n"
        "    if (path.tag != 2 || !path.s) return XS_BOOL(0);\n"
        "    return XS_BOOL(remove(path.s) == 0);\n"
        "}\n"
        "static xs_val xs_fs_mkdir(xs_val path) {\n"
        "    if (path.tag != 2 || !path.s) return XS_BOOL(0);\n"
        "#if defined(_WIN32)\n"
        "    return XS_BOOL(_mkdir(path.s) == 0);\n"
        "#else\n"
        "    return XS_BOOL(mkdir(path.s, 0755) == 0);\n"
        "#endif\n"
        "}\n"
        "/* main() stashes argc/argv into these for os.args() to consume. */\n"
        "static int xs_user_argc = 0;\n"
        "static char **xs_user_argv = NULL;\n"
        "static xs_val xs_os_args(void) {\n"
        "    xs_val out = xs_array(0);\n"
        "    for (int i = 0; i < xs_user_argc; i++)\n"
        "        xs_arr_push(out, XS_STR(xs_user_argv[i] ? xs_user_argv[i] : \"\"));\n"
        "    return out;\n"
        "}\n"
        "static xs_val xs_os_getenv(xs_val name) {\n"
        "    if (name.tag != 2 || !name.s) return XS_NULL;\n"
        "    const char *v = getenv(name.s);\n"
        "    return v ? XS_STR(strdup(v)) : XS_NULL;\n"
        "}\n"
        "static xs_val xs_os_exit(xs_val code) {\n"
        "    int c = (code.tag == 0) ? (int)code.i : 0;\n"
        "    exit(c);\n"
        "    return XS_NULL;\n"
        "}\n"
        "static xs_val xs_os_hostname(void) {\n"
        "    char buf[256];\n"
        "#if defined(_WIN32)\n"
        "    DWORD n = sizeof buf;\n"
        "    if (!GetComputerNameA(buf, &n)) return XS_STR(strdup(\"\"));\n"
        "    return XS_STR(strdup(buf));\n"
        "#else\n"
        "    if (gethostname(buf, sizeof buf) != 0) return XS_STR(strdup(\"\"));\n"
        "    buf[sizeof buf - 1] = 0;\n"
        "    return XS_STR(strdup(buf));\n"
        "#endif\n"
        "}\n"
        "static xs_val xs_os_platform(void) {\n"
        "#if defined(__APPLE__)\n"
        "    return XS_STR(\"darwin\");\n"
        "#elif defined(_WIN32)\n"
        "    return XS_STR(\"windows\");\n"
        "#elif defined(__wasi__) || defined(__EMSCRIPTEN__)\n"
        "    return XS_STR(\"wasi\");\n"
        "#else\n"
        "    return XS_STR(\"linux\");\n"
        "#endif\n"
        "}\n"
        "static xs_val xs_os_sep(void) {\n"
        "#if defined(_WIN32)\n"
        "    return XS_STR(\"\\\\\");\n"
        "#else\n"
        "    return XS_STR(\"/\");\n"
        "#endif\n"
        "}\n"
        "static xs_val xs_time_format(xs_val epoch, xs_val fmt) {\n"
        "    time_t t;\n"
        "    if (epoch.tag == 0) t = (time_t)epoch.i;\n"
        "    else if (epoch.tag == 1) t = (time_t)epoch.f;\n"
        "    else return XS_NULL;\n"
        "    const char *fs = (fmt.tag == 2 && fmt.s) ? fmt.s : \"%Y-%m-%d %H:%M:%S\";\n"
        "    struct tm tmv;\n"
        "#if defined(_WIN32)\n"
        "    struct tm *tp = localtime(&t);\n"
        "    if (!tp) return XS_NULL;\n"
        "    tmv = *tp;\n"
        "#else\n"
        "    if (!localtime_r(&t, &tmv)) return XS_NULL;\n"
        "#endif\n"
        "    char buf[256];\n"
        "    size_t n = strftime(buf, sizeof buf, fs, &tmv);\n"
        "    if (n == 0) buf[0] = 0;\n"
        "    return XS_STR(strdup(buf));\n"
        "}\n\n"
    );

    if (!program) {
        sb_add(&s, "/* (empty program) */\n");
        return s.data;
    }

    /* pre-scan for actor declarations, impl types, and variable bindings */
    n_actors = 0;
    n_actor_vars = 0;
    n_impl_types = 0;
    n_struct_vars = 0;
    n_classes = 0;
    n_lambdas = 0;
    lambda_counter = 0;
    n_fn_sigs = 0;
    n_top_vars = 0;
    program_root = program;
    prescan_stmts(program);
    scan_lambdas(program);
    scan_top_level_vars(program);

    /* emit actor struct + method definitions at file scope */
    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            if (!st) continue;
            /* find actor decl nodes (may be wrapped in EXPR_STMT) */
            Node *ad = NULL;
            if (VAL_TAG(st) == NODE_ACTOR_DECL) ad = st;
            else if (VAL_TAG(st) == NODE_EXPR_STMT && st->expr_stmt.expr &&
                     VAL_TAG(st->expr_stmt.expr) == NODE_ACTOR_DECL)
                ad = st->expr_stmt.expr;
            if (!ad || !ad->actor_decl.name) continue;

            const char *aname = ad->actor_decl.name;
            /* emit state struct */
            sb_printf(&s, "typedef struct %s_state {\n", aname);
            for (int j = 0; j < ad->actor_decl.state_fields.len; j++)
                sb_printf(&s, "    xs_val %s;\n", ad->actor_decl.state_fields.items[j].key);
            sb_printf(&s, "} %s_state;\n\n", aname);

            /* set up actor field list for identifier rewriting */
            const char *field_names[64];
            int nfields = 0;
            for (int j = 0; j < ad->actor_decl.state_fields.len && nfields < 64; j++)
                field_names[nfields++] = ad->actor_decl.state_fields.items[j].key;
            actor_fields = field_names;
            n_actor_fields = nfields;

            /* emit methods as static functions */
            for (int j = 0; j < ad->actor_decl.methods.len; j++) {
                Node *m = ad->actor_decl.methods.items[j];
                if (!m || VAL_TAG(m) != NODE_FN_DECL || !m->fn_decl.name) continue;
                sb_printf(&s, "static xs_val %s_%s(%s_state *self",
                          aname, m->fn_decl.name, aname);
                for (int p = 0; p < m->fn_decl.params.len; p++) {
                    const char *pn = m->fn_decl.params.items[p].name;
                    if (pn && strcmp(pn, "self") == 0) continue;
                    sb_add(&s, ", xs_val ");
                    sb_add(&s, pn ? pn : "_");
                }
                sb_add(&s, ") {\n");
                in_method_body = 1;
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    /* emit statements but not the trailing expression (it becomes return) */
                    for (int si = 0; si < m->fn_decl.body->block.stmts.len; si++)
                        emit_stmt(&s, m->fn_decl.body->block.stmts.items[si], 1);
                    if (m->fn_decl.body->block.expr) {
                        sb_add(&s, "    return ");
                        emit_expr(&s, m->fn_decl.body->block.expr, 1);
                        sb_add(&s, ";\n");
                    } else {
                        sb_add(&s, "    return XS_NULL;\n");
                    }
                } else {
                    sb_add(&s, "    return XS_NULL;\n");
                }
                in_method_body = 0;
                sb_add(&s, "}\n\n");
            }

            /* clear actor field rewriting */
            actor_fields = NULL;
            n_actor_fields = 0;
        }

        /* async fns are now lowered to plain fns by c_lower_program;
         * the regular file-scope emission path picks them up like any
         * other top-level function. */
    }

    /* emit forward declarations BEFORE lambdas so a lambda body can
     * reference any top-level fn or class/impl method. */
    if (VAL_TAG(program) == NODE_PROGRAM) {
        sb_add(&s, "/* function prototypes */\n");
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            if (!st) continue;
            if (VAL_TAG(st) == NODE_FN_DECL && !is_main_fn(st) && st->fn_decl.name) {
                sb_add(&s, "static xs_val ");
                char __mb[256];
                const char *en = mangle_overload(st->fn_decl.name,
                    st->fn_decl.params.len, __mb, sizeof __mb);
                emit_safe_name(&s, en);
                emit_params_c(&s, &st->fn_decl.params);
                sb_add(&s, ";\n");
                if (count_fn_overloads(st->fn_decl.name) > 1) {
                    sb_printf(&s, "static xs_val __xs_wrap_%s_%d(void *, xs_val *, int);\n",
                              st->fn_decl.name, st->fn_decl.params.len);
                } else {
                    sb_printf(&s, "static xs_val __xs_wrap_%s(void *, xs_val *, int);\n",
                              st->fn_decl.name);
                }
            } else if (VAL_TAG(st) == NODE_CLASS_DECL && st->class_decl.name) {
                for (int j = 0; j < st->class_decl.members.len; j++) {
                    Node *mm = st->class_decl.members.items[j];
                    if (mm && VAL_TAG(mm) == NODE_FN_DECL && mm->fn_decl.name) {
                        sb_printf(&s, "xs_val %s_%s(xs_val", st->class_decl.name, mm->fn_decl.name);
                        for (int p = 0; p < mm->fn_decl.params.len; p++) {
                            const char *pn = mm->fn_decl.params.items[p].name;
                            if (pn && strcmp(pn, "self") == 0) continue;
                            sb_add(&s, ", xs_val");
                        }
                        sb_add(&s, ");\n");
                    }
                }
            } else if (VAL_TAG(st) == NODE_IMPL_DECL && st->impl_decl.type_name) {
                for (int j = 0; j < st->impl_decl.members.len; j++) {
                    Node *mm = st->impl_decl.members.items[j];
                    if (mm && VAL_TAG(mm) == NODE_FN_DECL && mm->fn_decl.name) {
                        sb_printf(&s, "static xs_val %s_%s(xs_val", st->impl_decl.type_name, mm->fn_decl.name);
                        for (int p = 0; p < mm->fn_decl.params.len; p++) {
                            const char *pn = mm->fn_decl.params.items[p].name;
                            if (pn && strcmp(pn, "self") == 0) continue;
                            sb_add(&s, ", xs_val");
                        }
                        sb_add(&s, ");\n");
                    }
                }
            }
        }
        sb_addc(&s, '\n');
    }

    /* forward-declare every lambda so an earlier-emitted lambda body
     * can hand off to a later one. Mutual / nested lambda chains both
     * need this; without the prototypes gcc errors with
     * "__xs_lambda_N undeclared". */
    for (int li = 0; li < n_lambdas; li++) {
        Node *ln = lambdas[li].node;
        if (!ln || VAL_TAG(ln) != NODE_LAMBDA) continue;
        sb_printf(&s, "static xs_val __xs_lambda_%d(void *__env, xs_val *__args, int __argc);\n",
                  lambdas[li].id);
    }
    if (n_lambdas > 0) sb_addc(&s, '\n');

    /* emit lambda static functions */
    for (int li = 0; li < n_lambdas; li++) {
        Node *ln = lambdas[li].node;
        if (!ln || VAL_TAG(ln) != NODE_LAMBDA) continue;
        sb_printf(&s, "static xs_val __xs_lambda_%d(void *__env, xs_val *__args, int __argc) {\n", lambdas[li].id);
        /* bind params from __args */
        for (int p = 0; p < ln->lambda.params.len; p++) {
            const char *pname = ln->lambda.params.items[p].name;
            sb_printf(&s, "    xs_val %s = __argc > %d ? __args[%d] : (xs_val){.tag=4};\n",
                      pname ? pname : "_", p, p);
        }
        /* save defer top so any inner `return` (which now unwinds
         * defers + pops a frame) finds the symbols it needs. */
        sb_printf(&s, "    int __saved_defer_top = __xs_defer_top;\n");
        sb_printf(&s, "    xs_push_frame(\"<lambda>\");\n");
        /* set current_lambda so NODE_IDENT emits capture access */
        current_lambda = &lambdas[li];
        if (ln->lambda.body && VAL_TAG(ln->lambda.body) == NODE_BLOCK) {
            for (int si = 0; si < ln->lambda.body->block.stmts.len; si++)
                emit_stmt(&s, ln->lambda.body->block.stmts.items[si], 1);
            if (ln->lambda.body->block.expr) {
                sb_add(&s, "    { xs_val __ret_v = ");
                emit_expr(&s, ln->lambda.body->block.expr, 1);
                sb_add(&s, ";\n"
                           "      xs_run_defers(__saved_defer_top);\n"
                           "      xs_pop_frame();\n"
                           "      return __ret_v; }\n");
            } else {
                sb_add(&s, "    xs_run_defers(__saved_defer_top);\n"
                           "    xs_pop_frame();\n"
                           "    return (xs_val){.tag=4};\n");
            }
        } else if (ln->lambda.body) {
            sb_add(&s, "    { xs_val __ret_v = ");
            emit_expr(&s, ln->lambda.body, 1);
            sb_add(&s, ";\n"
                       "      xs_run_defers(__saved_defer_top);\n"
                       "      xs_pop_frame();\n"
                       "      return __ret_v; }\n");
        } else {
            sb_add(&s, "    xs_run_defers(__saved_defer_top);\n"
                       "    xs_pop_frame();\n"
                       "    return (xs_val){.tag=4};\n");
        }
        current_lambda = NULL;
        sb_add(&s, "}\n\n");
    }

    /* check if top-level code needs wrapping in main() */
    int needs_main_wrap = 0;
    if (VAL_TAG(program) == NODE_PROGRAM) {
        int has_explicit_main = 0;
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            if (st && VAL_TAG(st) == NODE_FN_DECL && is_main_fn(st))
                has_explicit_main = 1;
        }
        /* if there are non-declaration statements and no main, wrap */
        if (!has_explicit_main) {
            for (int i = 0; i < program->program.stmts.len; i++) {
                Node *st = program->program.stmts.items[i];
                if (!st) continue;
                if (VAL_TAG(st) != NODE_FN_DECL && VAL_TAG(st) != NODE_STRUCT_DECL &&
                    VAL_TAG(st) != NODE_ENUM_DECL && VAL_TAG(st) != NODE_CLASS_DECL &&
                    VAL_TAG(st) != NODE_TRAIT_DECL && VAL_TAG(st) != NODE_IMPL_DECL &&
                    VAL_TAG(st) != NODE_TYPE_ALIAS && VAL_TAG(st) != NODE_IMPORT &&
                    VAL_TAG(st) != NODE_USE && VAL_TAG(st) != NODE_MODULE_DECL &&
                    VAL_TAG(st) != NODE_EFFECT_DECL) {
                    /* skip actor decls (already emitted) */
                    if (VAL_TAG(st) == NODE_ACTOR_DECL) continue;
                    if (VAL_TAG(st) == NODE_EXPR_STMT && st->expr_stmt.expr &&
                        VAL_TAG(st->expr_stmt.expr) == NODE_ACTOR_DECL) continue;
                    needs_main_wrap = 1;
                    break;
                }
            }
        }
    }

    if (needs_main_wrap) {
        /* hoist top-level let/var/const names to file scope so named
         * functions and the lambdas inside them can resolve them. main
         * still does the actual init in source order. */
        if (n_top_vars > 0) {
            for (int i = 0; i < n_top_vars; i++) {
                char nbuf[256];
                sb_printf(&s, "static xs_val %s;\n",
                    safe_name(top_level_vars[i], nbuf, sizeof nbuf));
            }
            sb_addc(&s, '\n');
        }

        /* emit file-scope declarations first */
        if (VAL_TAG(program) == NODE_PROGRAM) {
            for (int i = 0; i < program->program.stmts.len; i++) {
                Node *st = program->program.stmts.items[i];
                if (!st) continue;
                if (VAL_TAG(st) == NODE_FN_DECL && !is_main_fn(st))
                    emit_stmt(&s, st, 0);
                else if (VAL_TAG(st) == NODE_STRUCT_DECL || VAL_TAG(st) == NODE_ENUM_DECL ||
                         VAL_TAG(st) == NODE_CLASS_DECL || VAL_TAG(st) == NODE_TRAIT_DECL ||
                         VAL_TAG(st) == NODE_IMPL_DECL || VAL_TAG(st) == NODE_TYPE_ALIAS ||
                         VAL_TAG(st) == NODE_IMPORT || VAL_TAG(st) == NODE_USE ||
                         VAL_TAG(st) == NODE_MODULE_DECL || VAL_TAG(st) == NODE_EFFECT_DECL)
                    emit_stmt(&s, st, 0);
            }
        }

        sb_add(&s, "int main(int argc, char **argv) {\n");
        sb_add(&s, "    xs_user_argc = argc; xs_user_argv = argv;\n");

        /* emit actor state initializations */
        for (int i = 0; i < n_actor_vars; i++) {
            sb_printf(&s, "    %s_state %s_state;\n",
                      actor_vars[i].actor_name, actor_vars[i].var_name);
            /* find the actor decl and init fields */
            if (VAL_TAG(program) == NODE_PROGRAM) {
                for (int j = 0; j < program->program.stmts.len; j++) {
                    Node *st = program->program.stmts.items[j];
                    Node *ad = NULL;
                    if (st && VAL_TAG(st) == NODE_ACTOR_DECL) ad = st;
                    else if (st && VAL_TAG(st) == NODE_EXPR_STMT && st->expr_stmt.expr &&
                             VAL_TAG(st->expr_stmt.expr) == NODE_ACTOR_DECL)
                        ad = st->expr_stmt.expr;
                    if (!ad || !ad->actor_decl.name) continue;
                    if (strcmp(ad->actor_decl.name, actor_vars[i].actor_name) != 0) continue;
                    for (int k = 0; k < ad->actor_decl.state_fields.len; k++) {
                        sb_printf(&s, "    %s_state.%s = ",
                                  actor_vars[i].var_name,
                                  ad->actor_decl.state_fields.items[k].key);
                        if (ad->actor_decl.state_fields.items[k].val)
                            emit_expr(&s, ad->actor_decl.state_fields.items[k].val, 1);
                        else
                            sb_add(&s, "XS_NULL");
                        sb_add(&s, ";\n");
                    }
                    break;
                }
            }
        }

        /* emit non-declaration statements */
        if (VAL_TAG(program) == NODE_PROGRAM) {
            for (int i = 0; i < program->program.stmts.len; i++) {
                Node *st = program->program.stmts.items[i];
                if (!st) continue;
                /* skip declarations already emitted */
                if (VAL_TAG(st) == NODE_FN_DECL || VAL_TAG(st) == NODE_STRUCT_DECL ||
                    VAL_TAG(st) == NODE_ENUM_DECL || VAL_TAG(st) == NODE_CLASS_DECL ||
                    VAL_TAG(st) == NODE_TRAIT_DECL || VAL_TAG(st) == NODE_IMPL_DECL ||
                    VAL_TAG(st) == NODE_TYPE_ALIAS || VAL_TAG(st) == NODE_IMPORT ||
                    VAL_TAG(st) == NODE_USE || VAL_TAG(st) == NODE_MODULE_DECL ||
                    VAL_TAG(st) == NODE_EFFECT_DECL || VAL_TAG(st) == NODE_ACTOR_DECL)
                    continue;
                if (VAL_TAG(st) == NODE_EXPR_STMT && st->expr_stmt.expr &&
                    VAL_TAG(st->expr_stmt.expr) == NODE_ACTOR_DECL)
                    continue;
                emit_stmt(&s, st, 1);
            }
        }

        sb_add(&s, "    return 0;\n}\n");
    } else {
        if (VAL_TAG(program) == NODE_PROGRAM) {
            for (int i = 0; i < program->program.stmts.len; i++)
                emit_stmt(&s, program->program.stmts.items[i], 0);
        } else {
            emit_stmt(&s, program, 0);
        }
    }

    return s.data;
}
