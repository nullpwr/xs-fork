#include "transpiler/c_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "core/strbuf.h"

/* forward declarations */
static void emit_expr(SB *s, Node *n, int depth);
static void emit_stmt(SB *s, Node *n, int depth);
static void emit_block_body(SB *s, Node *block, int depth);
static void emit_pattern_cond(SB *s, Node *pat, const char *subject, int depth);
static void emit_pattern_bindings(SB *s, Node *pat, const char *subject, int depth);

/* track if we've seen a main function */
static int seen_main = 0;
/* track defers for goto-based cleanup */
static int defer_label_counter = 0;

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
    "exit","_Exit","abort","atexit","getenv","system","rand","srand",
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
static struct { const char *type_name; } impl_types[MAX_IMPL_TYPES];
static int n_impl_types = 0;
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
static struct { const char *name; int n_params; } fn_sigs[MAX_FN_SIGS];
static int n_fn_sigs = 0;
static void register_fn_sig(const char *name, int n_params) {
    if (n_fn_sigs < MAX_FN_SIGS) {
        fn_sigs[n_fn_sigs].name = name;
        fn_sigs[n_fn_sigs].n_params = n_params;
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
        if (VAL_TAG(st) == NODE_IMPL_DECL && st->impl_decl.type_name)
            register_impl_type(st->impl_decl.type_name);
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
            register_fn_sig(st->fn_decl.name, st->fn_decl.params.len);
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
            if (p->name) sb_add(s, p->name);
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
    case NODE_LIT_BIGINT:
        sb_printf(s, "XS_INT(%s)", n->lit_bigint.bigint_str);
        break;
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
        /* Emit as xs_array_new + pushes */
        sb_printf(s, "xs_array(%d", n->lit_array.elems.len);
        for (int i = 0; i < n->lit_array.elems.len; i++) {
            sb_add(s, ", ");
            emit_expr(s, n->lit_array.elems.items[i], depth);
        }
        sb_addc(s, ')');
        break;
    }
    case NODE_LIT_MAP: {
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
    case NODE_IDENT:
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
        } else
            emit_safe_name(s, n->ident.name);
        break;
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
            sb_add(s, "xs_strcat(");
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
        } else {
            /* check if callee might be a closure (variable holding fn) */
            int might_be_closure = 0;
            if (n->call.callee && VAL_TAG(n->call.callee) == NODE_INDEX) {
                /* e.g. counter["inc"](): indexing into map, likely returns closure */
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
                if (class_has_init(cname)) {
                    sb_add(s, "({ xs_val __ci = xs_map(0); ");
                    sb_printf(s, "%s_init(__ci", cname);
                    for (int i = 0; i < n->call.args.len; i++) {
                        sb_add(s, ", ");
                        emit_expr(s, n->call.args.items[i], depth);
                    }
                    sb_add(s, "); __ci; })");
                } else {
                    sb_add(s, "xs_map(0)");
                }
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
                if (!dispatched_overload) emit_expr(s, n->call.callee, depth);
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
                strcmp(on, "fmt") == 0)
                mod_name = on;
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
            sb_add(s, "xs_channel_is_empty(");
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
            sb_printf(s, "xs_val __t = xs_array(2, XS_INT(__i), __a_%d->items[__i]); xs_arr_push(__r_%d, __t);\n", mid, mid);
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
        } else if (strcmp(meth, "sort") == 0) {
            sb_add(s, "xs_arr_sort(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
        } else if (strcmp(meth, "has") == 0) {
            sb_add(s, "xs_map_has(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
        } else if (strcmp(meth, "get") == 0) {
            sb_add(s, "xs_index(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
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
        } else if (strcmp(meth, "size") == 0 || strcmp(meth, "length") == 0 ||
                   strcmp(meth, "count") == 0) {
            sb_add(s, "xs_len(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
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
        } else if (strcmp(meth, "find") == 0) {
            sb_add(s, "xs_arr_find(");
            emit_expr(s, n->method_call.obj, depth);
            sb_add(s, ", ");
            if (n->method_call.args.len > 0) emit_expr(s, n->method_call.args.items[0], depth);
            else sb_add(s, "XS_NULL");
            sb_addc(s, ')');
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
        } else if (strcmp(meth, "trim") == 0) {
            sb_add(s, "xs_str_trim(");
            emit_expr(s, n->method_call.obj, depth);
            sb_addc(s, ')');
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
                /* reduce uses (fn, init); fold uses (init, fn). Pick
                   the callable slot for fn and the other for init -- the
                   VM/interp do the same dynamic dispatch, so the C
                   shape stays consistent regardless of which name was
                   used. With only one arg, that arg is the function. */
                int is_fold = strcmp(meth, "fold") == 0;
                int fn_arg  = is_fold ? 1 : 0;
                int init_arg = is_fold ? 0 : 1;
                sb_printf(s, "({ xs_val __acc_%d = ", mid);
                if (n->method_call.args.len > init_arg)
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
            if (n->method_call.obj && VAL_TAG(n->method_call.obj) == NODE_IDENT)
                stype = lookup_struct_var(n->method_call.obj->ident.name);
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
            } else {
                /* generic: function-style call */
                sb_add(s, meth);
                sb_addc(s, '(');
                emit_expr(s, n->method_call.obj, depth);
                for (int i = 0; i < n->method_call.args.len; i++) {
                    sb_add(s, ", ");
                    emit_expr(s, n->method_call.args.items[i], depth);
                }
                sb_addc(s, ')');
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
            sb_add(s, "xs_map_put(&");
            emit_expr(s, n->assign.target->index.obj, depth);
            sb_add(s, ", ");
            emit_expr(s, n->assign.target->index.index, depth);
            sb_add(s, ", ");
            emit_expr(s, n->assign.value, depth);
            sb_addc(s, ')');
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
                if (is_top_level_var(cap))
                    sb_printf(s, "__cenv_%d[%d] = &%s;\n", lid, ci, cap);
                else
                    sb_printf(s, "__cenv_%d[%d] = __box_%s;\n", lid, ci, cap);
            }
            sb_indent(s, depth);
            sb_printf(s, "xs_fn_new(__xs_lambda_%d, __cenv_%d); })", lid, lid);
        } else {
            sb_printf(s, "xs_fn_new(__xs_lambda_%d, NULL)", lid);
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
            /* emit struct init as map */
            sb_printf(s, "xs_map(%d", n->struct_init.fields.len);
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
        /* if as expression: ternary */
        sb_add(s, "(xs_truthy(");
        emit_expr(s, n->if_expr.cond, depth);
        sb_add(s, ") ? ");
        if (n->if_expr.then && VAL_TAG(n->if_expr.then) == NODE_BLOCK && n->if_expr.then->block.expr) {
            emit_expr(s, n->if_expr.then->block.expr, depth);
        } else {
            sb_add(s, "XS_NULL");
        }
        sb_add(s, " : ");
        if (n->if_expr.else_branch && VAL_TAG(n->if_expr.else_branch) == NODE_BLOCK
            && n->if_expr.else_branch->block.expr) {
            emit_expr(s, n->if_expr.else_branch->block.expr, depth);
        } else {
            sb_add(s, "XS_NULL");
        }
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
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_LOOP:
    case NODE_TRY:
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
        sb_printf(s, "(%s.tag == 5", subject); /* tag 5 = array/tuple */
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
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            if (pat->pat_struct.fields.items[i].val) {
                char sub[256];
                snprintf(sub, sizeof sub, "%s.%s", subject, pat->pat_struct.fields.items[i].key);
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
        sb_printf(s, "(%s.tag == 5", subject);
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
            if (pat->pat_struct.fields.items[i].val) {
                char sub[256];
                snprintf(sub, sizeof sub, "%s.%s", subject, pat->pat_struct.fields.items[i].key);
                emit_pattern_bindings(s, pat->pat_struct.fields.items[i].val, sub, depth);
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
        if (is_main_fn(n)) {
            seen_main = 1;
            sb_indent(s, depth);
            sb_add(s, "int main(int argc, char **argv) {\n");
            sb_indent(s, depth + 1);
            sb_add(s, "(void)argc; (void)argv;\n");
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
                    sb_indent(s, depth + 1);
                    sb_add(s, "xs_val __retval = XS_NULL;\n");
                    /* emit non-defer statements */
                    for (int i = 0; i < n->fn_decl.body->block.stmts.len; i++) {
                        Node *st = n->fn_decl.body->block.stmts.items[i];
                        if (st && VAL_TAG(st) != NODE_DEFER)
                            emit_stmt(s, st, depth + 1);
                    }
                    if (n->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "__retval = ");
                        emit_expr(s, n->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                    sb_indent(s, depth + 1);
                    sb_add(s, "goto __cleanup;\n");
                    sb_indent(s, depth);
                    sb_add(s, "__cleanup:\n");
                    emit_deferred_cleanup(s, n->fn_decl.body, depth + 1);
                    sb_indent(s, depth + 1);
                    sb_add(s, "xs_run_defers(__saved_defer_top);\n");
                    sb_indent(s, depth + 1);
                    sb_add(s, "xs_pop_frame();\n");
                    sb_indent(s, depth + 1);
                    sb_add(s, "return __retval;\n");
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
        }
        break;
    }
    case NODE_RETURN:
        sb_indent(s, depth);
        sb_add(s, "return ");
        if (n->ret.value)
            emit_expr(s, n->ret.value, depth);
        else
            sb_add(s, "XS_NULL");
        sb_add(s, ";\n");
        break;
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
                if (m->fn_decl.body && VAL_TAG(m->fn_decl.body) == NODE_BLOCK) {
                    emit_block_body(s, m->fn_decl.body, depth + 1);
                    if (m->fn_decl.body->block.expr) {
                        sb_indent(s, depth + 1);
                        sb_add(s, "return ");
                        emit_expr(s, m->fn_decl.body->block.expr, depth + 1);
                        sb_add(s, ";\n");
                    }
                }
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
char *transpile_c(Node *program, const char *filename) {
    SB s;
    sb_init(&s);
    seen_main = 0;
    defer_label_counter = 0;

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
        "#include <setjmp.h>\n\n"
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
        "typedef struct { xs_val *items; int len; int cap; } xs_arr;\n"
        "typedef struct { char **keys; xs_val *vals; int len; int cap; } xs_hmap;\n\n"
        "static const char *xs_to_str(xs_val v);\n\n"
        "static int xs_eq(xs_val a, xs_val b) {\n"
        "    if (a.tag != b.tag) return 0;\n"
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
        "        default: return 0;\n"
        "    }\n"
        "}\n\n"
        "static int xs_cmp(xs_val a, xs_val b) {\n"
        "    if (a.tag == 0 && b.tag == 0) return (a.i > b.i) - (a.i < b.i);\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return (af > bf) - (af < bf);\n"
        "    }\n"
        "    if (a.tag == 2 && b.tag == 2) return strcmp(a.s ? a.s : \"\", b.s ? b.s : \"\");\n"
        "    return 0;\n"
        "}\n\n"
        "static xs_val xs_add(xs_val a, xs_val b) {\n"
        "    if (a.tag == 0 && b.tag == 0) return XS_INT(a.i + b.i);\n"
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
        "        xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af - bf);\n"
        "    }\n"
        "    return XS_INT(a.i - b.i);\n"
        "}\n\n"
        "static xs_val xs_mul(xs_val a, xs_val b) {\n"
        "    if (a.tag == 1 || b.tag == 1) {\n"
        "        double af = a.tag == 1 ? a.f : (double)a.i;\n"
        "        double bf = b.tag == 1 ? b.f : (double)b.i;\n"
        "        return XS_FLOAT(af * bf);\n"
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
        "    /* int ** non-negative-int stays int (matches VM); any\n"
        "       other combo promotes to float. */\n"
        "    if (a.tag == 0 && b.tag == 0 && b.i >= 0) {\n"
        "        int64_t r = 1, base = a.i, exp = b.i;\n"
        "        while (exp > 0) { if (exp & 1) r *= base; base *= base; exp >>= 1; }\n"
        "        return XS_INT(r);\n"
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
        "        case 5: if (v.p) {\n"
        "            xs_arr *a = (xs_arr*)v.p;\n"
        "            int pos = 0;\n"
        "            pos += snprintf(buf + pos, 4096 - pos, \"[\");\n"
        "            for (int i = 0; i < a->len && pos < 4096 - 32; i++) {\n"
        "                if (i) pos += snprintf(buf + pos, 4096 - pos, \", \");\n"
        "                pos += snprintf(buf + pos, 4096 - pos, \"%s\", xs_to_str(a->items[i]));\n"
        "            }\n"
        "            snprintf(buf + pos, 4096 - pos, \"]\");\n"
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
        "    const char       *eff_name;\n"
        "    int               n_arms;\n"
        "    const char       *arm_op_names[16];\n"
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
        "        if (strcmp(f->eff_name, eff) == 0) {\n"
        "            for (int i = 0; i < f->n_arms; i++) {\n"
        "                if (strcmp(f->arm_op_names[i], op) == 0) {\n"
        "                    XsEffFrame *prev_active = __xs_eff_active_perform;\n"
        "                    __xs_eff_active_perform = f;\n"
        "                    if (setjmp(f->resume_jmp) == 0) {\n"
        "                        xs_val r = f->dispatch(i, arg);\n"
        "                        __xs_eff_active_perform = prev_active;\n"
        "                        f->exit_value = r;\n"
        "                        longjmp(f->exit_jmp, 1);\n"
        "                        return (xs_val){.tag=4}; /* unreachable */\n"
        "                    } else {\n"
        "                        __xs_eff_active_perform = prev_active;\n"
        "                        return f->resume_value;\n"
        "                    }\n"
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
        "    xs_arr *a = (xs_arr*)malloc(sizeof(xs_arr));\n"
        "    a->cap = n > 4 ? n : 4;\n"
        "    a->items = (xs_val*)malloc(sizeof(xs_val) * a->cap);\n"
        "    a->len = n;\n"
        "    va_list ap; va_start(ap, n);\n"
        "    for (int i = 0; i < n; i++) a->items[i] = va_arg(ap, xs_val);\n"
        "    va_end(ap);\n"
        "    return (xs_val){.tag=5, .p=a};\n"
        "}\n\n"
        "static void xs_arr_push(xs_val arr, xs_val v) {\n"
        "    if (arr.tag != 5 || !arr.p) return;\n"
        "    xs_arr *a = (xs_arr*)arr.p;\n"
        "    if (a->len >= a->cap) {\n"
        "        a->cap = a->cap * 2;\n"
        "        a->items = (xs_val*)realloc(a->items, sizeof(xs_val) * a->cap);\n"
        "    }\n"
        "    a->items[a->len++] = v;\n"
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
        "        xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    int64_t e = end.tag == 0 ? end.i : 0;\n"
        "    if (inclusive) e += 1;\n"
        "    int64_t step = s <= e ? 1 : -1;\n"
        "    int64_t count = (e - s) * step;\n"
        "    if (count < 0) count = 0;\n"
        "    xs_arr *a = (xs_arr*)malloc(sizeof(xs_arr));\n"
        "    a->cap = (int)(count > 4 ? count : 4);\n"
        "    a->items = (xs_val*)malloc(sizeof(xs_val) * a->cap);\n"
        "    a->len = 0;\n"
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
        "        xs_arr *state = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    xs_arr *ch = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "/* assert runtime */\n"
        "static void xs_assert_eq(xs_val a, xs_val b) {\n"
        "    if (!xs_eq(a, b)) {\n"
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
        "typedef struct { xs_val (*fn)(void*, xs_val*, int); void *env; } xs_fn_t;\n\n"
        "static xs_val xs_fn_new(xs_val (*fn)(void*, xs_val*, int), void *env) {\n"
        "    xs_fn_t *f = (xs_fn_t*)malloc(sizeof(xs_fn_t));\n"
        "    f->fn = fn; f->env = env;\n"
        "    return (xs_val){.tag=8, .p=f};\n"
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
        "    const char *d = (delim.tag == 2 && delim.s) ? delim.s : \" \";\n"
        "    xs_val result = xs_array(0);\n"
        "    char *copy = strdup(s.s);\n"
        "    char *tok = strtok(copy, d);\n"
        "    while (tok) { xs_arr_push(result, XS_STR(strdup(tok))); tok = strtok(NULL, d); }\n"
        "    free(copy);\n"
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
        "    xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "static xs_val xs_arr_contains(xs_val v, xs_val needle) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
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
        "    xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
        "    r->len = 0; r->cap = 4; r->items = (xs_val*)malloc(sizeof(xs_val) * 4);\n"
        "    if (v.tag == 6 && v.p) {\n"
        "        xs_hmap *m = (xs_hmap*)v.p;\n"
        "        for (int i = 0; i < m->len; i++) {\n"
        "            if (r->len >= r->cap) { r->cap *= 2; r->items = (xs_val*)realloc(r->items, sizeof(xs_val) * r->cap); }\n"
        "            xs_arr *pair = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "    xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "        xs_arr *r = (xs_arr*)malloc(sizeof(xs_arr));\n"
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
        "/* range methods: a range is just an array, so start/end peek */\n"
        "static xs_val xs_range_start(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (a->len > 0) return a->items[0];\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n"
        "static xs_val xs_range_end(xs_val v) {\n"
        "    if (v.tag == 5 && v.p) {\n"
        "        xs_arr *a = (xs_arr*)v.p;\n"
        "        if (a->len > 0) return a->items[a->len - 1];\n"
        "    }\n"
        "    return XS_NULL;\n"
        "}\n\n"
        "/* iterator-as-method: .next() returns the next element or null. */\n"
        "static xs_val xs_iter_next_val(xs_val *iter) {\n"
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
                    sb_add(&s, ", xs_val ");
                    sb_add(&s, m->fn_decl.params.items[p].name ?
                           m->fn_decl.params.items[p].name : "_");
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

        /* emit async functions at file scope */
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            if (!st || VAL_TAG(st) != NODE_FN_DECL) continue;
            if (is_main_fn(st)) continue;
            if (st->fn_decl.is_async) {
                /* async fn -> regular function at file scope */
                sb_add(&s, "static xs_val ");
                sb_add(&s, st->fn_decl.name);
                emit_params_c(&s, &st->fn_decl.params);
                sb_add(&s, " {\n");
                if (st->fn_decl.body && VAL_TAG(st->fn_decl.body) == NODE_BLOCK) {
                    emit_block_body(&s, st->fn_decl.body, 1);
                    if (st->fn_decl.body->block.expr) {
                        sb_add(&s, "    return ");
                        emit_expr(&s, st->fn_decl.body->block.expr, 1);
                        sb_add(&s, ";\n");
                    }
                }
                sb_add(&s, "    return XS_NULL;\n}\n\n");
            }
        }
    }

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
        /* set current_lambda so NODE_IDENT emits capture access */
        current_lambda = &lambdas[li];
        if (ln->lambda.body && VAL_TAG(ln->lambda.body) == NODE_BLOCK) {
            for (int si = 0; si < ln->lambda.body->block.stmts.len; si++)
                emit_stmt(&s, ln->lambda.body->block.stmts.items[si], 1);
            if (ln->lambda.body->block.expr) {
                sb_add(&s, "    return ");
                emit_expr(&s, ln->lambda.body->block.expr, 1);
                sb_add(&s, ";\n");
            } else {
                sb_add(&s, "    return (xs_val){.tag=4};\n");
            }
        } else if (ln->lambda.body) {
            sb_add(&s, "    return ");
            emit_expr(&s, ln->lambda.body, 1);
            sb_add(&s, ";\n");
        } else {
            sb_add(&s, "    return (xs_val){.tag=4};\n");
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

    /* emit forward declarations for pub functions (header-like) */
    if (VAL_TAG(program) == NODE_PROGRAM) {
        int has_pub = 0;
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *st = program->program.stmts.items[i];
            if (st && VAL_TAG(st) == NODE_FN_DECL && st->fn_decl.is_pub && !is_main_fn(st)) {
                if (!has_pub) {
                    sb_add(&s, "/* exported function prototypes */\n");
                    has_pub = 1;
                }
                sb_add(&s, "xs_val ");
                emit_safe_name(&s, st->fn_decl.name);
                emit_params_c(&s, &st->fn_decl.params);
                sb_add(&s, ";\n");
            }
        }
        if (has_pub) sb_addc(&s, '\n');
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
                if (VAL_TAG(st) == NODE_FN_DECL && !is_main_fn(st) && !st->fn_decl.is_async)
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
        sb_add(&s, "    (void)argc; (void)argv;\n");

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
