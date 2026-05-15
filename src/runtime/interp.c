#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/error.h"
#include "runtime/builtins.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/xs_bigint.h"
#include "core/xs_utils.h"
#include "core/gc.h"
#include "core/limits.h"
#include "plugins/pipeline.h"
#ifdef XSC_ENABLE_TRACER
#include "tracer/tracer.h"
#endif
/* use bundled regex engine everywhere for consistent cross-platform behavior */
#ifndef XS_REGEX_H
#include "core/xs_regex.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "core/utf8.h"
#include "optimizer/inline_cache.h"
#include "runtime/async.h"
#include "runtime/concurrent.h"
#include "runtime/triggers.h"
#ifdef XSC_ENABLE_VM
#include "vm/vm.h"
#endif
#include <time.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <sys/stat.h>

Interp *g_current_interp = NULL;

/* reactive bind dependency tracking */
static char **g_dep_track_names = NULL;
static int g_dep_track_len = 0, g_dep_track_cap = 0;
static int g_dep_tracking = 0;

static void dep_track_add(const char *name) {
    if (!g_dep_tracking) return;
    /* avoid duplicates */
    for (int i = 0; i < g_dep_track_len; i++)
        if (strcmp(g_dep_track_names[i], name) == 0) return;
    if (g_dep_track_len >= g_dep_track_cap) {
        g_dep_track_cap = g_dep_track_cap ? g_dep_track_cap * 2 : 8;
        g_dep_track_names = xs_realloc(g_dep_track_names,
            g_dep_track_cap * sizeof(char*));
    }
    g_dep_track_names[g_dep_track_len++] = xs_strdup(name);
}

#ifdef XSC_ENABLE_TRACER
#define TRACE_CALL(i, name, line) \
    do { if ((i)->tracer) tracer_record_call((XSTracer*)(i)->tracer, name, line); } while(0)
#define TRACE_RETURN(i, name, val) \
    do { if ((i)->tracer) tracer_record_return((XSTracer*)(i)->tracer, name, val); } while(0)
#define TRACE_STORE(i, name, val) \
    do { if ((i)->tracer) tracer_record_store((XSTracer*)(i)->tracer, name, val); } while(0)
#define TRACE_PROVENANCE(i, var, origin, detail, line) \
    do { if ((i)->tracer) tracer_record_provenance((XSTracer*)(i)->tracer, var, origin, detail, line); } while(0)

static void trace_provenance_for_node(Interp *interp, const char *var, Node *expr) {
    if (!interp->tracer || !expr) return;
    const char *origin = "unknown";
    const char *detail = "";
    switch (VAL_TAG(expr)) {
    case NODE_LIT_INT: case NODE_LIT_FLOAT: case NODE_LIT_STRING: case NODE_LIT_BOOL:
        origin = "literal"; break;
    case NODE_CALL:
        origin = "fn_return";
        if (expr->call.callee && VAL_TAG(expr->call.callee) == NODE_IDENT)
            detail = expr->call.callee->ident.name;
        break;
    case NODE_BINOP:
        origin = "binop";
        detail = expr->binop.op;
        break;
    case NODE_IDENT:
        origin = "variable";
        detail = expr->ident.name;
        break;
    case NODE_METHOD_CALL:
        origin = "method";
        detail = expr->method_call.method ? expr->method_call.method : "";
        break;
    case NODE_UNARY:
        origin = "unary";
        detail = expr->unary.op;
        break;
    default: break;
    }
    tracer_record_provenance((XSTracer*)interp->tracer, var, origin, detail, expr->span.line);
}
#else
#define TRACE_CALL(i, name, line)   ((void)0)
#define TRACE_RETURN(i, name, val)  ((void)0)
#define TRACE_STORE(i, name, val)   ((void)0)
#define TRACE_PROVENANCE(i, var, origin, detail, line) ((void)0)
#endif

/* forward decl for plugin system */
Value *call_value(Interp *i, Value *callee, Value **args, int argc,
                  const char *call_site);
static Value *native_cancel(Interp *interp, Value **args, int argc);
static Value *native_emit_runtime_hook(Interp *interp, Value **args, int argc);
static Value *native_default_handler(Interp *interp, Value **args, int argc);
static Value *native_tracer_active(Interp *interp, Value **args, int argc);
static Value *native_tracer_write_prov(Interp *interp, Value **args, int argc);
Value *node_to_xs_map(Node *n);
int node_tag_from_string(const char *s);

/* sema override chain state (for default() chaining) */
static SemaPluginRule **g_sema_chain = NULL;
static int g_sema_chain_len = 0;
static int g_sema_chain_pos = 0;
static Interp *g_sema_chain_interp = NULL;

/* forced plugin id for disambiguation syntax (defined in parser.c) */

/* plugin system globals */

#define MAX_PLUGIN_METHODS 256
typedef struct {
    char  *type_name;
    char  *method_name;
    Value *fn;
} PluginMethod;

static PluginMethod g_plugin_methods[MAX_PLUGIN_METHODS];
static int g_plugin_method_count = 0;

#define MAX_TEARDOWN_FNS 64
static Value *g_teardown_fns[MAX_TEARDOWN_FNS];
static int g_teardown_count = 0;

#define MAX_LOADED_XS_PLUGINS 64
typedef struct {
    char *name;
    char *version;
    char *source;    /* kept alive so AST nodes remain valid */
    char *filepath;  /* kept alive for span references */
    Node *program;   /* kept alive so function bodies remain valid */
} LoadedXSPlugin;
static LoadedXSPlugin g_xs_plugins[MAX_LOADED_XS_PLUGINS];
static int g_xs_plugin_count = 0;

static void plugin_register_method(const char *type_name, const char *method_name, Value *fn) {
    if (g_plugin_method_count >= MAX_PLUGIN_METHODS) {
        fprintf(stderr, "xs: plugin method limit reached (%d), ignoring %s.%s\n",
                MAX_PLUGIN_METHODS, type_name, method_name);
        return;
    }
    PluginMethod *pm = &g_plugin_methods[g_plugin_method_count++];
    pm->type_name = xs_strdup(type_name);
    pm->method_name = xs_strdup(method_name);
    pm->fn = value_incref(fn);
}

static Value *plugin_lookup_method(const char *type_name, const char *method_name) {
    for (int j = 0; j < g_plugin_method_count; j++) {
        if (strcmp(g_plugin_methods[j].type_name, type_name) == 0 &&
            strcmp(g_plugin_methods[j].method_name, method_name) == 0) {
            return g_plugin_methods[j].fn;
        }
    }
    return NULL;
}

static void plugin_run_teardowns(void) {
    for (int j = 0; j < g_teardown_count; j++) {
        if (g_teardown_fns[j] && g_current_interp) {
            Value *r = call_value(g_current_interp, g_teardown_fns[j], NULL, 0, "teardown");
            if (r) value_decref(r);
        }
    }
}

static void plugin_register_loaded(const char *name, const char *version) {
    if (g_xs_plugin_count >= MAX_LOADED_XS_PLUGINS) {
        fprintf(stderr, "xs: plugin limit reached (%d)\n", MAX_LOADED_XS_PLUGINS);
        return;
    }
    g_xs_plugins[g_xs_plugin_count].name = xs_strdup(name);
    g_xs_plugins[g_xs_plugin_count].version = version ? xs_strdup(version) : NULL;
    g_xs_plugin_count++;
}

static int plugin_is_loaded(const char *name) {
    for (int j = 0; j < g_xs_plugin_count; j++) {
        if (g_xs_plugins[j].name && strcmp(g_xs_plugins[j].name, name) == 0)
            return 1;
    }
    return 0;
}

/* phase 2: eval hooks */

typedef struct {
    Value *callback;
    int    tag_filter; /* -1 = all tags, otherwise specific NodeTag */
} EvalHook;

static EvalHook g_before_eval[64];
static int g_n_before_eval = 0;
static EvalHook g_after_eval[64];
static int g_n_after_eval = 0;
static int g_has_eval_hooks = 0;
static int g_in_eval_hook = 0; /* recursion guard */

/* phase 2: syntax extension globals */

static Value *g_syntax_handlers[16];
static char  *g_syntax_handler_targets[16]; /* target production name for extend handlers */
static int g_n_syntax_handlers = 0;
static Value *g_syntax_expr_handlers[16];
static int g_n_syntax_expr_handlers = 0;
static Value *g_postfix_handlers[16];
static int g_n_postfix_handlers = 0;

/* plugin keyword registry for lexer */
#define MAX_PLUGIN_KEYWORDS 64
static char *g_plugin_keywords[MAX_PLUGIN_KEYWORDS];
static int g_n_plugin_keywords = 0;

/* phase 3: parser override registry */
typedef struct {
    char keyword[64];
    Value *callback;
    Value *previous;
} ParserOverride;
static ParserOverride g_parser_overrides[32];
static int g_n_parser_overrides = 0;

/* parser production registry (pipeline parser { production NAME ... }) */
typedef struct {
    char keyword[64];
    char plugin_id[64];
    Value *callback;
} ParserProduction;
static ParserProduction g_parser_productions[32];
static int g_n_parser_productions = 0;

/* phase 3: lexer transform registry */
static Value *g_lexer_transforms[16];
static int g_n_lexer_transforms = 0;

/* phase 3: resolve_import hook chain */
static Value *g_resolve_import_hooks[16];
static int g_n_resolve_import = 0;

/* phase 3: on_error hook chain */
static Value *g_on_error_hooks[16];
static int g_n_on_error = 0;

/* phase 3: sandbox flags for current plugin being loaded */
#define SANDBOX_INJECT_ONLY  1
#define SANDBOX_NO_OVERRIDE  2
#define SANDBOX_NO_EVAL_HOOK 4
static int g_current_sandbox_flags = 0;

/* global interp pointer for use by parser-invoked plugin callbacks */
static Interp *g_plugin_interp = NULL;

/* forward decls for parser access from plugin callbacks */
struct Parser; /* already defined in parser.h but we need it here */

/* forward decls for phase 2 functions used in interp_eval */
/* AST constructors for plugin use: defined in interp_ast.c */
Value *node_to_xs_map(Node *n);
Value *native_ast_after(Interp *, Value **, int);
Value *native_ast_angle(Interp *, Value **, int);
Value *native_ast_array(Interp *, Value **, int);
Value *native_ast_assign(Interp *, Value **, int);
Value *native_ast_binop(Interp *, Value **, int);
Value *native_ast_block(Interp *, Value **, int);
Value *native_ast_bool_node(Interp *, Value **, int);
Value *native_ast_call(Interp *, Value **, int);
Value *native_ast_color(Interp *, Value **, int);
Value *native_ast_date(Interp *, Value **, int);
Value *native_ast_debounce(Interp *, Value **, int);
Value *native_ast_duration(Interp *, Value **, int);
Value *native_ast_every(Interp *, Value **, int);
Value *native_ast_float_node(Interp *, Value **, int);
Value *native_ast_fn_decl(Interp *, Value **, int);
Value *native_ast_for_loop(Interp *, Value **, int);
Value *native_ast_ident(Interp *, Value **, int);
Value *native_ast_if_else(Interp *, Value **, int);
Value *native_ast_if_expr(Interp *, Value **, int);
Value *native_ast_int_node(Interp *, Value **, int);
Value *native_ast_lambda(Interp *, Value **, int);
Value *native_ast_let_decl(Interp *, Value **, int);
Value *native_ast_map_node(Interp *, Value **, int);
Value *native_ast_method_call(Interp *, Value **, int);
Value *native_ast_null_node(Interp *, Value **, int);
Value *native_ast_return_node(Interp *, Value **, int);
Value *native_ast_size(Interp *, Value **, int);
Value *native_ast_str_node(Interp *, Value **, int);
Value *native_ast_timeout(Interp *, Value **, int);
Value *native_ast_unary(Interp *, Value **, int);
Value *native_ast_var_decl(Interp *, Value **, int);
Value *native_ast_while_loop(Interp *, Value **, int);
Node *node_from_xs_map(Value *map);
const char *node_tag_to_string(NodeTag tag);
static int plugin_is_keyword_impl(const char *word);
static Node *plugin_try_syntax_handler_impl(Parser *p, Token *tok);
static Node *plugin_try_syntax_expr_handler_impl(Parser *p, Token *tok);
static Node *plugin_try_parser_override_impl(Parser *p, const char *keyword);
/* phase 3 forward decls */
static Value *make_hook_handle(int idx, const char *type);

/* end plugin globals */
Value *make_enum_ctor_native(const char *type_name, const char *variant_name);
/* derive(...) trait impls live in interp_derive.c */

#define CF_CLEAR(i)  do { (i)->cf.signal=0; value_decref((i)->cf.value); (i)->cf.value=NULL; free((i)->cf.label); (i)->cf.label=NULL; } while(0)

static void hoist_functions(Interp *i, NodeList *stmts);
static Value *EVAL(Interp *i, Node *n) {
    if (!n || i->cf.signal) return value_incref(XS_NULL_VAL);
    return interp_eval(i, n);
}

/* Walk a fn_decl's decorator list, evaluate each arg in the current
   env, and append one entry per decorator to the trigger registry.
   @once is captured as a flag on whichever real trigger sits next to
   it instead of becoming its own entry. @export(name) additionally
   binds the fn into globals under the alias so other modules can
   resolve it by the public name. */
static void register_fn_decl_triggers(Interp *i, Node *stmt, Value *fn_v) {
    int nd = stmt->fn_decl.n_decorators;
    if (nd == 0) return;
    int has_once = 0;
    for (int k = 0; k < nd; k++)
        if (strcmp(stmt->fn_decl.decorators[k].name, "once") == 0) has_once = 1;
    for (int k = 0; k < nd; k++) {
        Decorator *d = &stmt->fn_decl.decorators[k];
        if (strcmp(d->name, "once") == 0) continue;
        Value **args = d->n_args ? xs_calloc(d->n_args, sizeof(Value*)) : NULL;
        for (int a = 0; a < d->n_args; a++)
            args[a] = EVAL(i, d->args[a]);
        trigger_registry_register(d->name, args, d->n_args, fn_v, has_once);
        if (strcmp(d->name, "export") == 0 && d->n_args >= 1 &&
            args[0] && VAL_TAG(args[0]) == XS_STR && args[0]->s) {
            env_define(i->globals, args[0]->s, fn_v, 1);
        }
    }
}

static void push_env(Interp *i) {
    Env *child = env_new(i->env);
    i->env = child;
}

static void pop_env(Interp *i) {
    Env *old = i->env;
    i->env = old->parent ? env_incref(old->parent) : NULL;
    env_decref(old);
}

static const char *find_similar_name(Env *env, const char *name) {
    const char *best = NULL;
    int best_dist = 3;
    for (Env *e = env; e != NULL; e = e->parent) {
        for (int j = 0; j < e->len; j++) {
            if (!e->bindings[j].name) continue;
            int d = xs_edit_distance(name, e->bindings[j].name);
            if (d > 0 && d < best_dist) {
                best_dist = d;
                best = e->bindings[j].name;
            }
        }
    }
    return best;
}

/* runtime type-annotation checking lives in interp_typecheck.c */

Interp *interp_new(const char *filename) {
    value_init_singletons();
    Interp *i = xs_calloc(1, sizeof(Interp));
    i->globals   = env_new(NULL);
    i->env       = env_incref(i->globals);
    /* Lowered from 1000: at ~700 C frames the default 8 MB stack starts to be
       cramped by try/catch + diagnostic paths, which could segfault before this
       guard fired. The interp recurses on the C stack, so the
       absolute ceiling is the OS thread stack (default 8 MB on
       Linux, ASan inflates it heavily). 500 is the sanitiser-safe
       number; release builds tolerate higher but not by enough to
       advertise. Bump XS_MAX_DEPTH if you need more on a non-ASan
       build. */
    i->max_depth = 500;
    const char *md = getenv("XS_MAX_DEPTH");
    if (md) {
        int v = atoi(md);
        if (v > 0) i->max_depth = v;
    }
    i->filename  = filename ? filename : "<stdin>";
    i->call_stack     = NULL;
    i->call_stack_len = 0;
    i->call_stack_cap = 0;
    i->diag           = NULL;
    i->n_tasks        = 0;
    i->source         = NULL;
    i->needs_reparse  = 0;
    i->pipeline       = pipeline_new();
    /* phase 2: register plugin parser bridge functions */
    g_plugin_is_keyword = plugin_is_keyword_impl;
    g_plugin_try_syntax_handler = plugin_try_syntax_handler_impl;
    g_plugin_try_syntax_expr_handler = plugin_try_syntax_expr_handler_impl;
    /* phase 3: parser override bridge */
    g_plugin_try_parser_override = plugin_try_parser_override_impl;
    ic_init();
    stdlib_register(i);
    /* plugin pipeline builtins */
    interp_define_native(i, "cancel", native_cancel);
    interp_define_native(i, "emit_runtime_hook", native_emit_runtime_hook);
    interp_define_native(i, "default", native_default_handler);
    interp_define_native(i, "__tracer_active", native_tracer_active);
    interp_define_native(i, "__tracer_write_prov", native_tracer_write_prov);
    return i;
}

void interp_free(Interp *i) {
    if (!i) return;
    /* run plugin teardown callbacks only for the main interpreter */
    if (i == g_current_interp) {
        plugin_run_teardowns();
        /* release eval hooks the main interpreter registered so the next
           interpreter (e.g. the next test file under `xs test`) does not
           fire callbacks that close over the freed env. */
        for (int _h = 0; _h < g_n_before_eval; _h++) {
            if (g_before_eval[_h].callback) value_decref(g_before_eval[_h].callback);
            g_before_eval[_h].callback = NULL;
        }
        g_n_before_eval = 0;
        for (int _h = 0; _h < g_n_after_eval; _h++) {
            if (g_after_eval[_h].callback) value_decref(g_after_eval[_h].callback);
            g_after_eval[_h].callback = NULL;
        }
        g_n_after_eval = 0;
        g_has_eval_hooks = 0;
        /* drop the process-wide async runtime too: any promise callbacks
           it holds reference closures from this interpreter's env. */
        async_shutdown();
    }
    CF_CLEAR(i);
    env_decref(i->env);
    env_decref(i->globals);
    free(i->defers.items);
    while (i->effect_stack) {
        EffectFrame *frame = i->effect_stack;
        i->effect_stack = frame->prev;
        free(frame->effect_name);
        free(frame->op_name);
        env_decref(frame->handler_env);
        if (frame->handle_body_env) env_decref(frame->handle_body_env);
        free(frame);
    }
    if (i->resume_value) value_decref(i->resume_value);
    if (i->last_expr_value) value_decref(i->last_expr_value);
    for (int t = 0; t < i->n_tasks; t++) {
        if (i->task_queue[t].fn)     value_decref(i->task_queue[t].fn);
        if (i->task_queue[t].result) value_decref(i->task_queue[t].result);
    }
    i->n_tasks = 0;
    free(i->call_stack);
    if (i->pipeline) pipeline_free(i->pipeline);
    free(i);
}

void interp_define_native(Interp *i, const char *name, NativeFn fn) {
    Value *v = xs_native(fn);
    env_define(i->globals, name, v, 1);
    value_decref(v);
}

static Value *eval_binop(Interp *i, Node *n);
Value *call_value(Interp *i, Value *callee, Value **args, int argc,
                          const char *call_site);
static int match_pattern(Interp *i, Node *pat, Value *val, Env *env);
static void hoist_functions(Interp *i, NodeList *stmts);

/* Walk the arm body and count NODE_RESUME calls, capping at 2 since
   that's all the caller cares about ("more than one resume? then use
   multi-shot"). Don't recurse into nested handles or function
   definitions -- those scopes own their own resumes. The count is a
   structural upper bound (a resume in one branch of an `if` still
   counts even if only one branch fires); single-resume arms always
   get the legacy single-shot path which is cheaper and matches what
   existing tests rely on. */
static int count_arm_resumes(Node *n, int cap) {
    if (!n) return 0;
    NodeTag t = (NodeTag)VAL_TAG(n);
    if (t == NODE_RESUME) return 1;
    if (t == NODE_HANDLE)  return 0;
    if (t == NODE_FN_DECL) return 0;
    if (t == NODE_LAMBDA)  return 0;
    int total = 0;
#define ADD(child) do { \
    total += count_arm_resumes((child), cap); \
    if (total >= cap) return total; \
} while (0)
    switch (t) {
    case NODE_BLOCK:
        for (int j = 0; j < n->block.stmts.len; j++) ADD(n->block.stmts.items[j]);
        if (n->block.expr) ADD(n->block.expr);
        break;
    case NODE_IF:
        ADD(n->if_expr.cond);
        ADD(n->if_expr.then);
        for (int j = 0; j < n->if_expr.elif_conds.len; j++) {
            ADD(n->if_expr.elif_conds.items[j]);
            ADD(n->if_expr.elif_thens.items[j]);
        }
        if (n->if_expr.else_branch) ADD(n->if_expr.else_branch);
        break;
    case NODE_WHILE:
        ADD(n->while_loop.cond);
        ADD(n->while_loop.body);
        break;
    case NODE_FOR:
        ADD(n->for_loop.iter);
        ADD(n->for_loop.body);
        break;
    case NODE_LOOP:
        ADD(n->loop.body);
        break;
    case NODE_LET:
    case NODE_VAR:
    case NODE_CONST:
        if (n->let.value) ADD(n->let.value);
        break;
    case NODE_CALL:
        ADD(n->call.callee);
        for (int j = 0; j < n->call.args.len; j++) ADD(n->call.args.items[j]);
        break;
    case NODE_METHOD_CALL:
        ADD(n->method_call.obj);
        for (int j = 0; j < n->method_call.args.len; j++) ADD(n->method_call.args.items[j]);
        break;
    case NODE_BINOP:
        ADD(n->binop.left);
        ADD(n->binop.right);
        break;
    case NODE_UNARY:
        ADD(n->unary.expr);
        break;
    case NODE_ASSIGN:
        ADD(n->assign.target);
        ADD(n->assign.value);
        break;
    case NODE_RETURN:
        if (n->ret.value) ADD(n->ret.value);
        break;
    case NODE_BREAK:
        if (n->brk.value) ADD(n->brk.value);
        break;
    case NODE_THROW:
        if (n->throw_.value) ADD(n->throw_.value);
        break;
    case NODE_EXPR_STMT:
        ADD(n->expr_stmt.expr);
        break;
    case NODE_MATCH:
        ADD(n->match.subject);
        for (int j = 0; j < n->match.arms.len; j++) {
            if (n->match.arms.items[j].guard) ADD(n->match.arms.items[j].guard);
            ADD(n->match.arms.items[j].body);
        }
        break;
    case NODE_TRY:
        ADD(n->try_.body);
        for (int j = 0; j < n->try_.catch_arms.len; j++)
            ADD(n->try_.catch_arms.items[j].body);
        if (n->try_.finally_block) ADD(n->try_.finally_block);
        break;
    case NODE_INDEX:
        ADD(n->index.obj);
        ADD(n->index.index);
        break;
    case NODE_FIELD:
        ADD(n->field.obj);
        break;
    case NODE_PERFORM:
        for (int j = 0; j < n->perform.args.len; j++) ADD(n->perform.args.items[j]);
        break;
    default:
        break;
    }
#undef ADD
    return total;
}

static int bind_pattern(Interp *i, Node *pat, Value *val, Env *env, int mutable) {
    if (!pat) return 1;
    switch (VAL_TAG(pat)) {
    case NODE_PAT_WILD: return 1;
    case NODE_PAT_IDENT:
        env_define(env, pat->pat_ident.name, val,
                   pat->pat_ident.mutable || mutable);
        return 1;
    case NODE_PAT_LIT: return 1;
    case NODE_PAT_TUPLE: {
        if (VAL_TAG(val) != XS_ARRAY && VAL_TAG(val) != XS_TUPLE) return 0;
        XSArray *arr = val->arr;
        if (pat->pat_tuple.elems.len != arr->len) return 0;
        for (int j = 0; j < pat->pat_tuple.elems.len; j++) {
            if (!bind_pattern(i, pat->pat_tuple.elems.items[j],
                              arr->items[j], env, mutable)) return 0;
        }
        return 1;
    }
    case NODE_PAT_STRUCT: {
        XSMap *m = NULL;
        if (VAL_TAG(val) == XS_MAP) m = val->map;
        else if (VAL_TAG(val) == XS_STRUCT_VAL) m = val->st->fields;
        else if (VAL_TAG(val) == XS_ENUM_VAL) m = val->en->map_data;
        else if (VAL_TAG(val) == XS_INST) m = val->inst->fields;
        if (!m) return 0;
        for (int j = 0; j < pat->pat_struct.fields.len; j++) {
            char *fname = pat->pat_struct.fields.items[j].key;
            Node *sub   = pat->pat_struct.fields.items[j].val;
            Value *fv   = map_get(m, fname);
            if ((!fv || VAL_TAG(fv) == XS_NULL) &&
                j < pat->pat_struct.defaults.len &&
                pat->pat_struct.defaults.items[j]) {
                fv = EVAL(i, pat->pat_struct.defaults.items[j]);
                if (sub) {
                    int ok = bind_pattern(i, sub, fv, env, mutable);
                    value_decref(fv);
                    if (!ok) return 0;
                } else {
                    env_define(env, fname, fv, mutable);
                    value_decref(fv);
                }
                continue;
            }
            if (!fv) fv = XS_NULL_VAL;
            if (sub) { if (!bind_pattern(i, sub, fv, env, mutable)) return 0; }
            else env_define(env, fname, fv, mutable);
        }
        return 1;
    }
    case NODE_PAT_CAPTURE: {
        if (!bind_pattern(i, pat->pat_capture.pattern, val, env, mutable)) return 0;
        env_define(env, pat->pat_capture.name, val, mutable);
        return 1;
    }
    case NODE_PAT_ENUM: {
        if (VAL_TAG(val) != XS_ENUM_VAL && VAL_TAG(val) != XS_ARRAY) return 0;
        XSArray *arr = NULL;
        if (VAL_TAG(val) == XS_ENUM_VAL && val->en->arr_data) arr = val->en->arr_data;
        else if (VAL_TAG(val) == XS_ARRAY) arr = val->arr;
        if (!arr) {
                return (pat->pat_enum.args.len == 0);
        }
        if (pat->pat_enum.args.len != arr->len) return 0;
        for (int j = 0; j < pat->pat_enum.args.len; j++) {
            if (!bind_pattern(i, pat->pat_enum.args.items[j],
                              arr->items[j], env, mutable)) return 0;
        }
        return 1;
    }
    case NODE_PAT_SLICE: {
        if (VAL_TAG(val) != XS_ARRAY && VAL_TAG(val) != XS_TUPLE) return 0;
        XSArray *arr = val->arr;
        int nfixed = pat->pat_slice.elems.len;
        if (!pat->pat_slice.rest && arr->len != nfixed) return 0;
        if (pat->pat_slice.rest  && arr->len < nfixed)  return 0;
        for (int j = 0; j < nfixed; j++) {
            if (!bind_pattern(i, pat->pat_slice.elems.items[j],
                              arr->items[j], env, mutable)) return 0;
        }
        if (pat->pat_slice.rest) {
            Value *rest_arr = xs_array_new();
            for (int j = nfixed; j < arr->len; j++)
                array_push(rest_arr->arr, value_incref(arr->items[j]));
            env_define(env, pat->pat_slice.rest, rest_arr, 1);
            value_decref(rest_arr);
        }
        return 1;
    }
    case NODE_PAT_MAP: {
        /* Accept any map-shaped value. Closed patterns (no `..`) require the
           set of keys to match exactly; open patterns allow extra keys. */
        XSMap *m = NULL;
        if (VAL_TAG(val) == XS_MAP || VAL_TAG(val) == XS_MODULE) m = val->map;
        else if (VAL_TAG(val) == XS_STRUCT_VAL) m = val->st->fields;
        else if (VAL_TAG(val) == XS_INST) m = val->inst->fields;
        if (!m) return 0;
        for (int j = 0; j < pat->pat_map.nfields; j++) {
            const char *k = pat->pat_map.keys[j];
            Value *fv = map_get(m, k);
            if (!fv) return 0; /* key missing -> no match */
            Node *sub = pat->pat_map.sub[j];
            if (!bind_pattern(i, sub, fv, env, mutable)) return 0;
        }
        return 1;
    }
    default: return 1;
    }
}

static int match_pattern(Interp *i, Node *pat, Value *val, Env *env) {
    if (!pat) return 1;
    switch (VAL_TAG(pat)) {
    case NODE_PAT_WILD: return 1;
    case NODE_PAT_IDENT:
        if (env) env_define(env, pat->pat_ident.name, val,
                             pat->pat_ident.mutable);
        return 1;
    case NODE_PAT_LIT: {
        switch (pat->pat_lit.tag) {
        case 0: return VAL_TAG(val) == XS_INT && VAL_INT(val) == pat->pat_lit.ival;
        case 1: return VAL_TAG(val) == XS_FLOAT && val->f == pat->pat_lit.fval;
        case 2: return VAL_TAG(val) == XS_STR && strcmp(val->s, pat->pat_lit.sval) == 0;
        case 3: return VAL_TAG(val) == XS_BOOL && (int)VAL_INT(val) == pat->pat_lit.bval;
        case 4: return VAL_TAG(val) == XS_NULL;
        default: return 0;
        }
    }
    case NODE_PAT_TUPLE: {
        /* Tuple patterns ((..)) match tuples only; arrays need [..]. */
        if (VAL_TAG(val) != XS_TUPLE) return 0;
        if (val->arr->len != pat->pat_tuple.elems.len) return 0;
        for (int j = 0; j < pat->pat_tuple.elems.len; j++) {
            if (!match_pattern(i, pat->pat_tuple.elems.items[j],
                               val->arr->items[j], env)) return 0;
        }
        return 1;
    }
    case NODE_PAT_ENUM: {
        const char *path = pat->pat_enum.path;
        const char *variant = path;
        const char *sep = strstr(path, "::");
        while (sep) { variant = sep + 2; sep = strstr(variant, "::"); }

        if (VAL_TAG(val) == XS_ENUM_VAL) {
            if (strcmp(val->en->variant, variant) != 0) return 0;
            XSArray *arr = val->en->arr_data;
            if (pat->pat_enum.args.len == 0) return 1;
            if (!arr || arr->len != pat->pat_enum.args.len) return 0;
            for (int j = 0; j < pat->pat_enum.args.len; j++) {
                if (!match_pattern(i, pat->pat_enum.args.items[j],
                                   arr->items[j], env)) return 0;
            }
            return 1;
        }
        if (VAL_TAG(val) == XS_STR && strcmp(val->s, variant) == 0)
            return pat->pat_enum.args.len == 0;
        return 0;
    }
    case NODE_PAT_STRUCT: {
        XSMap *m = NULL;
        const char *type_name = pat->pat_struct.path;
        if (VAL_TAG(val) == XS_MAP) m = val->map;
        else if (VAL_TAG(val) == XS_STRUCT_VAL) {
            if (type_name && strcmp(val->st->type_name, type_name) != 0) return 0;
            m = val->st->fields;
        } else if (VAL_TAG(val) == XS_ENUM_VAL) {
            if (type_name) {
                const char *variant = type_name;
                const char *sep = strstr(type_name, "::");
                while (sep) { variant = sep+2; sep = strstr(variant,"::"); }
                if (strcmp(val->en->variant, variant) != 0) return 0;
            }
            m = val->en->map_data;
        } else if (VAL_TAG(val) == XS_INST) {
            /* Walk the bases breadth-first so a base-class pattern still
               matches a subclass instance. Reject otherwise so e.g.
               `Circle { radius }` does not silently bind on a Rect. */
            if (type_name && val->inst->class_) {
                XSClass *stack[64];
                int sp = 0, ok = 0;
                stack[sp++] = val->inst->class_;
                while (sp > 0 && !ok) {
                    XSClass *c = stack[--sp];
                    if (c->name && strcmp(c->name, type_name) == 0) { ok = 1; break; }
                    for (int b = 0; b < c->nbases && sp < 64; b++)
                        if (c->bases[b]) stack[sp++] = c->bases[b];
                }
                if (!ok) return 0;
            }
            m = val->inst->fields;
        }
        if (!m) return 0;
        for (int j = 0; j < pat->pat_struct.fields.len; j++) {
            char *fname = pat->pat_struct.fields.items[j].key;
            Node *sub   = pat->pat_struct.fields.items[j].val;
            Value *fv   = map_get(m, fname);
            /* Apply default value if field is missing/null */
            if ((!fv || VAL_TAG(fv) == XS_NULL) &&
                j < pat->pat_struct.defaults.len &&
                pat->pat_struct.defaults.items[j]) {
                fv = EVAL(i, pat->pat_struct.defaults.items[j]);
                if (sub) {
                    int ok = match_pattern(i, sub, fv, env);
                    value_decref(fv);
                    if (!ok) return 0;
                } else if (env) {
                    env_define(env, fname, fv, 1);
                    value_decref(fv);
                }
                continue;
            }
            if (!fv) fv = XS_NULL_VAL;
            if (sub) { if (!match_pattern(i, sub, fv, env)) return 0; }
            else if (env) env_define(env, fname, fv, 1);
        }
        return 1;
    }
    case NODE_PAT_OR: {
        Env *tmp = env_new(NULL);
        int lm = match_pattern(i, pat->pat_or.left, val, tmp);
        if (lm) {
            /* copy bindings */
            for (int j = 0; j < tmp->len; j++)
                if (env) env_define(env, tmp->bindings[j].name,
                                    tmp->bindings[j].value, 1);
            env_decref(tmp);
            return 1;
        }
        /* reset and try right */
        env_decref(tmp);
        return match_pattern(i, pat->pat_or.right, val, env);
    }
    case NODE_PAT_RANGE: {
        Value *start = pat->pat_range.start ? EVAL(i, pat->pat_range.start) : NULL;
        Value *end   = pat->pat_range.end   ? EVAL(i, pat->pat_range.end)   : NULL;
        int ok = 1;
        if (start) {
            int cmp = value_cmp(val, start);
            ok = ok && (cmp >= 0);
            value_decref(start);
        }
        if (end) {
            int cmp = value_cmp(val, end);
            ok = ok && (pat->pat_range.inclusive ? cmp <= 0 : cmp < 0);
            value_decref(end);
        }
        return ok;
    }
    case NODE_PAT_SLICE: {
        /* Slice patterns ([..]) match arrays only; tuples need (..). */
        if (VAL_TAG(val) != XS_ARRAY) return 0;
        XSArray *arr = val->arr;
        int nfixed = pat->pat_slice.elems.len;
        if (!pat->pat_slice.rest && arr->len != nfixed) return 0;
        if (pat->pat_slice.rest  && arr->len < nfixed)  return 0;
        for (int j = 0; j < nfixed; j++) {
            if (!match_pattern(i, pat->pat_slice.elems.items[j],
                               arr->items[j], env)) return 0;
        }
        if (env && pat->pat_slice.rest) {
            Value *rest_arr = xs_array_new();
            for (int j = nfixed; j < arr->len; j++)
                array_push(rest_arr->arr, value_incref(arr->items[j]));
            env_define(env, pat->pat_slice.rest, rest_arr, 1);
            value_decref(rest_arr);
        }
        return 1;
    }
    case NODE_PAT_EXPR: {
        Value *ev = EVAL(i, pat->pat_expr.expr);
        int eq = value_equal(ev, val);
        value_decref(ev);
        return eq;
    }
    case NODE_PAT_CAPTURE: {
        if (!match_pattern(i, pat->pat_capture.pattern, val, env)) return 0;
        if (env) env_define(env, pat->pat_capture.name, val, 0);
        return 1;
    }
    case NODE_PAT_GUARD: {
        if (!match_pattern(i, pat->pat_guard.pattern, val, env)) return 0;
        Value *g = EVAL(i, pat->pat_guard.guard);
        int ok = value_truthy(g);
        value_decref(g);
        return ok;
    }
    case NODE_PAT_STRING_CONCAT: {
        if (VAL_TAG(val) != XS_STR) return 0;
        const char *prefix = pat->pat_str_concat.prefix;
        size_t plen = strlen(prefix);
        if (strncmp(val->s, prefix, plen) != 0) return 0;
        Value *rest_val = xs_str(val->s + plen);
        int ok = match_pattern(i, pat->pat_str_concat.rest, rest_val, env);
        value_decref(rest_val);
        return ok;
    }
    case NODE_PAT_REGEX: {
        if (VAL_TAG(val) != XS_STR || !pat->pat_regex.pattern) return 0;
        /* full-string match: run regex, check that match covers entire string */
        regex_t re;
        int rc = regcomp(&re, pat->pat_regex.pattern, REG_EXTENDED);
        if (rc != 0) return 0;
        regmatch_t m;
        int ok = 0;
        if (regexec(&re, val->s, 1, &m, 0) == 0) {
            ok = (m.rm_so == 0 && m.rm_eo == (int)strlen(val->s));
        }
        regfree(&re);
        return ok;
    }
    case NODE_PAT_MAP: {
        XSMap *m = NULL;
        if (VAL_TAG(val) == XS_MAP || VAL_TAG(val) == XS_MODULE) m = val->map;
        else if (VAL_TAG(val) == XS_STRUCT_VAL) m = val->st->fields;
        else if (VAL_TAG(val) == XS_INST) m = val->inst->fields;
        if (!m) return 0;
        for (int j = 0; j < pat->pat_map.nfields; j++) {
            const char *k = pat->pat_map.keys[j];
            Value *fv = map_get(m, k);
            if (!fv) return 0;
            Node *sub = pat->pat_map.sub[j];
            if (!match_pattern(i, sub, fv, env)) return 0;
        }
        return 1;
    }
    default: return 1;
    }
}

static void interp_push_frame(Interp *i, const char *func_name, Span span) {
    if (i->call_stack_len >= i->call_stack_cap) {
        int newcap = i->call_stack_cap ? i->call_stack_cap * 2 : 16;
        i->call_stack = (InterpFrame *)xs_realloc(
            i->call_stack, (size_t)newcap * sizeof(InterpFrame));
        i->call_stack_cap = newcap;
    }
    InterpFrame *f = &i->call_stack[i->call_stack_len++];
    f->func_name = func_name;
    f->call_span = span;
}

static void interp_pop_frame(Interp *i) {
    if (i->call_stack_len > 0) i->call_stack_len--;
}

Value *call_value(Interp *i, Value *callee, Value **args, int argc,
                          const char *call_site) {
    if (!callee) return value_incref(XS_NULL_VAL);

    /* Wrapping decorators (@memoize / @retry / @trace / @timed) bind
       the fn name to a map carrying _wrap_kind, so the call dispatcher
       has to recognise it first and route to the wrapper logic. */
    if (VAL_TAG(callee) == XS_MAP && callee->map &&
        map_get(callee->map, "_wrap_kind")) {
        extern Value *wrap_call_dispatch(Interp *, Value *, Value **, int);
        Value *r = wrap_call_dispatch(i, callee, args, argc);
        if (r) return r;
    }

    /* VM-mode entry: when a script is run from a file the bytecode VM
       calls native functions with NULL for the Interp* (vm_dispatch's
       OP_CALL / OP_METHOD_CALL pass NULL because the interp isn't the
       authoritative environment in that mode). Some natives -- notably
       http.serve, signal subscribers, map.filter / fold callbacks --
       turn around and call back into call_value with the same NULL.
       Without a guard this dereferences i->current_span the moment we
       reach for a TRACE_* macro and crashes the server. Route by callee
       kind and skip the frame/trace bookkeeping that's only meaningful
       for the AST interpreter. */
    if (!i) {
        if (VAL_TAG(callee) == XS_NATIVE) {
            Value *result = callee->native(NULL, args, argc);
            return result ? result : value_incref(XS_NULL_VAL);
        }
#ifdef XSC_ENABLE_VM
        if (VAL_TAG(callee) == XS_CLOSURE && g_vm_for_invoke) {
            Value *result = vm_invoke_public(g_vm_for_invoke, callee, args, argc);
            return result ? result : value_incref(XS_NULL_VAL);
        }
#endif
        return value_incref(XS_NULL_VAL);
    }

    const char *frame_name = call_site;
    if (!frame_name && VAL_TAG(callee) == XS_FUNC && callee->fn->name)
        frame_name = callee->fn->name;
    TRACE_CALL(i, frame_name ? frame_name : "<call>", i->current_span.line);
    interp_push_frame(i, frame_name, i->current_span);

    if (VAL_TAG(callee) == XS_OVERLOAD) {
        /* dispatch overloaded function by argument count */
        XSArray *oset = callee->overload;
        Value *best = NULL;
        for (int oi = 0; oi < oset->len; oi++) {
            Value *candidate = oset->items[oi];
            if (VAL_TAG(candidate) == XS_NATIVE) {
                if (!best) best = candidate; /* native is fallback */
                continue;
            }
            if (VAL_TAG(candidate) != XS_FUNC) continue;
            XSFunc *cfn = candidate->fn;
            int min_arity = 0, max_arity = cfn->nparams;
            int has_variadic = 0;
            for (int pi = 0; pi < cfn->nparams; pi++) {
                if (cfn->variadic_flags && cfn->variadic_flags[pi]) { has_variadic = 1; continue; }
                if (!cfn->default_vals || !cfn->default_vals[pi]) min_arity++;
            }
            if (has_variadic) {
                if (argc >= min_arity) { best = candidate; break; }
            } else if (argc >= min_arity && argc <= max_arity) {
                best = candidate; break;
            }
        }
        if (!best) {
            xs_runtime_error(i->current_span,
                "no overload matches argument count",
                "check the number of arguments",
                "called with %d argument(s), no matching overload found", argc);
            interp_pop_frame(i);
            return value_incref(XS_NULL_VAL);
        }
        interp_pop_frame(i);
        return call_value(i, best, args, argc, call_site);
    }

    if (VAL_TAG(callee) == XS_NATIVE) {
        Value *result = callee->native(i, args, argc);
        TRACE_RETURN(i, frame_name ? frame_name : "<call>", result);
        interp_pop_frame(i);
        return result ? result : value_incref(XS_NULL_VAL);
    }

#ifdef XSC_ENABLE_VM
    if (VAL_TAG(callee) == XS_CLOSURE) {
        /* Bytecode closure invoked from interp-side native (e.g.
           http.serve handler, signal subscriber, map.filter callback).
           The closure was built by the bytecode compiler and only the
           VM knows how to push the right frame. Route through the
           thread-local current VM. Falls through to the not-callable
           error below if we're somehow on a thread without a live VM. */
        if (g_vm_for_invoke) {
            Value *result = vm_invoke_public(g_vm_for_invoke, callee, args, argc);
            TRACE_RETURN(i, frame_name ? frame_name : "<call>", result);
            interp_pop_frame(i);
            return result ? result : value_incref(XS_NULL_VAL);
        }
    }
#endif

    if (VAL_TAG(callee) == XS_FUNC) {
        XSFunc *fn = callee->fn;
        if (fn->deprecated_msg) {
            fprintf(stderr, "xs: warning: function '%s' is deprecated: %s\n",
                    fn->name ? fn->name : "<anonymous>", fn->deprecated_msg);
        }
        if (i->depth >= i->max_depth) {
            /* Raise as a catchable throw instead of exit() so that
               try/catch around deep recursion can recover. */
            char msg[160];
            snprintf(msg, sizeof msg, "stack overflow in '%s' (depth %d)",
                     fn->name ? fn->name : "<anonymous>", i->depth);
            Value *err = xs_error_new("StackOverflow", msg, NULL);
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_THROW;
            i->cf.value  = err;
            interp_pop_frame(i);
            return value_incref(XS_NULL_VAL);
        }
        i->depth++;

        Env *saved_env = i->env;
        env_incref(saved_env);

        Value **tc_args = NULL;
        int     tc_argc = 0;
        int     owns_args = 0;

tail_call_entry: ;
        {
        Env *call_env  = env_new(fn->closure);
        i->env = call_env;

        {
            Value **cur_args = owns_args ? tc_args : args;
            int     cur_argc = owns_args ? tc_argc : argc;
            int arg_idx = 0;
            for (int j = 0; j < fn->nparams; j++) {
                Node *param = fn->params[j];
                int is_variadic = fn->variadic_flags && fn->variadic_flags[j];
                if (is_variadic) {
                    Value *rest = xs_array_new();
                    for (int k = arg_idx; k < cur_argc; k++)
                        array_push(rest->arr, value_incref(cur_args[k]));
                    if (VAL_TAG(param) == NODE_PAT_IDENT)
                        env_define(call_env, param->pat_ident.name, rest, 1);
                    else
                        bind_pattern(i, param, rest, call_env, 1);
                    value_decref(rest);
                    arg_idx = cur_argc;
                } else if (arg_idx < cur_argc) {
                    if (VAL_TAG(param) == NODE_PAT_IDENT) {
                        env_define(call_env, param->pat_ident.name, cur_args[arg_idx], 1);
                        /* fire after_eval hooks for param binding (as NODE_LET) */
                        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
                            g_in_eval_hook = 1;
                            for (int _h = 0; _h < g_n_after_eval; _h++) {
                                EvalHook *hook = &g_after_eval[_h];
                                if (!hook->callback) continue;
                                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_LET) continue;
                                Value *node_map = xs_map_new();
                                map_set(node_map->map, "tag", xs_str("let"));
                                map_set(node_map->map, "name", xs_str(param->pat_ident.name));
                                /* value is null: classify will fall through to param entry */
                                map_take(node_map->map, "line", xs_int(param->span.line));
                                Value *hargs[2] = { node_map, cur_args[arg_idx] };
                                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                                value_decref(node_map);
                                if (hresult) value_decref(hresult);
                                if (i->cf.signal) { g_in_eval_hook = 0; break; }
                            }
                            g_in_eval_hook = 0;
                        }
                    } else
                        bind_pattern(i, param, cur_args[arg_idx], call_env, 1);
                    arg_idx++;
                } else {
                    Node *def = fn->default_vals ? fn->default_vals[j] : NULL;
                    if (def) {
                        Value *dv = interp_eval(i, def);
                        if (VAL_TAG(param) == NODE_PAT_IDENT)
                            env_define(call_env, param->pat_ident.name, dv, 1);
                        else
                            bind_pattern(i, param, dv, call_env, 1);
                        value_decref(dv);
                    } else {
                        if (VAL_TAG(param) == NODE_PAT_IDENT)
                            env_define(call_env, param->pat_ident.name, XS_NULL_VAL, 1);
                    }
                }
            }
        }

        /* wire up 'super' proxy so subclass methods can call base */
        {
            Value **cur_args2 = owns_args ? tc_args : args;
            int     cur_argc2 = owns_args ? tc_argc : argc;
            if (fn->nparams > 0 && cur_argc2 > 0 &&
                VAL_TAG(fn->params[0]) == NODE_PAT_IDENT &&
                strcmp(fn->params[0]->pat_ident.name, "self") == 0 &&
                VAL_TAG(cur_args2[0]) == XS_INST &&
                cur_args2[0]->inst->class_ &&
                cur_args2[0]->inst->class_->nbases > 0 &&
                cur_args2[0]->inst->class_->bases[0]) {
                XSClass *base = cur_args2[0]->inst->class_->bases[0];
                XSInst *si = xs_calloc(1, sizeof(XSInst));
                si->class_  = base;
                base->refcount++;
                si->fields  = map_new(); /* own empty fields map */
                si->methods = map_new();
                if (base->methods) {
                    int nk = 0; char **ks = map_keys(base->methods, &nk);
                    for (int sk = 0; sk < nk; sk++) {
                        Value *mv = map_get(base->methods, ks[sk]);
                        if (mv) map_set(si->methods, ks[sk], value_incref(mv));
                        free(ks[sk]);
                    }
                    free(ks);
                }
                map_set(si->fields, "__super_self__", value_incref(cur_args2[0]));
                si->refcount = 1;
                Value *super_val = xs_calloc(1, sizeof(Value));
                super_val->tag = XS_INST; super_val->refcount = 1;
                super_val->inst = si;
                env_define(call_env, "super", super_val, 1);
                value_decref(super_val);
            }
        }

        if (fn->param_type_names) {
            Value **cur_args = owns_args ? tc_args : args;
            int     cur_argc = owns_args ? tc_argc : argc;
            int arg_idx2 = 0;
            for (int j = 0; j < fn->nparams && arg_idx2 < cur_argc; j++) {
                int is_variadic = fn->variadic_flags && fn->variadic_flags[j];
                if (is_variadic) break;
                if (fn->param_type_names[j] && !value_matches_type(cur_args[arg_idx2], fn->param_type_names[j])) {
                    xs_runtime_error(i->current_span, "type mismatch", NULL,
                        "argument %d of '%s' expected '%s', got '%s'",
                        j + 1, fn->name ? fn->name : "<anonymous>",
                        fn->param_type_names[j], value_type_str(cur_args[arg_idx2]));
                    i->cf.signal = CF_PANIC;
                    i->cf.value = xs_str("type error");
                    break;
                }
                arg_idx2++;
            }
        }

        /* check where contracts on parameters */
        if (fn->param_contracts && !i->cf.signal) {
            for (int j = 0; j < fn->nparams; j++) {
                if (!fn->param_contracts[j]) continue;
                Env *saved_pc = i->env;
                i->env = call_env;
                Value *check = interp_eval(i, fn->param_contracts[j]);
                i->env = saved_pc;
                if (!value_truthy(check)) {
                    value_decref(check);
                    char msg[512];
                    snprintf(msg, sizeof msg,
                        "contract violation: parameter %d of '%s' does not satisfy 'where' constraint",
                        j + 1, fn->name ? fn->name : "<anonymous>");
                    i->cf.signal = CF_THROW;
                    i->cf.value = xs_str(msg);
                    break;
                }
                value_decref(check);
            }
        }

        if (owns_args) {
            for (int j = 0; j < tc_argc; j++) value_decref(tc_args[j]);
            free(tc_args);
            tc_args = NULL; owns_args = 0;
        }

        if (i->cf.signal) {
            pop_env(i);
            i->depth--;
            interp_pop_frame(i);
            return value_incref(XS_NULL_VAL);
        }

        int defer_base = i->defers.len;
        Value *result = NULL;
        if (fn->body) {
            if (fn->is_generator) {
#ifdef XS_WASM
                {
                /* WASI has no real thread support, so the thread-per-
                   generator machinery deadlocks on pthread_cond_wait.
                   Run the body eagerly in the calling thread, collect
                   every yield into an array, and expose a .next()
                   that walks it. Infinite generators will OOM rather
                   than hang; use a hard yield cap to fail fast. */
                Value *w_bucket = xs_array_new();
                Value *w_prev_collect = i->yield_collect;
                int    w_prev_limit   = i->yield_limit;
                i->yield_collect = w_bucket;
                i->yield_limit   = 1000000; /* 1M yields is enough */
                int w_saved_signal = i->cf.signal;
                Value *w_body_val = interp_eval(i, fn->body);
                if (w_body_val) value_decref(w_body_val);
                i->yield_collect = w_prev_collect;
                i->yield_limit   = w_prev_limit;
                /* CF_RETURN from the yield-limit overflow is expected;
                   clear it here so the caller doesn't see a stale
                   signal. A real error (throw, panic, tail call) is
                   preserved. */
                if (i->cf.signal == CF_RETURN) {
                    if (i->cf.value) value_decref(i->cf.value);
                    i->cf.signal = w_saved_signal;
                    i->cf.value = NULL;
                }
                Value *w_gen = xs_map_new();
                Value *w_type_v = xs_str("generator");
                map_set(w_gen->map, "__type", w_type_v); value_decref(w_type_v);
                map_set(w_gen->map, "_array", w_bucket); value_decref(w_bucket);
                Value *w_idx_v = xs_int(0);
                map_set(w_gen->map, "_index", w_idx_v); value_decref(w_idx_v);
                Value *w_done_v = value_incref(XS_FALSE_VAL);
                map_set(w_gen->map, "_done", w_done_v); value_decref(w_done_v);
                result = w_gen;
                goto gen_done;
                }
#endif
                /* Lazy generator: spin up a worker thread that runs
                   the body. The worker blocks on a resume channel
                   between yields, so infinite generators with break
                   no longer hang. The generator object exposes
                   _yield_chan and _resume_chan for .next() / for. */
                Value *yield_chan  = xs_map_new();
                Value *resume_chan = xs_map_new();
                int yc_id = xs_chan_alloc(0);
                int rc_id = xs_chan_alloc(0);
                {
                    Value *t = xs_str("Channel");
                    map_set(yield_chan->map, "_type", t); value_decref(t);
                    Value *idv = xs_int(yc_id);
                    map_set(yield_chan->map, "_chan_id", idv); value_decref(idv);
                    Value *bf = xs_array_new();
                    map_set(yield_chan->map, "_buf", bf); value_decref(bf);
                }
                {
                    Value *t = xs_str("Channel");
                    map_set(resume_chan->map, "_type", t); value_decref(t);
                    Value *idv = xs_int(rc_id);
                    map_set(resume_chan->map, "_chan_id", idv); value_decref(idv);
                    Value *bf = xs_array_new();
                    map_set(resume_chan->map, "_buf", bf); value_decref(bf);
                }

                /* Build a zero-arg wrapper whose closure env IS the
                   already-populated call_env. The worker invokes it with
                   no args and the body sees the parameters as locals
                   (start, end, etc) just like the regular eager path. */
                XSFunc *gen_fn = func_new_ex(fn->name, NULL, 0,
                                             fn->body, call_env,
                                             NULL, NULL);
                gen_fn->is_generator = 0;
                Value *closure = xs_func_new(gen_fn);

                xs_spawn_generator(i, closure, yield_chan, resume_chan);
                value_decref(closure);

                Value *gen = xs_map_new();
                Value *type_v = xs_str("generator");
                map_set(gen->map, "__type", type_v); value_decref(type_v);
                map_set(gen->map, "_yield_chan",  yield_chan);
                map_set(gen->map, "_resume_chan", resume_chan);
                Value *done_v = value_incref(XS_FALSE_VAL);
                map_set(gen->map, "_done", done_v); value_decref(done_v);
                value_decref(yield_chan);
                value_decref(resume_chan);
                result = gen;
#ifdef XS_WASM
            gen_done:;
#endif
            } else {
                Value *body_val = interp_eval(i, fn->body);
                if (i->cf.signal == CF_TAIL_CALL) {
                    value_decref(body_val);
                    for (int di = i->defers.len - 1; di >= defer_base; di--) {
                        Node *db = i->defers.items[di];
                        int saved_sig = i->cf.signal;
                        i->cf.signal = 0; i->cf.value = NULL; i->cf.label = NULL;
                        interp_exec(i, db);
                        if (!i->cf.signal) {
                            i->cf.signal = saved_sig;
                        }
                    }
                    i->defers.len = defer_base;

                    Value *tc_callee = i->tc_callee; i->tc_callee = NULL;
                    tc_args = i->tc_args;   i->tc_args = NULL;
                    tc_argc = i->tc_argc;   i->tc_argc = 0;
                    owns_args = 1;
                    i->cf.signal = 0;

                    env_decref(i->env);
                    if (VAL_TAG(tc_callee) == XS_FUNC) {
                        fn = tc_callee->fn;
                        value_decref(tc_callee);
                        goto tail_call_entry;
                    }

                    i->env = saved_env;
                    i->depth--;
                    Value *r = call_value(i, tc_callee, tc_args, tc_argc, NULL);
                    value_decref(tc_callee);
                    for (int j = 0; j < tc_argc; j++) value_decref(tc_args[j]);
                    free(tc_args);
                    interp_pop_frame(i);
                    return r;
                } else if (i->cf.signal == CF_RETURN) {
                    if (i->cf.value) result = value_incref(i->cf.value);
                    CF_CLEAR(i);
                    value_decref(body_val);
                } else if (!i->cf.signal) {
                    result = body_val;
                } else {
                    value_decref(body_val);
                }
            }
        }
        if (!result) result = value_incref(XS_NULL_VAL);

        for (int di = i->defers.len - 1; di >= defer_base; di--) {
            Node *db = i->defers.items[di];
            int saved_sig = i->cf.signal;
            Value *saved_val = i->cf.value;
            char *saved_lbl = i->cf.label;
            i->cf.signal = 0; i->cf.value = NULL; i->cf.label = NULL;
            interp_exec(i, db);
            if (!i->cf.signal) {
                i->cf.signal = saved_sig;
                i->cf.value  = saved_val;
                i->cf.label  = saved_lbl;
            } else {
                value_decref(saved_val);
                free(saved_lbl);
            }
        }
        i->defers.len = defer_base;

        if (fn->ret_type_name && !value_matches_type(result, fn->ret_type_name)) {
            xs_runtime_error(i->current_span, "type mismatch", NULL,
                "function '%s' expected return type '%s', got '%s'",
                fn->name ? fn->name : "<anonymous>",
                fn->ret_type_name, value_type_str(result));
            i->cf.signal = CF_PANIC;
            i->cf.value = xs_str("type error");
        }

        TRACE_RETURN(i, fn->name ? fn->name : "<fn>", result);
        env_decref(i->env);
        i->env = saved_env;
        i->depth--;
        interp_pop_frame(i);
        return result;
        } /* end of tail_call_entry block */
    }

    if (VAL_TAG(callee) == XS_CLASS_VAL) {
        XSClass *cls = callee->cls;
        XSInst  *inst = xs_calloc(1, sizeof(XSInst));
        inst->class_  = cls; cls->refcount++;
        inst->fields  = map_new();
        inst->methods = map_new();
        inst->refcount = 1;

        if (cls->fields) {
            int nkeys = 0;
            char **keys = map_keys(cls->fields, &nkeys);
            for (int j = 0; j < nkeys; j++) {
                Value *fv = map_get(cls->fields, keys[j]);
                if (fv) {
                    /* Class-level field defaults are templates; mutable
                       defaults (arrays, maps) must be cloned per instance
                       so two instances don't accidentally share state. */
                    Value *cv = (VAL_TAG(fv) == XS_ARRAY ||
                                 VAL_TAG(fv) == XS_MAP) ? value_copy(fv) : value_incref(fv);
                    map_set(inst->fields, keys[j], cv);
                    if (VAL_TAG(fv) == XS_ARRAY || VAL_TAG(fv) == XS_MAP) value_decref(cv);
                }
                free(keys[j]);
            }
            free(keys);
        }
        if (cls->methods) {
            int nkeys = 0;
            char **keys = map_keys(cls->methods, &nkeys);
            for (int j = 0; j < nkeys; j++) {
                Value *mv = map_get(cls->methods, keys[j]);
                if (mv) map_set(inst->methods, keys[j], value_incref(mv));
                free(keys[j]);
            }
            free(keys);
        }

        Value *inst_val = xs_calloc(1, sizeof(Value));
        inst_val->tag = XS_INST; inst_val->refcount = 1;
        inst_val->inst = inst;

        Value *init_fn = map_get(inst->methods, "__init__");
        if (!init_fn) init_fn = map_get(inst->methods, "init");
        if (init_fn && VAL_TAG(init_fn) == XS_FUNC) {
            XSFunc *ifn = init_fn->fn;
            int expected = ifn->nparams > 0 ? ifn->nparams - 1 : 0;
            int min_arity = expected;
            int has_var = 0;
            for (int pi = 1; pi < ifn->nparams; pi++) {
                if (ifn->variadic_flags && ifn->variadic_flags[pi]) {
                    has_var = 1;
                    if (min_arity > 0) min_arity--;
                }
                if (ifn->default_vals && ifn->default_vals[pi] && min_arity > 0)
                    min_arity--;
            }
            if (argc < min_arity || (!has_var && argc > expected)) {
                const char *cname = (inst->class_ && inst->class_->name)
                                     ? inst->class_->name : "<class>";
                xs_runtime_error(i->current_span, "type mismatch",
                    "check the number of arguments to the constructor",
                    "init for '%s' expected %d arg%s, got %d",
                    cname, expected, expected == 1 ? "" : "s", argc);
                interp_pop_frame(i);
                return inst_val;
            }
            Value **new_args = xs_malloc((argc+1)*sizeof(Value*));
            new_args[0] = value_incref(inst_val);
            for (int j = 0; j < argc; j++) new_args[j+1] = args[j];
            Value *r = call_value(i, init_fn, new_args, argc+1, "__init__");
            value_decref(new_args[0]);
            free(new_args);
            value_decref(r);
        }
        interp_pop_frame(i);
        return inst_val;
    }

    if (VAL_TAG(callee) == XS_SIGNAL) {
        XSSignal *sig = callee->signal;
        if (argc > 0) {
            value_decref(sig->value);
            sig->value = value_incref(args[0]);
            if (!sig->notifying) {
                sig->notifying = 1;
                for (int j = 0; j < sig->nsubs; j++) {
                    Value *r = call_value(i, sig->subscribers[j], args, 1, "subscriber");
                    value_decref(r);
                }
                sig->notifying = 0;
            }
        }
        if (sig->compute) {
            Value *r = call_value(i, sig->compute, NULL, 0, "derived");
            interp_pop_frame(i);
            return r;
        }
        interp_pop_frame(i);
        return value_incref(sig->value);
    }

    if (VAL_TAG(callee) == XS_ACTOR && callee->actor) {
        XSActor *src = callee->actor;
        XSActor *actor = xs_calloc(1, sizeof(XSActor));
        actor->name     = xs_strdup(src->name);
        actor->state    = map_new();
        actor->methods  = map_new();
        actor->refcount = 1;
        if (src->state) {
            int nk = 0; char **ks = map_keys(src->state, &nk);
            for (int j = 0; j < nk; j++) {
                Value *sv = map_get(src->state, ks[j]);
                if (sv) map_set(actor->state, ks[j], value_incref(sv));
                free(ks[j]);
            }
            free(ks);
        }
        if (src->methods) {
            int nk = 0; char **ks = map_keys(src->methods, &nk);
            for (int j = 0; j < nk; j++) {
                Value *mv = map_get(src->methods, ks[j]);
                if (mv) map_set(actor->methods, ks[j], value_incref(mv));
                free(ks[j]);
            }
            free(ks);
        }
        if (src->handle_fn) {
            actor->handle_fn = src->handle_fn;
            actor->handle_fn->refcount++;
        }
        actor->closure = src->closure ? env_incref(src->closure) : NULL;
        Value *init_fn = map_get(actor->methods, "init");
        if (!init_fn) init_fn = map_get(actor->methods, "__init__");
        Value *actor_val = xs_calloc(1, sizeof(Value));
        actor_val->tag = XS_ACTOR;
        actor_val->refcount = 1;
        actor_val->actor    = actor;
        if (init_fn && VAL_TAG(init_fn) == XS_FUNC) {
            Env *wrapper = env_new(init_fn->fn->closure ? init_fn->fn->closure : actor->closure);
            env_define(wrapper, "self", value_incref(actor_val), 0);
            if (actor->state) {
                int nk2 = 0; char **ks2 = map_keys(actor->state, &nk2);
                for (int j = 0; j < nk2; j++) {
                    Value *sv = map_get(actor->state, ks2[j]);
                    if (sv) env_define(wrapper, ks2[j], value_incref(sv), 1);
                    free(ks2[j]);
                }
                free(ks2);
            }
            Env *orig = init_fn->fn->closure;
            env_incref(wrapper);
            init_fn->fn->closure = wrapper;
            Value *r = call_value(i, init_fn, args, argc, "init");
            env_decref(init_fn->fn->closure);
            init_fn->fn->closure = orig;
            if (actor->state) {
                int nk2 = 0; char **ks2 = map_keys(actor->state, &nk2);
                for (int j = 0; j < nk2; j++) {
                    Value *upd = env_get(wrapper, ks2[j]);
                    if (upd) map_set(actor->state, ks2[j], value_incref(upd));
                    free(ks2[j]);
                }
                free(ks2);
            }
            env_decref(wrapper);
            value_decref(r);
        }
        interp_pop_frame(i);
        return actor_val;
    }

    char *repr = value_repr(callee);
    const char *type_name = "unknown";
    switch (VAL_TAG(callee)) {
        case XS_INT: type_name = "int"; break;
        case XS_FLOAT: type_name = "float"; break;
        case XS_STR: type_name = "str"; break;
        case XS_BOOL: type_name = "bool"; break;
        case XS_ARRAY: type_name = "array"; break;
        case XS_MAP: type_name = "map"; break;
        case XS_NULL: type_name = "null"; break;
        default: type_name = "value"; break;
    }
    xs_runtime_error(i->current_span,
            "not a function: cannot call this",
            NULL,
            "value of type '%s' is not callable (got %s)",
            type_name, repr);
    free(repr);
    i->cf.signal = CF_ERROR;
    interp_pop_frame(i);
    return value_incref(XS_NULL_VAL);
}

/* method dispatch */

static Value *eval_method(Interp *i, Value *obj, const char *method,
                           Value **args, int argc) {
    if (VAL_TAG(obj) == XS_INST) {
        if (strcmp(method, "is_a") == 0) {
            if (argc >= 1 && VAL_TAG(args[0]) == XS_STR && obj->inst->class_) {
                int found = 0;
                XSClass *stack[64];
                int sp = 0;
                stack[sp++] = obj->inst->class_;
                while (sp > 0 && !found) {
                    XSClass *c = stack[--sp];
                    if (strcmp(c->name, args[0]->s) == 0) { found = 1; break; }
                    for (int ti = 0; ti < c->ntraits; ti++) {
                        if (c->traits[ti] && strcmp(c->traits[ti], args[0]->s) == 0)
                            { found = 1; break; }
                    }
                    if (found) break;
                    for (int bi = 0; bi < c->nbases && sp < 64; bi++) {
                        if (c->bases[bi]) stack[sp++] = c->bases[bi];
                    }
                }
                return found ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
            }
            return value_incref(XS_FALSE_VAL);
        }
        Value *fn = map_get(obj->inst->methods, method);
        if (!fn && obj->inst->class_ && obj->inst->class_->methods)
            fn = map_get(obj->inst->class_->methods, method);
        if (fn) {
            int has_self_param = 0;
            if (VAL_TAG(fn) == XS_FUNC && fn->fn->nparams > 0) {
                Node *p0 = fn->fn->params[0];
                if (VAL_TAG(p0) == NODE_PAT_IDENT && strcmp(p0->pat_ident.name, "self") == 0)
                    has_self_param = 1;
            }
            if (VAL_TAG(fn) == XS_NATIVE) has_self_param = 1;
            Value *r;
            if (has_self_param) {
                Value *real_self = obj;
                Value *super_self = map_get(obj->inst->fields, "__super_self__");
                if (super_self && VAL_TAG(super_self) == XS_INST)
                    real_self = super_self;
                Value **new_args = xs_malloc((argc+1)*sizeof(Value*));
                new_args[0] = value_incref(real_self);
                for (int j = 0; j < argc; j++) new_args[j+1] = args[j];
                r = call_value(i, fn, new_args, argc+1, method);
                value_decref(new_args[0]);
                free(new_args);
            } else {
                Env *saved = i->env;
                env_incref(saved);
                Env *wrapper = env_new(VAL_TAG(fn)==XS_FUNC ? fn->fn->closure : saved);
                env_define(wrapper, "self", obj, 1);
                if (obj->inst && obj->inst->fields) {
                    int nk = 0; char **ks = map_keys(obj->inst->fields, &nk);
                    for (int j = 0; j < nk; j++) {
                        Value *fv = map_get(obj->inst->fields, ks[j]);
                        if (fv) env_define(wrapper, ks[j], fv, 1);
                        free(ks[j]);
                    }
                    free(ks);
                }
                Env *orig_closure = NULL;
                if (VAL_TAG(fn) == XS_FUNC) {
                    orig_closure = fn->fn->closure;
                    env_incref(wrapper);
                    fn->fn->closure = wrapper;
                }
                r = call_value(i, fn, args, argc, method);
                if (VAL_TAG(fn) == XS_FUNC) {
                    env_decref(fn->fn->closure);
                    fn->fn->closure = orig_closure;
                }
                if (obj->inst && obj->inst->fields) {
                    int nk = 0; char **ks = map_keys(obj->inst->fields, &nk);
                    for (int j = 0; j < nk; j++) {
                        Value *new_fv = env_get(wrapper, ks[j]);
                        if (new_fv) map_set(obj->inst->fields, ks[j], value_incref(new_fv));
                        free(ks[j]);
                    }
                    free(ks);
                }
                i->env = saved;
                env_decref(wrapper);
                env_decref(saved);
            }
            return r;
        }
        Value *fv = map_get(obj->inst->fields, method);
        if (fv) return call_value(i, fv, args, argc, method);
    }

    if (VAL_TAG(obj) == XS_ACTOR && obj->actor) {
        XSActor *actor = obj->actor;
        if (strcmp(method, "send") == 0 && argc >= 1) {
            if (actor->handle_fn) {
                Env *wrapper = env_new(actor->handle_fn->closure ? actor->handle_fn->closure : actor->closure);
                env_define(wrapper, "self", value_incref(obj), 0);
                if (actor->state) {
                    int nk = 0; char **ks = map_keys(actor->state, &nk);
                    for (int j = 0; j < nk; j++) {
                        Value *sv = map_get(actor->state, ks[j]);
                        if (sv) env_define(wrapper, ks[j], value_incref(sv), 1);
                        free(ks[j]);
                    }
                    free(ks);
                }
                Env *orig_closure = actor->handle_fn->closure;
                env_incref(wrapper);
                actor->handle_fn->closure = wrapper;
                Value *fn_val = xs_func_new(actor->handle_fn);
                Value *r = call_value(i, fn_val, args, 1, "handle");
                value_decref(fn_val);
                env_decref(actor->handle_fn->closure);
                actor->handle_fn->closure = orig_closure;
                if (actor->state) {
                    int nk = 0; char **ks = map_keys(actor->state, &nk);
                    for (int j = 0; j < nk; j++) {
                        Value *updated = env_get(wrapper, ks[j]);
                        if (updated) map_set(actor->state, ks[j], value_incref(updated));
                        free(ks[j]);
                    }
                    free(ks);
                }
                env_decref(wrapper);
                return r;
            }
            return value_incref(XS_NULL_VAL);
        }
        Value *mfn = map_get(actor->methods, method);
        if (mfn) {
            Env *wrapper = env_new(VAL_TAG(mfn) == XS_FUNC ? mfn->fn->closure : actor->closure);
            env_define(wrapper, "self", value_incref(obj), 0);
            if (actor->state) {
                int nk = 0; char **ks = map_keys(actor->state, &nk);
                for (int j = 0; j < nk; j++) {
                    Value *sv = map_get(actor->state, ks[j]);
                    if (sv) env_define(wrapper, ks[j], value_incref(sv), 1);
                    free(ks[j]);
                }
                free(ks);
            }
            Env *orig_closure = NULL;
            if (VAL_TAG(mfn) == XS_FUNC) {
                orig_closure = mfn->fn->closure;
                env_incref(wrapper);
                mfn->fn->closure = wrapper;
            }
            Value *r = call_value(i, mfn, args, argc, method);
            if (VAL_TAG(mfn) == XS_FUNC) {
                env_decref(mfn->fn->closure);
                mfn->fn->closure = orig_closure;
            }
            if (actor->state) {
                int nk = 0; char **ks = map_keys(actor->state, &nk);
                for (int j = 0; j < nk; j++) {
                    Value *updated = env_get(wrapper, ks[j]);
                    if (updated) map_set(actor->state, ks[j], value_incref(updated));
                    free(ks[j]);
                }
                free(ks);
            }
            env_decref(wrapper);
            return r;
        }
        if (actor->state) {
            Value *sv = map_get(actor->state, method);
            if (sv) return value_incref(sv);
        }
        return value_incref(XS_NULL_VAL);
    }

    /* Optional-style helpers usable on any value: null behaves as
       None, anything else as Some(self). Lets users write
       `let n: int? = null; n.is_none()` without explicit Option/Some
       wrapping. */
    if (strcmp(method, "is_none") == 0)
        return (VAL_TAG(obj) == XS_NULL)
                ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
    if (strcmp(method, "is_some") == 0)
        return (VAL_TAG(obj) == XS_NULL)
                ? value_incref(XS_FALSE_VAL) : value_incref(XS_TRUE_VAL);
    if (strcmp(method, "unwrap_or") == 0) {
        if (VAL_TAG(obj) == XS_NULL)
            return argc >= 1 ? value_incref(args[0]) : value_incref(XS_NULL_VAL);
        return value_incref(obj);
    }
    /* to_str / to_string work on every value -- the type-specific
       blocks below already implement these for str/array/num/bool;
       the universal fallback lets bool/null/range/regex etc respond
       too without a per-type stub. */
    if (strcmp(method, "to_str") == 0 || strcmp(method, "to_string") == 0) {
        char *s = value_str(obj);
        Value *r = xs_str(s);
        free(s);
        return r;
    }

    // --- string methods
    if (VAL_TAG(obj) == XS_STR) {
        const char *s = obj->s;
        int slen = (int)strlen(s);
        if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0 ||
            strcmp(method, "size") == 0)
            return xs_int(utf8_strlen(s, slen));
        if (strcmp(method, "byte_len") == 0 || strcmp(method, "bytes_len") == 0)
            return xs_int(slen);
        if (strcmp(method, "upper") == 0 || strcmp(method, "to_upper") == 0) {
            int olen;
            char *r = utf8_str_upper(s, slen, &olen);
            Value *v = xs_str(r); free(r); return v;
        }
        if (strcmp(method, "lower") == 0 || strcmp(method, "to_lower") == 0) {
            int olen;
            char *r = utf8_str_lower(s, slen, &olen);
            Value *v = xs_str(r); free(r); return v;
        }
        if (strcmp(method, "trim") == 0) {
            const char *cs = (argc > 0 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : NULL;
            int start = 0, end = slen - 1;
            if (cs) {
                while (start <= end && strchr(cs, s[start])) start++;
                while (end >= start && strchr(cs, s[end]))   end--;
            } else {
                while (start <= end && isspace((unsigned char)s[start])) start++;
                while (end >= start && isspace((unsigned char)s[end]))   end--;
            }
            return xs_str_n(s+start, end-start+1);
        }
        if (strcmp(method, "starts_with") == 0 || strcmp(method, "startswith") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
            return strncmp(s, args[0]->s, strlen(args[0]->s)) == 0 ?
                   value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "ends_with") == 0 || strcmp(method, "endswith") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
            int plen = (int)strlen(args[0]->s);
            if (plen > slen) return value_incref(XS_FALSE_VAL);
            return strcmp(s + slen - plen, args[0]->s) == 0 ?
                   value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "contains") == 0 || strcmp(method, "includes") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
            return strstr(s, args[0]->s) ?
                   value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "split") == 0) {
            Value *arr = xs_array_new();
            const char *sep = (argc > 0 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : " ";
            int seplen = (int)strlen(sep);
            const char *cur = s;
            if (seplen == 0) {
                for (int j = 0; j < slen; j++)
                    array_push(arr->arr, xs_str_n(s+j, 1));
            } else {
                while (1) {
                    const char *pos = strstr(cur, sep);
                    if (!pos) { array_push(arr->arr, xs_str(cur)); break; }
                    array_push(arr->arr, xs_str_n(cur, pos-cur));
                    cur = pos + seplen;
                }
            }
            return arr;
        }
        if (strcmp(method, "replace") == 0 || strcmp(method, "replace_all") == 0 ||
            strcmp(method, "replace_first") == 0 || strcmp(method, "replace_one") == 0) {
            if (argc < 2) return value_incref(obj);
            const char *from = (VAL_TAG(args[0])==XS_STR)?args[0]->s:"";
            const char *to   = (VAL_TAG(args[1])==XS_STR)?args[1]->s:"";
            int flen=(int)strlen(from), tlen=(int)strlen(to);
            if (flen == 0) return value_incref(obj);
            int max_replace = -1; /* -1 means replace all */
            if (strcmp(method, "replace_first") == 0 || strcmp(method, "replace_one") == 0)
                max_replace = 1;
            else if (argc >= 3 && VAL_TAG(args[2]) == XS_INT) max_replace = (int)VAL_INT(args[2]);
            /* count occurrences (up to max_replace if set) */
            int count=0; const char *p=s;
            while ((p=strstr(p,from))) { count++; p+=flen; if (max_replace>=0 && count>=max_replace) break; }
            int newlen = slen + count*(tlen-flen);
            char *res = xs_malloc(newlen+1); int ri=0; int replaced=0;
            p=s;
            while (1) {
                const char *q=strstr(p,from);
                if (!q || (max_replace>=0 && replaced>=max_replace)) { strcpy(res+ri,p); break; }
                int part=(int)(q-p); memcpy(res+ri,p,part); ri+=part;
                memcpy(res+ri,to,tlen); ri+=tlen;
                p=q+flen; replaced++;
            }
            Value *v=xs_str(res); free(res); return v;
        }
        if (strcmp(method, "chars") == 0 || strcmp(method, "graphemes") == 0) {
            /* codepoint-aware: yield each UTF-8 codepoint as its own str.
               graphemes() is currently an alias (true grapheme clusters
               would require Unicode tables the compiler tries to ship
               without). */
            Value *arr = xs_array_new();
            int j = 0;
            while (j < slen) {
                int cp;
                int n = utf8_decode(s + j, slen - j, &cp);
                if (n <= 0) n = 1;
                array_push(arr->arr, xs_str_n(s + j, n));
                j += n;
            }
            return arr;
        }
        if (strcmp(method, "bytes") == 0 || strcmp(method, "to_bytes") == 0) {
            Value *arr = xs_array_new();
            for (int j=0;j<slen;j++) array_push(arr->arr, xs_int((unsigned char)s[j]));
            return arr;
        }
        if (strcmp(method, "parse_int") == 0 || strcmp(method, "parse") == 0) {
            int base = (argc > 0 && VAL_TAG(args[0]) == XS_INT) ? (int)VAL_INT(args[0]) : 10;
            char *endptr = NULL;
            long long val = strtoll(s, &endptr, base);
            if (endptr == s) return value_incref(XS_NULL_VAL);
            return xs_int(val);
        }
        if (strcmp(method, "parse_float") == 0) {
            char *endp = NULL;
            double fv = strtod(s, &endp);
            if (endp == s) return value_incref(XS_NULL_VAL);
            return xs_float(fv);
        }
        if (strcmp(method, "is_empty") == 0) {
            return slen==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "repeat") == 0) {
            int n2 = (argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (n2 < 0) n2 = 0;
            if (n2 > 0 && slen > (int)(INT_MAX - 1) / n2) return xs_str("");
            char *r = xs_malloc(slen*n2+1); int pos = 0;
            for (int j=0;j<n2;j++) { memcpy(r+pos,s,slen); pos+=slen; }
            r[pos]='\0';
            Value *v=xs_str(r); free(r); return v;
        }
        if (strcmp(method, "join") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_ARRAY) return value_incref(obj);
            /* s.join(arr) → join arr elements with sep=s */
            XSArray *arr2 = args[0]->arr;
            int total=0;
            for (int j=0;j<arr2->len;j++) {
                char *rs=value_str(arr2->items[j]);
                total+=(int)strlen(rs); free(rs);
                if (j<arr2->len-1) total+=slen;
            }
            char *res=xs_malloc(total+1); res[0]='\0';
            for (int j=0;j<arr2->len;j++) {
                char *rs=value_str(arr2->items[j]);
                strcat(res,rs); free(rs);
                if (j<arr2->len-1) strcat(res,s);
            }
            Value *v=xs_str(res); free(res); return v;
        }
        /* substr/slice: str.slice(start, end) or str[i..j] */
        if (strcmp(method, "slice") == 0 || strcmp(method, "substr") == 0 || strcmp(method, "substring") == 0) {
            int start2=0, end2=slen;
            if (argc>0&&VAL_TAG(args[0])==XS_INT) start2=(int)VAL_INT(args[0]);
            if (argc>1&&VAL_TAG(args[1])==XS_INT) end2=(int)VAL_INT(args[1]);
            if (start2<0) start2=slen+start2;
            if (end2<0)   end2=slen+end2;
            if (start2<0) start2=0;
            if (end2>slen) end2=slen;
            return (start2>=end2)?xs_str(""):xs_str_n(s+start2,end2-start2);
        }
        if (strcmp(method, "find") == 0 || strcmp(method, "index_of") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_int(-1);
            const char *pos=strstr(s,args[0]->s);
            return xs_int(pos?pos-s:-1);
        }
        /* to_str / to_string */
        if (strcmp(method, "to_str") == 0 || strcmp(method, "to_string") == 0)
            return value_incref(obj);
        /* trim_start / ltrim: optional char-set arg trims those chars
           instead of whitespace. */
        if (strcmp(method, "trim_start") == 0 || strcmp(method, "ltrim") == 0 || strcmp(method, "trim_left") == 0) {
            const char *cs = (argc > 0 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : NULL;
            int start = 0;
            if (cs) {
                while (start < slen && strchr(cs, s[start])) start++;
            } else {
                while (start < slen && isspace((unsigned char)s[start])) start++;
            }
            return xs_str(s+start);
        }
        /* trim_end / rtrim: same convention */
        if (strcmp(method, "trim_end") == 0 || strcmp(method, "rtrim") == 0 || strcmp(method, "trim_right") == 0) {
            const char *cs = (argc > 0 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : NULL;
            int end = slen - 1;
            if (cs) {
                while (end >= 0 && strchr(cs, s[end])) end--;
            } else {
                while (end >= 0 && isspace((unsigned char)s[end])) end--;
            }
            return xs_str_n(s, end+1);
        }
        /* lines: split by \n */
        if (strcmp(method, "lines") == 0) {
            Value *arr = xs_array_new();
            const char *cur = s;
            while (1) {
                const char *pos = strchr(cur, '\n');
                if (!pos) { array_push(arr->arr, xs_str(cur)); break; }
                array_push(arr->arr, xs_str_n(cur, pos-cur));
                cur = pos+1;
            }
            return arr;
        }
        /* count(substr) */
        if (strcmp(method, "count") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_int(0);
            const char *sub=args[0]->s; int sublen=(int)strlen(sub);
            if (sublen==0) return xs_int(0);
            int cnt=0; const char *p=s;
            while ((p=strstr(p,sub))) { cnt++; p+=sublen; }
            return xs_int(cnt);
        }
        /* title */
        if (strcmp(method, "title") == 0) {
            char *r=xs_strdup(s); int after_space=1;
            for (int j=0;r[j];j++) {
                if (isspace((unsigned char)r[j])) { after_space=1; }
                else if (after_space) { r[j]=toupper((unsigned char)r[j]); after_space=0; }
                else { r[j]=tolower((unsigned char)r[j]); }
            }
            Value *v=xs_str(r); free(r); return v;
        }
        /* capitalize */
        if (strcmp(method, "capitalize") == 0) {
            char *r=xs_strdup(s);
            for (int j=0;r[j];j++) r[j]=(j==0)?toupper((unsigned char)r[j]):tolower((unsigned char)r[j]);
            Value *v=xs_str(r); free(r); return v;
        }
        /* center(width, ch) */
        if (strcmp(method, "center") == 0) {
            int width=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):slen;
            char ch=(argc>1&&VAL_TAG(args[1])==XS_STR&&args[1]->s[0])?args[1]->s[0]:' ';
            if (width<=slen) return value_incref(obj);
            int total=width-slen; int left=total/2; int right=total-left;
            char *r=xs_malloc(width+1);
            for(int j=0;j<left;j++) r[j]=ch;
            memcpy(r+left,s,slen);
            for(int j=0;j<right;j++) r[left+slen+j]=ch;
            r[width]='\0';
            Value *v=xs_str(r); free(r); return v;
        }
        /* pad_left / lpad */
        if (strcmp(method, "pad_left") == 0 || strcmp(method, "lpad") == 0 || strcmp(method, "pad_start") == 0) {
            int width=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):slen;
            char ch=(argc>1 && (VAL_TAG(args[1])==XS_STR || VAL_TAG(args[1])==XS_CHAR) && args[1]->s && args[1]->s[0])?args[1]->s[0]:' ';
            if (width<=slen) return value_incref(obj);
            int pad=width-slen;
            char *r=xs_malloc(width+1);
            for(int j=0;j<pad;j++) r[j]=ch;
            memcpy(r+pad,s,slen); r[width]='\0';
            Value *v=xs_str(r); free(r); return v;
        }
        /* pad_right / rpad */
        if (strcmp(method, "pad_right") == 0 || strcmp(method, "rpad") == 0 || strcmp(method, "pad_end") == 0) {
            int width=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):slen;
            char ch=(argc>1 && (VAL_TAG(args[1])==XS_STR || VAL_TAG(args[1])==XS_CHAR) && args[1]->s && args[1]->s[0])?args[1]->s[0]:' ';
            if (width<=slen) return value_incref(obj);
            int pad=width-slen;
            char *r=xs_malloc(width+1);
            memcpy(r,s,slen);
            for(int j=0;j<pad;j++) r[slen+j]=ch;
            r[width]='\0';
            Value *v=xs_str(r); free(r); return v;
        }
        /* remove_prefix */
        if (strcmp(method, "remove_prefix") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(obj);
            int plen=(int)strlen(args[0]->s);
            if (plen<=slen && strncmp(s,args[0]->s,plen)==0) return xs_str(s+plen);
            return value_incref(obj);
        }
        /* remove_suffix */
        if (strcmp(method, "remove_suffix") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(obj);
            int plen=(int)strlen(args[0]->s);
            if (plen<=slen && strcmp(s+slen-plen,args[0]->s)==0) return xs_str_n(s,slen-plen);
            return value_incref(obj);
        }
        /* is_ascii */
        if (strcmp(method, "is_ascii") == 0) {
            for (int j=0;j<slen;j++) if ((unsigned char)s[j]>=128) return value_incref(XS_FALSE_VAL);
            return value_incref(XS_TRUE_VAL);
        }
        /* is_digit / is_numeric */
        if (strcmp(method, "is_digit") == 0 || strcmp(method, "is_numeric") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            for (int j=0;j<slen;j++) if (!isdigit((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
            return value_incref(XS_TRUE_VAL);
        }
        /* is_alpha */
        if (strcmp(method, "is_alpha") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            for (int j=0;j<slen;j++) if (!isalpha((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
            return value_incref(XS_TRUE_VAL);
        }
        /* is_alnum */
        if (strcmp(method, "is_alnum") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            for (int j=0;j<slen;j++) if (!isalnum((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
            return value_incref(XS_TRUE_VAL);
        }
        /* is_upper */
        if (strcmp(method, "is_upper") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            int has_alpha=0;
            for (int j=0;j<slen;j++) {
                if (isalpha((unsigned char)s[j])) {
                    has_alpha=1;
                    if (islower((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
                }
            }
            return has_alpha?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        /* is_lower */
        if (strcmp(method, "is_lower") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            int has_alpha=0;
            for (int j=0;j<slen;j++) {
                if (isalpha((unsigned char)s[j])) {
                    has_alpha=1;
                    if (isupper((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
                }
            }
            return has_alpha?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        /* is_space: every byte is a whitespace character (empty string is false). */
        if (strcmp(method, "is_space") == 0 || strcmp(method, "is_whitespace") == 0) {
            if (slen==0) return value_incref(XS_FALSE_VAL);
            for (int j=0;j<slen;j++) if (!isspace((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
            return value_incref(XS_TRUE_VAL);
        }
        /* swap_case: flip case for each ASCII alpha; non-alpha unchanged. */
        if (strcmp(method, "swap_case") == 0) {
            char *out = xs_malloc(slen+1);
            for (int j=0;j<slen;j++) {
                unsigned char c = (unsigned char)s[j];
                if      (isupper(c)) out[j] = (char)tolower(c);
                else if (islower(c)) out[j] = (char)toupper(c);
                else                 out[j] = (char)c;
            }
            out[slen] = '\0';
            Value *r = xs_str(out); free(out); return r;
        }
        /* to_int / as_int */
        if (strcmp(method, "to_int") == 0 || strcmp(method, "as_int") == 0) {
            return xs_int(atoll(s));
        }
        /* to_float / as_float */
        if (strcmp(method, "to_float") == 0 || strcmp(method, "as_float") == 0) {
            return xs_float(atof(s));
        }
        /* char_at */
        if (strcmp(method, "char_at") == 0) {
            int idx=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (idx<0) idx=slen+idx;
            if (idx<0||idx>=slen) return value_incref(XS_NULL_VAL);
            return xs_str_n(s+idx,1);
        }
        /* reverse / reversed: walk codepoints backward so multi-byte
           UTF-8 sequences stay intact in the output. Byte-level reverse
           used to produce invalid UTF-8 like `oll<bad bytes>h`. */
        if (strcmp(method, "reverse") == 0 || strcmp(method, "reversed") == 0) {
            char *out = xs_malloc((size_t)slen + 1);
            int wpos = 0;
            int i2 = slen;
            while (i2 > 0) {
                int start = i2 - 1;
                while (start > 0 && (((unsigned char)s[start] & 0xC0) == 0x80)) start--;
                int n = i2 - start;
                memcpy(out + wpos, s + start, (size_t)n);
                wpos += n;
                i2 = start;
            }
            out[wpos] = '\0';
            Value *v = xs_str(out); free(out); return v;
        }
        /* truncate(max_len, suffix) */
        if (strcmp(method, "truncate") == 0) {
            int maxlen=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):slen;
            const char *suf=(argc>1&&VAL_TAG(args[1])==XS_STR)?args[1]->s:"...";
            if (slen<=maxlen) return value_incref(obj);
            int suflen=(int)strlen(suf);
            int cutlen=maxlen-suflen; if (cutlen<0) cutlen=0;
            char *r=xs_malloc(maxlen+1);
            memcpy(r,s,cutlen); memcpy(r+cutlen,suf,suflen); r[maxlen]='\0';
            Value *v=xs_str(r); free(r); return v;
        }
        /* rfind */
        if (strcmp(method, "rfind") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_int(-1);
            int sublen=(int)strlen(args[0]->s);
            int last=-1;
            const char *p=s;
            while ((p=strstr(p,args[0]->s))) { last=(int)(p-s); p+=sublen; }
            return xs_int(last);
        }
        /* split_at */
        if (strcmp(method, "split_at") == 0) {
            int idx=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (idx<0) idx=slen+idx;
            if (idx<0) idx=0;
            if (idx>slen) idx=slen;
            Value *tup=xs_tuple_new();
            array_push(tup->arr, xs_str_n(s,idx));
            array_push(tup->arr, xs_str(s+idx));
            return tup;
        }
        /* to_chars (alias for chars) */
        if (strcmp(method, "to_chars") == 0) {
            Value *arr = xs_array_new();
            for (int j=0;j<slen;j++) array_push(arr->arr, xs_str_n(s+j,1));
            return arr;
        }
        /* format: "Hello {} and {}".format(a, b): substitute {} placeholders */
        if (strcmp(method, "format") == 0) {
            /* {} consumes args in order; {N} pulls a specific
               positional arg (allowing repeats and reordering). {{
               and }} are the literal-brace escapes. Useful on raw
               strings so the regular interpolation pass doesn't eat
               the placeholders first. */
            int cap = slen + 256;
            char *res = xs_malloc(cap); int ri = 0; int ai = 0;
            for (int j = 0; j < slen; j++) {
                if (s[j] == '{' && j+1 < slen && s[j+1] == '{') {
                    if (ri + 2 >= cap) { cap *= 2; res = realloc(res, cap); }
                    res[ri++] = '{'; j++;
                } else if (s[j] == '}' && j+1 < slen && s[j+1] == '}') {
                    if (ri + 2 >= cap) { cap *= 2; res = realloc(res, cap); }
                    res[ri++] = '}'; j++;
                } else if (s[j] == '{' && j+1 < slen) {
                    /* find matching '}' so we can read an optional
                       index. Stay at depth 1: we don't recurse. */
                    int k = j + 1;
                    while (k < slen && s[k] != '}') k++;
                    if (k >= slen) {
                        if (ri + 2 >= cap) { cap *= 2; res = realloc(res, cap); }
                        res[ri++] = s[j];
                        continue;
                    }
                    int idx = -1;
                    if (k > j + 1) {
                        char ibuf[32]; int il = k - j - 1;
                        if (il < (int)sizeof(ibuf)) {
                            memcpy(ibuf, s + j + 1, il); ibuf[il] = '\0';
                            char *endp = NULL;
                            long v = strtol(ibuf, &endp, 10);
                            if (endp && *endp == '\0') idx = (int)v;
                        }
                    }
                    if (idx < 0) idx = ai++;
                    char *rep = (idx >= 0 && idx < argc) ?
                                 value_str(args[idx]) : xs_strdup("");
                    int rlen = (int)strlen(rep);
                    while (ri + rlen + 1 >= cap) { cap *= 2; res = realloc(res, cap); }
                    memcpy(res + ri, rep, rlen); ri += rlen;
                    free(rep);
                    j = k;
                } else {
                    if (ri + 2 >= cap) { cap *= 2; res = realloc(res, cap); }
                    res[ri++] = s[j];
                }
            }
            res[ri] = '\0';
            Value *v = xs_str(res); free(res); return v;
        }
        /* byte_len: number of bytes in string */
        if (strcmp(method, "byte_len") == 0) {
            return xs_int(slen);
        }
        /* char_len: number of unicode codepoints */
        if (strcmp(method, "char_len") == 0) {
            return xs_int(utf8_strlen(s, slen));
        }
        /* from_chars: "".from_chars([...]): build string from char array */
        if (strcmp(method, "from_chars") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_str("");
            XSArray *arr2 = args[0]->arr;
            int total = 0;
            for (int j = 0; j < arr2->len; j++) {
                if (VAL_TAG(arr2->items[j]) == XS_STR) total += (int)strlen(arr2->items[j]->s);
                else total += 1;
            }
            char *res = xs_malloc(total + 1); int ri = 0;
            for (int j = 0; j < arr2->len; j++) {
                if (VAL_TAG(arr2->items[j]) == XS_STR) {
                    int l = (int)strlen(arr2->items[j]->s);
                    memcpy(res + ri, arr2->items[j]->s, l); ri += l;
                } else if (VAL_TAG(arr2->items[j]) == XS_INT) {
                    res[ri++] = (char)VAL_INT(arr2->items[j]);
                }
            }
            res[ri] = '\0';
            Value *v = xs_str(res); free(res); return v;
        }
        if (strcmp(method, "is_a") == 0) {
            if (argc >= 1 && VAL_TAG(args[0]) == XS_STR) {
                return xs_bool(strcmp(args[0]->s, "str") == 0 ||
                               strcmp(args[0]->s, "String") == 0);
            }
            return value_incref(XS_FALSE_VAL);
        }
    }

    // --- array methods
    if (VAL_TAG(obj) == XS_ARRAY || VAL_TAG(obj) == XS_TUPLE) {
        XSArray *arr = obj->arr;
        if (strcmp(method, "len") == 0 || strcmp(method, "size") == 0 ||
            strcmp(method, "length") == 0)
            return xs_int(arr->len);
        if (strcmp(method, "push") == 0 || strcmp(method, "append") == 0) {
            for (int j=0;j<argc;j++) array_push(arr, value_incref(args[j]));
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "pop") == 0) {
            if (arr->len == 0) return value_incref(XS_NULL_VAL);
            Value *v = arr->items[--arr->len]; /* consume refcount */
            return v;
        }
        if (strcmp(method, "first") == 0 || strcmp(method, "head") == 0) {
            return arr->len>0?value_incref(arr->items[0]):value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "last") == 0) {
            return arr->len>0?value_incref(arr->items[arr->len-1]):value_incref(XS_NULL_VAL);
        }
        /* Haskell-flavour pair to head/last: tail = everything after the
           first element, init = everything except the last. Empty arrays
           give back an empty array rather than throwing -- the user can
           combine with .first() / .last() to detect that case. */
        if (strcmp(method, "tail") == 0) {
            Value *res = xs_array_new();
            for (int j = 1; j < arr->len; j++)
                array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        if (strcmp(method, "init") == 0) {
            Value *res = xs_array_new();
            for (int j = 0; j + 1 < arr->len; j++)
                array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        if (strcmp(method, "get") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_INT)
                return argc >= 2 ? value_incref(args[1]) : value_incref(XS_NULL_VAL);
            int gi = (int)VAL_INT(args[0]);
            if (gi < 0) gi = arr->len + gi;
            if (gi < 0 || gi >= arr->len)
                return argc >= 2 ? value_incref(args[1]) : value_incref(XS_NULL_VAL);
            return value_incref(arr->items[gi]);
        }
        if (strcmp(method, "is_empty") == 0) {
            return arr->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "contains") == 0 || strcmp(method, "includes") == 0) {
            if (argc<1) return value_incref(XS_FALSE_VAL);
            Value *arg = args[0];
            int is_pred = arg && (VAL_TAG(arg)==XS_CLOSURE ||
                                  VAL_TAG(arg)==XS_FUNC ||
                                  VAL_TAG(arg)==XS_NATIVE);
            if (is_pred) {
                for (int j=0;j<arr->len;j++) {
                    Value *r = call_value(i, arg, &arr->items[j], 1, method);
                    int t = value_truthy(r);
                    value_decref(r);
                    if (t) return value_incref(XS_TRUE_VAL);
                }
                return value_incref(XS_FALSE_VAL);
            }
            for (int j=0;j<arr->len;j++)
                if (value_equal(arr->items[j],args[0]))
                    return value_incref(XS_TRUE_VAL);
            return value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "map") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res = xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1] = {arr->items[j]};
                Value *r = call_value(i, args[0], a, 1, "map");
                if (!i->cf.signal) array_push(res->arr, r);
                else { value_decref(r); break; }
            }
            return res;
        }
        if (strcmp(method, "filter") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res = xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1] = {arr->items[j]};
                Value *r = call_value(i, args[0], a, 1, "filter");
                int keep = value_truthy(r); value_decref(r);
                if (!i->cf.signal && keep) array_push(res->arr, value_incref(arr->items[j]));
                if (i->cf.signal) break;
            }
            return res;
        }
        if (strcmp(method, "reduce") == 0 || strcmp(method, "fold") == 0) {
            /* reduce / fold accept either order: (init, fn) or (fn, init).
               Detect by looking at which arg is callable. reduce(fn) with
               one arg uses the first element as the initial accumulator. */
            if (argc<1) return value_incref(XS_NULL_VAL);
            Value *fn_val, *init;
            int start_idx = 0;
            int is_fn_0 = argc >= 1 && args[0] &&
                          (VAL_TAG(args[0]) == XS_FUNC ||
                           VAL_TAG(args[0]) == XS_NATIVE ||
                           VAL_TAG(args[0]) == XS_CLOSURE);
            int is_fn_1 = argc >= 2 && args[1] &&
                          (VAL_TAG(args[1]) == XS_FUNC ||
                           VAL_TAG(args[1]) == XS_NATIVE ||
                           VAL_TAG(args[1]) == XS_CLOSURE);
            if (strcmp(method, "fold") == 0 && argc >= 2) {
                init = args[0]; fn_val = args[1];
            } else if (argc >= 2 && is_fn_1 && !is_fn_0) {
                init = args[0]; fn_val = args[1];
            } else if (argc >= 2) {
                fn_val = args[0]; init = args[1];
            } else {
                fn_val = args[0];
                if (arr->len == 0) return value_incref(XS_NULL_VAL);
                init = arr->items[0];
                start_idx = 1;
            }
            Value *acc = value_incref(init);
            for (int j=start_idx;j<arr->len;j++) {
                Value *a[2] = {acc, arr->items[j]};
                Value *r = call_value(i, fn_val, a, 2, "fold");
                value_decref(acc); acc = r;
                if (i->cf.signal) break;
            }
            return acc;
        }
        if (strcmp(method, "find") == 0) {
            if (argc<1) return value_incref(XS_NULL_VAL);
            for (int j=0;j<arr->len;j++) {
                Value *a[1] = {arr->items[j]};
                Value *r = call_value(i, args[0], a, 1, "find");
                int ok = value_truthy(r); value_decref(r);
                if (ok) return value_incref(arr->items[j]);
            }
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "any") == 0) {
            if (argc<1) return value_incref(XS_FALSE_VAL);
            for (int j=0;j<arr->len;j++) {
                Value *a[1] = {arr->items[j]};
                Value *r = call_value(i, args[0], a, 1, "any");
                int ok = value_truthy(r); value_decref(r);
                if (ok) return value_incref(XS_TRUE_VAL);
            }
            return value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "all") == 0) {
            if (argc<1) return value_incref(XS_TRUE_VAL);
            for (int j=0;j<arr->len;j++) {
                Value *a[1] = {arr->items[j]};
                Value *r = call_value(i, args[0], a, 1, "all");
                int ok = value_truthy(r); value_decref(r);
                if (!ok) return value_incref(XS_FALSE_VAL);
            }
            return value_incref(XS_TRUE_VAL);
        }
        if (strcmp(method, "sort") == 0) {
            /* O(n log n) sort. The previous shape was an insertion
               sort that turned a 1k-element list into a million
               compare-callback invocations; merge-sort with cb
               below is roughly 50x faster on big lists with a user
               comparator and matches qsort on the cb-less path. */
            Value *cmp_fn = (argc > 0) ? args[0] : NULL;
            int n = arr->len;
            if (n > 1) {
                Value **buf = xs_malloc((size_t)n * sizeof(Value*));
                /* bottom-up merge sort */
                for (int width = 1; width < n; width *= 2) {
                    for (int s = 0; s < n; s += 2 * width) {
                        int lo = s;
                        int mid = (s + width < n) ? s + width : n;
                        int hi  = (s + 2 * width < n) ? s + 2 * width : n;
                        int p = lo, q = mid, k = lo;
                        while (p < mid && q < hi) {
                            int cmp;
                            if (cmp_fn) {
                                Value *ca[2] = { arr->items[p], arr->items[q] };
                                Value *r = call_value(i, cmp_fn, ca, 2, "sort");
                                cmp = (VAL_TAG(r) == XS_INT) ? (int)VAL_INT(r) : 0;
                                value_decref(r);
                            } else {
                                cmp = value_cmp(arr->items[p], arr->items[q]);
                            }
                            if (cmp <= 0) buf[k++] = arr->items[p++];
                            else          buf[k++] = arr->items[q++];
                        }
                        while (p < mid) buf[k++] = arr->items[p++];
                        while (q < hi)  buf[k++] = arr->items[q++];
                        for (int j = lo; j < hi; j++) arr->items[j] = buf[j];
                    }
                }
                free(buf);
            }
            return value_incref(obj);
        }
        if (strcmp(method, "sorted") == 0) {
            Value *copy = xs_array_new();
            for (int j=0;j<arr->len;j++) array_push(copy->arr, value_incref(arr->items[j]));
            /* sort the copy */
            Value *sort_args[1] = {argc>0?args[0]:NULL};
            return eval_method(i, copy, "sort", sort_args, argc>0?1:0);
        }
        if (strcmp(method, "sort_by") == 0) {
            /* sort_by(key_fn): sort a copy by extracted key. Same
               complexity rationale as sort: merge-sort over an
               extracted keys array, so each element pays exactly one
               key-fn call instead of N²/2 comparator calls. */
            if (argc < 1) return value_incref(XS_NULL_VAL);
            Value *key_fn = args[0];
            Value *copy = xs_array_new();
            for (int j=0;j<arr->len;j++) array_push(copy->arr, value_incref(arr->items[j]));
            XSArray *ca = copy->arr;
            int n = ca->len;
            Value **keys = xs_malloc((size_t)n * sizeof(Value*));
            for (int j = 0; j < n; j++) {
                Value *arg[1] = { ca->items[j] };
                keys[j] = call_value(i, key_fn, arg, 1, "sort_by");
            }
            if (n > 1) {
                Value **kbuf = xs_malloc((size_t)n * sizeof(Value*));
                Value **ibuf = xs_malloc((size_t)n * sizeof(Value*));
                for (int width = 1; width < n; width *= 2) {
                    for (int s = 0; s < n; s += 2 * width) {
                        int lo = s;
                        int mid = (s + width < n) ? s + width : n;
                        int hi  = (s + 2 * width < n) ? s + 2 * width : n;
                        int p = lo, q = mid, k = lo;
                        while (p < mid && q < hi) {
                            if (value_cmp(keys[p], keys[q]) <= 0) {
                                kbuf[k] = keys[p]; ibuf[k] = ca->items[p]; k++; p++;
                            } else {
                                kbuf[k] = keys[q]; ibuf[k] = ca->items[q]; k++; q++;
                            }
                        }
                        while (p < mid) { kbuf[k] = keys[p]; ibuf[k] = ca->items[p]; k++; p++; }
                        while (q < hi)  { kbuf[k] = keys[q]; ibuf[k] = ca->items[q]; k++; q++; }
                        for (int j = lo; j < hi; j++) { keys[j] = kbuf[j]; ca->items[j] = ibuf[j]; }
                    }
                }
                free(kbuf); free(ibuf);
            }
            for (int j=0;j<ca->len;j++) value_decref(keys[j]);
            free(keys);
            return copy;
        }
        if (strcmp(method, "reverse") == 0) {
            for (int l2=0,r2=arr->len-1; l2<r2; l2++,r2--) {
                Value *tmp = arr->items[l2];
                arr->items[l2]=arr->items[r2]; arr->items[r2]=tmp;
            }
            return value_incref(obj);
        }
        if (strcmp(method, "reversed") == 0) {
            Value *copy = xs_array_new();
            for (int j=arr->len-1;j>=0;j--) array_push(copy->arr, value_incref(arr->items[j]));
            return copy;
        }
        if (strcmp(method, "join") == 0) {
            const char *sep = (argc>0&&VAL_TAG(args[0])==XS_STR)?args[0]->s:"";
            int seplen=(int)strlen(sep), total=0;
            char **strs = xs_malloc(arr->len*sizeof(char*));
            for (int j=0;j<arr->len;j++) {
                strs[j]=value_str(arr->items[j]);
                total+=(int)strlen(strs[j]);
                if (j<arr->len-1) total+=seplen;
            }
            char *res=xs_malloc(total+1); res[0]='\0';
            for (int j=0;j<arr->len;j++) {
                strcat(res,strs[j]); free(strs[j]);
                if (j<arr->len-1) strcat(res,sep);
            }
            free(strs);
            Value *v=xs_str(res); free(res); return v;
        }
        if (strcmp(method, "slice") == 0) {
            int start2=0, end2=arr->len;
            if (argc>0&&VAL_TAG(args[0])==XS_INT) start2=(int)VAL_INT(args[0]);
            if (argc>1&&VAL_TAG(args[1])==XS_INT) end2=(int)VAL_INT(args[1]);
            if (start2<0) start2=arr->len+start2;
            if (end2<0)   end2=arr->len+end2;
            if (start2<0) start2=0;
            if (end2>arr->len) end2=arr->len;
            Value *res=xs_array_new();
            for (int j=start2;j<end2;j++) array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        if (strcmp(method, "index_of") == 0 || strcmp(method, "find_index") == 0) {
            if (argc<1) return xs_int(-1);
            /* index_of(value) probes for equality; find_index(pred)
               runs the callable and returns the first truthy index.
               One method does both, picked by the arg's tag. */
            Value *arg = args[0];
            int is_pred = arg && (VAL_TAG(arg)==XS_CLOSURE ||
                                  VAL_TAG(arg)==XS_FUNC ||
                                  VAL_TAG(arg)==XS_NATIVE);
            if (is_pred) {
                for (int j=0;j<arr->len;j++) {
                    Value *r = call_value(i, arg, &arr->items[j], 1, method);
                    int t = value_truthy(r);
                    value_decref(r);
                    if (t) return xs_int(j);
                }
                return xs_int(-1);
            }
            for (int j=0;j<arr->len;j++)
                if (value_equal(arr->items[j],args[0])) return xs_int(j);
            return xs_int(-1);
        }
        if (strcmp(method, "flatten") == 0 || strcmp(method, "flat") == 0) {
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                if (VAL_TAG(arr->items[j])==XS_ARRAY) {
                    XSArray *inner=arr->items[j]->arr;
                    for (int k=0;k<inner->len;k++) array_push(res->arr, value_incref(inner->items[k]));
                } else {
                    array_push(res->arr, value_incref(arr->items[j]));
                }
            }
            return res;
        }
        if (strcmp(method, "step") == 0 || strcmp(method, "stride") == 0) {
            int64_t n = (argc>=1&&VAL_TAG(args[0])==XS_INT)?VAL_INT(args[0])
                      : (argc>=1&&VAL_TAG(args[0])==XS_FLOAT)?(int64_t)args[0]->f : 1;
            if (n <= 0) {
                Value *err = xs_error_new("ValueError",
                    "step() requires a positive integer", NULL);
                if (i->cf.value) value_decref(i->cf.value);
                i->cf.signal = CF_THROW;
                i->cf.value  = err;
                return value_incref(XS_NULL_VAL);
            }
            Value *res = xs_array_new();
            for (int j = 0; j < arr->len; j += (int)n)
                array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        if (strcmp(method, "chunks") == 0) {
            int64_t sz = (argc>=1&&VAL_TAG(args[0])==XS_INT)?VAL_INT(args[0])
                       : (argc>=1&&VAL_TAG(args[0])==XS_FLOAT)?(int64_t)args[0]->f : 0;
            if (sz <= 0) {
                Value *err = xs_error_new("ValueError",
                    "chunks() requires a positive integer size", NULL);
                if (i->cf.value) value_decref(i->cf.value);
                i->cf.signal = CF_THROW;
                i->cf.value  = err;
                return value_incref(XS_NULL_VAL);
            }
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j+=(int)sz) {
                Value *chunk=xs_array_new();
                int end=j+(int)sz; if (end>arr->len) end=arr->len;
                for (int k=j;k<end;k++) array_push(chunk->arr, value_incref(arr->items[k]));
                array_push(res->arr, chunk);
            }
            return res;
        }
        if (strcmp(method, "zip") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_ARRAY) return value_incref(obj);
            XSArray *other=args[0]->arr;
            int n2=arr->len<other->len?arr->len:other->len;
            Value *res=xs_array_new();
            for (int j=0;j<n2;j++) {
                Value *tup=xs_tuple_new();
                array_push(tup->arr, value_incref(arr->items[j]));
                array_push(tup->arr, value_incref(other->items[j]));
                array_push(res->arr, tup);
            }
            return res;
        }
        if (strcmp(method, "enumerate") == 0) {
            int64_t start_idx = 0;
            if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) start_idx = VAL_INT(args[0]);
            else if (argc >= 1 && VAL_TAG(args[0]) == XS_FLOAT) start_idx = (int64_t)args[0]->f;
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *tup=xs_tuple_new();
                array_push(tup->arr, xs_int(start_idx + j));
                array_push(tup->arr, value_incref(arr->items[j]));
                array_push(res->arr, tup);
            }
            return res;
        }
        if (strcmp(method, "sum") == 0) {
            int64_t si=0; double sf=0; int is_f=0;
            for (int j=0;j<arr->len;j++) {
                if (VAL_TAG(arr->items[j])==XS_FLOAT){is_f=1;sf+=arr->items[j]->f;}
                else if(VAL_TAG(arr->items[j])==XS_INT) si+=VAL_INT(arr->items[j]);
            }
            return is_f?xs_float(sf+(double)si):xs_int(si);
        }
        if (strcmp(method, "to_map") == 0) {
            /* Array of (key, value) tuples -> map. zip().to_map() is
               the natural way to build a map from two parallel arrays. */
            Value *res = xs_map_new();
            for (int j = 0; j < arr->len; j++) {
                Value *p = arr->items[j];
                if (!p || (VAL_TAG(p) != XS_TUPLE && VAL_TAG(p) != XS_ARRAY) ||
                    !p->arr || p->arr->len < 2) continue;
                Value *k = p->arr->items[0];
                if (!k) continue;
                char *ks = value_str(k);
                map_set(res->map, ks, value_incref(p->arr->items[1]));
                free(ks);
            }
            return res;
        }
        if (strcmp(method, "windows") == 0) {
            /* arr.windows(n): array of overlapping size-n slices.
               [1,2,3,4].windows(2) -> [[1,2], [2,3], [3,4]]. */
            int wn = (argc > 0 && VAL_TAG(args[0]) == XS_INT) ? (int)VAL_INT(args[0]) : 2;
            if (wn < 1) wn = 1;
            Value *res = xs_array_new();
            for (int j = 0; j + wn <= arr->len; j++) {
                Value *win = xs_array_new();
                for (int k = 0; k < wn; k++)
                    array_push(win->arr, value_incref(arr->items[j + k]));
                array_push(res->arr, win);
            }
            return res;
        }
        if (strcmp(method, "pairwise") == 0) {
            /* Adjacent (a, b) pairs as tuples. */
            Value *res = xs_array_new();
            for (int j = 0; j + 1 < arr->len; j++) {
                Value *t = xs_tuple_new();
                array_push(t->arr, value_incref(arr->items[j]));
                array_push(t->arr, value_incref(arr->items[j + 1]));
                array_push(res->arr, t);
            }
            return res;
        }
        if (strcmp(method, "avg") == 0 || strcmp(method, "mean") == 0) {
            if (arr->len == 0) return xs_float(0.0);
            double s = 0;
            for (int j = 0; j < arr->len; j++) {
                if (VAL_TAG(arr->items[j]) == XS_FLOAT) s += arr->items[j]->f;
                else if (VAL_TAG(arr->items[j]) == XS_INT) s += (double)VAL_INT(arr->items[j]);
            }
            return xs_float(s / (double)arr->len);
        }
        if (strcmp(method, "cycle") == 0) {
            int n = (argc > 0 && VAL_TAG(args[0]) == XS_INT) ? (int)VAL_INT(args[0]) : 1;
            if (n < 0) n = 0;
            Value *res = xs_array_new();
            for (int rep = 0; rep < n; rep++)
                for (int j = 0; j < arr->len; j++)
                    array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        if (strcmp(method, "count_by") == 0) {
            if (argc < 1) return xs_map_new();
            Value *res = xs_map_new();
            for (int j = 0; j < arr->len; j++) {
                Value *a[1] = { arr->items[j] };
                Value *k = call_value(i, args[0], a, 1, "count_by");
                if (i->cf.signal) { value_decref(k); break; }
                char *ks = value_str(k);
                Value *cur = map_get(res->map, ks);
                int64_t c = (cur && VAL_TAG(cur) == XS_INT) ? VAL_INT(cur) : 0;
                Value *nv = xs_int(c + 1);
                map_set(res->map, ks, nv);
                value_decref(nv);
                free(ks);
                value_decref(k);
            }
            return res;
        }
        if (strcmp(method, "min") == 0) {
            if (arr->len==0) return value_incref(XS_NULL_VAL);
            Value *m=arr->items[0];
            for (int j=1;j<arr->len;j++) if (value_cmp(arr->items[j],m)<0) m=arr->items[j];
            return value_incref(m);
        }
        if (strcmp(method, "max") == 0) {
            if (arr->len==0) return value_incref(XS_NULL_VAL);
            Value *m=arr->items[0];
            for (int j=1;j<arr->len;j++) if (value_cmp(arr->items[j],m)>0) m=arr->items[j];
            return value_incref(m);
        }
        if (strcmp(method, "remove") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_INT) return value_incref(XS_NULL_VAL);
            int idx=(int)VAL_INT(args[0]);
            if (idx<0) idx=arr->len+idx;
            if (idx<0||idx>=arr->len) return value_incref(XS_NULL_VAL);
            Value *v=arr->items[idx];
            for (int j=idx;j<arr->len-1;j++) arr->items[j]=arr->items[j+1];
            arr->len--;
            return v; /* consume refcount */
        }
        if (strcmp(method, "insert") == 0) {
            if (argc<2) return value_incref(XS_NULL_VAL);
            int idx=(int)((VAL_TAG(args[0])==XS_INT)?VAL_INT(args[0]):0);
            if (idx<0) idx=arr->len+idx;
            if (idx<0) idx=0;
            if (idx>arr->len) idx=arr->len;
            /* grow */
            if (arr->len>=arr->cap) {
                arr->cap=arr->cap?arr->cap*2:4;
                arr->items=xs_realloc(arr->items,arr->cap*sizeof(Value*));
            }
            for (int j=arr->len;j>idx;j--) arr->items[j]=arr->items[j-1];
            arr->items[idx]=value_incref(args[1]);
            arr->len++;
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "concat") == 0 || strcmp(method, "extend") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) array_push(res->arr, value_incref(arr->items[j]));
            for (int j=0;j<argc;j++) {
                if (VAL_TAG(args[j])==XS_ARRAY) {
                    XSArray *other=args[j]->arr;
                    for (int k=0;k<other->len;k++) array_push(res->arr, value_incref(other->items[k]));
                }
            }
            return res;
        }
        if (strcmp(method, "clone") == 0 || strcmp(method, "copy") == 0) {
            Value *copy=xs_array_new();
            for (int j=0;j<arr->len;j++) array_push(copy->arr, value_incref(arr->items[j]));
            return copy;
        }
        /* for_each(fn) */
        if (strcmp(method, "for_each") == 0 || strcmp(method, "each") == 0) {
            if (argc<1) return value_incref(XS_NULL_VAL);
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *r=call_value(i, args[0], a, 1, "for_each");
                value_decref(r);
                if (i->cf.signal) break;
            }
            return value_incref(XS_NULL_VAL);
        }
        /* count(fn_or_val) */
        if (strcmp(method, "count") == 0) {
            if (argc<1) return xs_int(arr->len);
            int64_t cnt=0;
            int is_callable = (VAL_TAG(args[0])==XS_FUNC||VAL_TAG(args[0])==XS_NATIVE
                              || VAL_TAG(args[0])==XS_CLOSURE);
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                if (is_callable) {
                    Value *r=call_value(i, args[0], a, 1, "count");
                    int ok=value_truthy(r); value_decref(r);
                    if (!i->cf.signal && ok) cnt++;
                } else {
                    if (value_equal(arr->items[j],args[0])) cnt++;
                }
                if (i->cf.signal) break;
            }
            return xs_int(cnt);
        }
        /* take(n) */
        if (strcmp(method, "take") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (n2>arr->len) n2=arr->len;
            Value *res=xs_array_new();
            for (int j=0;j<n2;j++) array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        /* skip(n) / drop(n) */
        if (strcmp(method, "skip") == 0 || strcmp(method, "drop") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (n2<0) n2=0;
            if (n2>arr->len) n2=arr->len;
            Value *res=xs_array_new();
            for (int j=n2;j<arr->len;j++) array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        /* chunk(n) */
        if (strcmp(method, "chunk") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):1;
            if (n2<1) n2=1;
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j+=n2) {
                Value *chunk=xs_array_new();
                for (int k=j;k<j+n2&&k<arr->len;k++) array_push(chunk->arr, value_incref(arr->items[k]));
                array_push(res->arr, chunk);
            }
            return res;
        }
        /* group_by(key_fn) */
        if (strcmp(method, "group_by") == 0) {
            if (argc<1) return value_incref(XS_NULL_VAL);
            Value *res=xs_map_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *k=call_value(i, args[0], a, 1, "group_by");
                if (i->cf.signal) { value_decref(k); break; }
                char *ks=value_str(k); value_decref(k);
                Value *bucket=map_get(res->map, ks);
                if (!bucket) {
                    bucket=xs_array_new();
                    map_set(res->map, ks, bucket);
                    value_decref(bucket);
                    bucket=map_get(res->map, ks);
                }
                array_push(bucket->arr, value_incref(arr->items[j]));
                free(ks);
            }
            return res;
        }
        /* partition(pred) */
        if (strcmp(method, "partition") == 0) {
            if (argc<1) return value_incref(XS_NULL_VAL);
            Value *trues=xs_array_new(), *falses=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *r=call_value(i, args[0], a, 1, "partition");
                int ok=value_truthy(r); value_decref(r);
                if (!i->cf.signal) {
                    if (ok) array_push(trues->arr, value_incref(arr->items[j]));
                    else    array_push(falses->arr, value_incref(arr->items[j]));
                }
                if (i->cf.signal) break;
            }
            Value *res=xs_tuple_new();
            array_push(res->arr, trues); array_push(res->arr, falses);
            return res;
        }
        /* window(n) */
        if (strcmp(method, "window") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):1;
            if (n2<1) n2=1;
            Value *res=xs_array_new();
            for (int j=0;j+n2<=arr->len;j++) {
                Value *win=xs_array_new();
                for (int k=j;k<j+n2;k++) array_push(win->arr, value_incref(arr->items[k]));
                array_push(res->arr, win);
            }
            return res;
        }
        /* sum_by(fn) */
        if (strcmp(method, "sum_by") == 0) {
            if (argc<1) return xs_int(0);
            double sf=0; int64_t si=0; int is_f=0;
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *r=call_value(i, args[0], a, 1, "sum_by");
                if (!i->cf.signal) {
                    if (VAL_TAG(r)==XS_FLOAT){is_f=1;sf+=r->f;}
                    else if(VAL_TAG(r)==XS_INT) si+=VAL_INT(r);
                }
                value_decref(r);
                if (i->cf.signal) break;
            }
            return is_f?xs_float(sf+(double)si):xs_int(si);
        }
        /* min_by(fn) */
        if (strcmp(method, "min_by") == 0) {
            if (argc<1||arr->len==0) return value_incref(XS_NULL_VAL);
            Value *best=arr->items[0];
            Value *best_key; { Value *a[1]={best}; best_key=call_value(i, args[0], a, 1, "min_by"); }
            for (int j=1;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *k=call_value(i, args[0], a, 1, "min_by");
                if (!i->cf.signal && value_cmp(k, best_key)<0) {
                    value_decref(best_key); best_key=k; best=arr->items[j];
                } else value_decref(k);
                if (i->cf.signal) break;
            }
            value_decref(best_key);
            return value_incref(best);
        }
        /* max_by(fn) */
        if (strcmp(method, "max_by") == 0) {
            if (argc<1||arr->len==0) return value_incref(XS_NULL_VAL);
            Value *best=arr->items[0];
            Value *best_key; { Value *a[1]={best}; best_key=call_value(i, args[0], a, 1, "max_by"); }
            for (int j=1;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *k=call_value(i, args[0], a, 1, "max_by");
                if (!i->cf.signal && value_cmp(k, best_key)>0) {
                    value_decref(best_key); best_key=k; best=arr->items[j];
                } else value_decref(k);
                if (i->cf.signal) break;
            }
            value_decref(best_key);
            return value_incref(best);
        }
        /* zip_with(other, fn) */
        if (strcmp(method, "zip_with") == 0) {
            if (argc<2||VAL_TAG(args[0])!=XS_ARRAY) return value_incref(XS_NULL_VAL);
            XSArray *other=args[0]->arr;
            int n2=arr->len<other->len?arr->len:other->len;
            Value *res=xs_array_new();
            for (int j=0;j<n2;j++) {
                Value *a[2]={arr->items[j], other->items[j]};
                Value *r=call_value(i, args[1], a, 2, "zip_with");
                if (!i->cf.signal) array_push(res->arr, r); else { value_decref(r); break; }
            }
            return res;
        }
        /* intersperse(val) */
        if (strcmp(method, "intersperse") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                array_push(res->arr, value_incref(arr->items[j]));
                if (j<arr->len-1) array_push(res->arr, value_incref(args[0]));
            }
            return res;
        }
        /* rotate(n) */
        if (strcmp(method, "rotate") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
            if (arr->len==0) return xs_array_new();
            n2=((n2%arr->len)+arr->len)%arr->len;
            Value *res=xs_array_new();
            for (int j=n2;j<arr->len;j++) array_push(res->arr, value_incref(arr->items[j]));
            for (int j=0;j<n2;j++) array_push(res->arr, value_incref(arr->items[j]));
            return res;
        }
        /* take_while(pred) */
        if (strcmp(method, "take_while") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *r=call_value(i, args[0], a, 1, "take_while");
                int ok=value_truthy(r); value_decref(r);
                if (!ok || i->cf.signal) break;
                array_push(res->arr, value_incref(arr->items[j]));
            }
            return res;
        }
        /* drop_while(pred) */
        if (strcmp(method, "drop_while") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_array_new();
            int dropping=1;
            for (int j=0;j<arr->len;j++) {
                if (dropping) {
                    Value *a[1]={arr->items[j]};
                    Value *r=call_value(i, args[0], a, 1, "drop_while");
                    int ok=value_truthy(r); value_decref(r);
                    if (!ok || i->cf.signal) dropping=0;
                    if (i->cf.signal) break;
                    if (dropping) continue;
                }
                array_push(res->arr, value_incref(arr->items[j]));
            }
            return res;
        }
        /* scan(init, fn) */
        if (strcmp(method, "scan") == 0) {
            if (argc<2) return value_incref(XS_NULL_VAL);
            Value *res=xs_array_new();
            Value *acc=value_incref(args[0]);
            array_push(res->arr, value_incref(acc));
            for (int j=0;j<arr->len;j++) {
                Value *a[2]={acc, arr->items[j]};
                Value *r=call_value(i, args[1], a, 2, "scan");
                value_decref(acc); acc=r;
                if (!i->cf.signal) array_push(res->arr, value_incref(acc));
                if (i->cf.signal) break;
            }
            value_decref(acc);
            return res;
        }
        /* product */
        if (strcmp(method, "product") == 0) {
            if (arr->len==0) return xs_int(1);
            int64_t pi=1; double pf=1.0; int is_f=0;
            for (int j=0;j<arr->len;j++) {
                if (VAL_TAG(arr->items[j])==XS_FLOAT){is_f=1;pf*=arr->items[j]->f;}
                else if(VAL_TAG(arr->items[j])==XS_INT) pi*=VAL_INT(arr->items[j]);
            }
            return is_f?xs_float(pf*(double)pi):xs_int(pi);
        }
        /* prepend(val) */
        if (strcmp(method, "prepend") == 0) {
            if (argc<1) return value_incref(XS_NULL_VAL);
            if (arr->len>=arr->cap) {
                arr->cap=arr->cap?arr->cap*2:4;
                arr->items=xs_realloc(arr->items,arr->cap*sizeof(Value*));
            }
            for (int j=arr->len;j>0;j--) arr->items[j]=arr->items[j-1];
            arr->items[0]=value_incref(args[0]);
            arr->len++;
            return value_incref(XS_NULL_VAL);
        }
        /* clear */
        if (strcmp(method, "clear") == 0) {
            for (int j=0;j<arr->len;j++) value_decref(arr->items[j]);
            arr->len=0;
            return value_incref(XS_NULL_VAL);
        }
        /* shuffle */
        if (strcmp(method, "shuffle") == 0) {
            for (int j=arr->len-1;j>0;j--) {
                int k=rand()%(j+1);
                Value *tmp=arr->items[j]; arr->items[j]=arr->items[k]; arr->items[k]=tmp;
            }
            return value_incref(obj);
        }
        /* sample(n) */
        if (strcmp(method, "sample") == 0) {
            int n2=(argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):1;
            if (n2>arr->len) n2=arr->len;
            /* build index array and partial Fisher-Yates */
            int *idx=xs_malloc(arr->len*sizeof(int));
            for (int j=0;j<arr->len;j++) idx[j]=j;
            Value *res=xs_array_new();
            for (int j=0;j<n2;j++) {
                int k=j+rand()%(arr->len-j);
                int tmp=idx[j]; idx[j]=idx[k]; idx[k]=tmp;
                array_push(res->arr, value_incref(arr->items[idx[j]]));
            }
            free(idx);
            return res;
        }
        /* flat_map(fn) */
        if (strcmp(method, "flat_map") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                Value *a[1]={arr->items[j]};
                Value *r=call_value(i, args[0], a, 1, "flat_map");
                if (!i->cf.signal) {
                    if (VAL_TAG(r)==XS_ARRAY) {
                        for (int k=0;k<r->arr->len;k++) array_push(res->arr, value_incref(r->arr->items[k]));
                        value_decref(r);
                    } else {
                        array_push(res->arr, r);
                    }
                } else { value_decref(r); break; }
            }
            return res;
        }
        /* dedup / unique */
        if (strcmp(method, "dedup") == 0 || strcmp(method, "unique") == 0) {
            Value *res=xs_array_new();
            for (int j=0;j<arr->len;j++) {
                int found=0;
                for (int k=0;k<res->arr->len;k++) {
                    if (value_equal(res->arr->items[k], arr->items[j])) { found=1; break; }
                }
                if (!found) array_push(res->arr, value_incref(arr->items[j]));
            }
            return res;
        }
        /* frequencies */
        if (strcmp(method, "frequencies") == 0) {
            Value *res=xs_map_new();
            for (int j=0;j<arr->len;j++) {
                char *ks=value_str(arr->items[j]);
                Value *cur=map_get(res->map, ks);
                int64_t cnt2=cur&&VAL_TAG(cur)==XS_INT?VAL_INT(cur):0;
                Value *nv=xs_int(cnt2+1);
                map_set(res->map, ks, nv);
                value_decref(nv); free(ks);
            }
            return res;
        }
        if (strcmp(method, "to_str") == 0 || strcmp(method, "to_string") == 0) {
            char *r = value_repr(obj); Value *v = xs_str(r); free(r); return v;
        }
    }

    // --- map methods
    if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
        XSMap *m = obj->map;
        /* Generator: .next() drives the worker via the resume channel
           and reads one value. End-of-stream is signaled by a
           {_gen_eos: true} sentinel sent by the worker. */
        {
            Value *gtype = map_get(m, "__type");
            Value *ych   = map_get(m, "_yield_chan");
            Value *rch   = map_get(m, "_resume_chan");
            /* Eager WASM generator: _array + _index replace the worker
               thread. .next() walks the pre-collected array. */
            Value *gen_arr = map_get(m, "_array");
            if (gtype && VAL_TAG(gtype) == XS_STR &&
                strcmp(gtype->s, "generator") == 0 && gen_arr &&
                VAL_TAG(gen_arr) == XS_ARRAY &&
                strcmp(method, "next") == 0) {
                Value *idx_v = map_get(m, "_index");
                int idx = (idx_v && VAL_IS_INT(idx_v)) ? (int)VAL_INT(idx_v) : 0;
                Value *result = xs_map_new();
                if (idx >= gen_arr->arr->len) {
                    Value *nv = value_incref(XS_NULL_VAL);
                    map_set(result->map, "value", nv); value_decref(nv);
                    Value *dv = value_incref(XS_TRUE_VAL);
                    map_set(result->map, "done", dv); value_decref(dv);
                    return result;
                }
                Value *v = value_incref(gen_arr->arr->items[idx]);
                map_set(result->map, "value", v); value_decref(v);
                Value *dv = value_incref(XS_FALSE_VAL);
                map_set(result->map, "done", dv); value_decref(dv);
                Value *new_idx = xs_int(idx + 1);
                map_set(m, "_index", new_idx); value_decref(new_idx);
                return result;
            }
            if (gtype && VAL_TAG(gtype) == XS_STR &&
                strcmp(gtype->s, "generator") == 0 &&
                ych && rch) {
                if (strcmp(method, "next") == 0) {
                    Value *done = map_get(m, "_done");
                    Value *result = xs_map_new();
                    if (done && VAL_TAG(done) == XS_BOOL && VAL_INT(done)) {
                        Value *nv = value_incref(XS_NULL_VAL);
                        map_set(result->map, "value", nv); value_decref(nv);
                        Value *dv = value_incref(XS_TRUE_VAL);
                        map_set(result->map, "done", dv); value_decref(dv);
                        return result;
                    }
                    /* The generator worker runs on a separate thread but
                       shares our interp. While it has the GIL its body
                       eval walks through push_env / call frames and
                       leaves i->env pointing into the generator's own
                       scope when it suspends on the yield channel. If
                       we don't snapshot + restore, the caller -- and
                       any subsequent for-loop -- runs with a stale env
                       and can't find its own locals. */
                    Env *saved_env = i->env ? env_incref(i->env) : NULL;
                    Value *go = value_incref(XS_NULL_VAL);
                    (void)xs_chan_send(rch, go); value_decref(go);
                    Value *v = xs_chan_recv(ych, i);
                    if (i->env) env_decref(i->env);
                    i->env = saved_env;
                    int eos = 0;
                    if (v && VAL_TAG(v) == XS_MAP) {
                        Value *e = map_get(v->map, "_gen_eos");
                        if (e && VAL_TAG(e) == XS_BOOL && VAL_INT(e)) eos = 1;
                    }
                    if (eos) {
                        Value *dv = value_incref(XS_TRUE_VAL);
                        map_set(m, "_done", dv); value_decref(dv);
                        Value *nv = value_incref(XS_NULL_VAL);
                        map_set(result->map, "value", nv); value_decref(nv);
                        Value *dv2 = value_incref(XS_TRUE_VAL);
                        map_set(result->map, "done", dv2); value_decref(dv2);
                        if (v) value_decref(v);
                    } else {
                        map_set(result->map, "value", v ? v : value_incref(XS_NULL_VAL));
                        if (v) value_decref(v);
                        Value *dv = value_incref(XS_FALSE_VAL);
                        map_set(result->map, "done", dv); value_decref(dv);
                    }
                    return result;
                }
            }
        }

        /* Channels: handle send/recv/etc. before the generic native
           dispatch so we can route through xs_chan_* with the channel
           as receiver (the bound natives don't get self prepended). */
        {
            Value *cid = map_get(m, "_chan_id");
            if (cid && VAL_TAG(cid) == XS_INT) {
                if (strcmp(method,"send")==0) {
                    if (argc < 1) return value_incref(XS_NULL_VAL);
                    if (!xs_chan_send(obj, args[0])) {
                        Value *err = xs_error_new("ChannelClosed",
                            "send on closed channel", NULL);
                        if (i->cf.value) value_decref(i->cf.value);
                        i->cf.signal = CF_THROW;
                        i->cf.value  = err;
                        return value_incref(XS_NULL_VAL);
                    }
                    return value_incref(XS_NULL_VAL);
                }
                if (strcmp(method,"recv")==0)
                    return xs_chan_recv(obj, i);
                if (strcmp(method,"recv_pair")==0) {
                    Value *v = xs_chan_recv(obj, i);
                    int ok = !(VAL_TAG(v) == XS_NULL
                               && xs_chan_is_closed(obj)
                               && xs_chan_len(obj) == 0);
                    Value *t = xs_tuple_new();
                    array_push(t->arr, v);
                    array_push(t->arr, ok ? value_incref(XS_TRUE_VAL)
                                          : value_incref(XS_FALSE_VAL));
                    return t;
                }
                if (strcmp(method,"try_recv")==0)
                    return xs_chan_try_recv(obj);
                if (strcmp(method,"close")==0) {
                    xs_chan_close(obj);
                    return value_incref(XS_NULL_VAL);
                }
                if (strcmp(method,"is_closed")==0)
                    return xs_chan_is_closed(obj)
                        ? value_incref(XS_TRUE_VAL)
                        : value_incref(XS_FALSE_VAL);
                if (strcmp(method,"len")==0)
                    return xs_int(xs_chan_len(obj));
                if (strcmp(method,"cap")==0)
                    return xs_int(xs_chan_cap(obj));
                if (strcmp(method,"is_empty")==0)
                    return xs_chan_len(obj) == 0
                        ? value_incref(XS_TRUE_VAL)
                        : value_incref(XS_FALSE_VAL);
                if (strcmp(method,"is_full")==0)
                    return xs_chan_is_full(obj)
                        ? value_incref(XS_TRUE_VAL)
                        : value_incref(XS_FALSE_VAL);
            }
        }
        /* Generator iterator protocol */
        {
            Value *ct = map_get(m, "__type");
            if (ct && VAL_TAG(ct) == XS_STR && strcmp(ct->s, "generator") == 0) {
                if (strcmp(method, "to_array") == 0 ||
                    strcmp(method, "collect") == 0 ||
                    strcmp(method, "to_list") == 0) {
                    /* eager-cached generators (WASM, VM) keep _yields; copy the
                     * unconsumed tail. The lazy worker-thread path doesn't
                     * pre-populate _yields, so callers there should iterate
                     * via for-in. */
                    Value *yields = map_get(m, "_yields");
                    if (yields && VAL_TAG(yields) == XS_ARRAY) {
                        Value *idx_v  = map_get(m, "_index");
                        int idx = idx_v && VAL_TAG(idx_v) == XS_INT ? (int)VAL_INT(idx_v) : 0;
                        Value *arr = xs_array_new();
                        for (int yi = idx; yi < yields->arr->len; yi++)
                            array_push(arr->arr, value_incref(yields->arr->items[yi]));
                        Value *new_idx = xs_int(yields->arr->len);
                        map_set(m, "_index", new_idx); value_decref(new_idx);
                        Value *dv = value_incref(XS_TRUE_VAL);
                        map_set(m, "_done", dv); value_decref(dv);
                        return arr;
                    }
                }
                if (strcmp(method, "next") == 0) {
                    Value *yields = map_get(m, "_yields");
                    Value *idx_v  = map_get(m, "_index");
                    int idx = idx_v && VAL_TAG(idx_v) == XS_INT ? (int)VAL_INT(idx_v) : 0;
                    Value *result = xs_map_new();
                    if (yields && VAL_TAG(yields) == XS_ARRAY && idx < yields->arr->len) {
                        map_set(result->map, "value", value_incref(yields->arr->items[idx]));
                        Value *dv = value_incref(XS_FALSE_VAL);
                        map_set(result->map, "done", dv); value_decref(dv);
                        Value *new_idx = xs_int(idx + 1);
                        map_set(m, "_index", new_idx); value_decref(new_idx);
                    } else {
                        map_set(result->map, "value", value_incref(XS_NULL_VAL));
                        Value *dv = value_incref(XS_TRUE_VAL);
                        map_set(result->map, "done", dv); value_decref(dv);
                        Value *done_v = value_incref(XS_TRUE_VAL);
                        map_set(m, "_done", done_v); value_decref(done_v);
                    }
                    return result;
                }
            }
        }
        /* User-defined callables stored in map take priority over built-in methods
         * (e.g. {get: fn, set: fn} state accessor objects) */
        {
            Value *fn = map_get(m, method);
            if (fn && VAL_TAG(fn) == XS_FUNC) {
                return call_value(i, fn, args, argc, method);
            }
            /* for native fns on maps that have _hook_idx (hook handles),
               prepend the map as self so the native can access fields */
            if (fn && VAL_TAG(fn) == XS_NATIVE) {
                if (map_has(m, "_hook_idx")) {
                    Value **new_args = xs_malloc((argc + 1) * sizeof(Value*));
                    new_args[0] = obj;
                    for (int j = 0; j < argc; j++) new_args[j + 1] = args[j];
                    Value *r = call_value(i, fn, new_args, argc + 1, method);
                    free(new_args);
                    return r;
                }
                return call_value(i, fn, args, argc, method);
            }
        }
        /* Stopwatch object: has _start field */
        if ((strcmp(method,"elapsed")==0||strcmp(method,"elapsed_ms")==0) &&
            map_has(m,"_start")) {
            Value *sv = map_get(m, "_start");
            double start = sv ? (VAL_TAG(sv)==XS_FLOAT ? sv->f : (double)VAL_INT(sv)) : 0.0;
            struct timespec ts2; clock_gettime(CLOCK_REALTIME, &ts2);
            double now2 = (double)ts2.tv_sec + (double)ts2.tv_nsec/1e9;
            double elapsed = now2 - start;
            if (strcmp(method,"elapsed_ms")==0) elapsed *= 1000.0;
            return xs_float(elapsed);
        }
        if (strcmp(method, "reset") == 0 && map_has(m, "_start")) {
            struct timespec ts2; clock_gettime(CLOCK_REALTIME, &ts2);
            double now2 = (double)ts2.tv_sec + (double)ts2.tv_nsec/1e9;
            Value *nv = xs_float(now2);
            map_set(m, "_start", nv); value_decref(nv);
            return value_incref(XS_NULL_VAL);
        }
        /* Collections early dispatch (must be before generic len/is_empty/get) */
        {
            Value *ct = map_get(m, "_type");
            if (ct && VAL_TAG(ct) == XS_STR) {
                /* All collection types: route to full dispatch for any method */
                if (strcmp(ct->s,"Stack")==0||strcmp(ct->s,"PriorityQueue")==0||
                    strcmp(ct->s,"Deque")==0||strcmp(ct->s,"Set")==0||
                    strcmp(ct->s,"OrderedMap")==0||strcmp(ct->s,"Counter")==0) {
                    goto collections_full_dispatch;
                }
                /* Signal reactive primitive */
                if (strcmp(ct->s,"Signal")==0) {
                    if (strcmp(method,"get")==0) {
                        Value *v=map_get(m,"_val");
                        return v?value_incref(v):value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method,"set")==0) {
                        if (argc<1) return value_incref(XS_NULL_VAL);
                        map_set(m,"_val",value_incref(args[0]));
                        /* call subscribers */
                        Value *subs=map_get(m,"_subs");
                        if (subs&&VAL_TAG(subs)==XS_ARRAY) {
                            for (int si=0;si<subs->arr->len;si++)
                                { Value *r=call_value(i, subs->arr->items[si], args, 1, "signal_sub"); if(r) value_decref(r); }
                        }
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method,"subscribe")==0) {
                        if (argc<1) return value_incref(XS_NULL_VAL);
                        Value *subs=map_get(m,"_subs");
                        if (subs&&VAL_TAG(subs)==XS_ARRAY)
                            array_push(subs->arr, value_incref(args[0]));
                        return value_incref(XS_NULL_VAL);
                    }
                }
                /* Derived reactive primitive */
                if (strcmp(ct->s,"Derived")==0) {
                    if (strcmp(method,"get")==0) {
                        Value *fn=map_get(m,"_fn");
                        if (!fn) return value_incref(XS_NULL_VAL);
                        return call_value(i, fn, NULL, 0, "derived_get");
                    }
                }
                /* Channel: dispatch via concurrent runtime so the same
                   buffer + condvar is shared with .send/.recv natives
                   bound on the map and with the VM-side channel calls. */
                if (strcmp(ct->s,"Channel")==0) {
                    if (strcmp(method,"send")==0) {
                        if (argc < 1) return value_incref(XS_NULL_VAL);
                        if (!xs_chan_send(obj, args[0])) {
                            Value *err = xs_error_new("ChannelClosed",
                                "send on closed channel", NULL);
                            if (i->cf.value) value_decref(i->cf.value);
                            i->cf.signal = CF_THROW;
                            i->cf.value  = err;
                            return value_incref(XS_NULL_VAL);
                        }
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method,"recv")==0)
                        return xs_chan_recv(obj, i);
                    if (strcmp(method,"recv_pair")==0) {
                        Value *v = xs_chan_recv(obj, i);
                        int ok = !(VAL_TAG(v) == XS_NULL
                                   && xs_chan_is_closed(obj)
                                   && xs_chan_len(obj) == 0);
                        Value *t = xs_tuple_new();
                        array_push(t->arr, v);
                        array_push(t->arr, ok ? value_incref(XS_TRUE_VAL)
                                              : value_incref(XS_FALSE_VAL));
                        return t;
                    }
                    if (strcmp(method,"try_recv")==0)
                        return xs_chan_try_recv(obj);
                    if (strcmp(method,"close")==0) {
                        xs_chan_close(obj);
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method,"is_closed")==0)
                        return xs_chan_is_closed(obj)
                            ? value_incref(XS_TRUE_VAL)
                            : value_incref(XS_FALSE_VAL);
                    if (strcmp(method,"len")==0)
                        return xs_int(xs_chan_len(obj));
                    if (strcmp(method,"cap")==0)
                        return xs_int(xs_chan_cap(obj));
                    if (strcmp(method,"is_empty")==0)
                        return xs_chan_len(obj) == 0
                            ? value_incref(XS_TRUE_VAL)
                            : value_incref(XS_FALSE_VAL);
                    if (strcmp(method,"is_full")==0)
                        return xs_chan_is_full(obj)
                            ? value_incref(XS_TRUE_VAL)
                            : value_incref(XS_FALSE_VAL);
                }
                /* Counter is now routed via the early goto above */
            }
        }
        if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0 ||
            strcmp(method, "count") == 0 || strcmp(method, "size") == 0) return xs_int(m->len);
        if (strcmp(method, "keys") == 0) {
            int nk=0; char **ks=map_keys(m,&nk);
            Value *arr=xs_array_new();
            for (int j=0;j<nk;j++) { array_push(arr->arr, xs_str(ks[j])); free(ks[j]); }
            free(ks); return arr;
        }
        if (strcmp(method, "values") == 0) {
            int nk=0; char **ks=map_keys(m,&nk);
            Value *arr=xs_array_new();
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                if (v) array_push(arr->arr,value_incref(v));
                free(ks[j]);
            }
            free(ks); return arr;
        }
        if (strcmp(method, "entries") == 0 || strcmp(method, "items") == 0 ||
            strcmp(method, "to_array") == 0 || strcmp(method, "to_pairs") == 0) {
            int nk=0; char **ks=map_keys(m,&nk);
            Value *arr=xs_array_new();
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                Value *tup=xs_tuple_new();
                array_push(tup->arr, xs_str(ks[j]));
                array_push(tup->arr, v?value_incref(v):value_incref(XS_NULL_VAL));
                array_push(arr->arr, tup);
                free(ks[j]);
            }
            free(ks); return arr;
        }
        if (strcmp(method, "has") == 0 || strcmp(method, "contains_key") == 0 ||
            strcmp(method, "has_key") == 0 || strcmp(method, "contains") == 0 ||
            strcmp(method, "includes") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
            return map_has(m,args[0]->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "get") == 0 || strcmp(method, "get_or") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_NULL_VAL);
            Value *v=map_get(m,args[0]->s);
            if (v) return value_incref(v);
            return argc>1?value_incref(args[1]):value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "set") == 0 || strcmp(method, "insert") == 0) {
            if (argc<2||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_NULL_VAL);
            map_set(m, args[0]->s, value_incref(args[1]));
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "remove") == 0 || strcmp(method, "delete") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_NULL_VAL);
            map_del(m, args[0]->s);
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "is_empty") == 0) {
            return m->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        /* clone / copy */
        if (strcmp(method, "clone") == 0 || strcmp(method, "copy") == 0) {
            Value *res=xs_map_new();
            int nk=0; char **ks=map_keys(m,&nk);
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                if (v) map_set(res->map, ks[j], value_incref(v));
                free(ks[j]);
            }
            free(ks); return res;
        }
        /* map(fn): apply fn(key, val) to each entry, return new map */
        if (strcmp(method, "map") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_map_new();
            int nk=0; char **ks=map_keys(m,&nk);
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                Value *kv=xs_str(ks[j]);
                Value *a[2]={kv, v?v:XS_NULL_VAL};
                Value *r=call_value(i, args[0], a, 2, "map");
                value_decref(kv);
                if (!i->cf.signal) { map_set(res->map, ks[j], r); value_decref(r); }
                else { value_decref(r); }
                free(ks[j]);
                if (i->cf.signal) {
                    for (int j2=j+1; j2<nk; j2++) free(ks[j2]);
                    break;
                }
            }
            free(ks); return res;
        }
        /* filter(fn): filter entries by fn(key, val), return new map */
        if (strcmp(method, "filter") == 0) {
            if (argc<1) return value_incref(obj);
            Value *res=xs_map_new();
            int nk=0; char **ks=map_keys(m,&nk);
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                Value *kv=xs_str(ks[j]);
                Value *a[2]={kv, v?v:XS_NULL_VAL};
                Value *r=call_value(i, args[0], a, 2, "filter");
                value_decref(kv);
                int ok=value_truthy(r); value_decref(r);
                if (!i->cf.signal && ok && v) map_set(res->map, ks[j], value_incref(v));
                free(ks[j]);
                if (i->cf.signal) break;
            }
            free(ks); return res;
        }
        /* merge(other): merge another map, return new map */
        if (strcmp(method, "merge") == 0) {
            Value *res=xs_map_new();
            int nk=0; char **ks=map_keys(m,&nk);
            for (int j=0;j<nk;j++) {
                Value *v=map_get(m,ks[j]);
                if (v) map_set(res->map, ks[j], value_incref(v));
                free(ks[j]);
            }
            free(ks);
            if (argc>0 && (VAL_TAG(args[0])==XS_MAP||VAL_TAG(args[0])==XS_MODULE)) {
                int nk2=0; char **ks2=map_keys(args[0]->map,&nk2);
                for (int j=0;j<nk2;j++) {
                    Value *v=map_get(args[0]->map,ks2[j]);
                    if (v) map_set(res->map, ks2[j], value_incref(v));
                    free(ks2[j]);
                }
                free(ks2);
            }
            return res;
        }
        /* Collections: Stack / PriorityQueue / Counter dispatch */
        collections_full_dispatch:;
        {
            Value *type_val = map_get(m, "_type");
            if (type_val && VAL_TAG(type_val) == XS_STR) {
                const char *ctype = type_val->s;
                Value *data = map_get(m, "_data");
                /* Stack */
                if (strcmp(ctype, "Stack") == 0 && data && VAL_TAG(data) == XS_ARRAY) {
                    XSArray *arr = data->arr;
                    if (strcmp(method, "push") == 0) {
                        if (argc >= 1) array_push(arr, value_incref(args[0]));
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "pop") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        Value *v = arr->items[arr->len-1];
                        arr->len--;
                        return v; /* transfer ownership */
                    }
                    if (strcmp(method, "peek") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        return value_incref(arr->items[arr->len-1]);
                    }
                    if (strcmp(method, "len") == 0) return xs_int(arr->len);
                    if (strcmp(method, "is_empty") == 0)
                        return arr->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    if (strcmp(method, "to_array") == 0) {
                        Value *res = xs_array_new();
                        for (int j = 0; j < arr->len; j++)
                            array_push(res->arr, value_incref(arr->items[j]));
                        return res;
                    }
                    if (strcmp(method, "clear") == 0) {
                        for (int j = 0; j < arr->len; j++)
                            value_decref(arr->items[j]);
                        arr->len = 0;
                        return value_incref(XS_NULL_VAL);
                    }
                }
                /* Deque */
                if (strcmp(ctype, "Deque") == 0 && data && VAL_TAG(data) == XS_ARRAY) {
                    XSArray *arr = data->arr;
                    if (strcmp(method, "push_back") == 0) {
                        if (argc >= 1) array_push(arr, value_incref(args[0]));
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "push_front") == 0) {
                        if (argc < 1) return value_incref(XS_NULL_VAL);
                        /* Extend array by one slot using a dummy value */
                        Value *dummy = value_incref(XS_NULL_VAL);
                        array_push(arr, dummy);
                        /* Now arr->len increased by 1. items[len-1] = dummy.
                           Shift existing items right: items[len-1..1] = items[len-2..0] */
                        for (int j = arr->len - 1; j > 0; j--)
                            arr->items[j] = arr->items[j-1];
                        /* items[0] still holds old items[0] (now also at items[1]).
                           Overwrite items[0] with the new value. The dummy NULL_VAL
                           that was at items[len-1] was overwritten by items[len-2] in
                           the shift, so we already lost that ref: decref it. */
                        value_decref(dummy);
                        arr->items[0] = value_incref(args[0]);
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "pop_back") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        Value *v = arr->items[arr->len-1];
                        arr->len--;
                        return v;
                    }
                    if (strcmp(method, "pop_front") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        Value *v = arr->items[0];
                        for (int j = 0; j < arr->len - 1; j++)
                            arr->items[j] = arr->items[j+1];
                        arr->len--;
                        return v;
                    }
                    if (strcmp(method, "len") == 0) return xs_int(arr->len);
                    if (strcmp(method, "is_empty") == 0)
                        return arr->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    if (strcmp(method, "front") == 0 || strcmp(method, "first") == 0 ||
                        strcmp(method, "peek_front") == 0)
                        return arr->len ? value_incref(arr->items[0]) : value_incref(XS_NULL_VAL);
                    if (strcmp(method, "back") == 0 || strcmp(method, "last") == 0 ||
                        strcmp(method, "peek_back") == 0)
                        return arr->len ? value_incref(arr->items[arr->len-1]) : value_incref(XS_NULL_VAL);
                    if (strcmp(method, "to_array") == 0) {
                        Value *res = xs_array_new();
                        for (int j = 0; j < arr->len; j++)
                            array_push(res->arr, value_incref(arr->items[j]));
                        return res;
                    }
                }
                /* Set */
                if (strcmp(ctype, "Set") == 0) {
                    Value *sdata = map_get(m, "_data");
                    if (sdata && VAL_TAG(sdata) == XS_MAP) {
                        XSMap *sd = sdata->map;
                        if (strcmp(method, "add") == 0) {
                            if (argc >= 1) {
                                char *k = value_str(args[0]);
                                Value *tv = value_incref(XS_TRUE_VAL);
                                map_set(sd, k, tv); value_decref(tv);
                                free(k);
                            }
                            return value_incref(XS_NULL_VAL);
                        }
                        if (strcmp(method, "remove") == 0) {
                            if (argc >= 1) {
                                char *k = value_str(args[0]);
                                map_del(sd, k);
                                free(k);
                            }
                            return value_incref(XS_NULL_VAL);
                        }
                        if (strcmp(method, "has") == 0 ||
                            strcmp(method, "contains") == 0) {
                            if (argc < 1) return value_incref(XS_FALSE_VAL);
                            char *k = value_str(args[0]);
                            int found = map_has(sd, k);
                            free(k);
                            return found ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
                        }
                        if (strcmp(method, "len") == 0) return xs_int(sd->len);
                        if (strcmp(method, "is_empty") == 0)
                            return sd->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                        if (strcmp(method, "to_array") == 0) {
                            Value *res = xs_array_new();
                            int nk = 0; char **ks = map_keys(sd, &nk);
                            for (int j = 0; j < nk; j++) {
                                array_push(res->arr, xs_str(ks[j]));
                                free(ks[j]);
                            }
                            free(ks);
                            return res;
                        }
                        if (strcmp(method, "union") == 0) {
                            Value *ns = xs_map_new();
                            Value *nt = xs_str("Set"); map_set(ns->map, "_type", nt); value_decref(nt);
                            Value *nd = xs_map_new(); map_set(ns->map, "_data", nd); value_decref(nd);
                            int nk = 0; char **ks = map_keys(sd, &nk);
                            for (int j = 0; j < nk; j++) {
                                Value *tv = value_incref(XS_TRUE_VAL);
                                map_set(nd->map, ks[j], tv); value_decref(tv);
                                free(ks[j]);
                            }
                            free(ks);
                            if (argc >= 1 && VAL_TAG(args[0]) == XS_MAP) {
                                Value *odata = map_get(args[0]->map, "_data");
                                if (odata && VAL_TAG(odata) == XS_MAP) {
                                    int nk2 = 0; char **ks2 = map_keys(odata->map, &nk2);
                                    for (int j = 0; j < nk2; j++) {
                                        Value *tv = value_incref(XS_TRUE_VAL);
                                        map_set(nd->map, ks2[j], tv); value_decref(tv);
                                        free(ks2[j]);
                                    }
                                    free(ks2);
                                }
                            }
                            return ns;
                        }
                        if (strcmp(method, "intersection") == 0) {
                            Value *ns = xs_map_new();
                            Value *nt = xs_str("Set"); map_set(ns->map, "_type", nt); value_decref(nt);
                            Value *nd = xs_map_new(); map_set(ns->map, "_data", nd); value_decref(nd);
                            if (argc >= 1 && VAL_TAG(args[0]) == XS_MAP) {
                                Value *odata = map_get(args[0]->map, "_data");
                                if (odata && VAL_TAG(odata) == XS_MAP) {
                                    int nk = 0; char **ks = map_keys(sd, &nk);
                                    for (int j = 0; j < nk; j++) {
                                        if (map_has(odata->map, ks[j])) {
                                            Value *tv = value_incref(XS_TRUE_VAL);
                                            map_set(nd->map, ks[j], tv); value_decref(tv);
                                        }
                                        free(ks[j]);
                                    }
                                    free(ks);
                                }
                            }
                            return ns;
                        }
                        if (strcmp(method, "difference") == 0) {
                            Value *ns = xs_map_new();
                            Value *nt = xs_str("Set"); map_set(ns->map, "_type", nt); value_decref(nt);
                            Value *nd = xs_map_new(); map_set(ns->map, "_data", nd); value_decref(nd);
                            int nk = 0; char **ks = map_keys(sd, &nk);
                            if (argc >= 1 && VAL_TAG(args[0]) == XS_MAP) {
                                Value *odata = map_get(args[0]->map, "_data");
                                if (odata && VAL_TAG(odata) == XS_MAP) {
                                    for (int j = 0; j < nk; j++) {
                                        if (!map_has(odata->map, ks[j])) {
                                            Value *tv = value_incref(XS_TRUE_VAL);
                                            map_set(nd->map, ks[j], tv); value_decref(tv);
                                        }
                                        free(ks[j]);
                                    }
                                } else {
                                    for (int j = 0; j < nk; j++) {
                                        Value *tv = value_incref(XS_TRUE_VAL);
                                        map_set(nd->map, ks[j], tv); value_decref(tv);
                                        free(ks[j]);
                                    }
                                }
                            } else {
                                for (int j = 0; j < nk; j++) {
                                    Value *tv = value_incref(XS_TRUE_VAL);
                                    map_set(nd->map, ks[j], tv); value_decref(tv);
                                    free(ks[j]);
                                }
                            }
                            free(ks);
                            return ns;
                        }
                    }
                }
                /* OrderedMap */
                if (strcmp(ctype, "OrderedMap") == 0) {
                    Value *om_keys = map_get(m, "_keys");
                    Value *om_data = map_get(m, "_data");
                    if (om_keys && VAL_TAG(om_keys) == XS_ARRAY && om_data && VAL_TAG(om_data) == XS_MAP) {
                        XSArray *ka = om_keys->arr;
                        XSMap *dm = om_data->map;
                        if (strcmp(method, "set") == 0) {
                            if (argc < 2 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_NULL_VAL);
                            const char *key = args[0]->s;
                            if (!map_has(dm, key))
                                array_push(ka, value_incref(args[0]));
                            map_set(dm, key, value_incref(args[1]));
                            return value_incref(XS_NULL_VAL);
                        }
                        if (strcmp(method, "get") == 0) {
                            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_NULL_VAL);
                            Value *v = map_get(dm, args[0]->s);
                            if (v) return value_incref(v);
                            return argc > 1 ? value_incref(args[1]) : value_incref(XS_NULL_VAL);
                        }
                        if (strcmp(method, "delete") == 0) {
                            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_NULL_VAL);
                            const char *key = args[0]->s;
                            map_del(dm, key);
                            for (int j = 0; j < ka->len; j++) {
                                if (VAL_TAG(ka->items[j]) == XS_STR && strcmp(ka->items[j]->s, key) == 0) {
                                    value_decref(ka->items[j]);
                                    for (int k = j; k < ka->len - 1; k++)
                                        ka->items[k] = ka->items[k+1];
                                    ka->len--;
                                    break;
                                }
                            }
                            return value_incref(XS_NULL_VAL);
                        }
                        if (strcmp(method, "has") == 0 ||
                            strcmp(method, "contains") == 0 ||
                            strcmp(method, "has_key") == 0 ||
                            strcmp(method, "includes") == 0) {
                            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
                            return map_has(dm, args[0]->s) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
                        }
                        if (strcmp(method, "keys") == 0) {
                            Value *res = xs_array_new();
                            for (int j = 0; j < ka->len; j++)
                                array_push(res->arr, value_incref(ka->items[j]));
                            return res;
                        }
                        if (strcmp(method, "values") == 0) {
                            Value *res = xs_array_new();
                            for (int j = 0; j < ka->len; j++) {
                                if (VAL_TAG(ka->items[j]) == XS_STR) {
                                    Value *v = map_get(dm, ka->items[j]->s);
                                    array_push(res->arr, v ? value_incref(v) : value_incref(XS_NULL_VAL));
                                }
                            }
                            return res;
                        }
                        if (strcmp(method, "entries") == 0) {
                            Value *res = xs_array_new();
                            for (int j = 0; j < ka->len; j++) {
                                if (VAL_TAG(ka->items[j]) == XS_STR) {
                                    Value *tup = xs_tuple_new();
                                    array_push(tup->arr, value_incref(ka->items[j]));
                                    Value *v = map_get(dm, ka->items[j]->s);
                                    array_push(tup->arr, v ? value_incref(v) : value_incref(XS_NULL_VAL));
                                    array_push(res->arr, tup);
                                }
                            }
                            return res;
                        }
                        if (strcmp(method, "len") == 0) return xs_int(ka->len);
                    }
                }
                /* PriorityQueue */
                if (strcmp(ctype, "PriorityQueue") == 0 && data && VAL_TAG(data) == XS_ARRAY) {
                    XSArray *arr = data->arr;
                    if (strcmp(method, "push") == 0) {
                        /* push(item, priority): store as [item, priority] tuple */
                        if (argc < 1) return value_incref(XS_NULL_VAL);
                        double prio = (argc >= 2) ? (VAL_TAG(args[1])==XS_INT?(double)VAL_INT(args[1]):
                                      VAL_TAG(args[1])==XS_FLOAT?args[1]->f:0.0) : 0.0;
                        Value *entry = xs_array_new();
                        array_push(entry->arr, value_incref(args[0]));
                        array_push(entry->arr, xs_float(prio));
                        /* insert sorted descending by priority */
                        int pos = arr->len;
                        for (int j = 0; j < arr->len; j++) {
                            Value *ej = arr->items[j];
                            if (VAL_TAG(ej) == XS_ARRAY && ej->arr->len >= 2) {
                                double ep = VAL_TAG(ej->arr->items[1])==XS_FLOAT?ej->arr->items[1]->f:
                                            (double)VAL_INT(ej->arr->items[1]);
                                if (prio > ep) { pos = j; break; }
                            }
                        }
                        /* shift right and insert */
                        array_push(arr, value_incref(XS_NULL_VAL)); /* extend */
                        for (int j = arr->len-1; j > pos; j--)
                            arr->items[j] = arr->items[j-1];
                        arr->items[pos] = entry;
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "pop") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        Value *entry = arr->items[0];
                        for (int j = 0; j < arr->len-1; j++) arr->items[j] = arr->items[j+1];
                        arr->len--;
                        Value *item = (VAL_TAG(entry)==XS_ARRAY&&entry->arr->len>=1) ?
                                      value_incref(entry->arr->items[0]) : value_incref(XS_NULL_VAL);
                        value_decref(entry);
                        return item;
                    }
                    if (strcmp(method, "peek") == 0) {
                        if (arr->len == 0) return value_incref(XS_NULL_VAL);
                        Value *entry = arr->items[0];
                        return (VAL_TAG(entry)==XS_ARRAY&&entry->arr->len>=1) ?
                               value_incref(entry->arr->items[0]) : value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "len") == 0) return xs_int(arr->len);
                    if (strcmp(method, "is_empty") == 0)
                        return arr->len==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                }
                /* Counter */
                if (strcmp(ctype, "Counter") == 0) {
                    if (strcmp(method, "get") == 0) {
                        if (argc < 1) return xs_int(0);
                        char *key = value_str(args[0]);
                        Value *v = map_get(m, key); free(key);
                        return v ? value_incref(v) : xs_int(0);
                    }
                    if (strcmp(method, "add") == 0) {
                        if (argc < 1) return value_incref(XS_NULL_VAL);
                        char *key = value_str(args[0]);
                        int64_t inc = (argc >= 2 && VAL_TAG(args[1]) == XS_INT) ? VAL_INT(args[1]) : 1;
                        Value *cur = map_get(m, key);
                        int64_t val = cur && VAL_TAG(cur) == XS_INT ? VAL_INT(cur) + inc : inc;
                        Value *nv = xs_int(val);
                        map_set(m, key, nv); value_decref(nv);
                        free(key);
                        return value_incref(XS_NULL_VAL);
                    }
                    if (strcmp(method, "total") == 0) {
                        int nk = 0; char **ks = map_keys(m, &nk);
                        int64_t total = 0;
                        for (int j = 0; j < nk; j++) {
                            if (strcmp(ks[j], "_type") != 0) {
                                Value *v = map_get(m, ks[j]);
                                if (v && VAL_TAG(v) == XS_INT) total += VAL_INT(v);
                            }
                            free(ks[j]);
                        }
                        free(ks);
                        return xs_int(total);
                    }
                    if (strcmp(method, "to_map") == 0) {
                        Value *res = xs_map_new();
                        int nk = 0; char **ks = map_keys(m, &nk);
                        for (int j = 0; j < nk; j++) {
                            if (strcmp(ks[j], "_type") != 0) {
                                Value *v = map_get(m, ks[j]);
                                if (v) map_set(res->map, ks[j], value_incref(v));
                            }
                            free(ks[j]);
                        }
                        free(ks);
                        return res;
                    }
                    if (strcmp(method, "keys") == 0) {
                        Value *res = xs_array_new();
                        int nk = 0; char **ks = map_keys(m, &nk);
                        for (int j = 0; j < nk; j++) {
                            if (strcmp(ks[j], "_type") != 0)
                                array_push(res->arr, xs_str(ks[j]));
                            free(ks[j]);
                        }
                        free(ks);
                        return res;
                    }
                    if (strcmp(method, "values") == 0) {
                        /* Skip the _type tag so sum()/avg()/count() over
                           a Counter's values produce the numeric totals
                           the user expects, not the implementation tag. */
                        Value *res = xs_array_new();
                        int nk = 0; char **ks = map_keys(m, &nk);
                        for (int j = 0; j < nk; j++) {
                            if (strcmp(ks[j], "_type") != 0) {
                                Value *v = map_get(m, ks[j]);
                                if (v) array_push(res->arr, value_incref(v));
                            }
                            free(ks[j]);
                        }
                        free(ks);
                        return res;
                    }
                    if (strcmp(method, "len") == 0 || strcmp(method, "size") == 0) {
                        int nk = 0; char **ks = map_keys(m, &nk);
                        int64_t sz = 0;
                        for (int j = 0; j < nk; j++) {
                            if (strcmp(ks[j], "_type") != 0) sz++;
                            free(ks[j]);
                        }
                        free(ks);
                        return xs_int(sz);
                    }
                    if (strcmp(method, "most_common") == 0) {
                        int topn = (argc>=1&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):m->len;
                        /* collect non-meta entries */
                        int nk=0; char **ks=map_keys(m,&nk);
                        /* simple selection sort for top-N */
                        /* build pairs array */
                        Value *result = xs_array_new();
                        /* We'll do a simple approach: collect all, sort, return top n */
                        /* Use a small struct on stack for sorting */
                        int valid = 0;
                        for (int j=0;j<nk;j++) if (strcmp(ks[j],"_type")!=0) valid++;
                        if (valid > 0) {
                            /* create sortable entries */
                            char **keys2 = xs_malloc(valid*sizeof(char*));
                            int64_t *counts = xs_malloc(valid*sizeof(int64_t));
                            int vi=0;
                            for (int j=0;j<nk;j++) {
                                if (strcmp(ks[j],"_type")==0) { free(ks[j]); continue; }
                                Value *v=map_get(m,ks[j]);
                                counts[vi] = v&&VAL_TAG(v)==XS_INT?VAL_INT(v):0;
                                keys2[vi] = ks[j]; vi++;
                            }
                            free(ks);
                            /* bubble sort descending */
                            for (int a2=0;a2<valid-1;a2++) for (int b2=a2+1;b2<valid;b2++)
                                if (counts[b2]>counts[a2]) {
                                    int64_t tc=counts[a2]; counts[a2]=counts[b2]; counts[b2]=tc;
                                    char *tk=keys2[a2]; keys2[a2]=keys2[b2]; keys2[b2]=tk;
                                }
                            int lim = topn < valid ? topn : valid;
                            for (int j=0;j<lim;j++) {
                                Value *tup=xs_tuple_new();
                                array_push(tup->arr, xs_str(keys2[j]));
                                array_push(tup->arr, xs_int(counts[j]));
                                array_push(result->arr, tup);
                            }
                            for (int j=0;j<valid;j++) free(keys2[j]);
                            free(keys2); free(counts);
                        } else { free(ks); }
                        return result;
                    }
                }
            }
        }
        /* Fall back: look up method in the map itself (e.g. module functions) */
        {
            Value *fn = map_get(m, method);
            if (fn) {
                return call_value(i, fn, args, argc, method);
            }
        }
    }

    if (VAL_TAG(obj) == XS_REGEX && obj->s) {
        const char *pat = obj->s;
        if (strcmp(method, "test") == 0 || strcmp(method, "is_match") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
            regex_t re;
            if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) return value_incref(XS_FALSE_VAL);
            int ok = (regexec(&re, args[0]->s, 0, NULL, 0) == 0);
            regfree(&re);
            return ok ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "match") == 0 || strcmp(method, "find") == 0) {
            if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return value_incref(XS_NULL_VAL);
            regex_t re;
            if (regcomp(&re, pat, REG_EXTENDED) != 0) return value_incref(XS_NULL_VAL);
            regmatch_t m[1];
            if (regexec(&re, args[0]->s, 1, m, 0) == 0) {
                int len = m[0].rm_eo - m[0].rm_so;
                Value *r = xs_str_n(args[0]->s + m[0].rm_so, (size_t)len);
                regfree(&re);
                return r;
            }
            regfree(&re);
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "replace") == 0) {
            if (argc < 2 || VAL_TAG(args[0]) != XS_STR || VAL_TAG(args[1]) != XS_STR)
                return value_incref(XS_NULL_VAL);
            regex_t re;
            if (regcomp(&re, pat, REG_EXTENDED) != 0) return value_incref(args[0]);
            regmatch_t m[1];
            if (regexec(&re, args[0]->s, 1, m, 0) == 0) {
                int pre_len = m[0].rm_so;
                int post_start = m[0].rm_eo;
                int rlen = (int)strlen(args[1]->s);
                int slen = (int)strlen(args[0]->s);
                char *buf = xs_malloc((size_t)(pre_len + rlen + slen - post_start + 1));
                memcpy(buf, args[0]->s, (size_t)pre_len);
                memcpy(buf + pre_len, args[1]->s, (size_t)rlen);
                memcpy(buf + pre_len + rlen, args[0]->s + post_start, (size_t)(slen - post_start));
                buf[pre_len + rlen + slen - post_start] = '\0';
                Value *r = xs_str(buf); free(buf);
                regfree(&re);
                return r;
            }
            regfree(&re);
            return value_incref(args[0]);
        }
        if (strcmp(method, "source") == 0 || strcmp(method, "pattern") == 0) {
            return xs_str(pat);
        }
        if (strcmp(method, "to_str") == 0) {
            char *s = value_str(obj); Value *v = xs_str(s); free(s); return v;
        }
    }

    if (VAL_TAG(obj) == XS_INT || VAL_TAG(obj) == XS_FLOAT || VAL_TAG(obj) == XS_BIGINT) {
        double num_f = (VAL_TAG(obj)==XS_FLOAT)?obj->f:(VAL_TAG(obj)==XS_BIGINT)?bigint_to_double(obj->bigint):(double)VAL_INT(obj);
        int64_t num_i = (VAL_TAG(obj)==XS_INT)?VAL_INT(obj):(VAL_TAG(obj)==XS_BIGINT)?bigint_to_i64(obj->bigint):(int64_t)obj->f;
        if (strcmp(method, "abs") == 0) {
            if (VAL_TAG(obj)==XS_FLOAT) return xs_float(fabs(obj->f));
            if (VAL_TAG(obj)==XS_BIGINT) return xs_bigint_val(bigint_abs(obj->bigint));
            return xs_int(num_i<0?-num_i:num_i);
        }
        if (strcmp(method, "pow") == 0) {
            double exp_v=(argc>0)?(VAL_TAG(args[0])==XS_INT?(double)VAL_INT(args[0]):args[0]->f):1.0;
            double r=pow(num_f, exp_v);
            if (VAL_TAG(obj)==XS_INT && VAL_TAG(args[0])==XS_INT && exp_v>=0)
                return xs_int((int64_t)r);
            return xs_float(r);
        }
        if (strcmp(method, "sqrt") == 0) return xs_float(sqrt(num_f));
        if (strcmp(method, "min") == 0) {
            if (argc<1) return value_incref(obj);
            if (value_cmp(obj, args[0])<=0) return value_incref(obj);
            return value_incref(args[0]);
        }
        if (strcmp(method, "max") == 0) {
            if (argc<1) return value_incref(obj);
            if (value_cmp(obj, args[0])>=0) return value_incref(obj);
            return value_incref(args[0]);
        }
        if (strcmp(method, "clamp") == 0) {
            if (argc<2) return value_incref(obj);
            if (value_cmp(obj, args[0])<0) return value_incref(args[0]);
            if (value_cmp(obj, args[1])>0) return value_incref(args[1]);
            return value_incref(obj);
        }
        if (strcmp(method, "floor") == 0) {
            if (VAL_TAG(obj)==XS_INT) return value_incref(obj);
            return xs_int((int64_t)floor(obj->f));
        }
        if (strcmp(method, "ceil") == 0) {
            if (VAL_TAG(obj)==XS_INT) return value_incref(obj);
            return xs_int((int64_t)ceil(obj->f));
        }
        if (strcmp(method, "round") == 0) {
            /* Optional `digits` argument: x.round(2) keeps two decimal
               places. Without it, returns the nearest int. */
            int digits = (argc > 0 && VAL_TAG(args[0]) == XS_INT) ?
                         (int)VAL_INT(args[0]) : -1;
            if (digits >= 0) {
                double v = (VAL_TAG(obj)==XS_INT) ? (double)VAL_INT(obj) : obj->f;
                double scale = 1.0;
                for (int k = 0; k < digits; k++) scale *= 10.0;
                return xs_float(round(v * scale) / scale);
            }
            if (VAL_TAG(obj)==XS_INT) return value_incref(obj);
            return xs_int((int64_t)round(obj->f));
        }
        if (strcmp(method, "to_int") == 0 || strcmp(method, "as_int") == 0) {
            return xs_int(num_i);
        }
        if (strcmp(method, "to_float") == 0 || strcmp(method, "as_float") == 0) {
            return xs_float(num_f);
        }
        if (strcmp(method, "to_str") == 0 || strcmp(method, "as_str") == 0 ||
            strcmp(method, "to_string") == 0) {
            char *r=value_str(obj); Value *v=xs_str(r); free(r); return v;
        }
        if (strcmp(method, "is_nan") == 0) {
            if (VAL_TAG(obj)!=XS_FLOAT) return value_incref(XS_FALSE_VAL);
            return isnan(obj->f)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_inf") == 0) {
            if (VAL_TAG(obj)!=XS_FLOAT) return value_incref(XS_FALSE_VAL);
            return isinf(obj->f)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_even") == 0) {
            if (VAL_TAG(obj)==XS_BIGINT) {
                int even = (obj->bigint->len==0) || (obj->bigint->limbs[0]%2==0);
                return even?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            }
            return (num_i%2==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_odd") == 0) {
            if (VAL_TAG(obj)==XS_BIGINT) {
                int odd = (obj->bigint->len>0) && (obj->bigint->limbs[0]%2!=0);
                return odd?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            }
            return (num_i%2!=0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "sign") == 0) {
            if (VAL_TAG(obj)==XS_FLOAT) {
                if (obj->f>0.0) return xs_int(1);
                if (obj->f<0.0) return xs_int(-1);
                return xs_int(0);
            }
            return xs_int(num_i>0?1:num_i<0?-1:0);
        }
        if (strcmp(method, "to_char") == 0 || strcmp(method, "chr") == 0) {
            char buf[5]={0}; /* up to 4 UTF-8 bytes */
            uint32_t cp=(uint32_t)num_i;
            if (cp<0x80) { buf[0]=(char)cp; }
            else if (cp<0x800) {
                buf[0]=(char)(0xC0|(cp>>6));
                buf[1]=(char)(0x80|(cp&0x3F));
            } else if (cp<0x10000) {
                buf[0]=(char)(0xE0|(cp>>12));
                buf[1]=(char)(0x80|((cp>>6)&0x3F));
                buf[2]=(char)(0x80|(cp&0x3F));
            } else {
                buf[0]=(char)(0xF0|(cp>>18));
                buf[1]=(char)(0x80|((cp>>12)&0x3F));
                buf[2]=(char)(0x80|((cp>>6)&0x3F));
                buf[3]=(char)(0x80|(cp&0x3F));
            }
            return xs_str(buf);
        }
        if (strcmp(method, "digits") == 0) {
            Value *arr = xs_array_new();
            int64_t n = num_i < 0 ? -num_i : num_i;
            if (n == 0) {
                array_push(arr->arr, xs_int(0));
                return arr;
            }
            /* collect digits in reverse, then reverse the array */
            int count = 0;
            while (n > 0) {
                array_push(arr->arr, xs_int(n % 10));
                n /= 10;
                count++;
            }
            /* reverse in place */
            for (int j = 0; j < count / 2; j++) {
                Value *tmp = arr->arr->items[j];
                arr->arr->items[j] = arr->arr->items[count - 1 - j];
                arr->arr->items[count - 1 - j] = tmp;
            }
            return arr;
        }
        if (strcmp(method, "to_hex") == 0) {
            char buf[32];
            if (num_i < 0)
                snprintf(buf, sizeof(buf), "-0x%llx", (unsigned long long)(-num_i));
            else
                snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)num_i);
            return xs_str(buf);
        }
        if (strcmp(method, "to_bin") == 0) {
            char buf[80]; /* enough for 64-bit + "0b" + sign */
            uint64_t val = (uint64_t)(num_i < 0 ? -num_i : num_i);
            int pos = 0;
            if (num_i < 0) buf[pos++] = '-';
            buf[pos++] = '0'; buf[pos++] = 'b';
            if (val == 0) { buf[pos++] = '0'; buf[pos] = '\0'; }
            else {
                char tmp[65]; int tlen = 0;
                while (val > 0) { tmp[tlen++] = '0' + (val & 1); val >>= 1; }
                for (int j = tlen - 1; j >= 0; j--) buf[pos++] = tmp[j];
                buf[pos] = '\0';
            }
            return xs_str(buf);
        }
        if (strcmp(method, "to_oct") == 0) {
            char buf[32];
            if (num_i < 0)
                snprintf(buf, sizeof(buf), "-0o%llo", (unsigned long long)(-num_i));
            else
                snprintf(buf, sizeof(buf), "0o%llo", (unsigned long long)num_i);
            return xs_str(buf);
        }
        (void)num_f; (void)num_i;
    }

    if (VAL_TAG(obj) == XS_ENUM_VAL) {
        const char *variant = obj->en->variant;
        Value *inner = (obj->en->arr_data && obj->en->arr_data->len>0)
                       ? obj->en->arr_data->items[0] : XS_NULL_VAL;
        if (strcmp(method, "is_some") == 0) {
            return strcmp(variant,"Some")==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_none") == 0) {
            return strcmp(variant,"None")==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_ok") == 0) {
            return strcmp(variant,"Ok")==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "is_err") == 0) {
            return strcmp(variant,"Err")==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
        if (strcmp(method, "unwrap") == 0) {
            if (strcmp(variant,"Some")==0 || strcmp(variant,"Ok")==0)
                return value_incref(inner);
            fprintf(stderr, "xs: error at %s:%d:%d: unwrap called on %s\n",
                    i->current_span.file ? i->current_span.file : "<unknown>",
                    i->current_span.line, i->current_span.col, variant);
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "unwrap_or") == 0) {
            if (strcmp(variant,"Some")==0 || strcmp(variant,"Ok")==0)
                return value_incref(inner);
            return argc>0?value_incref(args[0]):value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "map") == 0) {
            if (argc<1) return value_incref(obj);
            if (strcmp(variant,"Some")==0) {
                Value *a[1]={inner};
                Value *r=call_value(i, args[0], a, 1, "option_map");
                if (i->cf.signal) return r;
                /* wrap in Some */
                XSEnum *en=xs_calloc(1,sizeof(XSEnum));
                en->type_name=xs_strdup(obj->en->type_name);
                en->variant=xs_strdup("Some");
                en->arr_data=xs_calloc(1,sizeof(XSArray));
                en->arr_data->len=1; en->arr_data->cap=1;
                en->arr_data->items=xs_malloc(sizeof(Value*));
                en->arr_data->items[0]=r;
                en->refcount=1;
                Value *ev=xs_calloc(1,sizeof(Value));
                ev->tag = XS_ENUM_VAL; ev->refcount=1; ev->en=en;
                return ev;
            }
            return value_incref(obj); /* None/Err pass through */
        }
        if (strcmp(method, "or_else") == 0) {
            if (strcmp(variant,"None")==0) {
                if (argc<1) return value_incref(XS_NULL_VAL);
                return call_value(i, args[0], NULL, 0, "or_else");
            }
            return value_incref(obj);
        }
        if (strcmp(method, "map_err") == 0) {
            if (argc<1) return value_incref(obj);
            if (strcmp(variant,"Err")==0) {
                Value *a[1]={inner};
                Value *r=call_value(i, args[0], a, 1, "map_err");
                if (i->cf.signal) return r;
                XSEnum *en=xs_calloc(1,sizeof(XSEnum));
                en->type_name=xs_strdup(obj->en->type_name);
                en->variant=xs_strdup("Err");
                en->arr_data=xs_calloc(1,sizeof(XSArray));
                en->arr_data->len=1; en->arr_data->cap=1;
                en->arr_data->items=xs_malloc(sizeof(Value*));
                en->arr_data->items[0]=r;
                en->refcount=1;
                Value *ev=xs_calloc(1,sizeof(Value));
                ev->tag = XS_ENUM_VAL; ev->refcount=1; ev->en=en;
                return ev;
            }
            return value_incref(obj); /* Ok passes through */
        }
        if (strcmp(method, "ok") == 0) {
            if (strcmp(variant,"Ok")==0) {
                /* wrap inner in Some */
                XSEnum *en=xs_calloc(1,sizeof(XSEnum));
                en->type_name=xs_strdup("Option");
                en->variant=xs_strdup("Some");
                en->arr_data=xs_calloc(1,sizeof(XSArray));
                en->arr_data->len=1; en->arr_data->cap=1;
                en->arr_data->items=xs_malloc(sizeof(Value*));
                en->arr_data->items[0]=value_incref(inner);
                en->refcount=1;
                Value *ev=xs_calloc(1,sizeof(Value));
                ev->tag = XS_ENUM_VAL; ev->refcount=1; ev->en=en;
                return ev;
            }
            /* Err → None */
            XSEnum *en=xs_calloc(1,sizeof(XSEnum));
            en->type_name=xs_strdup("Option");
            en->variant=xs_strdup("None");
            en->refcount=1;
            Value *ev=xs_calloc(1,sizeof(Value));
            ev->tag = XS_ENUM_VAL; ev->refcount=1; ev->en=en;
            return ev;
        }
    }

    if (VAL_TAG(obj) == XS_SIGNAL) {
        XSSignal *sig = obj->signal;
        if (strcmp(method, "get") == 0) {
            if (sig->compute) {
                return call_value(i, sig->compute, NULL, 0, "derived_compute");
            }
            return value_incref(sig->value);
        }
        if (strcmp(method, "set") == 0 && argc >= 1) {
            value_decref(sig->value);
            sig->value = value_incref(args[0]);
            if (!sig->notifying) {
                sig->notifying = 1;
                for (int j = 0; j < sig->nsubs; j++) {
                    Value *r = call_value(i, sig->subscribers[j], args, 1, "subscriber");
                    value_decref(r);
                }
                sig->notifying = 0;
            }
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "subscribe") == 0 && argc >= 1) {
            if (sig->nsubs >= sig->subcap) {
                sig->subcap = sig->subcap ? sig->subcap * 2 : 4;
                sig->subscribers = xs_realloc(sig->subscribers, sig->subcap * sizeof(Value*));
            }
            sig->subscribers[sig->nsubs++] = value_incref(args[0]);
            return value_incref(XS_NULL_VAL);
        }
        if (strcmp(method, "value") == 0) {
            if (sig->compute) {
                return call_value(i, sig->compute, NULL, 0, "derived_compute");
            }
            return value_incref(sig->value);
        }
        return value_incref(XS_NULL_VAL);
    }

    if (VAL_TAG(obj) == XS_RANGE) {
        if (strcmp(method, "len") == 0) {
            int64_t span = obj->range->end - obj->range->start;
            if (obj->range->inclusive) span += (span >= 0) ? 1 : -1;
            int64_t step = obj->range->step ? obj->range->step : 1;
            int64_t n2;
            if (step > 0) n2 = (span > 0) ? (span + step - 1) / step : 0;
            else           n2 = (span < 0) ? (-span + (-step) - 1) / (-step) : 0;
            return xs_int(n2);
        }
        if (strcmp(method, "start") == 0) return xs_int(obj->range->start);
        if (strcmp(method, "end") == 0)   return xs_int(obj->range->end);
        if (strcmp(method, "is_empty") == 0) {
            int64_t step = obj->range->step ? obj->range->step : 1;
            int64_t s = obj->range->start, e = obj->range->end;
            int empty;
            if (obj->range->inclusive) empty = (step > 0 ? s > e : s < e);
            else                        empty = (step > 0 ? s >= e : s <= e);
            return value_incref(empty ? XS_TRUE_VAL : XS_FALSE_VAL);
        }
        if (strcmp(method, "contains") == 0 && argc >= 1 && VAL_TAG(args[0]) == XS_INT) {
            int64_t v2 = VAL_INT(args[0]);
            int64_t step = obj->range->step ? obj->range->step : 1;
            int64_t s = obj->range->start, e = obj->range->end;
            int in;
            if (step > 0) {
                in = v2 >= s && (obj->range->inclusive ? v2 <= e : v2 < e) &&
                     ((v2 - s) % step == 0);
            } else {
                in = v2 <= s && (obj->range->inclusive ? v2 >= e : v2 > e) &&
                     ((s - v2) % (-step) == 0);
            }
            return value_incref(in ? XS_TRUE_VAL : XS_FALSE_VAL);
        }
        if (strcmp(method, "step") == 0) {
            /* No-arg form is a getter; one-arg form returns a fresh
               range with that step so `(1..10).step(2)` actually
               iterates by 2 instead of returning the integer 1. */
            if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) {
                Value *r = xs_range(obj->range->start, obj->range->end, obj->range->inclusive);
                if (r->range) r->range->step = VAL_INT(args[0]);
                return r;
            }
            return xs_int(obj->range->step ? obj->range->step : 1);
        }
        if (strcmp(method, "to_array") == 0) {
            Value *arr = xs_array_new();
            int64_t step = obj->range->step ? obj->range->step : 1;
            int64_t end2 = obj->range->inclusive ? obj->range->end + (step > 0 ? 1 : -1) : obj->range->end;
            for (int64_t j = obj->range->start; step > 0 ? j < end2 : j > end2; j += step)
                array_push(arr->arr, xs_int(j));
            return arr;
        }
        if (strcmp(method, "filter") == 0 || strcmp(method, "map") == 0 ||
            strcmp(method, "for_each") == 0 || strcmp(method, "fold") == 0 ||
            strcmp(method, "reduce") == 0 || strcmp(method, "any") == 0 ||
            strcmp(method, "all") == 0 || strcmp(method, "find") == 0 ||
            strcmp(method, "sum") == 0 || strcmp(method, "min") == 0 ||
            strcmp(method, "max") == 0 || strcmp(method, "count") == 0) {
            Value *arr = xs_array_new();
            int64_t step = obj->range->step ? obj->range->step : 1;
            int64_t end2 = obj->range->inclusive ? obj->range->end + (step > 0 ? 1 : -1) : obj->range->end;
            for (int64_t j2 = obj->range->start; step > 0 ? j2 < end2 : j2 > end2; j2 += step)
                array_push(arr->arr, xs_int(j2));
            Value *res2 = eval_method(i, arr, method, args, argc);
            value_decref(arr);
            return res2;
        }
        if (strcmp(method, "contains") == 0) {
            if (argc<1||VAL_TAG(args[0])!=XS_INT) return value_incref(XS_FALSE_VAL);
            int64_t v2=VAL_INT(args[0]);
            int64_t step = obj->range->step ? obj->range->step : 1;
            int ok;
            if (step > 0) {
                ok = v2 >= obj->range->start &&
                     (obj->range->inclusive ? v2 <= obj->range->end : v2 < obj->range->end) &&
                     ((v2 - obj->range->start) % step == 0);
            } else {
                ok = v2 <= obj->range->start &&
                     (obj->range->inclusive ? v2 >= obj->range->end : v2 > obj->range->end) &&
                     ((obj->range->start - v2) % (-step) == 0);
            }
            return ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        }
    }

    if (VAL_TAG(obj) == XS_ENUM_VAL && obj->en->type_name) {
        Value *type_mod = env_get(i->env, obj->en->type_name);
        if (type_mod && (VAL_TAG(type_mod) == XS_MODULE || VAL_TAG(type_mod) == XS_MAP)) {
            Value *impl_val = map_get(type_mod->map, "__impl__");
            if (impl_val && (VAL_TAG(impl_val) == XS_MAP || VAL_TAG(impl_val) == XS_MODULE)) {
                Value *fn = map_get(impl_val->map, method);
                if (fn && (VAL_TAG(fn) == XS_FUNC || VAL_TAG(fn) == XS_NATIVE)) {
                    Value **new_args = xs_malloc((argc+1)*sizeof(Value*));
                    new_args[0] = obj;
                    for (int j = 0; j < argc; j++) new_args[j+1] = args[j];
                    Value *r = call_value(i, fn, new_args, argc+1, method);
                    free(new_args);
                    return r;
                }
            }
        }
    }

    if (VAL_TAG(obj) == XS_CLASS_VAL && obj->cls) {
        Value *sfn = NULL;
        if (obj->cls->static_methods) sfn = map_get(obj->cls->static_methods, method);
        if (!sfn && obj->cls->methods) sfn = map_get(obj->cls->methods, method);
        if (sfn && (VAL_TAG(sfn) == XS_FUNC || VAL_TAG(sfn) == XS_NATIVE))
            return call_value(i, sfn, args, argc, method);
    }

    Value *fn = env_get(i->env, method);
    if (fn) return call_value(i, fn, args, argc, method);

    /* check plugin method registry */
    {
        const char *tname = value_type_str(obj);
        Value *pfn = plugin_lookup_method(tname, method);
        if (pfn) {
            Value **new_args = xs_malloc((argc + 1) * sizeof(Value*));
            new_args[0] = value_incref(obj);
            for (int j = 0; j < argc; j++) new_args[j + 1] = args[j];
            Value *r = call_value(i, pfn, new_args, argc + 1, method);
            value_decref(new_args[0]);
            free(new_args);
            return r;
        }
    }

    const char *tname = value_type_str(obj);
    static const char *str_methods[] = {"len","trim","upper","lower","split","contains","replace","starts_with","ends_with","chars","to_str","bytes","repeat","join","slice","index_of","parse_int","parse_float","is_empty","reverse","capitalize","pad_left","pad_right","count",NULL};
    static const char *arr_methods[] = {"len","push","pop","map","filter","reduce","sort","reverse","contains","join","slice","flatten","chunks","enumerate","zip","any","all","find","index_of",NULL};
    static const char *map_methods[] = {"len","keys","values","contains","remove","clear","entries","merge","filter","map","clone",NULL};
    static const char *num_methods[] = {"abs","to_str","floor","ceil","round","clamp",NULL};
    const char **methods_list = NULL;
    switch (VAL_TAG(obj)) {
        case XS_STR: methods_list = str_methods; break;
        case XS_ARRAY: case XS_TUPLE: methods_list = arr_methods; break;
        case XS_MAP: methods_list = map_methods; break;
        case XS_INT: case XS_FLOAT: methods_list = num_methods; break;
        default: break;
    }
    char hint_buf[256] = {0};
    if (methods_list) {
        const char *best = NULL;
        int best_dist = 4;
        for (const char **m = methods_list; *m; m++) {
            int d = xs_edit_distance(method, *m);
            if (d > 0 && d < best_dist) { best_dist = d; best = *m; }
        }
        if (best)
            snprintf(hint_buf, sizeof hint_buf, "did you mean '%s'?", best);
        else {
            /* List a few available methods */
            int pos = 0;
            pos += snprintf(hint_buf + pos, sizeof(hint_buf) - pos, "available: ");
            for (const char **m = methods_list; *m && pos < (int)sizeof(hint_buf) - 20; m++) {
                if (m != methods_list) pos += snprintf(hint_buf + pos, sizeof(hint_buf) - pos, ", ");
                pos += snprintf(hint_buf + pos, sizeof(hint_buf) - pos, "%s", *m);
            }
        }
    }
    char label_buf[128];
    snprintf(label_buf, sizeof label_buf, "no method '%s' on type '%s'", method, tname);
    xs_runtime_error(i->current_span, label_buf,
            hint_buf[0] ? hint_buf : NULL,
            "value of type '%s' has no method '%s'", tname, method);
    i->cf.signal = CF_ERROR;
    return value_incref(XS_NULL_VAL);
}

static Value *eval_binop(Interp *i, Node *n) {
    const char *op = n->binop.op;

    if (strcmp(op,"&&")==0 || strcmp(op,"and")==0) {
        Value *l = EVAL(i, n->binop.left);
        if (!value_truthy(l)) return l;
        value_decref(l);
        return EVAL(i, n->binop.right);
    }
    if (strcmp(op,"||")==0 || strcmp(op,"or")==0) {
        Value *l = EVAL(i, n->binop.left);
        if (value_truthy(l)) return l;
        value_decref(l);
        return EVAL(i, n->binop.right);
    }
    if (strcmp(op,"??")==0) {
        Value *l = EVAL(i, n->binop.left);
        if (VAL_TAG(l) != XS_NULL) return l;
        value_decref(l);
        return EVAL(i, n->binop.right);
    }

    Value *left  = EVAL(i, n->binop.left);
    if (i->cf.signal) return left;
    Value *right = EVAL(i, n->binop.right);
    if (i->cf.signal) {
        value_decref(left);
        return right;
    }
    Value *result = NULL;

    if (strcmp(op, "<=>") == 0) {
        if (VAL_TAG(left) == XS_INT && VAL_TAG(right) == XS_INT) {
            int64_t cmp = (VAL_INT(left) > VAL_INT(right)) - (VAL_INT(left) < VAL_INT(right));
            result = xs_int(cmp);
        } else if ((VAL_TAG(left) == XS_INT || VAL_TAG(left) == XS_FLOAT) &&
                   (VAL_TAG(right) == XS_INT || VAL_TAG(right) == XS_FLOAT)) {
            double a = (VAL_TAG(left) == XS_INT) ? (double)VAL_INT(left) : left->f;
            double b = (VAL_TAG(right) == XS_INT) ? (double)VAL_INT(right) : right->f;
            result = xs_int((a > b) - (a < b));
        } else if (VAL_TAG(left) == XS_STR && VAL_TAG(right) == XS_STR) {
            int cmp = strcmp(left->s, right->s);
            result = xs_int((cmp > 0) - (cmp < 0));
        } else {
            result = xs_int(0);
        }
        goto done;
    }

    /* struct/class operator overloading: check before numeric paths */
    if (VAL_TAG(left) == XS_STRUCT_VAL || VAL_TAG(left) == XS_ENUM_VAL) {
        Value *op_fn = env_get(i->env, op);
        if (op_fn && (VAL_TAG(op_fn) == XS_FUNC || VAL_TAG(op_fn) == XS_NATIVE)) {
            Value *call_args[2] = { left, right };
            result = call_value(i, op_fn, call_args, 2, op);
            goto done;
        }
    }

    /* Duration arithmetic. Two durations add/sub in ns; comparisons map
       directly. dur*scalar / scalar*dur / dur/scalar scale ns. dur/dur
       is a unitless float ratio. Anything else with a duration is a
       type error. */
    if (VAL_TAG(left) == XS_DURATION || VAL_TAG(right) == XS_DURATION) {
        int ld = VAL_TAG(left) == XS_DURATION;
        int rd = VAL_TAG(right) == XS_DURATION;
        int ls = VAL_TAG(left) == XS_INT || VAL_TAG(left) == XS_FLOAT;
        int rs = VAL_TAG(right) == XS_INT || VAL_TAG(right) == XS_FLOAT;
        int64_t lns = ld ? left->i : 0;
        int64_t rns = rd ? right->i : 0;
        double lscale = ls ? (VAL_TAG(left) == XS_FLOAT ? left->f : (double)VAL_INT(left)) : 0.0;
        double rscale = rs ? (VAL_TAG(right) == XS_FLOAT ? right->f : (double)VAL_INT(right)) : 0.0;
        if (op[0]=='=' && op[1]=='=') { result = (ld && rd && lns == rns) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='!' && op[1]=='=') { result = !(ld && rd && lns == rns) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
        if (ld && rd) {
            if (op[0]=='<' && op[1]=='\0') { result = lns <  rns ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
            if (op[0]=='>' && op[1]=='\0') { result = lns >  rns ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
            if (op[0]=='<' && op[1]=='=') { result = lns <= rns ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
            if (op[0]=='>' && op[1]=='=') { result = lns >= rns ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL); goto done; }
            if (op[0]=='+' && op[1]=='\0') { result = xs_duration(lns + rns); goto done; }
            if (op[0]=='-' && op[1]=='\0') { result = xs_duration(lns - rns); goto done; }
            if (op[0]=='/' && op[1]=='\0') {
                if (rns == 0) { xs_runtime_error(n->span, "division by zero", NULL, "cannot divide by zero"); result = value_incref(XS_NULL_VAL); goto done; }
                result = xs_float((double)lns / (double)rns); goto done;
            }
            if (op[0]=='%' && op[1]=='\0') {
                if (rns == 0) { xs_runtime_error(n->span, "modulo by zero", NULL, "cannot take remainder with divisor zero"); result = value_incref(XS_NULL_VAL); goto done; }
                result = xs_duration(lns % rns); goto done;
            }
        }
        if (ld && rs && op[0]=='*' && op[1]=='\0') { result = xs_duration((int64_t)((double)lns * rscale)); goto done; }
        if (ls && rd && op[0]=='*' && op[1]=='\0') { result = xs_duration((int64_t)(lscale * (double)rns)); goto done; }
        if (ld && rs && op[0]=='/' && op[1]=='\0') {
            if (rscale == 0.0) { xs_runtime_error(n->span, "division by zero", NULL, "cannot divide by zero"); result = value_incref(XS_NULL_VAL); goto done; }
            result = xs_duration((int64_t)((double)lns / rscale)); goto done;
        }
        xs_runtime_error(n->span, "type mismatch", NULL, "operator '%s' is not defined for these operands", op);
        result = value_incref(XS_NULL_VAL); goto done;
    }

    if (VAL_TAG(left) == XS_INT && VAL_TAG(right) == XS_INT) {
        int64_t a = VAL_INT(left), b = VAL_INT(right);
        if (op[0]=='=' && op[1]=='=') { result=a==b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='\0') { result=a<b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='\0') { result=a>b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='=') { result=a<=b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='=') { result=a>=b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='!' && op[1]=='=') { result=a!=b?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='+' && op[1]=='\0') {
            result = xs_safe_add(a, b); goto done;
        }
        if (op[0]=='-' && op[1]=='\0') {
            result = xs_safe_sub(a, b); goto done;
        }
        if (op[0]=='*' && op[1]=='\0') {
            result = xs_safe_mul(a, b); goto done;
        }
        if (op[0]=='/' && op[1]=='\0') {
            if (!b) { xs_runtime_error(n->span, "division by zero", NULL, "cannot divide by zero"); result=value_incref(XS_NULL_VAL); goto done; }
            result=xs_int(a/b); goto done;
        }
        if (op[0]=='%' && op[1]=='\0') {
            if (!b) { xs_runtime_error(n->span, "modulo by zero", NULL, "cannot take remainder with divisor zero"); result=value_incref(XS_NULL_VAL); goto done; }
            /* Truncated modulo (sign follows the dividend), matching C. */
            result=xs_int(a % b); goto done;
        }
        if (op[0]=='*' && op[1]=='*') {
            if (b < 0) { result=xs_float(pow((double)a,(double)b)); goto done; }
            result = xs_safe_pow(a, b); goto done;
        }
        if (op[0]=='/' && op[1]=='/') {
            if (!b) { xs_runtime_error(n->span, "division by zero", NULL, "cannot floor-divide by zero"); result=value_incref(XS_NULL_VAL); goto done; }
            int64_t q = a / b;
            if ((a ^ b) < 0 && a % b != 0) q--; /* floor toward -inf */
            result=xs_int(q); goto done;
        }
        if (op[0]=='&' && op[1]=='\0') { result=xs_int(a&b); goto done; }
        if (op[0]=='|' && op[1]=='\0') { result=xs_int(a|b); goto done; }
        if (op[0]=='^' && op[1]=='\0') { result=xs_int(a^b); goto done; }
        if (op[0]=='<' && op[1]=='<') {
            if (b < 0) {
                xs_runtime_error(n->span, "ValueError", NULL, "shift count cannot be negative");
                result = value_incref(XS_NULL_VAL); goto done;
            }
            if (b >= 64) { result = xs_int(0); goto done; }
            if (b > 0 && (a >> (63 - b)) != 0 && (a >> (63 - b)) != -1) {
                XSBigInt *bi = bigint_from_i64(a);
                XSBigInt *out = bigint_shl(bi, (int)b);
                bigint_free(bi);
                result = xs_bigint_val(out); goto done;
            }
            result=xs_int(a<<b); goto done;
        }
        if (op[0]=='>' && op[1]=='>') {
            if (b < 0) {
                xs_runtime_error(n->span, "ValueError", NULL, "shift count cannot be negative");
                result = value_incref(XS_NULL_VAL); goto done;
            }
            if (b >= 64) { result = xs_int(a < 0 ? -1 : 0); goto done; }
            result=xs_int(a>>b); goto done;
        }
    }

    if ((VAL_TAG(left) == XS_INT || VAL_TAG(left) == XS_BIGINT) &&
        (VAL_TAG(right) == XS_INT || VAL_TAG(right) == XS_BIGINT) &&
        (VAL_TAG(left) == XS_BIGINT || VAL_TAG(right) == XS_BIGINT)) {
        if (op[0]=='+' && op[1]=='\0') { result=xs_numeric_add(left,right); goto done; }
        if (op[0]=='-' && op[1]=='\0') { result=xs_numeric_sub(left,right); goto done; }
        if (op[0]=='*' && op[1]=='\0') { result=xs_numeric_mul(left,right); goto done; }
        if (op[0]=='/' && op[1]=='\0') { result=xs_numeric_div(left,right); goto done; }
        if (op[0]=='%' && op[1]=='\0') { result=xs_numeric_mod(left,right); goto done; }
        if (op[0]=='*' && op[1]=='*') { result=xs_numeric_pow(left,right); goto done; }
        if (op[0]=='/' && op[1]=='/') { result=xs_numeric_floordiv(left,right); goto done; }
        if (op[0]=='=' && op[1]=='=') { result=value_equal(left,right)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='!' && op[1]=='=') { result=!value_equal(left,right)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        int cmp = value_cmp(left, right);
        if (op[0]=='<' && op[1]=='\0') { result=cmp<0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='\0') { result=cmp>0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='=') { result=cmp<=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='=') { result=cmp>=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
    }

    /* Lexicographic array/tuple comparisons: delegate to value_cmp, which
       already handles the shorter-side-is-less rule. */
    if ((VAL_TAG(left) == XS_ARRAY || VAL_TAG(left) == XS_TUPLE) &&
        VAL_TAG(left) == VAL_TAG(right)) {
        int cmp = value_cmp(left, right);
        if (op[0]=='<' && op[1]=='\0') { result=cmp<0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='\0') { result=cmp>0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='=') { result=cmp<=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='=') { result=cmp>=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
    }

    if ((VAL_TAG(left)==XS_INT||VAL_TAG(left)==XS_FLOAT||VAL_TAG(left)==XS_BIGINT) &&
        (VAL_TAG(right)==XS_INT||VAL_TAG(right)==XS_FLOAT||VAL_TAG(right)==XS_BIGINT)) {
        double a = VAL_TAG(left)==XS_FLOAT ? left->f : (VAL_TAG(left)==XS_BIGINT ? bigint_to_double(left->bigint) : (double)VAL_INT(left));
        double b2 = VAL_TAG(right)==XS_FLOAT ? right->f : (VAL_TAG(right)==XS_BIGINT ? bigint_to_double(right->bigint) : (double)VAL_INT(right));
        if (op[0]=='+') { result=xs_float(a+b2); goto done; }
        if (op[0]=='-') { result=xs_float(a-b2); goto done; }
        if (op[0]=='*' && op[1]=='\0') { result=xs_float(a*b2); goto done; }
        if (op[0]=='/' && op[1]=='\0') {
            /* Float division: IEEE 754 rules. 1.0/0 -> Infinity,
               0.0/0.0 -> NaN. Keeps `nan == nan` true-to-form for
               numeric code; int/int by zero is the only path that
               throws (caught earlier in the int-int block). */
            result=xs_float(a/b2); goto done;
        }
        if (op[0]=='%') { result=xs_float(fmod(a,b2)); goto done; }
        if (op[0]=='*' && op[1]=='*') { result=xs_float(pow(a,b2)); goto done; }
        if (op[0]=='/' && op[1]=='/') { result=xs_float(floor(a/b2)); goto done; }
        if (op[0]=='=' && op[1]=='=') { result=a==b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='!' && op[1]=='=') { result=a!=b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='\0') { result=a<b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='\0') { result=a>b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<' && op[1]=='=') { result=a<=b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='=') { result=a>=b2?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
    }

    if ((VAL_TAG(left) == XS_STR || VAL_TAG(right) == XS_STR) && op[0]=='+' && op[1]=='\0') {
        char *ls = value_str(left);
        char *rs = value_str(right);
        int la=(int)strlen(ls), lb=(int)strlen(rs);
        char *buf=xs_malloc(la+lb+1);
        memcpy(buf,ls,la); memcpy(buf+la,rs,lb+1);
        free(ls); free(rs);
        result=xs_str(buf); free(buf); goto done;
    }
    if (VAL_TAG(left) == XS_STR) {
        if (op[0]=='+') {
            char *rs = value_str(right);
            int la = (int)strlen(left->s), lb = (int)strlen(rs);
            char *buf = xs_malloc(la+lb+1);
            memcpy(buf, left->s, la);
            memcpy(buf+la, rs, lb+1);
            free(rs);
            result = xs_str(buf); free(buf); goto done;
        }
        if (op[0]=='+' && op[1]=='+') {
            char *rs = value_str(right);
            int la=(int)strlen(left->s), lb=(int)strlen(rs);
            char *buf=xs_malloc(la+lb+1);
            memcpy(buf,left->s,la); memcpy(buf+la,rs,lb+1);
            free(rs); result=xs_str(buf); free(buf); goto done;
        }
        if (op[0]=='=' && op[1]=='=') {
            result = (VAL_TAG(right)==XS_STR && strcmp(left->s,right->s)==0) ?
                     value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
            goto done;
        }
        if (op[0]=='!' && op[1]=='=') {
            result = (VAL_TAG(right)!=XS_STR || strcmp(left->s,right->s)!=0) ?
                     value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
            goto done;
        }
        if (op[0]=='<' && op[1]=='=') { result=value_cmp(left,right)<=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>' && op[1]=='=') { result=value_cmp(left,right)>=0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='<') { result=value_cmp(left,right)<0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='>') { result=value_cmp(left,right)>0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL); goto done; }
        if (op[0]=='*' && VAL_TAG(right)==XS_INT) {
            int n2 = (int)VAL_INT(right); if (n2<0) n2=0;
            int slen=(int)strlen(left->s);
            char *buf=xs_malloc(slen*n2+1); buf[0]='\0';
            for (int j=0;j<n2;j++) strcat(buf,left->s);
            result=xs_str(buf); free(buf); goto done;
        }
    }

    /* int * str: same as str * int. Make repetition commutative. */
    if (op[0]=='*' && op[1]=='\0' &&
        VAL_TAG(left)==XS_INT && VAL_TAG(right)==XS_STR) {
        int n2 = (int)VAL_INT(left); if (n2<0) n2=0;
        int slen=(int)strlen(right->s);
        char *buf=xs_malloc(slen*n2+1); buf[0]='\0';
        for (int j=0;j<n2;j++) strcat(buf,right->s);
        result=xs_str(buf); free(buf); goto done;
    }

    /* arr * int / int * arr: array repetition, mirrors string. */
    if (op[0]=='*' && op[1]=='\0' &&
        ((VAL_TAG(left)==XS_ARRAY && VAL_TAG(right)==XS_INT) ||
         (VAL_TAG(left)==XS_INT && VAL_TAG(right)==XS_ARRAY))) {
        Value *av = VAL_TAG(left)==XS_ARRAY ? left : right;
        Value *iv = VAL_TAG(left)==XS_ARRAY ? right : left;
        int64_t count = VAL_INT(iv);
        Value *out = xs_array_new();
        if (count > 0 && av->arr) {
            for (int64_t ci = 0; ci < count; ci++)
                for (int j = 0; j < av->arr->len; j++)
                    array_push(out->arr, value_incref(av->arr->items[j]));
        }
        result = out; goto done;
    }

    if (op[0]=='+' && op[1]=='+') {
        if (VAL_TAG(left)==XS_ARRAY) {
            Value *res=xs_array_new();
            XSArray *la=left->arr;
            for (int j=0;j<la->len;j++) array_push(res->arr, value_incref(la->items[j]));
            if (VAL_TAG(right)==XS_ARRAY) {
                XSArray *ra=right->arr;
                for (int j=0;j<ra->len;j++) array_push(res->arr, value_incref(ra->items[j]));
            }
            result=res; goto done;
        }
        /* ++ on two plain maps merges them. Skip when either has _type
           so typed wrappers (Set, Counter, class instances, ...) keep
           their identity instead of silently becoming bare maps. */
        if (VAL_TAG(left)==XS_MAP && VAL_TAG(right)==XS_MAP &&
            left->map && right->map &&
            !map_get(left->map, "_type") && !map_get(right->map, "_type")) {
            Value *res = xs_map_new();
            int nk = 0;
            char **keys = map_keys(left->map, &nk);
            for (int j = 0; j < nk; j++) {
                Value *v = map_get(left->map, keys[j]);
                if (v) map_set(res->map, keys[j], v);
                free(keys[j]);
            }
            free(keys);
            keys = map_keys(right->map, &nk);
            for (int j = 0; j < nk; j++) {
                Value *v = map_get(right->map, keys[j]);
                if (v) map_set(res->map, keys[j], v);
                free(keys[j]);
            }
            free(keys);
            result = res; goto done;
        }
    }

    /* array + array concatenates (same shape as ++ for arrays). Without
       this branch the two arrays fell through to the float coerce and
       silently produced null. */
    if (op[0]=='+' && op[1]=='\0' &&
        VAL_TAG(left)==XS_ARRAY && VAL_TAG(right)==XS_ARRAY) {
        Value *res=xs_array_new();
        for (int j=0;j<left->arr->len;j++)
            array_push(res->arr, left->arr->items[j]);
        for (int j=0;j<right->arr->len;j++)
            array_push(res->arr, right->arr->items[j]);
        result=res; goto done;
    }

    if (strcmp(op, "is") == 0) {
        const char *tname = (VAL_TAG(right) == XS_STR) ? right->s : "";
        int match = 0;
        if (strcmp(tname, "int") == 0 || strcmp(tname, "i64") == 0)
            match = (VAL_TAG(left) == XS_INT);
        else if (strcmp(tname, "float") == 0 || strcmp(tname, "f64") == 0)
            match = (VAL_TAG(left) == XS_FLOAT);
        else if (strcmp(tname, "str") == 0 || strcmp(tname, "string") == 0)
            match = (VAL_TAG(left) == XS_STR);
        else if (strcmp(tname, "bool") == 0)
            match = (VAL_TAG(left) == XS_BOOL);
        else if (strcmp(tname, "array") == 0)
            match = (VAL_TAG(left) == XS_ARRAY);
        else if (strcmp(tname, "map") == 0)
            match = (VAL_TAG(left) == XS_MAP);
        else if (strcmp(tname, "null") == 0)
            match = (VAL_TAG(left) == XS_NULL);
        else if (strcmp(tname, "fn") == 0 || strcmp(tname, "function") == 0)
            match = (VAL_TAG(left) == XS_FUNC || VAL_TAG(left) == XS_NATIVE);
        else if (strcmp(tname, "tuple") == 0)
            match = (VAL_TAG(left) == XS_TUPLE);
        else if (VAL_TAG(left) == XS_STRUCT_VAL && left->st)
            match = (strcmp(left->st->type_name, tname) == 0);
        else if (VAL_TAG(left) == XS_ENUM_VAL && left->en)
            match = (strcmp(left->en->type_name, tname) == 0);
        else if (VAL_TAG(left) == XS_INST && left->inst && left->inst->class_) {
            /* Walk the inheritance chain so `dog is Animal` is true
               when Dog : Animal. */
            XSClass *c = left->inst->class_;
            XSClass *stack[64]; int sp = 0;
            stack[sp++] = c;
            while (sp > 0) {
                XSClass *cur = stack[--sp];
                if (cur && cur->name && strcmp(cur->name, tname) == 0) {
                    match = 1; break;
                }
                if (cur)
                    for (int b = 0; b < cur->nbases && sp < 64; b++)
                        if (cur->bases[b]) stack[sp++] = cur->bases[b];
            }
        }
        result = match ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
        goto done;
    }

    if (strcmp(op,"in")==0) {
        if (VAL_TAG(right)==XS_ARRAY) {
            for (int j=0;j<right->arr->len;j++)
                if (value_equal(left,right->arr->items[j])) { result=value_incref(XS_TRUE_VAL); goto done; }
            result=value_incref(XS_FALSE_VAL); goto done;
        }
        if (VAL_TAG(right)==XS_MAP || VAL_TAG(right)==XS_MODULE) {
            if (VAL_TAG(left)==XS_STR) result=map_has(right->map,left->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            else result=value_incref(XS_FALSE_VAL);
            goto done;
        }
        if (VAL_TAG(right)==XS_STR && VAL_TAG(left)==XS_STR) {
            result=strstr(right->s,left->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            goto done;
        }
        if (VAL_TAG(right)==XS_RANGE) {
            if (VAL_TAG(left)==XS_INT) {
                int64_t v2=VAL_INT(left);
                int ok=v2>=right->range->start &&
                       (right->range->inclusive?v2<=right->range->end:v2<right->range->end);
                result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            } else result=value_incref(XS_FALSE_VAL);
            goto done;
        }
        result=value_incref(XS_FALSE_VAL); goto done;
    }

    if (strcmp(op,"not in")==0) {
        Node fakeop = *n;
        char tmp[8]; strcpy(tmp,"in");
        memcpy(fakeop.binop.op, tmp, 8);
        Value *inr = eval_binop(i, &fakeop);
        result = value_truthy(inr) ? value_incref(XS_FALSE_VAL) : value_incref(XS_TRUE_VAL);
        value_decref(inr); goto done;
    }

    /* operator overloading via dunder methods */
    if (VAL_TAG(left) == XS_INST) {
        const char *dunder = NULL;
        if (op[0]=='+' && op[1]=='\0') dunder = "__add__";
        else if (op[0]=='-' && op[1]=='\0') dunder = "__sub__";
        else if (op[0]=='*' && op[1]=='\0') dunder = "__mul__";
        else if (op[0]=='/' && op[1]=='\0') dunder = "__div__";
        else if (op[0]=='%' && op[1]=='\0') dunder = "__mod__";
        else if (op[0]=='=' && op[1]=='=') dunder = "__eq__";
        else if (op[0]=='!' && op[1]=='=') dunder = "__ne__";
        else if (op[0]=='<' && op[1]=='\0') dunder = "__lt__";
        else if (op[0]=='>' && op[1]=='\0') dunder = "__gt__";
        else if (op[0]=='<' && op[1]=='=') dunder = "__le__";
        else if (op[0]=='>' && op[1]=='=') dunder = "__ge__";

        Value *op_fn = NULL;
        if (dunder) op_fn = map_get(left->inst->methods, dunder);
        if (!op_fn) op_fn = map_get(left->inst->methods, op);
        if (!op_fn && left->inst->class_) {
            if (dunder) op_fn = map_get(left->inst->class_->methods, dunder);
            if (!op_fn) op_fn = map_get(left->inst->class_->methods, op);
        }
        if (!op_fn && op[0]=='!' && op[1]=='=') {
            op_fn = map_get(left->inst->methods, "__eq__");
            if (!op_fn && left->inst->class_)
                op_fn = map_get(left->inst->class_->methods, "__eq__");
            if (op_fn && (VAL_TAG(op_fn) == XS_FUNC || VAL_TAG(op_fn) == XS_NATIVE)) {
                int has_self = 0;
                if (VAL_TAG(op_fn) == XS_FUNC && op_fn->fn->nparams > 0) {
                    Node *p0 = op_fn->fn->params[0];
                    if (VAL_TAG(p0) == NODE_PAT_IDENT && strcmp(p0->pat_ident.name, "self") == 0)
                        has_self = 1;
                }
                Value *call_args[2] = { left, right };
                Value *eq_result = call_value(i, op_fn, call_args, has_self ? 2 : 2,
                                              "__eq__");
                result = value_truthy(eq_result) ?
                    value_incref(XS_FALSE_VAL) : value_incref(XS_TRUE_VAL);
                value_decref(eq_result);
                goto done;
            }
            op_fn = NULL;
        }
        if (!op_fn && op[0]=='>' && op[1]=='\0') {
            op_fn = map_get(left->inst->methods, "__lt__");
            if (!op_fn && left->inst->class_)
                op_fn = map_get(left->inst->class_->methods, "__lt__");
            if (op_fn && VAL_TAG(right) == XS_INST) {
                Value *call_args[2] = { right, left };
                result = call_value(i, op_fn, call_args, 2, "__lt__");
                goto done;
            }
            op_fn = NULL;
        }
        if (op_fn && (VAL_TAG(op_fn) == XS_FUNC || VAL_TAG(op_fn) == XS_NATIVE)) {
            Value *call_args[2] = { left, right };
            result = call_value(i, op_fn, call_args, 2, dunder ? dunder : op);
            goto done;
        }
    }

    if (op[0]=='=' && op[1]=='=') {
        result=value_equal(left,right)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        goto done;
    }
    if (op[0]=='!' && op[1]=='=') {
        result=value_equal(left,right)?value_incref(XS_FALSE_VAL):value_incref(XS_TRUE_VAL);
        goto done;
    }

    /* Arithmetic / numeric operator applied to types where we have no
       sensible interpretation (e.g. "x" - "y"). Emit a catchable runtime
       error rather than silently returning null, so try/catch sees a
       real exception. */
    if (op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/' ||
        op[0] == '%' || op[0] == '<' || op[0] == '>') {
        xs_runtime_error(n->span, "type mismatch", NULL,
                         "operator '%s' is not defined for these operands", op);
    }
    result = value_incref(XS_NULL_VAL);

done:
    value_decref(left);
    value_decref(right);
    return result ? result : value_incref(XS_NULL_VAL);
}

static void interp_for_each(Interp *i, Value *iter,
                              Node *pat, Node *body) {
    if (VAL_TAG(iter) == XS_ARRAY || VAL_TAG(iter) == XS_TUPLE) {
        XSArray *arr = iter->arr;
        for (int j = 0; j < arr->len; j++) {
            push_env(i);
            bind_pattern(i, pat, arr->items[j], i->env, 1);
            interp_exec(i, body);
            pop_env(i);
            if (i->cf.signal == CF_BREAK) { CF_CLEAR(i); break; }
            if (i->cf.signal == CF_CONTINUE) { CF_CLEAR(i); continue; }
            if (i->cf.signal) break;
        }
    } else if (VAL_TAG(iter) == XS_RANGE) {
        XSRange *r = iter->range;
        int64_t step = r->step ? r->step : 1;
        int64_t end2 = r->inclusive ? r->end + (step > 0 ? 1 : -1) : r->end;
        for (int64_t j = r->start; step > 0 ? j < end2 : j > end2; j += step) {
            Value *v = xs_int(j);
            push_env(i);
            bind_pattern(i, pat, v, i->env, 1);
            value_decref(v);
            interp_exec(i, body);
            pop_env(i);
            if (i->cf.signal == CF_BREAK) { CF_CLEAR(i); break; }
            if (i->cf.signal == CF_CONTINUE) { CF_CLEAR(i); continue; }
            if (i->cf.signal) break;
        }
    } else if (VAL_TAG(iter) == XS_STR) {
        const char *s = iter->s;
        for (int j = 0; s[j]; j++) {
            Value *v = xs_str_n(s+j, 1);
            push_env(i);
            bind_pattern(i, pat, v, i->env, 1);
            value_decref(v);
            interp_exec(i, body);
            pop_env(i);
            if (i->cf.signal == CF_BREAK) { CF_CLEAR(i); break; }
            if (i->cf.signal == CF_CONTINUE) { CF_CLEAR(i); continue; }
            if (i->cf.signal) break;
        }
    } else if (VAL_TAG(iter) == XS_MAP || VAL_TAG(iter) == XS_MODULE) {
        int nkeys = 0;
        int want_pairs = (pat && VAL_TAG(pat) == NODE_PAT_TUPLE);
        char **keys = map_keys(iter->map, &nkeys);
        for (int j = 0; j < nkeys; j++) {
            Value *v;
            if (want_pairs) {
                v = xs_tuple_new();
                Value *ks = xs_str(keys[j]);
                Value *val = map_get(iter->map, keys[j]);
                array_push(v->arr, ks);
                array_push(v->arr, val ? val : XS_NULL_VAL);
                value_decref(ks);
            } else {
                v = xs_str(keys[j]);
            }
            push_env(i);
            bind_pattern(i, pat, v, i->env, 1);
            value_decref(v);
            interp_exec(i, body);
            pop_env(i);
            free(keys[j]);
            if (i->cf.signal == CF_BREAK) { CF_CLEAR(i); break; }
            if (i->cf.signal == CF_CONTINUE) { CF_CLEAR(i); continue; }
            if (i->cf.signal) { /* free remaining */ for(int k=j+1;k<nkeys;k++) free(keys[k]); break; }
        }
        free(keys);
    } else {
        /* Anything else (null, int, float, bool, ...) used to silently
           do nothing. Raise a catchable error so typos like
           `for x in user.entries` (where entries is missing) surface. */
        char *s = value_str(iter);
        xs_runtime_error(span_zero(), "TypeError", NULL,
                         "for-in expected an iterable, got %s",
                         s ? s : "<unprintable>");
        free(s);
    }
}

/* recursive list comprehension: nest for clauses */
static void list_comp_recurse(Interp *i, Node *n, Value *result, int cl) {
    int nclauses = n->list_comp.clause_pats.len;
    if (cl >= nclauses) {
        Value *elem = interp_eval(i, n->list_comp.element);
        if (!i->cf.signal) array_push(result->arr, elem);
        else value_decref(elem);
        return;
    }
    Node *pat = n->list_comp.clause_pats.items[cl];
    Node *iter_expr = n->list_comp.clause_iters.items[cl];
    Node *cond = (cl < n->list_comp.clause_conds.len)
                 ? n->list_comp.clause_conds.items[cl] : NULL;
    Value *iter_val = interp_eval(i, iter_expr);
    if (i->cf.signal) { value_decref(iter_val); return; }

    int iter_len = 0;
    Value **iter_items = NULL;
    Value *range_arr = NULL;
    if (VAL_TAG(iter_val) == XS_ARRAY || VAL_TAG(iter_val) == XS_TUPLE) {
        iter_len = iter_val->arr->len;
        iter_items = iter_val->arr->items;
    } else if (VAL_TAG(iter_val) == XS_RANGE) {
        range_arr = xs_array_new();
        int64_t start = iter_val->range->start;
        int64_t end = iter_val->range->end;
        int64_t step = iter_val->range->step ? iter_val->range->step : 1;
        if (step > 0) {
            int64_t e = iter_val->range->inclusive ? end + 1 : end;
            for (int64_t ri = start; ri < e; ri += step)
                array_push(range_arr->arr, xs_int(ri));
        } else if (step < 0) {
            int64_t e = iter_val->range->inclusive ? end - 1 : end;
            for (int64_t ri = start; ri > e; ri += step)
                array_push(range_arr->arr, xs_int(ri));
        }
        iter_len = range_arr->arr->len;
        iter_items = range_arr->arr->items;
    } else if (VAL_TAG(iter_val) == XS_MAP || VAL_TAG(iter_val) == XS_MODULE) {
        /* Iterate map entries the same way the for-loop does: emit a
           (key, value) tuple when the pattern is a tuple, else just the
           key. Lets `[k for k in m]` and `[(k, v) for k, v in m]` both
           work without forcing the user to pre-call `.entries()`. */
        int want_pairs = (pat && VAL_TAG(pat) == NODE_PAT_TUPLE);
        int nkeys = 0;
        char **keys = map_keys(iter_val->map, &nkeys);
        range_arr = xs_array_new();
        for (int j = 0; j < nkeys; j++) {
            if (want_pairs) {
                Value *t = xs_tuple_new();
                Value *ks = xs_str(keys[j]);
                Value *val = map_get(iter_val->map, keys[j]);
                array_push(t->arr, ks);
                array_push(t->arr, val ? val : XS_NULL_VAL);
                value_decref(ks);
                array_push(range_arr->arr, t);
                value_decref(t);
            } else {
                Value *ks = xs_str(keys[j]);
                array_push(range_arr->arr, ks);
                value_decref(ks);
            }
            free(keys[j]);
        }
        free(keys);
        iter_len = range_arr->arr->len;
        iter_items = range_arr->arr->items;
    }

    push_env(i);
    for (int idx = 0; idx < iter_len && !i->cf.signal; idx++) {
        bind_pattern(i, pat, iter_items[idx], i->env, 1);
        if (cond) {
            Value *cv = interp_eval(i, cond);
            int ok = value_truthy(cv);
            value_decref(cv);
            if (!ok) continue;
        }
        list_comp_recurse(i, n, result, cl + 1);
    }
    pop_env(i);
    if (range_arr) value_decref(range_arr);
    value_decref(iter_val);
}

Value *interp_eval(Interp *i, Node *n) {
    if (!n) return value_incref(XS_NULL_VAL);
    if (i->cf.signal) return value_incref(XS_NULL_VAL);
    i->current_span = n->span;
    if (i->coverage && n->span.line > 0)
        coverage_record_line(i->coverage, n->span.line);

    /* phase 2: before_eval hooks (skip if already inside a hook to prevent recursion) */
    if (g_has_eval_hooks && g_n_before_eval > 0 && !g_in_eval_hook) {
        g_in_eval_hook = 1;
        for (int _h = 0; _h < g_n_before_eval; _h++) {
            EvalHook *hook = &g_before_eval[_h];
            if (!hook->callback) continue;
            if (hook->tag_filter >= 0 && hook->tag_filter != (int)VAL_TAG(n)) continue;
            Value *node_map = node_to_xs_map(n);
            Value *args[1] = { node_map };
            Value *result = call_value(i, hook->callback, args, 1, "before_eval");
            value_decref(node_map);
            if (i->hook_cancelled) {
                i->hook_cancelled = 0;
                if (result) value_decref(result);
                g_in_eval_hook = 0;
                return value_incref(XS_NULL_VAL);
            }
            if (!result || VAL_TAG(result) == XS_NULL) {
                if (result) value_decref(result);
                g_in_eval_hook = 0;
                return value_incref(XS_NULL_VAL);
            }
            value_decref(result);
            if (i->cf.signal) { g_in_eval_hook = 0; return value_incref(XS_NULL_VAL); }
        }
        g_in_eval_hook = 0;
    }

    switch (VAL_TAG(n)) {
    case NODE_LIT_INT:   return xs_int(n->lit_int.ival);
    case NODE_LIT_BIGINT: {
        /* base=0 lets bigint_from_str detect 0x/0b/0o prefixes; otherwise
           "0xFFF..." with base=10 hits the 'x' on the first iteration
           and silently parses as zero. */
        XSBigInt *b = bigint_from_str(n->lit_bigint.bigint_str, 0);
        return xs_bigint_val(b);
    }
    case NODE_LIT_FLOAT: return xs_float(n->lit_float.fval);
    case NODE_LIT_BOOL:  return n->lit_bool.bval?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    case NODE_LIT_NULL:  return value_incref(XS_NULL_VAL);
    case NODE_LIT_CHAR:  return xs_char(n->lit_char.cval);
    case NODE_LIT_REGEX: return xs_regex(n->lit_regex.pattern);
    case NODE_LIT_STRING:
        return xs_str(n->lit_string.sval ? n->lit_string.sval : "");

    case NODE_INTERP_STRING: {
        char *result = xs_strdup("");
        int result_len = 0;
        for (int j = 0; j < n->lit_string.parts.len; j++) {
            Node *part = n->lit_string.parts.items[j];
            char *piece;
            if (VAL_TAG(part) == NODE_LIT_STRING) {
                piece = xs_strdup(part->lit_string.sval ? part->lit_string.sval : "");
            } else {
                Value *v = EVAL(i, part);
                /* Use inst_to_str so user-defined __str__ runs inside
                   interpolated strings: `"got: {v}"` mirrors `str(v)`
                   semantics on the same object. */
                extern char *inst_to_str_export(Interp *, Value *, int);
                piece = inst_to_str_export(i, v, 0);
                value_decref(v);
            }
            int plen = (int)strlen(piece);
            result = xs_realloc(result, result_len + plen + 1);
            memcpy(result + result_len, piece, plen + 1);
            result_len += plen;
            free(piece);
        }
        Value *v = xs_str(result);
        free(result);
        return v;
    }

    case NODE_LIT_ARRAY: {
        Value *arr = xs_array_new();
        /* [expr; N] repeat literal */
        if (n->lit_array.repeat_val && n->lit_array.repeat_cnt > 0) {
            Value *fill = EVAL(i, n->lit_array.repeat_val);
            for (int64_t j = 0; j < n->lit_array.repeat_cnt; j++)
                array_push(arr->arr, value_incref(fill));
            value_decref(fill);
            return arr;
        }
        for (int j = 0; j < n->lit_array.elems.len; j++) {
            Node *elem = n->lit_array.elems.items[j];
            if (VAL_TAG(elem) == NODE_SPREAD) {
                Value *sv = EVAL(i, elem->spread.expr);
                if (VAL_TAG(sv) == XS_ARRAY) {
                    for (int k=0;k<sv->arr->len;k++) array_push(arr->arr, value_incref(sv->arr->items[k]));
                }
                value_decref(sv);
            } else {
                Value *ev = EVAL(i, elem);
                if (!i->cf.signal) array_push(arr->arr, ev);
                else { value_decref(ev); break; }
            }
        }
        return arr;
    }

    case NODE_LIST_COMP: {
        /* [expr for pat in iter (for pat in iter)* (if cond)?] */
        Value *result = xs_array_new();
        list_comp_recurse(i, n, result, 0);
        return result;
    }

    case NODE_MAP_COMP: {
        /* {key_expr: val_expr for pat in iter if cond} */
        Value *result = xs_map_new();
        for (int cl = 0; cl < n->map_comp.clause_pats.len; cl++) {
            Node *pat  = n->map_comp.clause_pats.items[cl];
            Node *iter_expr = n->map_comp.clause_iters.items[cl];
            Node *cond = (cl < n->map_comp.clause_conds.len)
                         ? n->map_comp.clause_conds.items[cl] : NULL;
            Value *iter_val = EVAL(i, iter_expr);
            if (i->cf.signal) { value_decref(iter_val); return result; }

            /* Iterate over the value */
            int iter_len = 0;
            Value **iter_items = NULL;
            Value *range_arr = NULL;
            if (VAL_TAG(iter_val) == XS_ARRAY || VAL_TAG(iter_val) == XS_TUPLE) {
                iter_len = iter_val->arr->len;
                iter_items = iter_val->arr->items;
            } else if (VAL_TAG(iter_val) == XS_RANGE) {
                range_arr = xs_array_new();
                int64_t start = iter_val->range->start;
                int64_t end   = iter_val->range->end;
                if (iter_val->range->inclusive) end++;
                for (int64_t ri = start; ri < end; ri++)
                    array_push(range_arr->arr, xs_int(ri));
                iter_len = range_arr->arr->len;
                iter_items = range_arr->arr->items;
            }

            push_env(i);
            for (int idx = 0; idx < iter_len && !i->cf.signal; idx++) {
                bind_pattern(i, pat, iter_items[idx], i->env, 1);
                if (cond) {
                    Value *cv = EVAL(i, cond);
                    int ok = value_truthy(cv);
                    value_decref(cv);
                    if (!ok) continue;
                }
                Value *k = EVAL(i, n->map_comp.key);
                Value *v = EVAL(i, n->map_comp.value);
                if (!i->cf.signal) {
                    if (VAL_TAG(k) == XS_STR) {
                        map_set(result->map, k->s, value_incref(v));
                    } else if (VAL_TAG(k) == XS_INT) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%lld", (long long)VAL_INT(k));
                        map_set(result->map, buf, value_incref(v));
                    }
                }
                value_decref(k);
                value_decref(v);
            }
            pop_env(i);
            if (range_arr) value_decref(range_arr);
            value_decref(iter_val);
        }
        return result;
    }

    case NODE_LIT_TUPLE: {
        Value *tup = xs_tuple_new();
        for (int j = 0; j < n->lit_array.elems.len; j++) {
            Value *ev = EVAL(i, n->lit_array.elems.items[j]);
            if (!i->cf.signal) array_push(tup->arr, ev);
            else { value_decref(ev); break; }
        }
        return tup;
    }

    case NODE_LIT_MAP: {
        Value *map = xs_map_new();
        for (int j = 0; j < n->lit_map.keys.len && j < n->lit_map.vals.len; j++) {
            Node *kn = n->lit_map.keys.items[j];
            if (VAL_TAG(kn) == NODE_SPREAD) {
                /* Spread: merge source map into target */
                Value *src = EVAL(i, kn->spread.expr);
                if (src && VAL_TAG(src) == XS_MAP) {
                    int nk = 0;
                    char **keys = map_keys(src->map, &nk);
                    for (int ki = 0; ki < nk; ki++) {
                        Value *v = map_get(src->map, keys[ki]);
                        if (v) { value_incref(v); map_set(map->map, keys[ki], v); value_decref(v); }
                        free(keys[ki]);
                    }
                    free(keys);
                }
                if (src) value_decref(src);
                continue;
            }
            Value *vv = EVAL(i, n->lit_map.vals.items[j]);
            /* Identifier keys (e.g. { x: 5 }) are treated as string keys "x" */
            if (VAL_TAG(kn) == NODE_IDENT) {
                map_set(map->map, kn->ident.name, value_incref(vv));
            } else {
                Value *kv = EVAL(i, kn);
                if (VAL_TAG(kv) == XS_STR) {
                    map_set(map->map, kv->s, value_incref(vv));
                }
                value_decref(kv);
            }
            value_decref(vv);
        }
        return map;
    }

    case NODE_IDENT: {
        dep_track_add(n->ident.name);
        Value *v = env_get(i->env, n->ident.name);
        if (!v || v == XS_DELETED_VAL) {
            const char *suggestion = (!v) ? find_similar_name(i->env, n->ident.name) : NULL;
            char hint_buf[128];
            if (suggestion) {
                snprintf(hint_buf, sizeof hint_buf, "did you mean '%s'?", suggestion);
                xs_runtime_error(n->span, "not found in this scope", hint_buf,
                        "name '%s' is not defined", n->ident.name);
            } else {
                xs_runtime_error(n->span, "not found in this scope", NULL,
                        "name '%s' is not defined", n->ident.name);
            }
            /* xs_runtime_error sets CF_THROW when inside a try block; only
               fall back to CF_ERROR when the signal wasn't already set. */
            if (!i->cf.signal) i->cf.signal = CF_ERROR;
            return value_incref(XS_NULL_VAL);
        }
        return value_incref(v);
    }

    case NODE_SCOPE: {
        /* A::B::C: look up the chain */
        if (n->scope.nparts == 0) return value_incref(XS_NULL_VAL);
        Value *v = env_get(i->env, n->scope.parts[0]);
        if (!v) return value_incref(XS_NULL_VAL);
        v = value_incref(v);
        for (int j = 1; j < n->scope.nparts; j++) {
            Value *next = NULL;
            if (VAL_TAG(v) == XS_MODULE || VAL_TAG(v) == XS_MAP)
                next = map_get(v->map, n->scope.parts[j]);
            else if (VAL_TAG(v) == XS_INST)
                next = map_get(v->inst->fields, n->scope.parts[j]);
            else if (VAL_TAG(v) == XS_CLASS_VAL)
                next = map_get(v->cls->methods, n->scope.parts[j]);
            value_decref(v);
            v = next ? value_incref(next) : value_incref(XS_NULL_VAL);
            if (!next) break;
        }
        return v;
    }

    case NODE_BINOP: {
        Value *result = eval_binop(i, n);
        /* after_eval hooks for binop nodes (observe-only) */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_BINOP) continue;
                Value *node_map = node_to_xs_map(n);
                Value *hargs[2] = { node_map, result };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        return result;
    }

    case NODE_UNARY: {
        if (n->unary.op[0] == '?') {
            Value *v = EVAL(i, n->unary.expr);
            if (VAL_TAG(v) == XS_ENUM_VAL && strcmp(v->en->variant, "Err") == 0) {
                if (i->cf.value) value_decref(i->cf.value);
                i->cf.signal = CF_RETURN;
                i->cf.value  = v; /* transfer ownership */
                return value_incref(XS_NULL_VAL);
            }
            if (VAL_TAG(v) == XS_ENUM_VAL && strcmp(v->en->variant, "Ok") == 0) {
                Value *inner = (v->en->arr_data && v->en->arr_data->len > 0)
                               ? value_incref(v->en->arr_data->items[0])
                               : value_incref(XS_NULL_VAL);
                value_decref(v);
                return inner;
            }
            return v;
        }
        Value *v = EVAL(i, n->unary.expr);
        Value *result = NULL;
        if (n->unary.op[0] == '-') {
            if (VAL_TAG(v) == XS_INT)   result = xs_safe_neg(VAL_INT(v));
            else if (VAL_TAG(v) == XS_BIGINT) result = xs_numeric_neg(v);
            else if (VAL_TAG(v)==XS_FLOAT) result = xs_float(-v->f);
            else result = value_incref(XS_NULL_VAL);
        } else if (n->unary.op[0] == '!') {
            result = value_truthy(v) ? value_incref(XS_FALSE_VAL) : value_incref(XS_TRUE_VAL);
        } else if (n->unary.op[0] == '~') {
            result = (VAL_TAG(v)==XS_INT) ? xs_int(~VAL_INT(v)) : value_incref(XS_NULL_VAL);
        } else {
            result = value_incref(v);
        }
        value_decref(v);
        return result;
    }

    case NODE_ASSIGN: {
        Value *val = EVAL(i, n->assign.value);
        Node *target = n->assign.target;
        Value *result = val;
        if (n->assign.op[0] != '=' || n->assign.op[1] != '\0') {
            Value *old = EVAL(i, target);
            char opbuf[8] = {0};
            const char *aop = n->assign.op;
            if (strcmp(aop,"+=") == 0)   strcpy(opbuf,"+");
            else if (strcmp(aop,"-=") == 0) strcpy(opbuf,"-");
            else if (strcmp(aop,"*=") == 0) strcpy(opbuf,"*");
            else if (strcmp(aop,"/=") == 0) strcpy(opbuf,"/");
            else if (strcmp(aop,"%=") == 0) strcpy(opbuf,"%");
            else if (strcmp(aop,"&=") == 0) strcpy(opbuf,"&");
            else if (strcmp(aop,"|=") == 0) strcpy(opbuf,"|");
            else if (strcmp(aop,"^=") == 0) strcpy(opbuf,"^");
            else if (strcmp(aop,"**=") == 0) strcpy(opbuf,"**");
            else strcpy(opbuf,"=");
            Value *computed = NULL;
            if (VAL_TAG(old)==XS_INT && VAL_TAG(val)==XS_INT) {
                int64_t a=VAL_INT(old), b=VAL_INT(val);
                if (opbuf[0]=='+') computed=xs_int(a+b);
                else if(opbuf[0]=='-') computed=xs_int(a-b);
                else if(opbuf[0]=='*'&&opbuf[1]=='\0') computed=xs_int(a*b);
                else if(opbuf[0]=='/'&&opbuf[1]=='\0') { if(!b){xs_runtime_error(n->span,"division by zero",NULL,"cannot divide by zero");computed=value_incref(XS_NULL_VAL);}else computed=xs_int(a/b); }
                else if(opbuf[0]=='%') { if(!b){xs_runtime_error(n->span,"modulo by zero",NULL,"cannot take remainder with divisor zero");computed=value_incref(XS_NULL_VAL);}else computed=xs_int(a%b); }
                else if(opbuf[0]=='&') computed=xs_int(a&b);
                else if(opbuf[0]=='|') computed=xs_int(a|b);
                else if(opbuf[0]=='^') computed=xs_int(a^b);
                else computed=value_incref(val);
            } else if ((VAL_TAG(old)==XS_FLOAT||VAL_TAG(old)==XS_INT) &&
                       (VAL_TAG(val)==XS_FLOAT||VAL_TAG(val)==XS_INT)) {
                double a2=(VAL_TAG(old)==XS_FLOAT)?old->f:(double)VAL_INT(old);
                double b3=(VAL_TAG(val)==XS_FLOAT)?val->f:(double)VAL_INT(val);
                if (opbuf[0]=='+') computed=xs_float(a2+b3);
                else if(opbuf[0]=='-') computed=xs_float(a2-b3);
                else if(opbuf[0]=='*') computed=xs_float(a2*b3);
                else if(opbuf[0]=='/') computed=b3?xs_float(a2/b3):xs_float(0);
                else computed=value_incref(val);
            } else if (VAL_TAG(old)==XS_STR && opbuf[0]=='+') {
                char *rs=value_str(val);
                int la=(int)strlen(old->s),lb=(int)strlen(rs);
                char *buf=xs_malloc(la+lb+1);
                memcpy(buf,old->s,la); memcpy(buf+la,rs,lb+1);
                free(rs); computed=xs_str(buf); free(buf);
            } else {
                computed=value_incref(val);
            }
            value_decref(old);
            value_decref(val);
            result=computed; val=computed;
        }

        if (VAL_TAG(target) == NODE_IDENT) {
            TRACE_STORE(i, target->ident.name, result);
#ifdef XSC_ENABLE_TRACER
            trace_provenance_for_node(i, target->ident.name, n->assign.value);
#endif
            int r = env_set(i->env, target->ident.name, result);
            if (r == -1) {
                env_define(i->env, target->ident.name, result, 1);
            } else if (r == -2) {
                xs_runtime_error(target->span,
                        "cannot assign: declared with 'let'",
                        "use 'var' instead of 'let' to allow mutation",
                        "cannot assign to immutable variable '%s'",
                        target->ident.name);
                i->cf.signal = CF_ERROR;
                value_decref(result);
                return value_incref(XS_NULL_VAL);
            }
        } else if (VAL_TAG(target) == NODE_INDEX) {
            Value *obj = EVAL(i, target->index.obj);
            Value *idx = EVAL(i, target->index.index);
            if (VAL_TAG(obj) == XS_ARRAY || VAL_TAG(obj) == XS_TUPLE) {
                int orig_idx = (int)((VAL_TAG(idx)==XS_INT)?VAL_INT(idx):0);
                int ai = orig_idx;
                if (ai < 0) ai = obj->arr->len + ai;
                if (ai >= 0 && ai < obj->arr->len) {
                    value_decref(obj->arr->items[ai]);
                    obj->arr->items[ai] = value_incref(result);
                } else {
                    /* Out-of-bounds set used to silently no-op. */
                    xs_runtime_error(target->span, "IndexError", NULL,
                                     "index %d out of bounds (len %d)",
                                     orig_idx, obj->arr->len);
                    value_decref(obj); value_decref(idx); value_decref(result);
                    return value_incref(XS_NULL_VAL);
                }
            } else if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
                if (VAL_TAG(idx) == XS_STR) {
                    map_set(obj->map, idx->s, value_incref(result));
                } else {
                    char *ks = value_str(idx);
                    map_set(obj->map, ks, value_incref(result));
                    free(ks);
                }
            }
            value_decref(obj); value_decref(idx);
            /* Reactive notify: walk the target back to its root identifier
               so reactive bindings that depend on it re-evaluate. Without
               this, `arr[1] = v` mutated the array in place but no bind
               that referenced `arr` ever fired. */
            {
                Node *cur = target;
                while (cur && VAL_TAG(cur) != NODE_IDENT) {
                    if (VAL_TAG(cur) == NODE_INDEX) cur = cur->index.obj;
                    else if (VAL_TAG(cur) == NODE_FIELD) cur = cur->field.obj;
                    else break;
                }
                if (cur && VAL_TAG(cur) == NODE_IDENT)
                    env_notify_reactive(i->env, cur->ident.name);
            }
        } else if (VAL_TAG(target) == NODE_LIT_TUPLE) {
            /* Parallel tuple assignment `(a, b) = (b, a)`. The RHS has
               already been fully evaluated into `result`, so the swap
               case is safe even though `a` and `b` are read again on
               the LHS. Walk LHS elements; for each, recurse via a
               synthesised assign node so nested patterns and field /
               index targets keep working. */
            if (VAL_TAG(result) == XS_TUPLE || VAL_TAG(result) == XS_ARRAY) {
                NodeList *elems = &target->lit_array.elems;
                XSArray *src = result->arr;
                for (int ei = 0; ei < elems->len; ei++) {
                    Node *sub = elems->items[ei];
                    Value *piece = (ei < src->len)
                        ? value_incref(src->items[ei])
                        : value_incref(XS_NULL_VAL);
                    /* Reuse the IDENT/INDEX/FIELD branches by handling
                       each kind inline; avoids allocating a temp node. */
                    if (VAL_TAG(sub) == NODE_IDENT) {
                        int r2 = env_set(i->env, sub->ident.name, piece);
                        if (r2 == -1) env_define(i->env, sub->ident.name, piece, 1);
                    } else if (VAL_TAG(sub) == NODE_FIELD) {
                        Value *obj = EVAL(i, sub->field.obj);
                        if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE)
                            map_set(obj->map, sub->field.name, piece);
                        else if (VAL_TAG(obj) == XS_INST && obj->inst)
                            map_set(obj->inst->fields, sub->field.name, piece);
                        value_decref(obj);
                    } else {
                        value_decref(piece);
                    }
                }
            }
        } else if (VAL_TAG(target) == NODE_FIELD) {
            Value *obj = EVAL(i, target->field.obj);
            if (VAL_TAG(obj) == XS_INST) {
                map_set(obj->inst->fields, target->field.name, value_incref(result));
            } else if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
                map_set(obj->map, target->field.name, value_incref(result));
            } else if (VAL_TAG(obj) == XS_STRUCT_VAL) {
                map_set(obj->st->fields, target->field.name, value_incref(result));
            } else if (VAL_TAG(obj) == XS_ACTOR && obj->actor) {
                map_set(obj->actor->state, target->field.name, value_incref(result));
            }
            value_decref(obj);
            /* Reactive notify: see comment above. */
            {
                Node *cur = target;
                while (cur && VAL_TAG(cur) != NODE_IDENT) {
                    if (VAL_TAG(cur) == NODE_INDEX) cur = cur->index.obj;
                    else if (VAL_TAG(cur) == NODE_FIELD) cur = cur->field.obj;
                    else break;
                }
                if (cur && VAL_TAG(cur) == NODE_IDENT)
                    env_notify_reactive(i->env, cur->ident.name);
            }
        }

        Value *ret = value_incref(result);
        if (val != result) value_decref(val);
        else value_decref(result);
        /* after_eval hooks for assign nodes (observe-only) */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_ASSIGN) continue;
                Value *node_map = node_to_xs_map(n);
                Value *hargs[2] = { node_map, ret };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        return ret;
    }

    case NODE_CALL: {
        if (VAL_TAG(n->call.callee) == NODE_IDENT &&
            strcmp(n->call.callee->ident.name, "dbg") == 0) {
            int argc = n->call.args.len;
            Value *last = NULL;
            for (int j = 0; j < argc; j++) {
                Node *arg_node = n->call.args.items[j];
                Value *val = EVAL(i, arg_node);
                if (i->cf.signal) { value_decref(val); return value_incref(XS_NULL_VAL); }
                char *repr = value_repr(val);
                if (VAL_TAG(arg_node) == NODE_IDENT) {
                    fprintf(stderr, "[dbg] %s = %s\n", arg_node->ident.name, repr);
                } else {
                    fprintf(stderr, "[dbg] %s\n", repr);
                }
                free(repr);
                if (last) value_decref(last);
                last = val;
            }
            return last ? last : value_incref(XS_NULL_VAL);
        }

        Value *callee = EVAL(i, n->call.callee);
        if (i->cf.signal) {
            value_decref(callee);
            return value_incref(XS_NULL_VAL);
        }
        int argc = n->call.args.len;
        Value **args = argc ? xs_malloc(argc * sizeof(Value*)) : NULL;
        for (int j = 0; j < argc; j++) {
            Node *an = n->call.args.items[j];
            if (VAL_TAG(an) == NODE_SPREAD) {
                Value *sv = EVAL(i, an->spread.expr);
                if (VAL_TAG(sv) == XS_ARRAY) {
                    int extra = sv->arr->len;
                    int new_argc = j + extra + (argc - j - 1);
                    Value **new_args = new_argc ? xs_malloc(new_argc*sizeof(Value*)) : NULL;
                    for (int k=0;k<j;k++) new_args[k]=args[k];
                    for (int k=0;k<extra;k++) new_args[j+k]=value_incref(sv->arr->items[k]);
                    for (int k=j+1;k<argc;k++) {
                        new_args[j+extra+(k-j-1)] = EVAL(i, n->call.args.items[k]);
                    }
                    value_decref(sv); free(args);
                    args = new_args; argc = new_argc;
                    goto do_call;
                }
                value_decref(sv);
                args[j] = value_incref(XS_NULL_VAL);
            } else {
                args[j] = EVAL(i, an);
                if (i->cf.signal) {
                    value_decref(callee);
                    for (int k = 0; k <= j; k++) if (args[k]) value_decref(args[k]);
                    free(args);
                    return value_incref(XS_NULL_VAL);
                }
            }
        }
do_call: ;
        /* named arguments: merge kwargs into positional args */
        if (n->call.kwargs.len > 0) {
            Value *fn_val = callee;
            if (VAL_TAG(fn_val) == XS_OVERLOAD && fn_val->overload->len > 0)
                fn_val = fn_val->overload->items[0];
            if (VAL_TAG(fn_val) == XS_FUNC && fn_val->fn->nparams > 0) {
                XSFunc *fn = fn_val->fn;
                int total = fn->nparams;
                Value **merged = xs_malloc(total * sizeof(Value*));
                for (int j = 0; j < total; j++) merged[j] = NULL;
                /* place positional args */
                for (int j = 0; j < argc && j < total; j++) merged[j] = args[j];
                /* place named args by matching param names */
                for (int j = 0; j < n->call.kwargs.len; j++) {
                    const char *kname = n->call.kwargs.items[j].key;
                    Node *kval = n->call.kwargs.items[j].val;
                    for (int p = 0; p < fn->nparams; p++) {
                        Node *param = fn->params[p];
                        const char *pname = (VAL_TAG(param) == NODE_PAT_IDENT)
                            ? param->pat_ident.name : NULL;
                        if (pname && strcmp(pname, kname) == 0) {
                            if (merged[p]) value_decref(merged[p]);
                            merged[p] = EVAL(i, kval);
                            break;
                        }
                    }
                }
                /* fill remaining with null */
                for (int j = 0; j < total; j++)
                    if (!merged[j]) merged[j] = value_incref(XS_NULL_VAL);
                free(args);
                args = merged;
                argc = total;
            }
        }
        i->current_span = n->span;
        Value *result = call_value(i, callee, args, argc, NULL);
        value_decref(callee);
        for (int j=0;j<argc;j++) value_decref(args[j]);
        if (args) free(args);
        /* phase 2: after_eval hooks for call nodes (observe-only) */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_CALL) continue;
                Value *node_map = node_to_xs_map(n);
                Value *hargs[2] = { node_map, result };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        return result;
    }

    case NODE_METHOD_CALL: {
        Value *obj = EVAL(i, n->method_call.obj);
        if (n->method_call.optional && VAL_TAG(obj) == XS_NULL) {
            return obj;
        }
        /* null.is_none() / null.is_some() / null.unwrap_or(d) are
           legitimate calls -- only null-propagate for everything else. */
        const char *mc_method_name = n->method_call.method;
        int null_method_ok = mc_method_name && (
            strcmp(mc_method_name, "is_none") == 0 ||
            strcmp(mc_method_name, "is_some") == 0 ||
            strcmp(mc_method_name, "unwrap_or") == 0);
        if ((!obj || VAL_TAG(obj) == XS_NULL) && !null_method_ok) {
            if (obj) value_decref(obj);
            return value_incref(XS_NULL_VAL);
        }
        int argc = n->method_call.args.len;
        Value **args = argc ? xs_malloc(argc*sizeof(Value*)) : NULL;
        for (int j=0;j<argc;j++) args[j] = EVAL(i, n->method_call.args.items[j]);
        i->current_span = n->span;

        /* inline cache lookup for method dispatch */
        Value *result = NULL;
        int ic_sid = ic_site_id(n->node_id);
        Value *ic_cached = ic_lookup(ic_sid, (int64_t)VAL_TAG(obj),
                                     n->method_call.method);
        if (ic_cached && VAL_TAG(ic_cached) == XS_FUNC) {
            /* cache hit: skip the full method resolution */
            result = call_value(i, ic_cached, args, argc, n->method_call.method);
        }
        if (!result) {
            result = eval_method(i, obj, n->method_call.method, args, argc);
        }
        value_decref(obj);
        for (int j=0;j<argc;j++) value_decref(args[j]);
        if (args) free(args);
        /* after_eval hooks for method_call nodes */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != (int)VAL_TAG(n)) continue;
                Value *node_map = node_to_xs_map(n);
                Value *hargs[2] = { node_map, result };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        return result;
    }

    case NODE_INDEX: {
        Value *obj = EVAL(i, n->index.obj);
        Value *idx = EVAL(i, n->index.index);
        Value *result = NULL;

        if (VAL_TAG(obj) == XS_ARRAY || VAL_TAG(obj) == XS_TUPLE) {
            if (VAL_TAG(idx) == XS_INT) {
                int orig_idx = (int)VAL_INT(idx);
                int ai = orig_idx;
                if (ai < 0) ai = obj->arr->len + ai;
                if (ai < 0 || ai >= obj->arr->len) {
                    xs_runtime_error(n->span, "IndexError",
                                     "use .get(i) for nullable lookup",
                                     "index %d out of bounds (len %d)",
                                     orig_idx, obj->arr->len);
                    result = value_incref(XS_NULL_VAL);
                } else {
                    result = value_incref(obj->arr->items[ai]);
                }
            } else if (VAL_TAG(idx) == XS_RANGE) {
                XSRange *r = idx->range;
                Value *slice = xs_array_new();
                int64_t step = r->step ? r->step : 1;
                int len = obj->arr->len;
                int64_t start = r->start;
                int64_t end = r->end;
                /* Wrap negative bounds and clamp the open-end sentinel
                   to len so `a[-2..]` returns the last two items. */
                if (start < 0) start += len;
                if (end < 0) end += len;
                if (start < 0) start = 0;
                if (end > len) end = len;
                int64_t end2 = r->inclusive ? end + (step > 0 ? 1 : -1) : end;
                if (end2 > len) end2 = len;
                for (int64_t j = start; step > 0 ? (j < end2 && j < len) : (j > end2 && j >= 0); j += step) {
                    array_push(slice->arr, value_incref(array_get(obj->arr,(int)j)));
                }
                result = slice;
            } else result = value_incref(XS_NULL_VAL);
        } else if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
            int is_chan = 0;
            {
                Value *cid = map_get(obj->map, "_chan_id");
                if (cid && VAL_TAG(cid) == XS_INT) is_chan = 1;
            }
            const char *key_str = NULL;
            char *key_owned = NULL;
            if (VAL_TAG(idx) == XS_STR) key_str = idx->s;
            else { key_owned = value_str(idx); key_str = key_owned; }
            if (is_chan && key_str && (strcmp(key_str, "_buf") == 0 ||
                                       strcmp(key_str, "_cap") == 0 ||
                                       strcmp(key_str, "_chan_id") == 0 ||
                                       strcmp(key_str, "_type") == 0 ||
                                       strcmp(key_str, "__type") == 0)) {
                result = value_incref(XS_NULL_VAL);
            } else {
                Value *v = map_get(obj->map, key_str ? key_str : "");
                result = v ? value_incref(v) : value_incref(XS_NULL_VAL);
            }
            if (key_owned) free(key_owned);
        } else if (VAL_TAG(obj) == XS_STR) {
            if (VAL_TAG(idx) == XS_INT) {
                int ai=(int)VAL_INT(idx); int slen=(int)strlen(obj->s);
                if (ai<0) ai=slen+ai;
                result=(ai>=0&&ai<slen)?xs_str_n(obj->s+ai,1):value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(idx) == XS_RANGE) {
                int slen = (int)strlen(obj->s);
                int64_t start = idx->range->start;
                int64_t end = idx->range->end;
                if (start < 0) start = slen + start;
                if (end < 0) end = slen + end;
                if (idx->range->inclusive) end++;
                if (start < 0) start = 0;
                if (end > slen) end = slen;
                if (start >= end) result = xs_str("");
                else result = xs_str_n(obj->s + start, (int)(end - start));
            } else result = value_incref(XS_NULL_VAL);
        } else if (VAL_TAG(obj) == XS_RANGE) {
            /* range[int] → range.start + int */
            if (VAL_TAG(idx) == XS_INT) {
                int64_t ri = obj->range->start + VAL_INT(idx);
                int64_t rend = obj->range->inclusive ? obj->range->end : obj->range->end - 1;
                result = (ri <= rend) ? xs_int(ri) : value_incref(XS_NULL_VAL);
            } else result = value_incref(XS_NULL_VAL);
        } else if (VAL_TAG(obj) == XS_INST && obj->inst) {
            /* __index__ dunder method on instances */
            Value *fn = map_get(obj->inst->methods, "__index__");
            if (!fn && obj->inst->class_ && obj->inst->class_->methods)
                fn = map_get(obj->inst->class_->methods, "__index__");
            if (fn && (VAL_TAG(fn) == XS_FUNC || VAL_TAG(fn) == XS_NATIVE)) {
                int has_self = 0;
                if (VAL_TAG(fn) == XS_FUNC && fn->fn->nparams > 0) {
                    Node *p0 = fn->fn->params[0];
                    if (VAL_TAG(p0) == NODE_PAT_IDENT && strcmp(p0->pat_ident.name, "self") == 0)
                        has_self = 1;
                }
                if (has_self) {
                    Value *call_args[2] = { obj, idx };
                    result = call_value(i, fn, call_args, 2, "__index__");
                } else {
                    Value *call_args[1] = { idx };
                    result = call_value(i, fn, call_args, 1, "__index__");
                }
            } else {
                result = value_incref(XS_NULL_VAL);
            }
        } else if (VAL_TAG(obj) == XS_NULL) {
            xs_runtime_error(n->span, "null index", NULL,
                "cannot index a null value");
            result = value_incref(XS_NULL_VAL);
        } else if (VAL_TAG(obj) == XS_INT || VAL_TAG(obj) == XS_FLOAT ||
                   VAL_TAG(obj) == XS_BOOL || VAL_TAG(obj) == XS_BIGINT) {
            xs_runtime_error(n->span, "not indexable", NULL,
                "cannot index a value of tag %d (only arrays, tuples, maps, strings, ranges)",
                (int)VAL_TAG(obj));
            result = value_incref(XS_NULL_VAL);
        } else {
            result = value_incref(XS_NULL_VAL);
        }

        value_decref(obj); value_decref(idx);
        /* after_eval hooks for index nodes */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != (int)VAL_TAG(n)) continue;
                Value *node_map = node_to_xs_map(n);
                Value *hargs[2] = { node_map, result };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        return result;
    }

    case NODE_FIELD: {
        Value *obj = EVAL(i, n->field.obj);
        if (n->field.optional && VAL_TAG(obj) == XS_NULL) return obj;
        if (!obj || VAL_TAG(obj) == XS_NULL) {
            if (obj) value_decref(obj);
            return value_incref(XS_NULL_VAL);
        }
        Value *result = NULL;
        const char *name = n->field.name;
        if (VAL_TAG(obj) == XS_INST) {
            Value *v = map_get(obj->inst->fields, name);
            if (!v) v = map_get(obj->inst->methods, name);
            if (v) result = value_incref(v);
        }
        if (!result && (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE)) {
            /* Channel internals (_buf, _cap, _chan_id, _type, __type)
               aren't user-facing API. Block field reads on channel
               maps so a typo doesn't expose the buffer or condvar id. */
            int is_chan = 0;
            {
                Value *cid = map_get(obj->map, "_chan_id");
                if (cid && VAL_TAG(cid) == XS_INT) is_chan = 1;
            }
            if (is_chan && (strcmp(name, "_buf") == 0 ||
                            strcmp(name, "_cap") == 0 ||
                            strcmp(name, "_chan_id") == 0 ||
                            strcmp(name, "_type") == 0 ||
                            strcmp(name, "__type") == 0)) {
                result = value_incref(XS_NULL_VAL);
            } else {
                Value *v = map_get(obj->map, name);
                if (v) result = value_incref(v);
            }
        }
        if (!result && VAL_TAG(obj) == XS_STRUCT_VAL) {
            Value *v = map_get(obj->st->fields, name);
            if (v) result = value_incref(v);
        }
        if (!result && VAL_TAG(obj) == XS_ENUM_VAL) {
            if (obj->en->map_data) {
                Value *v = map_get(obj->en->map_data, name);
                if (v) result = value_incref(v);
            }
        }
        if (!result && VAL_TAG(obj) == XS_CLASS_VAL) {
            /* Static methods first, then regular methods, then fields */
            Value *v = NULL;
            if (obj->cls->static_methods) v = map_get(obj->cls->static_methods, name);
            if (!v) v = map_get(obj->cls->methods, name);
            if (!v) v = map_get(obj->cls->fields, name);
            if (v) result = value_incref(v);
        }
        if (!result && VAL_TAG(obj) == XS_ACTOR && obj->actor) {
            Value *v = map_get(obj->actor->state, name);
            if (!v && obj->actor->methods) v = map_get(obj->actor->methods, name);
            if (v) result = value_incref(v);
        }
        /* Numeric field access for tuples/arrays: tup.0, tup.1, etc. */
        if (!result && (VAL_TAG(obj) == XS_TUPLE || VAL_TAG(obj) == XS_ARRAY)) {
            char *endp;
            long idx = strtol(name, &endp, 10);
            if (*endp == '\0' && endp != name) {
                if (idx >= 0 && idx < obj->arr->len)
                    result = value_incref(obj->arr->items[idx]);
                else
                    result = value_incref(XS_NULL_VAL);
            }
        }
        /* Duration component accessors */
        if (!result && VAL_TAG(obj) == XS_DURATION) {
            int64_t ns = obj->i;
            if      (strcmp(name, "ns") == 0) result = xs_int(ns);
            else if (strcmp(name, "us") == 0) result = xs_float((double)ns / 1e3);
            else if (strcmp(name, "ms") == 0) result = xs_float((double)ns / 1e6);
            else if (strcmp(name, "s")  == 0) result = xs_float((double)ns / 1e9);
            else if (strcmp(name, "m")  == 0) result = xs_float((double)ns / 60e9);
            else if (strcmp(name, "h")  == 0) result = xs_float((double)ns / 3600e9);
            else if (strcmp(name, "d")  == 0) result = xs_float((double)ns / 86400e9);
        }
        /* Property-style: .len, .is_empty, etc. */
        if (!result) {
            if (strcmp(name,"len")==0) {
                if (VAL_TAG(obj)==XS_ARRAY||VAL_TAG(obj)==XS_TUPLE) result=xs_int(obj->arr->len);
                else if (VAL_TAG(obj)==XS_STR) result=xs_int((int64_t)strlen(obj->s));
                else if (VAL_TAG(obj)==XS_MAP||VAL_TAG(obj)==XS_MODULE) result=xs_int(obj->map->len);
            } else if (strcmp(name,"is_empty")==0) {
                int empty=0;
                if (VAL_TAG(obj)==XS_ARRAY||VAL_TAG(obj)==XS_TUPLE) empty=obj->arr->len==0;
                else if (VAL_TAG(obj)==XS_STR) empty=!obj->s||!obj->s[0];
                else if (VAL_TAG(obj)==XS_MAP) empty=obj->map->len==0;
                result=empty?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
            }
        }
        if (!result) result = value_incref(XS_NULL_VAL);
        value_decref(obj);
        return result;
    }

    case NODE_IF: {
        Value *cond = EVAL(i, n->if_expr.cond);
        int ok = value_truthy(cond);
        value_decref(cond);
        if (i->coverage && n->if_expr.cond->span.line > 0)
            coverage_record_branch(i->coverage, n->if_expr.cond->span.line, ok);
        if (ok) {
            Value *r = EVAL(i, n->if_expr.then);
            return r;
        }
        for (int j = 0; j < n->if_expr.elif_conds.len; j++) {
            Value *ec = EVAL(i, n->if_expr.elif_conds.items[j]);
            int eok = value_truthy(ec);
            value_decref(ec);
            if (i->coverage && n->if_expr.elif_conds.items[j]->span.line > 0)
                coverage_record_branch(i->coverage, n->if_expr.elif_conds.items[j]->span.line, eok);
            if (eok) {
                return EVAL(i, n->if_expr.elif_thens.items[j]);
            }
        }
        if (n->if_expr.else_branch) return EVAL(i, n->if_expr.else_branch);
        return value_incref(XS_NULL_VAL);
    }

    case NODE_MATCH: {
        Value *subject = EVAL(i, n->match.subject);
        Value *result = NULL;
        int matched = 0;
        for (int j = 0; j < n->match.arms.len; j++) {
            MatchArm *arm = &n->match.arms.items[j];
            Env *arm_env = env_new(i->env);
            if (match_pattern(i, arm->pattern, subject, arm_env)) {
                if (arm->guard) {
                    Env *saved = i->env; i->env = env_incref(arm_env);
                    Value *g = EVAL(i, arm->guard);
                    int gok = value_truthy(g); value_decref(g);
                    env_decref(i->env); i->env = saved;
                    if (!gok) { env_decref(arm_env); continue; }
                }
                Env *saved = i->env; i->env = env_incref(arm_env);
                result = EVAL(i, arm->body);
                env_decref(i->env); i->env = saved;
                env_decref(arm_env);
                matched = 1;
                break;
            }
            env_decref(arm_env);
        }
        if (!matched) {
            /* Non-exhaustive match used to silently return null, hiding
               missing-case bugs. Raise a catchable error so the gap is
               visible. */
            char *s = value_str(subject);
            xs_runtime_error(n->span, "MatchError", NULL,
                             "no match arm fits %s", s ? s : "<?>");
            free(s);
            result = value_incref(XS_NULL_VAL);
        }
        value_decref(subject);
        return result;
    }

    case NODE_WHILE: {
        const char *my_label = n->while_loop.label;
        Value *result = value_incref(XS_NULL_VAL);
        while (1) {
            /* Loop-head budget tick. Statement-level ticks miss pure
               expression loops like `while true { ... }`. */
            if (xs_limits_tick()) {
                xs_runtime_error(span_zero(), "ResourceLimit", NULL,
                                 "%s exceeded", xs_limits_exceeded_name());
                if (i->cf.signal) break;
            }
            Value *cond = EVAL(i, n->while_loop.cond);
            int ok = value_truthy(cond); value_decref(cond);
            if (i->coverage && n->while_loop.cond->span.line > 0)
                coverage_record_branch(i->coverage, n->while_loop.cond->span.line, ok);
            if (!ok) break;
            value_decref(result);
            result = EVAL(i, n->while_loop.body);
            if (i->cf.signal == CF_BREAK) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0))
                    { CF_CLEAR(i); break; }
                break; /* labeled break for outer loop */
            }
            if (i->cf.signal == CF_CONTINUE) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0))
                    { CF_CLEAR(i); continue; }
                break; /* labeled continue for outer loop */
            }
            if (i->cf.signal) break;
        }
        return result;
    }

    case NODE_FOR: {
        const char *my_label = n->for_loop.label;
        Value *iter = EVAL(i, n->for_loop.iter);
        /* Macro to handle break/continue with label awareness */
#define FOR_BREAK_CHECK \
    if (i->cf.signal == CF_BREAK) { \
        if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0)) \
            { CF_CLEAR(i); break; } \
        break; /* labeled break for an outer loop */ \
    } \
    if (i->cf.signal == CF_CONTINUE) { \
        if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0)) \
            { CF_CLEAR(i); continue; } \
        break; /* labeled continue for an outer loop */ \
    } \
    if (i->cf.signal) break;

        if (VAL_TAG(iter) == XS_ARRAY || VAL_TAG(iter) == XS_TUPLE) {
            XSArray *arr = iter->arr;
            /* Snapshot the length so the loop terminates if the body
               grows the array (push during iteration). Without this we
               re-read arr->len each iteration and run forever. */
            int snap_len = arr->len;
            for (int fi = 0; fi < snap_len && fi < arr->len; fi++) {
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, arr->items[fi], i->env, 1);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                FOR_BREAK_CHECK
            }
        } else if (VAL_TAG(iter) == XS_RANGE) {
            XSRange *r = iter->range;
            int64_t step = r->step ? r->step : 1;
            int64_t end2 = r->inclusive ? r->end + (step > 0 ? 1 : -1) : r->end;
            for (int64_t fi = r->start; step > 0 ? fi < end2 : fi > end2; fi += step) {
                Value *v = xs_int(fi);
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, v, i->env, 1);
                value_decref(v);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                FOR_BREAK_CHECK
            }
        } else if (VAL_TAG(iter) == XS_STR) {
            const char *s = iter->s;
            for (int fi = 0; s[fi]; fi++) {
                Value *v = xs_str_n(s+fi, 1);
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, v, i->env, 1);
                value_decref(v);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                FOR_BREAK_CHECK
            }
        } else if (VAL_TAG(iter) == XS_MAP && map_get(iter->map, "_chan_id") &&
                   VAL_TAG(map_get(iter->map, "_chan_id")) == XS_INT) {
            /* Channel iteration: blocking recv until the channel is
               closed and drained. Matches what people coming from Go's
               `for v := range ch` expect; the previous non-blocking
               drain only ran over what happened to be buffered the
               instant the loop started, which is rarely useful. Use
               `ch.try_recv()` if you want the old non-blocking shape. */
            while (1) {
                int closed = xs_chan_is_closed(iter);
                int buffered = xs_chan_len(iter);
                if (closed && buffered <= 0) break;
                Value *val;
                if (closed) val = xs_chan_try_recv(iter);
                else        val = xs_chan_recv(iter, i);
                /* recv returns the XS_NULL_VAL singleton (a real Value*,
                   not C NULL) when the channel is closed-and-drained,
                   so the C-null check alone misses the close signal and
                   the loop binds null once before exiting. */
                int got_null = !val || VAL_TAG(val) == XS_NULL;
                if (got_null) {
                    if (val) value_decref(val);
                    if (xs_chan_is_closed(iter) && xs_chan_len(iter) <= 0) break;
                    continue;
                }
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, val, i->env, 1);
                value_decref(val);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                FOR_BREAK_CHECK
            }
        } else if (VAL_TAG(iter) == XS_MAP && map_get(iter->map, "__type") &&
                   map_get(iter->map, "__type")->tag == XS_STR &&
                   strcmp(map_get(iter->map, "__type")->s, "generator") == 0) {
            /* Generator iterator: drive the worker via .next() until
               EOS. Break inside the body unwinds the loop without
               draining the generator (the worker eventually finishes
               on its own when the channel handle is dropped). */
            while (1) {
                Value *res = eval_method(i, iter, "next", NULL, 0);
                if (!res || VAL_TAG(res) != XS_MAP) { if (res) value_decref(res); break; }
                Value *done = map_get(res->map, "done");
                if (done && VAL_TAG(done) == XS_BOOL && VAL_INT(done)) { value_decref(res); break; }
                Value *val = map_get(res->map, "value");
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, val ? val : XS_NULL_VAL, i->env, 1);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                value_decref(res);
                FOR_BREAK_CHECK
            }
        } else if ((VAL_TAG(iter) == XS_INST &&
                    (map_has(iter->inst->methods, "next") ||
                     map_has(iter->inst->fields, "next"))) ||
                   (VAL_TAG(iter) == XS_MAP && map_has(iter->map, "next"))) {
            /* Iterator protocol: call .iter() if present, then loop .next() */
            Value *iter_obj = NULL;
            /* Check for .iter() method first */
            int has_iter = 0;
            if (VAL_TAG(iter) == XS_INST)
                has_iter = map_has(iter->inst->methods, "iter") ||
                            map_has(iter->inst->fields, "iter");
            else if (VAL_TAG(iter) == XS_MAP)
                has_iter = map_has(iter->map, "iter");
            if (has_iter) {
                iter_obj = eval_method(i, iter, "iter", NULL, 0);
                if (!iter_obj || VAL_TAG(iter_obj) == XS_NULL) {
                    if (iter_obj) value_decref(iter_obj);
                    iter_obj = value_incref(iter); /* .iter() returned null: use iter directly */
                }
            } else {
                iter_obj = value_incref(iter); /* no .iter(): use iter directly */
            }
            while (1) {
                Value *result = eval_method(i, iter_obj, "next", NULL, 0);
                if (!result) break;
                /* Stop when not Some(v): None returns XS_NULL or a non-Some enum */
                if (VAL_TAG(result) != XS_ENUM_VAL ||
                    !result->en->variant ||
                    strcmp(result->en->variant, "Some") != 0) {
                    value_decref(result);
                    break;
                }
                /* Unwrap Some(v) */
                Value *item = (result->en->arr_data && result->en->arr_data->len > 0)
                    ? value_incref(result->en->arr_data->items[0])
                    : value_incref(XS_NULL_VAL);
                value_decref(result);
                push_env(i);
                bind_pattern(i, n->for_loop.pattern, item, i->env, 1);
                value_decref(item);
                interp_exec(i, n->for_loop.body);
                pop_env(i);
                FOR_BREAK_CHECK
            }
            value_decref(iter_obj);
        } else {
            /* fallback for other types */
            interp_for_each(i, iter, n->for_loop.pattern, n->for_loop.body);
            /* interp_for_each clears unlabeled breaks; if a labeled break bubbled up,
               check if it matches this loop's label */
            if (i->cf.signal == CF_BREAK) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0))
                    CF_CLEAR(i);
            } else if (i->cf.signal == CF_CONTINUE) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0))
                    CF_CLEAR(i);
            }
        }
#undef FOR_BREAK_CHECK
        value_decref(iter);
        return value_incref(XS_NULL_VAL);
    }

    case NODE_LOOP: {
        const char *my_label = n->loop.label;
        Value *loop_result = NULL;
        while (1) {
            interp_exec(i, n->loop.body);
            if (i->cf.signal == CF_BREAK) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0)) {
                    /* Save break value before CF_CLEAR destroys it */
                    loop_result = i->cf.value ? value_incref(i->cf.value) : NULL;
                    CF_CLEAR(i); break;
                }
                break;
            }
            if (i->cf.signal == CF_CONTINUE) {
                if (!i->cf.label || (my_label && strcmp(i->cf.label, my_label)==0))
                    { CF_CLEAR(i); continue; }
                break;
            }
            if (i->cf.signal) break;
        }
        if (loop_result) return loop_result;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_BREAK: {
        Value *val = n->brk.value ? EVAL(i, n->brk.value) : value_incref(XS_NULL_VAL);
        if (i->cf.value) value_decref(i->cf.value);
        free(i->cf.label); i->cf.label = n->brk.label ? xs_strdup(n->brk.label) : NULL;
        i->cf.signal = CF_BREAK;
        i->cf.value  = val;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_CONTINUE: {
        free(i->cf.label); i->cf.label = n->cont.label ? xs_strdup(n->cont.label) : NULL;
        i->cf.signal = CF_CONTINUE;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_RETURN: {
        /* Tail call optimization: if returning a plain call, signal CF_TAIL_CALL
           so call_value can loop instead of recursing */
        if (n->ret.value && VAL_TAG(n->ret.value) == NODE_CALL) {
            Node *cn = n->ret.value;
            Value *callee = EVAL(i, cn->call.callee);
            if (i->cf.signal) { value_decref(callee); return value_incref(XS_NULL_VAL); }
            /* Only trampoline for XS_FUNC calls (not natives, classes, etc.) */
            if (VAL_TAG(callee) == XS_FUNC) {
                int argc = cn->call.args.len;
                Value **args = argc ? xs_malloc(argc * sizeof(Value*)) : NULL;
                for (int j = 0; j < argc; j++) {
                    args[j] = EVAL(i, cn->call.args.items[j]);
                    if (i->cf.signal) {
                        for (int k = 0; k <= j; k++) value_decref(args[k]);
                        free(args); value_decref(callee);
                        return value_incref(XS_NULL_VAL);
                    }
                }
                /* Stash tail call info */
                i->tc_callee = callee; /* transfer ownership */
                i->tc_args   = args;
                i->tc_argc   = argc;
                i->cf.signal = CF_TAIL_CALL;
                if (i->cf.value) value_decref(i->cf.value);
                i->cf.value  = NULL;
                return value_incref(XS_NULL_VAL);
            }
            value_decref(callee);
        }
        Value *val = n->ret.value ? EVAL(i, n->ret.value) : value_incref(XS_NULL_VAL);
        /* If evaluating the return expression propagated a throw (or any
           other signal), do not overwrite it with CF_RETURN. */
        if (i->cf.signal) {
            value_decref(val);
            return value_incref(XS_NULL_VAL);
        }
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_RETURN;
        i->cf.value  = val;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_YIELD: {
        /* Check if we're inside a tag (have __block in scope) */
        Value *block = env_get(i->env, "__block");
        if (block && (VAL_TAG(block) == XS_FUNC || VAL_TAG(block) == XS_NATIVE
#ifdef XSC_ENABLE_VM
                      || VAL_TAG(block) == XS_CLOSURE
#endif
                      )) {
            /* Tag mode: yield means "call the block" */
            Value *result = call_value(i, block, NULL, 0, "yield");
            return result;
        }
        Value *val = n->yield_.value ? EVAL(i, n->yield_.value) : value_incref(XS_NULL_VAL);
        if (i->cf.signal) { value_decref(val); return value_incref(XS_NULL_VAL); }
        Value *ych = xs_gen_tls_yield_chan();
        Value *rch = xs_gen_tls_resume_chan();
        if (ych && rch) {
            /* Lazy mode: hand the value to the consumer and block until
               they ask for the next one. The chan funcs release the GIL
               during the wait so the consumer can actually run. While
               we're suspended the main thread mutates i->env for its own
               work; snapshot ours so we come back to the generator scope
               we yielded from rather than inheriting whatever frame the
               consumer happened to leave behind. The channels come from
               TLS so a sibling generator can't swap them out from under
               us between yields. */
            Env *gen_env = i->env ? env_incref(i->env) : NULL;
            (void)xs_chan_send(ych, val);
            value_decref(val);
            Value *go = xs_chan_recv(rch, i);
            if (go) value_decref(go);
            if (i->env) env_decref(i->env);
            i->env = gen_env;
        } else if (i->yield_collect) {
            /* legacy eager mode: push value into collector array */
            array_push(i->yield_collect->arr, val);
            if (i->yield_limit > 0 && i->yield_collect->arr->len >= i->yield_limit) {
                i->cf.signal = CF_RETURN;
                i->cf.value = value_incref(XS_NULL_VAL);
            }
        } else {
            /* standalone yield outside generator context: set CF_YIELD */
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_YIELD;
            i->cf.value  = val;
        }
        return value_incref(XS_NULL_VAL);
    }

    case NODE_THROW: {
        Value *val = EVAL(i, n->throw_.value);
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_THROW;
        i->cf.value  = val;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_TRY: {
        /* Bump try_depth so xs_runtime_error knows the throw it raises
           will land in a catch arm (and can suppress its inline render). */
        i->try_depth++;
        /* Execute body and capture result */
        Value *result = EVAL(i, n->try_.body);
        i->try_depth--;
        if (i->cf.signal == CF_THROW) {
            value_decref(result);
            result = value_incref(XS_NULL_VAL);
            Value *exc = i->cf.value;
            i->cf.value = NULL;  /* take ownership; CF_CLEAR would decref */
            i->cf.signal = 0;
            /* Find matching catch arm */
            int caught = 0;
            for (int j = 0; j < n->try_.catch_arms.len; j++) {
                MatchArm *arm = &n->try_.catch_arms.items[j];
                Env *arm_env = env_new(i->env);
                int matches = !arm->pattern ||
                              match_pattern(i, arm->pattern, exc ? exc : XS_NULL_VAL, arm_env);
                if (matches) {
                    Env *saved = i->env; i->env = env_incref(arm_env);
                    value_decref(result);
                    result = EVAL(i, arm->body);
                    env_decref(i->env); i->env = saved;
                    env_decref(arm_env);
                    caught = 1;
                    break;
                }
                env_decref(arm_env);
            }
            if (!caught && exc) {
                /* No catch arm matched: park the throw so finally still
                   runs and the throw propagates out to the next handler. */
                if (i->cf.value) value_decref(i->cf.value);
                i->cf.signal = CF_THROW;
                i->cf.value  = exc;
                exc = NULL;
            }
            if (exc) value_decref(exc);
        }
        /* Finally always runs, even on return/throw/break/continue.
           Save the pending signal+value, clear them while running the
           finally body, then restore unless finally itself throws. */
        if (n->try_.finally_block) {
            int saved_signal = i->cf.signal;
            Value *saved_value = i->cf.value;
            char *saved_label = i->cf.label;
            i->cf.signal = 0;
            i->cf.value = NULL;
            i->cf.label = NULL;
            interp_exec(i, n->try_.finally_block);
            if (i->cf.signal) {
                /* finally raised something; that wins over the original. */
                if (saved_value) value_decref(saved_value);
                if (saved_label) free(saved_label);
            } else {
                i->cf.signal = saved_signal;
                i->cf.value = saved_value;
                i->cf.label = saved_label;
            }
        }
        return result;
    }

    case NODE_LAMBDA: {
        int nparams = n->lambda.params.len;
        Node **params   = nparams ? xs_malloc(nparams * sizeof(Node*)) : NULL;
        Node **defaults = nparams ? xs_calloc(nparams, sizeof(Node*)) : NULL;
        int  *varflags  = nparams ? xs_calloc(nparams, sizeof(int)) : NULL;
        for (int j = 0; j < nparams; j++) {
            Param *pm = &n->lambda.params.items[j];
            if (pm->pattern) {
                if (VAL_TAG(pm->pattern) == NODE_IDENT) {
                    Node *pn = node_new(NODE_PAT_IDENT, pm->pattern->span);
                    pn->pat_ident.name    = xs_strdup(pm->pattern->ident.name);
                    pn->pat_ident.mutable = 0;
                    params[j] = pn;
                } else {
                    params[j] = pm->pattern;
                }
            } else {
                Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                pn->pat_ident.name    = xs_strdup(pm->name ? pm->name : "_");
                pn->pat_ident.mutable = 0;
                params[j] = pn;
            }
            defaults[j] = pm->default_val;
            varflags[j]  = pm->variadic;
        }
        XSFunc *fn = func_new_ex("<lambda>", params, nparams,
                               n->lambda.body, i->env, defaults, varflags);
        fn->is_generator = n->lambda.is_generator;
        if (nparams > 0) {
            int has_types = 0;
            for (int j = 0; j < nparams; j++) {
                Param *pm = &n->lambda.params.items[j];
                if (pm->type_ann && pm->type_ann->name) { has_types = 1; break; }
            }
            if (has_types) {
                fn->param_type_names = xs_calloc(nparams, sizeof(char*));
                for (int j = 0; j < nparams; j++) {
                    Param *pm = &n->lambda.params.items[j];
                    if (pm->type_ann && pm->type_ann->name)
                        fn->param_type_names[j] = xs_strdup(pm->type_ann->name);
                }
            }
        }
        Value *v = xs_func_new(fn);
        return v;
    }

    case NODE_BLOCK: {
        int has_decls = n->block.has_decls;
        if (has_decls == -1) {
            has_decls = 0;
            for (int j = 0; j < n->block.stmts.len; j++) {
                NodeTag t = (NodeTag)VAL_TAG(n->block.stmts.items[j]);
                if (t==NODE_LET||t==NODE_VAR||t==NODE_CONST||
                    t==NODE_FN_DECL||t==NODE_CLASS_DECL||
                    t==NODE_STRUCT_DECL||t==NODE_ENUM_DECL) {
                    has_decls = 1; break;
                }
            }
            n->block.has_decls = has_decls;
        }
        Env *saved = NULL;
        if (has_decls) {
            saved = i->env;
            push_env(i);
        }
        Value *result = value_incref(XS_NULL_VAL);
        for (int j = 0; j < n->block.stmts.len; j++) {
            interp_exec(i, n->block.stmts.items[j]);
            if (i->cf.signal) break;
        }
        if (!i->cf.signal && n->block.expr) {
            value_decref(result);
            result = EVAL(i, n->block.expr);
        }
        /* process hook table entries for nodes in this block */
        if (has_decls && i->pipeline) {
            PluginPipeline *pp = (PluginPipeline *)i->pipeline;
            if (pp->nhook_entries > 0) {
                for (int j = 0; j < n->block.stmts.len; j++) {
                    Node *s = n->block.stmts.items[j];
                    int hcount = 0;
                    HookTableEntry *hooks = pipeline_lookup_hooks(pp, s->node_id, &hcount);
                    if (hooks) {
                        for (int h = 0; h < hcount; h++) {
                            if (hooks[h].target && strcmp(hooks[h].hook_kind, "destructor") == 0) {
                                Value *target_val = env_get(i->env, hooks[h].target);
                                if (target_val && VAL_TAG(target_val) == XS_MAP) {
                                    Value *destroy_fn = map_get(target_val->map, "destroy");
                                    if (destroy_fn && (VAL_TAG(destroy_fn) == XS_CLOSURE || VAL_TAG(destroy_fn) == XS_NATIVE)) {
                                        Value *r = call_value(i, destroy_fn, NULL, 0, "destructor_hook");
                                        if (r) value_decref(r);
                                    }
                                }
                            }
                        }
                        free(hooks);
                    }
                }
            }
        }
        if (has_decls) {
            pop_env(i);
            i->env = saved;
        }
        return result;
    }

    case NODE_RANGE: {
        Value *start = n->range.start ? EVAL(i, n->range.start) : xs_int(0);
        /* Open-ended range like `2..` means "to the end" when used as
           a slice. INT64_MAX is the sentinel; slicing clamps to len. */
        Value *end   = n->range.end   ? EVAL(i, n->range.end)   : xs_int(INT64_MAX);
        int64_t sv = (VAL_TAG(start)==XS_INT)?VAL_INT(start):(int64_t)start->f;
        int64_t ev = (VAL_TAG(end)==XS_INT)?VAL_INT(end):(int64_t)end->f;
        value_decref(start); value_decref(end);
        return xs_range(sv, ev, n->range.inclusive);
    }

    case NODE_STRUCT_INIT: {
        const char *path = n->struct_init.path ? n->struct_init.path : "";
        const char *type_name = path;
        const char *sep = strstr(path, "::");
        while (sep) { type_name = sep+2; sep = strstr(type_name,"::"); }

        /* Check if it's an enum variant */
        Value *cls = env_get(i->env, type_name);
        if (!cls) {
            char first[128] = {0};
            const char *c = path;
            int fi=0;
            while (*c && *c!=':' && fi<127) first[fi++]=*c++;
            cls = env_get(i->env, first);
        }

        if (cls && VAL_TAG(cls) == XS_CLASS_VAL) {
            Value *args_empty[1];
            Value *inst = call_value(i, cls, args_empty, 0, path);
            if (inst && VAL_TAG(inst) == XS_INST) {
                for (int j=0;j<n->struct_init.fields.len;j++) {
                    char *fname = n->struct_init.fields.items[j].key;
                    if (cls->cls->fields && !map_has(cls->cls->fields, fname)) {
                        fprintf(stderr, "xs: error at %s:%d:%d: unknown field '%s' in struct '%s'\n",
                                n->span.file ? n->span.file : "<unknown>",
                                n->span.line, n->span.col, fname, path);
                        return value_incref(XS_NULL_VAL);
                    }
                }
                /* Apply spread/rest base first (explicit fields override) */
                if (n->struct_init.rest) {
                    Value *base = EVAL(i, n->struct_init.rest);
                    if (base && VAL_TAG(base) == XS_INST) {
                        int nk=0; char **ks=map_keys(base->inst->fields,&nk);
                        for (int j=0;j<nk;j++) {
                            Value *v=map_get(base->inst->fields,ks[j]);
                            if (v) map_set(inst->inst->fields,ks[j],value_incref(v));
                            free(ks[j]);
                        }
                        free(ks);
                    } else if (base && (VAL_TAG(base) == XS_MAP || VAL_TAG(base) == XS_MODULE)) {
                        int nk=0; char **ks=map_keys(base->map,&nk);
                        for (int j=0;j<nk;j++) {
                            Value *v=map_get(base->map,ks[j]);
                            if (v) map_set(inst->inst->fields,ks[j],value_incref(v));
                            free(ks[j]);
                        }
                        free(ks);
                    }
                    value_decref(base);
                }
                /* Set explicit fields (override spread values) */
                for (int j=0;j<n->struct_init.fields.len;j++) {
                    char *fname = n->struct_init.fields.items[j].key;
                    Value *fv = EVAL(i, n->struct_init.fields.items[j].val);
                    map_set(inst->inst->fields, fname, value_incref(fv));
                    value_decref(fv);
                }
                /* Validate: check for missing required fields */
                if (cls->cls->fields) {
                    int nkeys = 0;
                    char **keys = map_keys(cls->cls->fields, &nkeys);
                    for (int j = 0; j < nkeys; j++) {
                        /* Check instance fields (includes both spread and explicit) */
                        int found = map_has(inst->inst->fields, keys[j]);
                        if (!found) {
                            /* Check if the field has a non-null default */
                            Value *def = map_get(cls->cls->fields, keys[j]);
                            if (!def || VAL_TAG(def) == XS_NULL) {
                                fprintf(stderr, "xs: error at %s:%d:%d: missing field '%s' in struct '%s'\n",
                                        n->span.file ? n->span.file : "<unknown>",
                                        n->span.line, n->span.col, keys[j], path);
                                for (int f = j; f < nkeys; f++) free(keys[f]);
                                free(keys);
                                value_decref(inst);
                                return value_incref(XS_NULL_VAL);
                            }
                        }
                        free(keys[j]);
                    }
                    free(keys);
                }
            }
            return inst;
        }

        /* Build map-based struct */
        Value *m = xs_map_new();
        for (int j=0;j<n->struct_init.fields.len;j++) {
            char *fname = n->struct_init.fields.items[j].key;
            Value *fv = EVAL(i, n->struct_init.fields.items[j].val);
            map_set(m->map, fname, value_incref(fv));
            value_decref(fv);
        }
        if (n->struct_init.rest) {
            Value *base = EVAL(i, n->struct_init.rest);
            if (VAL_TAG(base) == XS_MAP || VAL_TAG(base) == XS_MODULE) {
                int nk=0; char **ks=map_keys(base->map,&nk);
                for (int j=0;j<nk;j++) {
                    if (!map_has(m->map, ks[j])) {
                        Value *v=map_get(base->map,ks[j]);
                        if (v) map_set(m->map,ks[j],value_incref(v));
                    }
                    free(ks[j]);
                }
                free(ks);
            }
            value_decref(base);
        }
        return m;
    }

    case NODE_CAST: {
        Value *v = EVAL(i, n->cast.expr);
        /* Basic type coercions */
        const char *t = n->cast.type_name ? n->cast.type_name : "";
        Value *result = NULL;
        if (strcmp(t,"i64")==0||strcmp(t,"int")==0||strcmp(t,"i32")==0||
            strcmp(t,"i128")==0||strcmp(t,"isize")==0||
            strcmp(t,"u64")==0||strcmp(t,"u32")==0||strcmp(t,"u8")==0||
            strcmp(t,"u16")==0||strcmp(t,"u128")==0||strcmp(t,"usize")==0) {
            if (VAL_TAG(v)==XS_INT)   result=value_incref(v);
            else if(VAL_TAG(v)==XS_FLOAT) result=xs_int((int64_t)v->f);
            else if(VAL_TAG(v)==XS_CHAR)  result=xs_int((unsigned char)(v->s?v->s[0]:0));
            else if(VAL_TAG(v)==XS_STR) {
                /* single-char string → ASCII value; otherwise parse as number */
                if (v->s && v->s[0] && !v->s[1]) result=xs_int((unsigned char)v->s[0]);
                else result=xs_int(atoll(v->s));
            }
            else if(VAL_TAG(v)==XS_BOOL)  result=xs_int(VAL_INT(v));
            else result=xs_int(0);
        } else if (strcmp(t,"f64")==0||strcmp(t,"float")==0||strcmp(t,"f32")==0) {
            if (VAL_TAG(v)==XS_FLOAT) result=value_incref(v);
            else if(VAL_TAG(v)==XS_INT)   result=xs_float((double)VAL_INT(v));
            else if(VAL_TAG(v)==XS_STR)   result=xs_float(atof(v->s));
            else result=xs_float(0.0);
        } else if (strcmp(t,"str")==0||strcmp(t,"String")==0) {
            char *s=value_str(v); result=xs_str(s); free(s);
        } else if (strcmp(t,"bool")==0) {
            result=value_truthy(v)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
        } else if (strcmp(t,"char")==0) {
            if (VAL_TAG(v)==XS_INT) result=xs_char((char)VAL_INT(v));
            else if(VAL_TAG(v)==XS_STR&&v->s[0]) result=xs_char(v->s[0]);
            else result=xs_char(0);
        } else {
            result = value_incref(v);
        }
        value_decref(v);
        return result;
    }

    case NODE_EXPR_STMT:
        /* Should be handled by exec, but just in case */
        return EVAL(i, n->expr_stmt.expr);

    case NODE_PROGRAM:
        interp_run(i, n);
        return value_incref(XS_NULL_VAL);

    case NODE_EFFECT_DECL:
        return value_incref(XS_NULL_VAL);

    case NODE_HANDLE: {
        int arms_pushed = 0;
        for (int j = 0; j < n->handle.arms.len; j++) {
            EffectArm *arm = &n->handle.arms.items[j];
            EffectFrame *frame = xs_calloc(1, sizeof(EffectFrame));
            frame->effect_name     = xs_strdup(arm->effect_name);
            frame->op_name         = xs_strdup(arm->op_name);
            frame->params          = arm->params; /* borrow: do not free here */
            frame->handler_body    = arm->body;
            frame->handler_env     = env_incref(i->env);
            frame->handle_body     = n->handle.expr;
            frame->handle_body_env = env_incref(i->env);
            frame->arm_is_multishot = count_arm_resumes(arm->body, 2) > 1;
            frame->prev            = i->effect_stack;
            i->effect_stack        = frame;
            arms_pushed++;
        }

        Value *result = EVAL(i, n->handle.expr);
        /* Multi-shot resume bail-out: arm body finished after one or
           more re-entrant resume calls and parked the handle's value
           on cf.value. Take it as the handle expression's result and
           clear the signal so it doesn't keep unwinding past us. */
        if (i->cf.signal == CF_HANDLE_DONE) {
            if (result) value_decref(result);
            result = i->cf.value ? i->cf.value : value_incref(XS_NULL_VAL);
            i->cf.signal = 0;
            i->cf.value  = NULL;
        }
        for (int j = 0; j < arms_pushed; j++) {
            EffectFrame *frame = i->effect_stack;
            i->effect_stack = frame->prev;
            free(frame->effect_name);
            free(frame->op_name);
            env_decref(frame->handler_env);
            if (frame->handle_body_env) env_decref(frame->handle_body_env);
            free(frame);
        }

        return result;
    }

    case NODE_PERFORM: {
        /* Multi-shot resume override: a NODE_RESUME up the stack is
           re-evaluating the body with this value as the perform's
           result. Single-use -- a second perform inside the same
           re-eval falls through to normal handler dispatch. */
        if (i->perform_override_active) {
            i->perform_override_active = 0;
            Value *r = i->perform_override
                ? value_incref(i->perform_override)
                : value_incref(XS_NULL_VAL);
            for (int j = 0; j < n->perform.args.len; j++) {
                Value *av = EVAL(i, n->perform.args.items[j]);
                if (av) value_decref(av);
            }
            return r;
        }

        const char *eff = n->perform.effect_name;
        const char *op  = n->perform.op_name;
        EffectFrame *frame = i->effect_stack;
        while (frame) {
            if (strcmp(frame->effect_name, eff) == 0 &&
                strcmp(frame->op_name, op) == 0)
                break;
            frame = frame->prev;
        }
        if (!frame) {
            fprintf(stderr, "xs: unhandled effect %s.%s at %s:%d:%d\n", eff, op,
                    n->span.file ? n->span.file : "<unknown>",
                    n->span.line, n->span.col);
            return value_incref(XS_NULL_VAL);
        }

        int argc = n->perform.args.len;
        Value **args = argc ? xs_malloc(argc * sizeof(Value*)) : NULL;
        for (int j = 0; j < argc; j++)
            args[j] = EVAL(i, n->perform.args.items[j]);

        Env *handler_call_env = env_new(frame->handler_env);
        for (int j = 0; j < frame->params.len && j < argc; j++) {
            Param *pm = &frame->params.items[j];
            const char *pname = pm->name ? pm->name :
                (pm->pattern && VAL_TAG(pm->pattern) == NODE_PAT_IDENT ?
                 pm->pattern->pat_ident.name : NULL);
            if (pname)
                env_define(handler_call_env, pname, args[j], 1);
        }
        for (int j = 0; j < argc; j++) value_decref(args[j]);
        free(args);

        Env *saved_env = i->env;
        env_incref(saved_env);
        i->env = env_incref(handler_call_env);

        Value *saved_resume = i->resume_value;
        i->resume_value = value_incref(XS_NULL_VAL);

        int saved_in_handler = i->in_handler;
        i->in_handler = 1;
        int saved_multi_shot = i->multi_shot_used;
        i->multi_shot_used = 0;

        Value *body_val = EVAL(i, frame->handler_body);

        int resumed = (i->cf.signal == CF_RESUME);
        if (resumed) CF_CLEAR(i);
        int multi_done = i->multi_shot_used;

        Value *resume_val = i->resume_value;
        i->resume_value   = saved_resume;
        i->in_handler     = saved_in_handler;
        i->multi_shot_used = saved_multi_shot;

        env_decref(i->env);
        i->env = saved_env;
        env_decref(handler_call_env);

        if (resumed) {
            /* Handler called single-shot resume: drop the body's value
               and feed resume's value back into the perform expression. */
            value_decref(body_val);
            return resume_val;
        }
        if (resume_val) value_decref(resume_val);
        if (multi_done) {
            /* Arm body finished after a multi-shot resume. Its final
               value becomes the handle expression's value; signal
               CF_HANDLE_DONE so call boundaries above us pass it
               through unmolested instead of treating it as a regular
               function return. */
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_HANDLE_DONE;
            i->cf.value  = body_val;
            return value_incref(XS_NULL_VAL);
        }
        /* Handler short-circuited: abort the enclosing function with the
           handler's body value so `handle ... with ...` evaluates to it. */
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_RETURN;
        i->cf.value  = body_val;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_RESUME: {
        Value *val = n->resume_.value ? EVAL(i, n->resume_.value) : value_incref(XS_NULL_VAL);

        /* Multi-shot path: if there's a handle body to replay, run it
           with val as the perform-override. The body's final value
           becomes this resume(...) call's result, so the arm body can
           use it (and call resume again). After the arm body
           eventually finishes, NODE_PERFORM raises CF_HANDLE_DONE
           with the arm's value, which the outer handle catches. */
        EffectFrame *frame = i->effect_stack;
        if (frame && frame->arm_is_multishot &&
            frame->handle_body && frame->handle_body_env) {
            Value *prev_override = i->perform_override;
            int prev_active = i->perform_override_active;
            i->perform_override = val;
            i->perform_override_active = 1;
            /* Mark this arm body as having taken the multi-shot path
               so NODE_PERFORM bails to the enclosing handle via
               CF_HANDLE_DONE rather than the legacy CF_RETURN that
               would let the body's post-perform tail re-execute. */
            i->multi_shot_used = 1;

            Env *saved_env = i->env;
            env_incref(saved_env);
            i->env = env_incref(frame->handle_body_env);

            Value *body_val = EVAL(i, frame->handle_body);

            env_decref(i->env);
            i->env = saved_env;

            i->perform_override = prev_override;
            i->perform_override_active = prev_active;

            value_decref(val);
            return body_val;
        }

        if (i->resume_value) value_decref(i->resume_value);
        i->resume_value = val;
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_RESUME;
        i->cf.value  = NULL;
        return value_incref(XS_NULL_VAL);
    }

    case NODE_AWAIT: {
        /* await on a future returned by spawn: blocks until the spawned
           thread finishes, releasing the GIL so the spawned thread can
           actually run. */
        Value *v = EVAL(i, n->await_.expr);
        if (i->cf.signal) { value_decref(v); return value_incref(XS_NULL_VAL); }
        if (VAL_TAG(v) == XS_MAP) {
            Value *tid_val = map_get(v->map, "_task_id");
            if (tid_val && VAL_TAG(tid_val) == XS_INT) {
                int tid = (int)VAL_INT(tid_val);
                Value *r = xs_await_task(tid);
                Value *sv = xs_str("done"); map_set(v->map, "_status", sv); value_decref(sv);
                map_set(v->map, "_result", r);
                value_decref(v);
                return r;
            }
            Value *status = map_get(v->map, "_status");
            if (status && VAL_TAG(status) == XS_STR && strcmp(status->s, "done") == 0) {
                Value *res = map_get(v->map, "_result");
                Value *ret = res ? value_incref(res) : value_incref(XS_NULL_VAL);
                value_decref(v);
                return ret;
            }
        }
        if (VAL_TAG(v) == XS_FUNC || VAL_TAG(v) == XS_NATIVE) {
            Value *result = call_value(i, v, NULL, 0, "await");
            value_decref(v);
            return result;
        }
        return v;
    }

    case NODE_NURSERY: {
        /* Spawn each NODE_SPAWN inside the body on a real OS thread
           so siblings actually run in parallel and can observe each
           other's cancellation. The nursery_queue collects the
           returned future maps; when the body completes we await
           each task and (per the policy in concurrent.c) propagate
           cancel on the first throw, surfacing the original error
           while suppressing the cleanup-noise Cancelled errors that
           the siblings raise on wake-up. */
#ifdef XS_WASM
        /* WASI has no usable threads -- fall back to the legacy
           sequential drain so the language semantics still hold,
           sans real concurrency. */
        Value *saved_queue = i->nursery_queue;
        Value *queue = xs_array_new();
        i->nursery_queue = queue;
        Value *body_val = EVAL(i, n->nursery_.body);
        value_decref(body_val);
        i->nursery_queue = saved_queue;
        XSArray *tasks = queue->arr;
        for (int j = 0; j < tasks->len; j++) {
            if (i->cf.signal) break;
            Value *task = tasks->items[j];
            if (VAL_TAG(task) == XS_FUNC || VAL_TAG(task) == XS_NATIVE) {
                Value *r = call_value(i, task, NULL, 0, "nursery_task");
                if (!i->cf.signal && r &&
                    (VAL_TAG(r) == XS_FUNC || VAL_TAG(r) == XS_NATIVE)) {
                    Value *r2 = call_value(i, r, NULL, 0, "nursery_task");
                    value_decref(r2);
                }
                value_decref(r);
            }
        }
        value_decref(queue);
        return value_incref(XS_NULL_VAL);
#else
        int saved_nursery_id = xs_nursery_current_id();
        int new_nursery_id   = xs_nursery_alloc_id();
        xs_nursery_set_current_id(new_nursery_id);

        Value *saved_queue = i->nursery_queue;
        Value *queue = xs_array_new();   /* collects task futures */
        i->nursery_queue = queue;

        Value *body_val = EVAL(i, n->nursery_.body);
        value_decref(body_val);

        i->nursery_queue = saved_queue;
        xs_nursery_set_current_id(saved_nursery_id);

        /* Await every spawned task. Stash the first non-Cancelled
           error and re-raise it after the join so a sibling's
           cleanup throw doesn't mask the originator. */
        XSArray *tasks = queue->arr;
        Value *real_err = NULL;
        for (int j = 0; j < tasks->len; j++) {
            Value *fut = tasks->items[j];
            if (!fut || VAL_TAG(fut) != XS_MAP) continue;
            Value *tid_v = map_get(fut->map, "_task_id");
            if (!tid_v || VAL_TAG(tid_v) != XS_INT) continue;
            int errored = 0;
            Value *err  = NULL;
            Value *r = xs_await_task_ex((int)VAL_INT(tid_v), &errored, &err);
            if (r) value_decref(r);
            if (errored && err) {
                int is_cancel = 0;
                if (VAL_TAG(err) == XS_MAP) {
                    Value *kind = map_get(err->map, "kind");
                    if (kind && VAL_TAG(kind) == XS_STR &&
                        strcmp(kind->s, "Cancelled") == 0) is_cancel = 1;
                }
                if (is_cancel) {
                    value_decref(err);
                } else if (!real_err) {
                    real_err = err;
                } else {
                    value_decref(err);
                }
            } else if (err) {
                value_decref(err);
            }
        }
        value_decref(queue);
        if (real_err) {
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_THROW;
            i->cf.value  = real_err;
        }
        return value_incref(XS_NULL_VAL);
#endif
    }

    case NODE_SPAWN: {
        /* Nursery-scoped spawn: spawn on a real thread (so siblings
           run in parallel and a sibling failure can cancel the rest)
           and stash the future in the parent nursery's queue for it
           to await. WASI keeps the legacy queue-then-call path
           because it has no usable threads. */
        if (i->nursery_queue) {
#ifdef XS_WASM
            XSFunc *fn = func_new_ex("__spawn__", NULL, 0,
                                     n->spawn_.expr, i->env, NULL, NULL);
            Value *task = xs_func_new(fn);
            array_push(i->nursery_queue->arr, task);
            return value_incref(XS_NULL_VAL);
#else
            XSFunc *fn = func_new_ex("__spawn__", NULL, 0,
                                     n->spawn_.expr, i->env, NULL, NULL);
            Value *task_fn = xs_func_new(fn);
            Value *fut = xs_spawn_thread(i, task_fn);
            value_decref(task_fn);
            array_push(i->nursery_queue->arr, value_incref(fut));
            return fut;
#endif
        }

        /* Actor spawn: `spawn ActorClass` constructs a new actor instance. */
        if (VAL_TAG(n->spawn_.expr) == NODE_IDENT) {
            Value *check = EVAL(i, n->spawn_.expr);
            if (!i->cf.signal && check && VAL_TAG(check) == XS_ACTOR) {
                Value *inst = call_value(i, check, NULL, 0, "spawn");
                value_decref(check);
                return inst;
            }
            if (check) value_decref(check);
            i->cf.signal = 0;
        }

        /* Real concurrent spawn: wrap the expression as a zero-arg
           closure and hand it to the pthread-backed runtime. The
           resulting future has _task_id which the parent can pass to
           xs_await_task to block until completion. */
        XSFunc *spawn_fn = func_new_ex("__spawn__", NULL, 0,
                                       n->spawn_.expr, i->env, NULL, NULL);
        Value *v = xs_func_new(spawn_fn);
#ifdef XS_WASM
        /* WASI has no usable threads; run the spawned body synchronously
           and return a resolved-future map so `await` sees the result
           directly via the pre-completed path. Loses real concurrency
           but matches the observable semantics for sequential
           await-all patterns. */
        Value *res = call_value(i, v, NULL, 0, "spawn");
        value_decref(v);
        Value *fut = xs_map_new();
        Value *status_v = xs_str("done");
        map_set(fut->map, "_status", status_v); value_decref(status_v);
        map_set(fut->map, "_result", res ? res : value_incref(XS_NULL_VAL));
        if (res) value_decref(res);
        return fut;
#else
        Value *fut = xs_spawn_thread(i, v);
        value_decref(v);
        return fut;
#endif
    }

    case NODE_SEND_EXPR: {
        /* actor ! message: synchronous dispatch */
        Value *target = EVAL(i, n->send_expr.target);
        Value *msg = EVAL(i, n->send_expr.message);
        Value *result = value_incref(XS_NULL_VAL);
        if (VAL_TAG(target) == XS_ACTOR && target->actor && target->actor->handle_fn) {
            XSActor *actor = target->actor;
            /* Create env with actor state */
            Env *wrapper = env_new(actor->handle_fn->closure ? actor->handle_fn->closure : actor->closure);
            env_define(wrapper, "self", value_incref(target), 0);
            /* Import actor state into env */
            if (actor->state) {
                int nkeys = 0;
                char **keys = map_keys(actor->state, &nkeys);
                for (int j = 0; j < nkeys; j++) {
                    Value *sv = map_get(actor->state, keys[j]);
                    if (sv) env_define(wrapper, keys[j], value_incref(sv), 1);
                    free(keys[j]);
                }
                free(keys);
            }
            /* Temporarily swap closure of handle_fn to wrapper */
            Env *orig_closure = actor->handle_fn->closure;
            env_incref(wrapper);
            actor->handle_fn->closure = wrapper;
            /* Call handle(msg) */
            Value *fn_val = xs_func_new(actor->handle_fn);
            value_decref(result);
            result = call_value(i, fn_val, &msg, 1, "handle");
            value_decref(fn_val);
            /* Restore closure */
            env_decref(actor->handle_fn->closure);
            actor->handle_fn->closure = orig_closure;
            /* Flush state back from wrapper env */
            if (actor->state) {
                int nkeys = 0;
                char **keys = map_keys(actor->state, &nkeys);
                for (int j = 0; j < nkeys; j++) {
                    Value *updated = env_get(wrapper, keys[j]);
                    if (updated) map_set(actor->state, keys[j], value_incref(updated));
                    free(keys[j]);
                }
                free(keys);
            }
            env_decref(wrapper);
        } else if (VAL_TAG(target) == XS_INST) {
            /* Fallback: treat ! on instances as calling .handle(msg) */
            Value *r = eval_method(i, target, "handle", &msg, 1);
            value_decref(result);
            result = r;
        }
        value_decref(target);
        value_decref(msg);
        return result;
    }

    case NODE_LIT_DURATION: {
        return xs_duration(n->lit_duration.ns);
    }
    case NODE_PAUSE: {
        Value *dur = EVAL(i, n->pause_.duration);
        if (i->cf.signal) { value_decref(dur); return value_incref(XS_NULL_VAL); }
        double ms = 0;
        if (VAL_TAG(dur) == XS_DURATION) ms = (double)dur->i / 1e6;
        else if (VAL_TAG(dur) == XS_INT) ms = (double)VAL_INT(dur);
        else if (VAL_TAG(dur) == XS_FLOAT) ms = dur->f;
        value_decref(dur);
        if (ms > 0) {
#ifdef _WIN32
            Sleep((DWORD)ms);
#else
            struct timespec ts;
            ts.tv_sec = (time_t)(ms / 1000.0);
            ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1000000.0);
            nanosleep(&ts, NULL);
#endif
        }
        return value_incref(XS_NULL_VAL);
    }

    case NODE_DEL: {
        if (!env_delete(i->env, n->del_.name)) {
            i->current_span = n->span;
            xs_runtime_error(n->span, "del failed", NULL,
                "cannot delete '%s': not found", n->del_.name);
            i->cf.signal = CF_ERROR;
        }
        return value_incref(XS_NULL_VAL);
    }

    case NODE_DO_EXPR: {
        return EVAL(i, n->do_expr.body);
    }

    case NODE_WITH: {
        Value *resource = EVAL(i, n->with_.expr);
        if (i->cf.signal) { value_decref(resource); return value_incref(XS_NULL_VAL); }
        if (n->with_.name) {
            env_define(i->env, n->with_.name, resource, 0);
        }
        Value *result = EVAL(i, n->with_.body);
        /* call .close() on resource if it has one */
        if (VAL_TAG(resource) == XS_STRUCT_VAL || VAL_TAG(resource) == XS_INST ||
            VAL_TAG(resource) == XS_MAP) {
            Value *close_args[1] = { resource };
            Value *close_fn = NULL;
            if (VAL_TAG(resource) == XS_STRUCT_VAL && resource->map)
                close_fn = map_get(resource->map, "close");
            else if (VAL_TAG(resource) == XS_INST && resource->inst->fields)
                close_fn = map_get(resource->inst->fields, "close");
            else if (VAL_TAG(resource) == XS_MAP)
                close_fn = map_get(resource->map, "close");
            if (close_fn && (VAL_TAG(close_fn) == XS_FUNC || VAL_TAG(close_fn) == XS_NATIVE)) {
                Value *cr = call_value(i, close_fn, close_args, 1, "close");
                value_decref(cr);
            }
        }
        value_decref(resource);
        return result;
    }

    default: {
        /* check for RT_HOOK_EXEC runtime hooks matching this node type */
        PluginPipeline *exec_pp = (PluginPipeline *)i->pipeline;
        if (exec_pp && exec_pp->nruntime_hooks > 0) {
            const char *tag_name = node_tag_to_string((NodeTag)VAL_TAG(n));
            for (int rh = 0; rh < exec_pp->nruntime_hooks; rh++) {
                RuntimePluginHook *hook = &exec_pp->runtime_hooks[rh];
                if (hook->kind != RT_HOOK_EXEC || !hook->callback) continue;
                if (hook->target && strcmp(hook->target, tag_name) == 0) {
                    Value *node_map = node_to_xs_map(n);
                    Value *env_map = xs_map_new();
                    Value *hargs[2] = { node_map, env_map };
                    Value *hr = call_value(i, hook->callback, hargs, 2, "exec_hook");
                    value_decref(node_map);
                    value_decref(env_map);
                    if (hr && VAL_TAG(hr) != XS_NULL) return hr;
                    if (hr) value_decref(hr);
                }
            }
        }
        /* Delegate to exec for statement nodes used in expression context */
        interp_exec(i, n);
        return value_incref(XS_NULL_VAL);
    }
    }
}

/* Walk a parsed program's top-level statements to find names the file
   wants exported. A name is public when:
     - the decl carries `pub` (fn, let, var, const, struct, enum, type
       alias, module, tag), or
     - a fn carries an `@export("alias")` decorator (both the local name
       and the alias become public).
   If a file has zero pub/@export declarations we fall back to exposing
   every top-level binding -- this preserves the old (loose) behaviour
   for code written before `pub` did anything, and gives users a
   migration path. The fallback is a one-liner: as soon as someone adds
   `pub fn` to the file, the loose mode flips off and only-pub wins. */
typedef struct PubName { char *name; struct PubName *next; } PubName;

static int pubname_has(PubName *list, const char *name) {
    for (PubName *p = list; p; p = p->next)
        if (strcmp(p->name, name) == 0) return 1;
    return 0;
}

static void pubname_add(PubName **list, const char *name) {
    if (!name || pubname_has(*list, name)) return;
    PubName *n = xs_malloc(sizeof(*n));
    n->name = xs_strdup(name);
    n->next = *list;
    *list = n;
}

static void pubname_free(PubName *list) {
    while (list) {
        PubName *next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
}

static PubName *collect_public_names(Node *prog, int *any_pub) {
    PubName *out = NULL;
    *any_pub = 0;
    if (!prog || VAL_TAG(prog) != NODE_PROGRAM) return out;
    for (int j = 0; j < prog->program.stmts.len; j++) {
        Node *s = prog->program.stmts.items[j];
        if (!s) continue;
        switch (VAL_TAG(s)) {
            case NODE_FN_DECL: {
                int has_export = 0;
                const char *export_name = NULL;
                for (int d = 0; d < s->fn_decl.n_decorators; d++) {
                    if (s->fn_decl.decorators[d].name &&
                        strcmp(s->fn_decl.decorators[d].name, "export") == 0) {
                        has_export = 1;
                        if (s->fn_decl.decorators[d].n_args > 0 &&
                            s->fn_decl.decorators[d].args[0] &&
                            VAL_TAG(s->fn_decl.decorators[d].args[0]) == NODE_LIT_STRING) {
                            export_name = s->fn_decl.decorators[d].args[0]->lit_string.sval;
                        }
                    }
                }
                if (s->fn_decl.is_pub || has_export) {
                    *any_pub = 1;
                    pubname_add(&out, s->fn_decl.name);
                    if (export_name) pubname_add(&out, export_name);
                }
                break;
            }
            case NODE_LET: case NODE_VAR:
                if (s->let.is_pub) { *any_pub = 1; pubname_add(&out, s->let.name); }
                break;
            case NODE_CONST:
                if (s->const_.is_pub) { *any_pub = 1; pubname_add(&out, s->const_.name); }
                break;
            case NODE_STRUCT_DECL:
                if (s->struct_decl.is_pub) { *any_pub = 1; pubname_add(&out, s->struct_decl.name); }
                break;
            case NODE_ENUM_DECL:
                if (s->enum_decl.is_pub) {
                    *any_pub = 1;
                    pubname_add(&out, s->enum_decl.name);
                    /* variant constructors live in the same namespace */
                    for (int v = 0; v < s->enum_decl.variants.len; v++) {
                        EnumVariant *ev = &s->enum_decl.variants.items[v];
                        if (ev->name) pubname_add(&out, ev->name);
                    }
                }
                break;
            case NODE_MODULE_DECL:
                if (s->module_decl.is_pub) { *any_pub = 1; pubname_add(&out, s->module_decl.name); }
                break;
            case NODE_TYPE_ALIAS:
                if (s->type_alias.is_pub) { *any_pub = 1; pubname_add(&out, s->type_alias.name); }
                break;
            case NODE_TAG_DECL:
                if (s->tag_decl.is_pub) { *any_pub = 1; pubname_add(&out, s->tag_decl.name); }
                break;
            default: break;
        }
    }
    return out;
}

static Value *load_xs_module_file(Interp *i, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = xs_malloc((size_t)(sz + 1));
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        free(src);
        fclose(f);
        return NULL;
    }
    src[sz] = '\0';
    fclose(f);

    char *filepath_owned = xs_strdup(filepath);
    Lexer lex;
    lexer_init(&lex, src, filepath_owned);
    TokenArray ta = lexer_tokenize(&lex);
    Parser p;
    parser_init(&p, &ta, filepath_owned);
    Node *prog = parser_parse(&p);
    token_array_free(&ta);
    if (!prog || p.had_error) {
        free(src);
        free(filepath_owned);
        if (prog) node_free(prog);
        return NULL;
    }

    Env *saved = i->env;
    i->env = env_new(i->globals);
    /* Hoist top-level fn decls so wrapping decorators and @export
       aliases run, same as interp_run does for the main program.
       Without this, `pub fn` lands in env but @export("name") never
       binds the alias into globals, and the loader can't expose it. */
    hoist_functions(i, &prog->program.stmts);
    for (int j = 0; j < prog->program.stmts.len; j++) {
        Node *s = prog->program.stmts.items[j];
        if (s && VAL_TAG(s) == NODE_FN_DECL) continue; /* already hoisted */
        interp_exec(i, s);
        if (i->cf.signal == CF_RETURN) CF_CLEAR(i);
        else if (i->cf.signal) break;
    }
    if (i->cf.signal == CF_ERROR || i->cf.signal == CF_PANIC || i->cf.signal == CF_THROW)
        CF_CLEAR(i);

    int any_pub = 0;
    PubName *pubs = collect_public_names(prog, &any_pub);

    XSMap *m = map_new();
    for (int j = 0; j < i->env->len; j++) {
        const char *bname = i->env->bindings[j].name;
        /* Only expose names the author marked public. If the file has no
           pub/@export anywhere, fall back to exposing everything so that
           older code keeps working unchanged. */
        if (any_pub && !pubname_has(pubs, bname)) continue;
        map_set(m, bname, value_incref(i->env->bindings[j].value));
    }
    /* @export("alias") binds the fn into globals (not env) under the
       alias. Pull those in too so `mod.alias` resolves through the
       module map. */
    if (any_pub) {
        for (PubName *pn = pubs; pn; pn = pn->next) {
            if (map_get(m, pn->name)) continue;
            Value *v = env_get(i->globals, pn->name);
            if (v) map_set(m, pn->name, value_incref(v));
        }
    }
    pubname_free(pubs);

    env_decref(i->env);
    i->env = saved;
    /* prog and src intentionally kept alive: function bodies
       reference AST nodes and the source buffer */
    (void)prog;
    (void)src;

    return xs_module(m);
}

/* VM-callable wrapper: spin up a fresh interpreter to load a user
   module file. Used by the bytecode VM's `use "file.xs"` lowering --
   it doesn't have a long-lived Interp of its own. The caller is
   responsible for path resolution; we just open whatever it gives us. */
Value *xs_load_user_module_file(const char *filepath) {
    if (!filepath) return NULL;
    Interp *tmp = interp_new(filepath);
    Value *mod = load_xs_module_file(tmp, filepath);
    /* The Interp owns a globals env that holds incref'd values pulled
       in by the module; freeing it now would tear those down before
       callers can use them. We leak the Interp on purpose -- modules
       are loaded once per process and never unloaded. */
    (void)tmp;
    return mod;
}

static Value *try_load_xs_module(Interp *i, const char *modname) {
    char path[2048];
    struct stat st;

    /* search both .xs_lib/ and xs_lib/ */
    static const char *lib_dirs[] = { ".xs_lib", "xs_lib", NULL };
    static const char *entry_files[] = { "main.xs", "lib.xs", "src/lib.xs", "src/main.xs", NULL };

    for (int d = 0; lib_dirs[d]; d++) {
        for (int e = 0; entry_files[e]; e++) {
            snprintf(path, sizeof(path), "%s/%s/%s", lib_dirs[d], modname, entry_files[e]);
            if (stat(path, &st) == 0) return load_xs_module_file(i, path);
        }
        /* try <lib>/<name>/<name>.xs */
        snprintf(path, sizeof(path), "%s/%s/%s.xs", lib_dirs[d], modname, modname);
        if (stat(path, &st) == 0) return load_xs_module_file(i, path);
    }

    /* fallback: global install dir ~/.xs/lib/ */
    const char *home = getenv("HOME");
    if (home) {
        for (int e = 0; entry_files[e]; e++) {
            snprintf(path, sizeof(path), "%s/.xs/lib/%s/%s", home, modname, entry_files[e]);
            if (stat(path, &st) == 0) return load_xs_module_file(i, path);
        }
        snprintf(path, sizeof(path), "%s/.xs/lib/%s/%s.xs", home, modname, modname);
        if (stat(path, &st) == 0) return load_xs_module_file(i, path);
    }

    /* check XS_LIB_PATH env var */
    const char *xs_lib_path = getenv("XS_LIB_PATH");
    if (xs_lib_path) {
        for (int e = 0; entry_files[e]; e++) {
            snprintf(path, sizeof(path), "%s/%s/%s", xs_lib_path, modname, entry_files[e]);
            if (stat(path, &st) == 0) return load_xs_module_file(i, path);
        }
        snprintf(path, sizeof(path), "%s/%s/%s.xs", xs_lib_path, modname, modname);
        if (stat(path, &st) == 0) return load_xs_module_file(i, path);
    }

    return NULL;
}

/* XS plugin system native functions */

static Env *s_plugin_host_globals = NULL;

static Value *native_plugin_global_set(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2 || !args[0] || VAL_TAG(args[0]) != XS_STR || !s_plugin_host_globals)
        return value_incref(XS_NULL_VAL);
    if (g_current_sandbox_flags & SANDBOX_INJECT_ONLY) {
        Value *existing = env_get(s_plugin_host_globals, args[0]->s);
        if (existing) {
            fprintf(stderr, "xs: sandbox: inject_only prevents overwriting '%s'\n", args[0]->s);
            return value_incref(XS_NULL_VAL);
        }
    }
    env_define(s_plugin_host_globals, args[0]->s, value_incref(args[1]), 1);
    return value_incref(XS_NULL_VAL);
}

static Value *native_plugin_global_get(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || VAL_TAG(args[0]) != XS_STR || !s_plugin_host_globals)
        return value_incref(XS_NULL_VAL);
    Value *v = env_get(s_plugin_host_globals, args[0]->s);
    return v ? value_incref(v) : value_incref(XS_NULL_VAL);
}

static Value *native_plugin_global_names(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    Value *arr = xs_array_new();
    if (!s_plugin_host_globals) return arr;
    for (int j = 0; j < s_plugin_host_globals->len; j++)
        array_push(arr->arr, xs_str(s_plugin_host_globals->bindings[j].name));
    return arr;
}

static Value *native_plugin_add_method(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 3 || !args[0] || VAL_TAG(args[0]) != XS_STR ||
        !args[1] || VAL_TAG(args[1]) != XS_STR ||
        !args[2] || (VAL_TAG(args[2]) != XS_FUNC && VAL_TAG(args[2]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    plugin_register_method(args[0]->s, args[1]->s, args[2]);
    return value_incref(XS_NULL_VAL);
}

static Value *native_plugin_teardown(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_teardown_count < MAX_TEARDOWN_FNS) {
        g_teardown_fns[g_teardown_count++] = value_incref(args[0]);
    }
    return value_incref(XS_NULL_VAL);
}

static Value *native_plugin_requires(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || VAL_TAG(args[0]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    if (!plugin_is_loaded(args[0]->s)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "required plugin '%s' not loaded", args[0]->s);
        fprintf(stderr, "xs: error: %s\n", buf);
        if (g_current_interp) {
            g_current_interp->cf.signal = CF_PANIC;
            g_current_interp->cf.value = xs_str(buf);
        }
    }
    return value_incref(XS_NULL_VAL);
}

/* phase 2: eval hook natives */

static Value *native_plugin_before_eval(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (g_current_sandbox_flags & SANDBOX_NO_EVAL_HOOK) {
        fprintf(stderr, "xs: sandbox: before_eval hooks are disabled\n");
        return value_incref(XS_NULL_VAL);
    }
    if (argc < 1) return value_incref(XS_NULL_VAL);

    int tag_filter = -1;
    Value *callback = NULL;

    if (argc >= 2 && args[0] && VAL_TAG(args[0]) == XS_STR &&
        args[1] && (VAL_TAG(args[1]) == XS_FUNC || VAL_TAG(args[1]) == XS_NATIVE)) {
        tag_filter = node_tag_from_string(args[0]->s);
        callback = args[1];
    } else if (args[0] && (VAL_TAG(args[0]) == XS_FUNC || VAL_TAG(args[0]) == XS_NATIVE)) {
        callback = args[0];
    } else {
        return value_incref(XS_NULL_VAL);
    }

    if (g_n_before_eval >= 64) return value_incref(XS_NULL_VAL);
    int idx = g_n_before_eval++;
    g_before_eval[idx].callback = value_incref(callback);
    g_before_eval[idx].tag_filter = tag_filter;
    g_has_eval_hooks = 1;

    return make_hook_handle(idx, "before_eval");
}

static Value *native_plugin_after_eval(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (g_current_sandbox_flags & SANDBOX_NO_EVAL_HOOK) {
        fprintf(stderr, "xs: sandbox: after_eval hooks are disabled\n");
        return value_incref(XS_NULL_VAL);
    }
    if (argc < 1) return value_incref(XS_NULL_VAL);

    int tag_filter = -1;
    Value *callback = NULL;

    if (argc >= 2 && args[0] && VAL_TAG(args[0]) == XS_STR &&
        args[1] && (VAL_TAG(args[1]) == XS_FUNC || VAL_TAG(args[1]) == XS_NATIVE)) {
        tag_filter = node_tag_from_string(args[0]->s);
        callback = args[1];
    } else if (args[0] && (VAL_TAG(args[0]) == XS_FUNC || VAL_TAG(args[0]) == XS_NATIVE)) {
        callback = args[0];
    } else {
        return value_incref(XS_NULL_VAL);
    }

    if (g_n_after_eval >= 64) return value_incref(XS_NULL_VAL);
    int idx = g_n_after_eval++;
    g_after_eval[idx].callback = value_incref(callback);
    g_after_eval[idx].tag_filter = tag_filter;
    g_has_eval_hooks = 1;

    return make_hook_handle(idx, "after_eval");
}

/* phase 2: syntax handler natives */

static Value *native_plugin_on_unknown(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_syntax_handlers >= 16) return value_incref(XS_NULL_VAL);
    int idx = g_n_syntax_handlers++;
    g_syntax_handlers[idx] = value_incref(args[0]);
    g_syntax_handler_targets[idx] = NULL; /* old API: no specific target */
    return make_hook_handle(idx, "on_unknown");
}

static Value *native_plugin_on_unknown_expr(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_syntax_expr_handlers >= 16) return value_incref(XS_NULL_VAL);
    int idx = g_n_syntax_expr_handlers++;
    g_syntax_expr_handlers[idx] = value_incref(args[0]);
    return make_hook_handle(idx, "on_unknown_expr");
}

static Value *native_plugin_on_postfix(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_postfix_handlers >= 16) return value_incref(XS_NULL_VAL);
    int idx = g_n_postfix_handlers++;
    g_postfix_handlers[idx] = value_incref(args[0]);
    return make_hook_handle(idx, "on_postfix");
}

/* phase 2: lexer extension natives */

static Value *native_plugin_add_keyword(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || VAL_TAG(args[0]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    if (g_n_plugin_keywords >= MAX_PLUGIN_KEYWORDS) return value_incref(XS_NULL_VAL);
    /* check for duplicates */
    for (int j = 0; j < g_n_plugin_keywords; j++) {
        if (strcmp(g_plugin_keywords[j], args[0]->s) == 0)
            return value_incref(XS_NULL_VAL);
    }
    g_plugin_keywords[g_n_plugin_keywords++] = xs_strdup(args[0]->s);
    return value_incref(XS_NULL_VAL);
}

/* phase 2: parser method natives (require active parser) */

/* These are declared here but implemented in parser.c through a callback mechanism.
   The parser sets a global pointer, and these natives use it. */

/* Active parser pointer - set/cleared during plugin handler invocation */
static void *g_active_parser = NULL;

static Node *parse_expr_from_parser(void *parser, int min_prec) {
    return parser_parse_expr((Parser *)parser, min_prec);
}
static Node *parse_block_from_parser(void *parser) {
    return parser_parse_block((Parser *)parser);
}
static Token *parser_peek_token(void *parser, int offset) {
    return parser_peek((Parser *)parser, offset);
}
static Token *parser_advance_token(void *parser) {
    return parser_advance((Parser *)parser);
}
static Token *parser_expect_kind(void *parser, int kind, const char *msg) {
    return parser_expect((Parser *)parser, (TokenKind)kind, msg);
}

static Value *native_parser_expr(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) {
        fprintf(stderr, "xs: error: parser methods can only be called during parsing\n");
        return value_incref(XS_NULL_VAL);
    }
    Node *n = parse_expr_from_parser(g_active_parser, 0);
    Value *result = node_to_xs_map(n);
    return result;
}

static Value *native_parser_block(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) {
        fprintf(stderr, "xs: error: parser methods can only be called during parsing\n");
        return value_incref(XS_NULL_VAL);
    }
    Node *n = parse_block_from_parser(g_active_parser);
    Value *result = node_to_xs_map(n);
    return result;
}

static Value *native_parser_ident(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) {
        fprintf(stderr, "xs: error: parser methods can only be called during parsing\n");
        return value_incref(XS_NULL_VAL);
    }
    Token *tok = parser_peek_token(g_active_parser, 0);
    if (tok->kind == TK_IDENT) {
        parser_advance_token(g_active_parser);
        return xs_str(tok->sval ? tok->sval : "");
    }
    return value_incref(XS_NULL_VAL);
}

static Value *native_parser_expect(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (!g_active_parser || argc < 1 || !args[0] || VAL_TAG(args[0]) != XS_STR) {
        fprintf(stderr, "xs: error: parser methods can only be called during parsing\n");
        return value_incref(XS_NULL_VAL);
    }
    const char *kind_str = args[0]->s;
    int kind = -1;
    if (strcmp(kind_str, "{") == 0) kind = TK_LBRACE;
    else if (strcmp(kind_str, "}") == 0) kind = TK_RBRACE;
    else if (strcmp(kind_str, "(") == 0) kind = TK_LPAREN;
    else if (strcmp(kind_str, ")") == 0) kind = TK_RPAREN;
    else if (strcmp(kind_str, "[") == 0) kind = TK_LBRACKET;
    else if (strcmp(kind_str, "]") == 0) kind = TK_RBRACKET;
    else if (strcmp(kind_str, ";") == 0) kind = TK_SEMICOLON;
    else if (strcmp(kind_str, ",") == 0) kind = TK_COMMA;
    else if (strcmp(kind_str, ":") == 0) kind = TK_COLON;
    else if (strcmp(kind_str, "=") == 0) kind = TK_ASSIGN;

    if (kind >= 0) {
        parser_expect_kind(g_active_parser, kind, "expected token");
    }
    return value_incref(XS_NULL_VAL);
}

static Value *native_parser_at(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (!g_active_parser || argc < 1 || !args[0] || VAL_TAG(args[0]) != XS_STR)
        return value_incref(XS_FALSE_VAL);
    Token *tok = parser_peek_token(g_active_parser, 0);
    const char *kind_str = args[0]->s;
    /* check if the current token matches */
    if (strcmp(kind_str, "{") == 0) return xs_bool(tok->kind == TK_LBRACE);
    if (strcmp(kind_str, "}") == 0) return xs_bool(tok->kind == TK_RBRACE);
    if (strcmp(kind_str, "(") == 0) return xs_bool(tok->kind == TK_LPAREN);
    if (strcmp(kind_str, ")") == 0) return xs_bool(tok->kind == TK_RPAREN);
    if (strcmp(kind_str, "IDENT") == 0) return xs_bool(tok->kind == TK_IDENT);
    if (strcmp(kind_str, "EOF") == 0) return xs_bool(tok->kind == TK_EOF);
    /* check against the token value for identifiers/keywords */
    if (tok->kind == TK_IDENT && tok->sval && strcmp(tok->sval, kind_str) == 0)
        return value_incref(XS_TRUE_VAL);
    return value_incref(XS_FALSE_VAL);
}

static Value *native_parser_peek(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    int offset = 0;
    if (argc > 0 && args[0] && VAL_TAG(args[0]) == XS_INT)
        offset = (int)VAL_INT(args[0]);
    Token *tok = parser_peek_token(g_active_parser, offset);
    Value *m = xs_map_new();
    map_set(m->map, "kind", xs_str(token_kind_name(tok->kind)));
    map_set(m->map, "value", xs_str(tok->sval ? tok->sval : token_kind_name(tok->kind)));
    map_take(m->map, "line", xs_int(tok->span.line));
    map_take(m->map, "col", xs_int(tok->span.col));
    return m;
}

/* phase 2: plugin-parser bridge functions (called from parser.c via fn ptrs) */

static int plugin_is_keyword_impl(const char *word) {
    if (!word) return 0;
    for (int j = 0; j < g_n_plugin_keywords; j++) {
        if (strcmp(g_plugin_keywords[j], word) == 0)
            return 1;
    }
    return 0;
}

static Node *plugin_try_syntax_handler_impl(Parser *p, Token *tok) {
    if ((g_n_syntax_handlers == 0 && g_n_parser_productions == 0) || !g_plugin_interp)
        return NULL;

    /* build token map: #{ kind: "...", value: "...", line: N, col: N } */
    Value *token_map = xs_map_new();
    map_set(token_map->map, "kind", xs_str(token_kind_name(tok->kind)));
    map_set(token_map->map, "value", xs_str(tok->sval ? tok->sval : token_kind_name(tok->kind)));
    map_take(token_map->map, "line", xs_int(tok->span.line));
    map_take(token_map->map, "col", xs_int(tok->span.col));

    void *saved_parser = g_active_parser;
    g_active_parser = p;

    Node *result = NULL;

    /* try on_unknown handlers first */
    for (int j = 0; j < g_n_syntax_handlers; j++) {
        Value *handler = g_syntax_handlers[j];
        if (!handler) continue;
        Value *args[1] = { token_map };
        Value *ret = call_value(g_plugin_interp, handler, args, 1, "on_unknown");
        if (ret && VAL_TAG(ret) == XS_MAP) {
            result = node_from_xs_map(ret);
            value_decref(ret);
            if (result) break;
        } else if (ret) {
            value_decref(ret);
        }
        if (g_plugin_interp->cf.signal) break;
    }

    /* try production handlers keyed on token value */
    if (!result && tok->sval) {
        for (int j = 0; j < g_n_parser_productions; j++) {
            if (!g_parser_productions[j].callback) continue;
            if (strcmp(g_parser_productions[j].keyword, tok->sval) != 0) continue;
            /* disambiguation: skip if forced plugin id doesn't match */
            if (g_plugin_forced_id && g_parser_productions[j].plugin_id[0] &&
                strcmp(g_parser_productions[j].plugin_id, g_plugin_forced_id) != 0) continue;
            /* build a parser state map so callback can use plugin.parser methods */
            Value *parser_map = xs_map_new();
            map_take(parser_map->map, "expr", xs_native(native_parser_expr));
            map_take(parser_map->map, "block", xs_native(native_parser_block));
            map_take(parser_map->map, "ident", xs_native(native_parser_ident));
            map_take(parser_map->map, "expect", xs_native(native_parser_expect));
            map_take(parser_map->map, "at", xs_native(native_parser_at));
            map_take(parser_map->map, "peek", xs_native(native_parser_peek));
            Value *args[2] = { parser_map, token_map };
            Value *ret = call_value(g_plugin_interp, g_parser_productions[j].callback,
                                    args, 2, "parser.production");
            value_decref(parser_map);
            if (ret && VAL_TAG(ret) == XS_MAP) {
                result = node_from_xs_map(ret);
                value_decref(ret);
                if (result) break;
            } else if (ret) {
                value_decref(ret);
            }
            if (g_plugin_interp->cf.signal) break;
        }
    }

    g_active_parser = saved_parser;
    value_decref(token_map);
    return result;
}

static Node *plugin_try_syntax_expr_handler_impl(Parser *p, Token *tok) {
    if (g_n_syntax_expr_handlers == 0 || !g_plugin_interp) return NULL;

    Value *token_map = xs_map_new();
    map_set(token_map->map, "kind", xs_str(token_kind_name(tok->kind)));
    map_set(token_map->map, "value", xs_str(tok->sval ? tok->sval : token_kind_name(tok->kind)));
    map_take(token_map->map, "line", xs_int(tok->span.line));
    map_take(token_map->map, "col", xs_int(tok->span.col));

    void *saved_parser = g_active_parser;
    g_active_parser = p;

    Node *result = NULL;
    for (int j = 0; j < g_n_syntax_expr_handlers; j++) {
        Value *handler = g_syntax_expr_handlers[j];
        if (!handler) continue;
        Value *args[1] = { token_map };
        Value *ret = call_value(g_plugin_interp, handler, args, 1, "on_unknown_expr");
        if (ret && VAL_TAG(ret) == XS_MAP) {
            result = node_from_xs_map(ret);
            value_decref(ret);
            if (result) break;
        } else if (ret) {
            value_decref(ret);
        }
        if (g_plugin_interp->cf.signal) break;
    }

    g_active_parser = saved_parser;
    value_decref(token_map);
    return result;
}

/* end phase 2 natives */

/* phase 3: hook handle .remove() */

static Value *native_hook_remove(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    /* 'self' is passed as a hidden first arg by the method-call mechanism,
       but for native functions on maps we need to fish the index/type out
       of the map that contains this native. We use a trick: store _hook_idx
       and _hook_type on the handle map, and the remove fn closes over the handle. */

    /* the handle map is accessible via interp->env where "self" might be bound,
       but for simplicity we encode the hook info in a closure context.
       Actually: since XS calls map.remove() like a method, 'self' is args[0]. */
    Value *self = (argc > 0 && args[0] && VAL_TAG(args[0]) == XS_MAP) ? args[0] : NULL;
    if (!self) return value_incref(XS_NULL_VAL);

    Value *idx_v = map_get(self->map, "_hook_idx");
    Value *type_v = map_get(self->map, "_hook_type");
    if (!idx_v || VAL_TAG(idx_v) != XS_INT || !type_v || VAL_TAG(type_v) != XS_STR)
        return value_incref(XS_NULL_VAL);

    int idx = (int)VAL_INT(idx_v);
    const char *type = type_v->s;

    if (strcmp(type, "before_eval") == 0 && idx >= 0 && idx < g_n_before_eval) {
        if (g_before_eval[idx].callback) {
            value_decref(g_before_eval[idx].callback);
            g_before_eval[idx].callback = NULL;
        }
    } else if (strcmp(type, "after_eval") == 0 && idx >= 0 && idx < g_n_after_eval) {
        if (g_after_eval[idx].callback) {
            value_decref(g_after_eval[idx].callback);
            g_after_eval[idx].callback = NULL;
        }
    } else if (strcmp(type, "on_unknown") == 0 && idx >= 0 && idx < g_n_syntax_handlers) {
        if (g_syntax_handlers[idx]) {
            value_decref(g_syntax_handlers[idx]);
            g_syntax_handlers[idx] = NULL;
        }
        if (g_syntax_handler_targets[idx]) {
            free(g_syntax_handler_targets[idx]);
            g_syntax_handler_targets[idx] = NULL;
        }
    } else if (strcmp(type, "on_unknown_expr") == 0 && idx >= 0 && idx < g_n_syntax_expr_handlers) {
        if (g_syntax_expr_handlers[idx]) {
            value_decref(g_syntax_expr_handlers[idx]);
            g_syntax_expr_handlers[idx] = NULL;
        }
    } else if (strcmp(type, "on_postfix") == 0 && idx >= 0 && idx < g_n_postfix_handlers) {
        if (g_postfix_handlers[idx]) {
            value_decref(g_postfix_handlers[idx]);
            g_postfix_handlers[idx] = NULL;
        }
    } else if (strcmp(type, "override") == 0 && idx >= 0 && idx < g_n_parser_overrides) {
        if (g_parser_overrides[idx].callback) {
            value_decref(g_parser_overrides[idx].callback);
            g_parser_overrides[idx].callback = NULL;
            if (g_parser_overrides[idx].previous) {
                value_decref(g_parser_overrides[idx].previous);
                g_parser_overrides[idx].previous = NULL;
            }
            g_parser_overrides[idx].keyword[0] = '\0';
        }
    } else if (strcmp(type, "transform") == 0 && idx >= 0 && idx < g_n_lexer_transforms) {
        if (g_lexer_transforms[idx]) {
            value_decref(g_lexer_transforms[idx]);
            g_lexer_transforms[idx] = NULL;
        }
    } else if (strcmp(type, "resolve_import") == 0 && idx >= 0 && idx < g_n_resolve_import) {
        if (g_resolve_import_hooks[idx]) {
            value_decref(g_resolve_import_hooks[idx]);
            g_resolve_import_hooks[idx] = NULL;
        }
    } else if (strcmp(type, "on_error") == 0 && idx >= 0 && idx < g_n_on_error) {
        if (g_on_error_hooks[idx]) {
            value_decref(g_on_error_hooks[idx]);
            g_on_error_hooks[idx] = NULL;
        }
    }
    return value_incref(XS_NULL_VAL);
}

static Value *make_hook_handle(int idx, const char *type) {
    Value *handle = xs_map_new();
    Value *idx_v = xs_int(idx);
    map_set(handle->map, "_hook_idx", idx_v); value_decref(idx_v);
    Value *type_v = xs_str(type);
    map_set(handle->map, "_hook_type", type_v); value_decref(type_v);
    map_take(handle->map, "remove", xs_native(native_hook_remove));
    return handle;
}

/* phase 3: parser override native */

/* wraps a built-in parser function as an XS callable for the "previous" chain */
typedef struct {
    const char *keyword;
} BuiltinParseCtx;

static Value *native_builtin_parse_if(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    Node *n = parser_parse_if((Parser *)g_active_parser);
    return node_to_xs_map(n);
}
static Value *native_builtin_parse_for(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    Node *n = parser_parse_for((Parser *)g_active_parser);
    return node_to_xs_map(n);
}
static Value *native_builtin_parse_while(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    Node *n = parser_parse_while((Parser *)g_active_parser);
    return node_to_xs_map(n);
}
static Value *native_builtin_parse_match(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    Node *n = parser_parse_match((Parser *)g_active_parser);
    return node_to_xs_map(n);
}
static Value *native_builtin_parse_fn(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    if (!g_active_parser) return value_incref(XS_NULL_VAL);
    Node *n = parser_parse_fn_decl((Parser *)g_active_parser, 0, 0, 0);
    return node_to_xs_map(n);
}

static Value *get_builtin_parser_for(const char *keyword) {
    if (strcmp(keyword, "if") == 0) return xs_native(native_builtin_parse_if);
    if (strcmp(keyword, "for") == 0) return xs_native(native_builtin_parse_for);
    if (strcmp(keyword, "while") == 0) return xs_native(native_builtin_parse_while);
    if (strcmp(keyword, "match") == 0) return xs_native(native_builtin_parse_match);
    if (strcmp(keyword, "fn") == 0) return xs_native(native_builtin_parse_fn);
    return NULL;
}

static Value *native_plugin_parser_override(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (g_current_sandbox_flags & SANDBOX_NO_OVERRIDE) {
        fprintf(stderr, "xs: sandbox: plugin.parser.override is disabled\n");
        return value_incref(XS_NULL_VAL);
    }
    if (argc < 2 || !args[0] || VAL_TAG(args[0]) != XS_STR ||
        !args[1] || (VAL_TAG(args[1]) != XS_FUNC && VAL_TAG(args[1]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_parser_overrides >= 32) return value_incref(XS_NULL_VAL);

    const char *keyword = args[0]->s;
    Value *callback = args[1];

    /* find previous handler: either another plugin's override or the built-in */
    Value *previous = NULL;
    for (int j = g_n_parser_overrides - 1; j >= 0; j--) {
        if (strcmp(g_parser_overrides[j].keyword, keyword) == 0 &&
            g_parser_overrides[j].callback) {
            previous = value_incref(g_parser_overrides[j].callback);
            break;
        }
    }
    if (!previous) {
        previous = get_builtin_parser_for(keyword);
    }

    int idx = g_n_parser_overrides++;
    strncpy(g_parser_overrides[idx].keyword, keyword, 63);
    g_parser_overrides[idx].keyword[63] = '\0';
    g_parser_overrides[idx].callback = value_incref(callback);
    g_parser_overrides[idx].previous = previous;

    return make_hook_handle(idx, "override");
}

/* phase 3: lexer transform native */

static Value *native_plugin_lexer_transform(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_lexer_transforms >= 16) return value_incref(XS_NULL_VAL);

    int idx = g_n_lexer_transforms++;
    g_lexer_transforms[idx] = value_incref(args[0]);
    return make_hook_handle(idx, "transform");
}

/* phase 3: resolve_import native */

static Value *native_plugin_resolve_import(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_resolve_import >= 16) return value_incref(XS_NULL_VAL);

    int idx = g_n_resolve_import++;
    g_resolve_import_hooks[idx] = value_incref(args[0]);
    return make_hook_handle(idx, "resolve_import");
}

/* phase 3: on_error native */

static Value *native_plugin_on_error(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !args[0] || (VAL_TAG(args[0]) != XS_FUNC && VAL_TAG(args[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    if (g_n_on_error >= 16) return value_incref(XS_NULL_VAL);

    int idx = g_n_on_error++;
    g_on_error_hooks[idx] = value_incref(args[0]);
    return make_hook_handle(idx, "on_error");
}

/* phase 3: plugin.hooks inspection */

static Value *native_plugin_hooks(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    Value *hooks = xs_map_new();

    /* before_eval array */
    Value *be_arr = xs_array_new();
    for (int j = 0; j < g_n_before_eval; j++) {
        if (g_before_eval[j].callback)
            array_push(be_arr->arr, value_incref(g_before_eval[j].callback));
    }
    map_set(hooks->map, "before_eval", be_arr); value_decref(be_arr);

    /* after_eval array */
    Value *ae_arr = xs_array_new();
    for (int j = 0; j < g_n_after_eval; j++) {
        if (g_after_eval[j].callback)
            array_push(ae_arr->arr, value_incref(g_after_eval[j].callback));
    }
    map_set(hooks->map, "after_eval", ae_arr); value_decref(ae_arr);

    /* on_unknown array */
    Value *ou_arr = xs_array_new();
    for (int j = 0; j < g_n_syntax_handlers; j++) {
        if (g_syntax_handlers[j])
            array_push(ou_arr->arr, value_incref(g_syntax_handlers[j]));
    }
    map_set(hooks->map, "on_unknown", ou_arr); value_decref(ou_arr);

    /* on_unknown_expr array */
    Value *oue_arr = xs_array_new();
    for (int j = 0; j < g_n_syntax_expr_handlers; j++) {
        if (g_syntax_expr_handlers[j])
            array_push(oue_arr->arr, value_incref(g_syntax_expr_handlers[j]));
    }
    map_set(hooks->map, "on_unknown_expr", oue_arr); value_decref(oue_arr);

    /* on_postfix array */
    Value *op_arr = xs_array_new();
    for (int j = 0; j < g_n_postfix_handlers; j++) {
        if (g_postfix_handlers[j])
            array_push(op_arr->arr, value_incref(g_postfix_handlers[j]));
    }
    map_set(hooks->map, "on_postfix", op_arr); value_decref(op_arr);

    /* overrides map: keyword -> handler */
    Value *ov_map = xs_map_new();
    for (int j = 0; j < g_n_parser_overrides; j++) {
        if (g_parser_overrides[j].callback && g_parser_overrides[j].keyword[0])
            map_set(ov_map->map, g_parser_overrides[j].keyword,
                    value_incref(g_parser_overrides[j].callback));
    }
    map_set(hooks->map, "overrides", ov_map); value_decref(ov_map);

    /* transforms array */
    Value *tr_arr = xs_array_new();
    for (int j = 0; j < g_n_lexer_transforms; j++) {
        if (g_lexer_transforms[j])
            array_push(tr_arr->arr, value_incref(g_lexer_transforms[j]));
    }
    map_set(hooks->map, "transforms", tr_arr); value_decref(tr_arr);

    /* resolve_import array */
    Value *ri_arr = xs_array_new();
    for (int j = 0; j < g_n_resolve_import; j++) {
        if (g_resolve_import_hooks[j])
            array_push(ri_arr->arr, value_incref(g_resolve_import_hooks[j]));
    }
    map_set(hooks->map, "resolve_import", ri_arr); value_decref(ri_arr);

    /* on_error array */
    Value *oe_arr = xs_array_new();
    for (int j = 0; j < g_n_on_error; j++) {
        if (g_on_error_hooks[j])
            array_push(oe_arr->arr, value_incref(g_on_error_hooks[j]));
    }
    map_set(hooks->map, "on_error", oe_arr); value_decref(oe_arr);

    return hooks;
}

/* phase 3: parser override bridge (called from parser.c) */

static Node *plugin_try_parser_override_impl(Parser *p, const char *keyword) {
    if (g_n_parser_overrides == 0 || !g_plugin_interp) return NULL;

    /* find the last (most recent) override for this keyword */
    int found = -1;
    for (int j = g_n_parser_overrides - 1; j >= 0; j--) {
        if (g_parser_overrides[j].callback &&
            strcmp(g_parser_overrides[j].keyword, keyword) == 0) {
            found = j;
            break;
        }
    }
    if (found < 0) return NULL;

    void *saved_parser = g_active_parser;
    g_active_parser = p;

    /* build a "previous" function for the callback.
       If this override has a stored previous, use it;
       otherwise use the built-in parser wrapper. */
    Value *previous = g_parser_overrides[found].previous;
    if (!previous) previous = get_builtin_parser_for(keyword);

    Value *args[1] = { previous };
    Value *ret = call_value(g_plugin_interp, g_parser_overrides[found].callback,
                            args, 1, "parser.override");

    g_active_parser = saved_parser;

    if (ret && VAL_TAG(ret) == XS_MAP) {
        Node *result = node_from_xs_map(ret);
        value_decref(ret);
        return result;
    }
    if (ret) value_decref(ret);
    return NULL;
}

void build_plugin_map(Value *plugin_map) {
    /* plugin.lexer */
    Value *lexer_map = xs_map_new();
    map_take(lexer_map->map, "add_keyword", xs_native(native_plugin_add_keyword));
    map_take(lexer_map->map, "transform", xs_native(native_plugin_lexer_transform));
    map_set(plugin_map->map, "lexer", lexer_map);
    value_decref(lexer_map);

    /* plugin.parser */
    Value *parser_map = xs_map_new();
    map_take(parser_map->map, "on_unknown", xs_native(native_plugin_on_unknown));
    map_take(parser_map->map, "on_unknown_expr", xs_native(native_plugin_on_unknown_expr));
    map_take(parser_map->map, "on_postfix", xs_native(native_plugin_on_postfix));
    map_take(parser_map->map, "override", xs_native(native_plugin_parser_override));
    map_take(parser_map->map, "expr", xs_native(native_parser_expr));
    map_take(parser_map->map, "block", xs_native(native_parser_block));
    map_take(parser_map->map, "ident", xs_native(native_parser_ident));
    map_take(parser_map->map, "expect", xs_native(native_parser_expect));
    map_take(parser_map->map, "at", xs_native(native_parser_at));
    map_take(parser_map->map, "peek", xs_native(native_parser_peek));
    map_set(plugin_map->map, "parser", parser_map);
    value_decref(parser_map);

    /* plugin.hooks: callable that returns current hook state */
    map_take(plugin_map->map, "hooks", xs_native(native_plugin_hooks));

    /* plugin.runtime */
    Value *runtime_map = xs_map_new();

    /* plugin.runtime.global with .set/.get/.names */
    Value *global_map = xs_map_new();
    map_take(global_map->map, "set", xs_native(native_plugin_global_set));
    map_take(global_map->map, "get", xs_native(native_plugin_global_get));
    map_take(global_map->map, "names", xs_native(native_plugin_global_names));
    map_set(runtime_map->map, "global", global_map);
    value_decref(global_map);

    /* plugin.runtime.add_method */
    map_take(runtime_map->map, "add_method", xs_native(native_plugin_add_method));

    /* plugin.runtime.before_eval / after_eval */
    map_take(runtime_map->map, "before_eval", xs_native(native_plugin_before_eval));
    map_take(runtime_map->map, "after_eval", xs_native(native_plugin_after_eval));

    /* plugin.runtime.resolve_import / on_error */
    map_take(runtime_map->map, "resolve_import", xs_native(native_plugin_resolve_import));
    map_take(runtime_map->map, "on_error", xs_native(native_plugin_on_error));

    map_set(plugin_map->map, "runtime", runtime_map);
    value_decref(runtime_map);

    /* plugin.teardown */
    map_take(plugin_map->map, "teardown", xs_native(native_plugin_teardown));

    /* plugin.requires */
    map_take(plugin_map->map, "requires", xs_native(native_plugin_requires));

    /* plugin.ast constructors */
    Value *ast_map = xs_map_new();
    map_take(ast_map->map, "int_node", xs_native(native_ast_int_node));
    map_take(ast_map->map, "float_node", xs_native(native_ast_float_node));
    map_take(ast_map->map, "str_node", xs_native(native_ast_str_node));
    map_take(ast_map->map, "bool_node", xs_native(native_ast_bool_node));
    map_take(ast_map->map, "null_node", xs_native(native_ast_null_node));
    map_take(ast_map->map, "ident", xs_native(native_ast_ident));
    map_take(ast_map->map, "binop", xs_native(native_ast_binop));
    map_take(ast_map->map, "unary", xs_native(native_ast_unary));
    map_take(ast_map->map, "call", xs_native(native_ast_call));
    map_take(ast_map->map, "method_call", xs_native(native_ast_method_call));
    map_take(ast_map->map, "if_expr", xs_native(native_ast_if_expr));
    map_take(ast_map->map, "if_else", xs_native(native_ast_if_else));
    map_take(ast_map->map, "block", xs_native(native_ast_block));
    map_take(ast_map->map, "let_decl", xs_native(native_ast_let_decl));
    map_take(ast_map->map, "var_decl", xs_native(native_ast_var_decl));
    map_take(ast_map->map, "fn_decl", xs_native(native_ast_fn_decl));
    map_take(ast_map->map, "lambda", xs_native(native_ast_lambda));
    map_take(ast_map->map, "return_node", xs_native(native_ast_return_node));
    map_take(ast_map->map, "assign", xs_native(native_ast_assign));
    map_take(ast_map->map, "for_loop", xs_native(native_ast_for_loop));
    map_take(ast_map->map, "while_loop", xs_native(native_ast_while_loop));
    map_take(ast_map->map, "array", xs_native(native_ast_array));
    map_take(ast_map->map, "map", xs_native(native_ast_map_node));
    map_take(ast_map->map, "duration", xs_native(native_ast_duration));
    map_take(ast_map->map, "color", xs_native(native_ast_color));
    map_take(ast_map->map, "date", xs_native(native_ast_date));
    map_take(ast_map->map, "size", xs_native(native_ast_size));
    map_take(ast_map->map, "angle", xs_native(native_ast_angle));
    map_take(ast_map->map, "every", xs_native(native_ast_every));
    map_take(ast_map->map, "after", xs_native(native_ast_after));
    map_take(ast_map->map, "timeout", xs_native(native_ast_timeout));
    map_take(ast_map->map, "debounce", xs_native(native_ast_debounce));
    map_set(plugin_map->map, "ast", ast_map);
    value_decref(ast_map);
}

static void exec_plugin_load(Interp *i, Node *stmt, const char *resolved) {
    const char *use_path = stmt->use_.path;

    FILE *pf = fopen(resolved, "rb");
    if (!pf) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "plugin \"%s\" failed to load", use_path);
        xs_runtime_error(stmt->span, errbuf, NULL,
            "could not open '%s'", resolved);
        i->cf.signal = CF_PANIC;
        i->cf.value = xs_str("plugin load error");
        return;
    }
    fseek(pf, 0, SEEK_END);
    long psz = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    char *psrc = xs_malloc((size_t)(psz + 1));
    if (fread(psrc, 1, (size_t)psz, pf) != (size_t)psz) {
        free(psrc); fclose(pf);
        i->cf.signal = CF_PANIC;
        i->cf.value = xs_str("plugin read error");
        return;
    }
    psrc[psz] = '\0';
    fclose(pf);

    char *pfpath = xs_strdup(resolved);
    Lexer plex;
    lexer_init(&plex, psrc, pfpath);
    TokenArray pta = lexer_tokenize(&plex);
    Parser pp;
    parser_init(&pp, &pta, pfpath);
    Node *pprog = parser_parse(&pp);
    token_array_free(&pta);
    if (!pprog || pp.had_error) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "plugin \"%s\" failed to load", use_path);
        xs_runtime_error(stmt->span, errbuf, NULL, "parse error in '%s'", resolved);
        free(psrc); free(pfpath);
        if (pprog) node_free(pprog);
        i->cf.signal = CF_PANIC;
        i->cf.value = xs_str("plugin parse error");
        return;
    }

    /* phase 3: set sandbox flags for this plugin load */
    int saved_sandbox_flags = g_current_sandbox_flags;
    g_current_sandbox_flags = stmt->use_.sandbox_flags;

    /* save state for rollback on error */
    int saved_method_count = g_plugin_method_count;
    int saved_teardown_count = g_teardown_count;
    int saved_xs_plugin_count = g_xs_plugin_count;
    int saved_before_eval = g_n_before_eval;
    int saved_after_eval = g_n_after_eval;
    int saved_syntax_handlers = g_n_syntax_handlers;
    int saved_syntax_expr_handlers = g_n_syntax_expr_handlers;
    int saved_postfix_handlers = g_n_postfix_handlers;
    int saved_plugin_keywords = g_n_plugin_keywords;
    int saved_parser_overrides = g_n_parser_overrides;
    int saved_parser_productions = g_n_parser_productions;
    int saved_lexer_transforms = g_n_lexer_transforms;
    int saved_resolve_import = g_n_resolve_import;
    int saved_on_error = g_n_on_error;

    /* set the host globals pointer for native functions */
    s_plugin_host_globals = i->globals;
    g_plugin_interp = i;

    /* build the plugin map */
    Value *plugin_map = xs_map_new();
    build_plugin_map(plugin_map);

    /* create temp interpreter and inject plugin */
    Interp *tmp = interp_new(pfpath);
    /* share the main interpreter's pipeline so plugin meta accumulates globally */
    if (i->pipeline && tmp->pipeline) {
        pipeline_free(tmp->pipeline);
        tmp->pipeline = i->pipeline;
    }
    env_define(tmp->globals, "plugin", value_incref(plugin_map), 1);

    /* save and restore the main interp pointer */
    Interp *saved_interp = g_current_interp;
    Interp *saved_plugin_interp = g_plugin_interp;

    /* run the plugin file in the temp interpreter */
    interp_run(tmp, pprog);

    /* restore main interp */
    g_current_interp = saved_interp;
    g_plugin_interp = saved_plugin_interp;

    int had_error = (tmp->cf.signal == CF_ERROR || tmp->cf.signal == CF_PANIC ||
                     tmp->cf.signal == CF_THROW);

    if (had_error) {
        /* rollback: undo any methods/teardowns/plugins registered */
        for (int j = saved_method_count; j < g_plugin_method_count; j++) {
            free(g_plugin_methods[j].type_name);
            free(g_plugin_methods[j].method_name);
            value_decref(g_plugin_methods[j].fn);
        }
        g_plugin_method_count = saved_method_count;

        for (int j = saved_teardown_count; j < g_teardown_count; j++)
            value_decref(g_teardown_fns[j]);
        g_teardown_count = saved_teardown_count;

        for (int j = saved_xs_plugin_count; j < g_xs_plugin_count; j++) {
            free(g_xs_plugins[j].name);
            free(g_xs_plugins[j].version);
        }
        g_xs_plugin_count = saved_xs_plugin_count;

        /* rollback phase 2 hooks */
        for (int j = saved_before_eval; j < g_n_before_eval; j++)
            value_decref(g_before_eval[j].callback);
        g_n_before_eval = saved_before_eval;
        for (int j = saved_after_eval; j < g_n_after_eval; j++)
            value_decref(g_after_eval[j].callback);
        g_n_after_eval = saved_after_eval;
        for (int j = saved_syntax_handlers; j < g_n_syntax_handlers; j++) {
            value_decref(g_syntax_handlers[j]);
            if (g_syntax_handler_targets[j]) { free(g_syntax_handler_targets[j]); g_syntax_handler_targets[j] = NULL; }
        }
        g_n_syntax_handlers = saved_syntax_handlers;
        for (int j = saved_syntax_expr_handlers; j < g_n_syntax_expr_handlers; j++)
            value_decref(g_syntax_expr_handlers[j]);
        g_n_syntax_expr_handlers = saved_syntax_expr_handlers;
        for (int j = saved_postfix_handlers; j < g_n_postfix_handlers; j++)
            value_decref(g_postfix_handlers[j]);
        g_n_postfix_handlers = saved_postfix_handlers;
        for (int j = saved_plugin_keywords; j < g_n_plugin_keywords; j++)
            free(g_plugin_keywords[j]);
        g_n_plugin_keywords = saved_plugin_keywords;

        /* rollback phase 3 hooks */
        for (int j = saved_parser_overrides; j < g_n_parser_overrides; j++) {
            value_decref(g_parser_overrides[j].callback);
            if (g_parser_overrides[j].previous)
                value_decref(g_parser_overrides[j].previous);
        }
        g_n_parser_overrides = saved_parser_overrides;
        for (int j = saved_parser_productions; j < g_n_parser_productions; j++)
            value_decref(g_parser_productions[j].callback);
        g_n_parser_productions = saved_parser_productions;
        for (int j = saved_lexer_transforms; j < g_n_lexer_transforms; j++)
            value_decref(g_lexer_transforms[j]);
        g_n_lexer_transforms = saved_lexer_transforms;
        for (int j = saved_resolve_import; j < g_n_resolve_import; j++)
            value_decref(g_resolve_import_hooks[j]);
        g_n_resolve_import = saved_resolve_import;
        for (int j = saved_on_error; j < g_n_on_error; j++)
            value_decref(g_on_error_hooks[j]);
        g_n_on_error = saved_on_error;

        g_has_eval_hooks = (g_n_before_eval > 0 || g_n_after_eval > 0);

        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "plugin \"%s\" failed to load", use_path);
        xs_runtime_error(stmt->span, errbuf, NULL,
            "error while executing plugin '%s'", resolved);
        i->cf.signal = CF_PANIC;
        i->cf.value = xs_str("plugin execution error");
    } else {
        /* success: read plugin.meta to register the loaded plugin */
        const char *pname = use_path;
        const char *pver = NULL;
        Value *pval = env_get(tmp->globals, "plugin");
        if (pval && VAL_TAG(pval) == XS_MAP) {
            Value *meta = map_get(pval->map, "meta");
            if (meta && VAL_TAG(meta) == XS_MAP) {
                Value *name_v = map_get(meta->map, "name");
                Value *ver_v = map_get(meta->map, "version");
                if (name_v && VAL_TAG(name_v) == XS_STR) pname = name_v->s;
                if (ver_v && VAL_TAG(ver_v) == XS_STR) pver = ver_v->s;
            }
        }
        plugin_register_loaded(pname, pver);
        /* flag re-parse if syntax handlers, overrides, or productions were newly registered */
        if (g_n_syntax_handlers > saved_syntax_handlers ||
            g_n_plugin_keywords > saved_plugin_keywords ||
            g_n_parser_overrides > saved_parser_overrides ||
            g_n_parser_productions > saved_parser_productions)
            i->needs_reparse = 1;
        /* store source, filepath, and AST so closures remain valid */
        int idx = g_xs_plugin_count - 1;
        if (idx >= 0) {
            g_xs_plugins[idx].source = psrc;
            g_xs_plugins[idx].filepath = pfpath;
            g_xs_plugins[idx].program = pprog;
            psrc = NULL;
            pfpath = NULL;
            pprog = NULL;
        }
    }

    value_decref(plugin_map);
    /* detach shared pipeline before freeing temp interp so it doesn't get freed */
    if (tmp->pipeline == i->pipeline)
        tmp->pipeline = NULL;
    interp_free(tmp);
    /* restore sandbox flags */
    g_current_sandbox_flags = saved_sandbox_flags;
    /* if error, free psrc/pprog/pfpath; if success, they were moved into g_xs_plugins */
    free(psrc);
    free(pfpath);
    if (pprog) node_free(pprog);

    /* run any newly registered passes on the current program AST.
       closure envs survive because env refcounting keeps them alive
       after the temp interpreter is freed. callbacks are called through
       the main interpreter so native fns work via s_plugin_host_globals. */
    if (!had_error && i->pipeline && i->current_program && !i->cf.signal) {
        PluginPipeline *pp = (PluginPipeline *)i->pipeline;
        if (pp->npasses > 0) {
            pipeline_run_passes(pp, i->current_program, "parser", 0, i);
            if (i->cf.signal) CF_CLEAR(i);
            pipeline_run_passes(pp, i->current_program, "parser", 1, i);
            if (i->cf.signal) CF_CLEAR(i);
            pipeline_run_passes(pp, i->current_program, "sema", 0, i);
            if (i->cf.signal) CF_CLEAR(i);
            pipeline_run_passes(pp, i->current_program, "sema", 1, i);
            if (i->cf.signal) CF_CLEAR(i);
        }
        if (pp->nsema_rules > 0) {
            pipeline_dispatch_sema(pp, i->current_program, i);
            if (i->cf.signal) CF_CLEAR(i);
        }
    }
}

/* load statement rename info (set before exec_plugin_load, cleared after) */
static char **g_load_rename_keys = NULL;
static char **g_load_rename_vals = NULL;
static int    g_load_nrenames = 0;

/* exec_load_stmt: handle NODE_LOAD by creating a temp NODE_USE wrapper and
   calling exec_plugin_load */
static void exec_load_stmt(Interp *i, Node *stmt) {
    /* The parser already evaluated this plugin eagerly so any keywords
       it registered were in scope for the rest of the parse pass. Skip
       the runtime path; running the plugin body twice would re-register
       its hooks. */
    if (stmt->load_.preloaded) return;
    const char *load_path = stmt->load_.path;

    /* resolve relative to current file's directory */
    char resolved[PATH_MAX];
    if (load_path[0] != '/') {
        const char *fn = i->filename ? i->filename : "";
        const char *last_slash = strrchr(fn, '/');
        if (last_slash) {
            int dirlen = (int)(last_slash - fn);
            snprintf(resolved, sizeof(resolved), "%.*s/%s", dirlen, fn, load_path);
        } else {
            snprintf(resolved, sizeof(resolved), "%s", load_path);
        }
    } else {
        snprintf(resolved, sizeof(resolved), "%s", load_path);
    }

    /* try lib paths if not found -- only for bare names (no slash, no .xs ext) */
    struct stat pst;
    if (stat(resolved, &pst) != 0 &&
        !strchr(load_path, '/') &&
        !(strlen(load_path) > 3 && strcmp(load_path + strlen(load_path) - 3, ".xs") == 0)) {
        char lib_try[PATH_MAX];
        static const char *pdirs[] = { ".xs_lib", "xs_lib", NULL };
        static const char *pentries[] = {
            "plugin.xs", "main.xs", "lib.xs",
            "src/plugin.xs", "src/main.xs", "src/lib.xs", NULL
        };
        int found = 0;

        /* search local lib dirs */
        for (int pd = 0; pdirs[pd] && !found; pd++) {
            for (int pe = 0; pentries[pe] && !found; pe++) {
                snprintf(lib_try, sizeof(lib_try), "%s/%s/%s", pdirs[pd], load_path, pentries[pe]);
                if (stat(lib_try, &pst) == 0) {
                    snprintf(resolved, sizeof(resolved), "%s", lib_try);
                    found = 1;
                }
            }
            /* try <lib>/<name>/<name>.xs */
            if (!found) {
                snprintf(lib_try, sizeof(lib_try), "%s/%s/%s.xs", pdirs[pd], load_path, load_path);
                if (stat(lib_try, &pst) == 0) {
                    snprintf(resolved, sizeof(resolved), "%s", lib_try);
                    found = 1;
                }
            }
        }

        /* fallback: global install dir ~/.xs/lib/ */
        if (!found) {
            const char *home = getenv("HOME");
            if (home) {
                for (int pe = 0; pentries[pe] && !found; pe++) {
                    snprintf(lib_try, sizeof(lib_try), "%s/.xs/lib/%s/%s", home, load_path, pentries[pe]);
                    if (stat(lib_try, &pst) == 0) {
                        snprintf(resolved, sizeof(resolved), "%s", lib_try);
                        found = 1;
                    }
                }
                if (!found) {
                    snprintf(lib_try, sizeof(lib_try), "%s/.xs/lib/%s/%s.xs", home, load_path, load_path);
                    if (stat(lib_try, &pst) == 0) {
                        snprintf(resolved, sizeof(resolved), "%s", lib_try);
                        found = 1;
                    }
                }
            }
        }

        /* check XS_LIB_PATH env var */
        if (!found) {
            const char *xs_lib_path = getenv("XS_LIB_PATH");
            if (xs_lib_path) {
                for (int pe = 0; pentries[pe] && !found; pe++) {
                    snprintf(lib_try, sizeof(lib_try), "%s/%s/%s", xs_lib_path, load_path, pentries[pe]);
                    if (stat(lib_try, &pst) == 0) {
                        snprintf(resolved, sizeof(resolved), "%s", lib_try);
                        found = 1;
                    }
                }
                if (!found) {
                    snprintf(lib_try, sizeof(lib_try), "%s/%s/%s.xs", xs_lib_path, load_path, load_path);
                    if (stat(lib_try, &pst) == 0) {
                        snprintf(resolved, sizeof(resolved), "%s", lib_try);
                        found = 1;
                    }
                }
            }
        }
    }

    /* store rename info globally so exec_plugin_load can use it if needed */
    g_load_rename_keys = stmt->load_.rename_keys;
    g_load_rename_vals = stmt->load_.rename_vals;
    g_load_nrenames = stmt->load_.nrenames;

    /* create a temporary NODE_USE to pass to exec_plugin_load */
    Node tmp_use;
    memset(&tmp_use, 0, sizeof(tmp_use));
    tmp_use.tag = NODE_USE;
    tmp_use.span = stmt->span;
    tmp_use.use_.path = stmt->load_.path;
    tmp_use.use_.is_plugin = 1;
    tmp_use.use_.sandbox_flags = stmt->load_.sandbox_flags;
    tmp_use.use_.alias = NULL;
    tmp_use.use_.names = NULL;
    tmp_use.use_.name_aliases = NULL;
    tmp_use.use_.nnames = 0;
    tmp_use.use_.import_all = 0;

    exec_plugin_load(i, &tmp_use, resolved);

    /* clear rename info */
    g_load_rename_keys = NULL;
    g_load_rename_vals = NULL;
    g_load_nrenames = 0;
}

/* AST walker for custom passes. returns replacement node (or original) for transforms. */
static Node *walk_node_for_passes(Interp *interp, Node *n, CustomPass *pass) {
    if (!n || !pass) return n;
    /* check if this node's tag matches any visitor */
    for (int v = 0; v < pass->nvisitors; v++) {
        int tag_match = (pass->visitor_tags[v] == (int)VAL_TAG(n));
        if (!tag_match && pass->visitor_tags[v] == -1) {
            /* name-based matching for custom/unresolved tag names */
            if (pass->visitor_names && pass->visitor_names[v]) {
                const char *cur_tag = node_tag_to_string((NodeTag)VAL_TAG(n));
                tag_match = (strcmp(pass->visitor_names[v], cur_tag) == 0);
            } else {
                tag_match = 1; /* wildcard: no name, match all */
            }
        }
        if (tag_match) {
            Value *cb = pass->visitors[v];
            if (!cb || (VAL_TAG(cb) != XS_FUNC && VAL_TAG(cb) != XS_NATIVE)) continue;
            /* reset sema chain position for each node visit */
            if (g_sema_chain) g_sema_chain_pos = 0;
            Value *node_map = node_to_xs_map(n);
            /* pass state as second argument if available */
            Value *args[2] = { node_map, pass->state ? pass->state : XS_NULL_VAL };
            int nargs = pass->state ? 2 : 1;
            /* suppress error output during pass execution */
            DiagContext *saved_diag = interp->diag;
            interp->diag = NULL;
            Value *result = call_value(interp, cb, args, nargs, "pass_visitor");
            interp->diag = saved_diag;
            /* transform passes: if callback returns a map, replace the node */
            if (pass->kind == PASS_TRANSFORM && result && VAL_TAG(result) == XS_MAP) {
                Node *replacement = node_from_xs_map(result);
                if (replacement) {
                    value_decref(result);
                    value_decref(node_map);
                    if (interp->cf.signal) { CF_CLEAR(interp); }
                    return replacement;
                }
            }
            if (result) value_decref(result);
            value_decref(node_map);
            if (interp->cf.signal) { CF_CLEAR(interp); }
        }
    }
    /* helper macro: walk child and replace in-place if transform returns new node */
#define WALK_CHILD(ptr) do { \
    Node *_r = walk_node_for_passes(interp, (ptr), pass); \
    if (_r != (ptr)) (ptr) = _r; \
} while (0)

    /* helper: fire on_scope_exit callback if present */
#define FIRE_SCOPE_EXIT(block_node) do { \
    if (pass->on_scope_exit && (VAL_TAG(pass->on_scope_exit) == XS_FUNC || VAL_TAG(pass->on_scope_exit) == XS_NATIVE)) { \
        Value *scope_info = xs_map_new(); \
        map_take(scope_info->map, "line", xs_int((block_node)->span.line)); \
        map_take(scope_info->map, "col", xs_int((block_node)->span.col)); \
        Value *se_args[1] = { scope_info }; \
        DiagContext *sd = interp->diag; \
        interp->diag = NULL; \
        Value *se_r = call_value(interp, pass->on_scope_exit, se_args, 1, "scope_exit"); \
        interp->diag = sd; \
        if (se_r) value_decref(se_r); \
        value_decref(scope_info); \
        if (interp->cf.signal) CF_CLEAR(interp); \
    } \
} while (0)

    /* recurse into children based on node tag */
    switch (VAL_TAG(n)) {
    case NODE_PROGRAM:
        for (int j = 0; j < n->program.stmts.len; j++)
            WALK_CHILD(n->program.stmts.items[j]);
        break;
    case NODE_BLOCK:
        for (int j = 0; j < n->block.stmts.len; j++)
            WALK_CHILD(n->block.stmts.items[j]);
        if (n->block.expr) WALK_CHILD(n->block.expr);
        FIRE_SCOPE_EXIT(n);
        break;
    case NODE_FN_DECL:
        if (n->fn_decl.body) WALK_CHILD(n->fn_decl.body);
        FIRE_SCOPE_EXIT(n);
        break;
    case NODE_LET:
    case NODE_VAR:
        if (n->let.value) WALK_CHILD(n->let.value);
        break;
    case NODE_CONST:
        if (n->const_.value) WALK_CHILD(n->const_.value);
        break;
    case NODE_IF:
        WALK_CHILD(n->if_expr.cond);
        WALK_CHILD(n->if_expr.then);
        for (int j = 0; j < n->if_expr.elif_conds.len; j++) {
            WALK_CHILD(n->if_expr.elif_conds.items[j]);
            WALK_CHILD(n->if_expr.elif_thens.items[j]);
        }
        if (n->if_expr.else_branch) WALK_CHILD(n->if_expr.else_branch);
        break;
    case NODE_FOR:
        WALK_CHILD(n->for_loop.iter);
        WALK_CHILD(n->for_loop.body);
        break;
    case NODE_WHILE:
        WALK_CHILD(n->while_loop.cond);
        WALK_CHILD(n->while_loop.body);
        break;
    case NODE_LOOP:
        WALK_CHILD(n->loop.body);
        break;
    case NODE_CALL:
        WALK_CHILD(n->call.callee);
        for (int j = 0; j < n->call.args.len; j++)
            WALK_CHILD(n->call.args.items[j]);
        break;
    case NODE_METHOD_CALL:
        WALK_CHILD(n->method_call.obj);
        for (int j = 0; j < n->method_call.args.len; j++)
            WALK_CHILD(n->method_call.args.items[j]);
        break;
    case NODE_BINOP:
        WALK_CHILD(n->binop.left);
        WALK_CHILD(n->binop.right);
        break;
    case NODE_UNARY:
        WALK_CHILD(n->unary.expr);
        break;
    case NODE_ASSIGN:
        WALK_CHILD(n->assign.target);
        WALK_CHILD(n->assign.value);
        break;
    case NODE_RETURN:
        if (n->ret.value) WALK_CHILD(n->ret.value);
        break;
    case NODE_EXPR_STMT:
        WALK_CHILD(n->expr_stmt.expr);
        break;
    case NODE_LAMBDA:
        WALK_CHILD(n->lambda.body);
        break;
    case NODE_MATCH:
        WALK_CHILD(n->match.subject);
        for (int j = 0; j < n->match.arms.len; j++)
            WALK_CHILD(n->match.arms.items[j].body);
        break;
    case NODE_TRY:
        WALK_CHILD(n->try_.body);
        for (int j = 0; j < n->try_.catch_arms.len; j++)
            WALK_CHILD(n->try_.catch_arms.items[j].body);
        if (n->try_.finally_block) WALK_CHILD(n->try_.finally_block);
        break;
    case NODE_CLASS_DECL:
        for (int j = 0; j < n->class_decl.members.len; j++)
            WALK_CHILD(n->class_decl.members.items[j]);
        break;
    case NODE_INDEX:
        WALK_CHILD(n->index.obj);
        WALK_CHILD(n->index.index);
        break;
    case NODE_FIELD:
        WALK_CHILD(n->field.obj);
        break;
    default:
        break;
    }
#undef WALK_CHILD
#undef FIRE_SCOPE_EXIT
    return n;
}

int pipeline_run_passes(PluginPipeline *p, Node *program, const char *phase_ref, int is_after, void *interp) {
    if (!p || !program || !interp) return 0;
    Interp *i = (Interp *)interp;
    /* use topologically sorted order when available, fall back to registration order */
    if (p->sorted_passes && p->nsorted == p->npasses) {
        for (int j = 0; j < p->nsorted; j++) {
            CustomPass *pass = p->sorted_passes[j];
            if (!pass->phase_ref) continue;
            if (strcmp(pass->phase_ref, phase_ref) != 0) continue;
            if (pass->is_after != is_after) continue;
            if (pass->nvisitors == 0) continue;
            walk_node_for_passes(i, program, pass);
        }
    } else {
        for (int j = 0; j < p->npasses; j++) {
            CustomPass *pass = &p->passes[j];
            if (!pass->phase_ref) continue;
            if (strcmp(pass->phase_ref, phase_ref) != 0) continue;
            if (pass->is_after != is_after) continue;
            if (pass->nvisitors == 0) continue;
            walk_node_for_passes(i, program, pass);
        }
    }
    return 0;
}

/* comparison for sorting sema rules by priority (lower = earlier) */
static int cmp_sema_rule_priority(const void *a, const void *b) {
    SemaPluginRule *ra = *(SemaPluginRule **)a;
    SemaPluginRule *rb = *(SemaPluginRule **)b;
    return ra->priority - rb->priority;
}

int pipeline_dispatch_sema(PluginPipeline *p, Node *program, void *interp) {
    if (!p || !program || !interp) return 0;
    if (p->nsema_rules == 0) return 0;
    Interp *ip = (Interp *)interp;

    /* group override rules by target and dispatch through chains.
       for each unique target, collect matching rules, sort by priority,
       then run as a chain where default() advances to the next handler. */

    /* first, collect unique targets */
    const char *targets[64];
    int ntargets = 0;
    for (int j = 0; j < p->nsema_rules && ntargets < 64; j++) {
        const char *t = p->sema_rules[j].target;
        int found = 0;
        for (int k = 0; k < ntargets; k++) {
            if (strcmp(targets[k], t) == 0) { found = 1; break; }
        }
        if (!found) targets[ntargets++] = t;
    }

    /* for each target, build a chain of matching rules sorted by priority */
    for (int ti = 0; ti < ntargets; ti++) {
        int tag = node_tag_from_string(targets[ti]);
        const char *tag_str = (tag == -1) ? targets[ti] : NULL; /* for name-based matching */
        SemaPluginRule *chain[64];
        int chain_len = 0;
        for (int j = 0; j < p->nsema_rules && chain_len < 64; j++) {
            if (strcmp(p->sema_rules[j].target, targets[ti]) == 0 &&
                p->sema_rules[j].callback) {
                chain[chain_len++] = &p->sema_rules[j];
            }
        }
        if (chain_len == 0) continue;

        /* sort by priority (lower number = runs first) */
        qsort(chain, chain_len, sizeof(SemaPluginRule *), cmp_sema_rule_priority);

        /* if only one rule (or no overrides), just run as a simple pass */
        if (chain_len == 1) {
            CustomPass sp;
            memset(&sp, 0, sizeof(sp));
            sp.nvisitors = 1;
            sp.visitors = (Value **)&chain[0]->callback;
            sp.visitor_tags = &tag;
            char *sema_name = tag_str ? xs_strdup(tag_str) : NULL;
            sp.visitor_names = &sema_name;
            walk_node_for_passes(ip, program, &sp);
            if (sema_name) free(sema_name);
            continue;
        }

        /* multiple rules: set up the chain for default() chaining */
        /* save previous chain state (for nesting) */
        SemaPluginRule **saved_chain = g_sema_chain;
        int saved_len = g_sema_chain_len;
        int saved_pos = g_sema_chain_pos;

        /* build a pass that only calls the first handler;
           default() will advance through the rest */
        CustomPass sp;
        memset(&sp, 0, sizeof(sp));
        sp.nvisitors = 1;
        Value *first_cb = chain[0]->callback;
        sp.visitors = &first_cb;
        sp.visitor_tags = &tag;
        char *sema_name2 = tag_str ? xs_strdup(tag_str) : NULL;
        sp.visitor_names = &sema_name2;

        SemaPluginRule *chain_heap[64];
        memcpy(chain_heap, chain, chain_len * sizeof(SemaPluginRule *));
        g_sema_chain = chain_heap;
        g_sema_chain_len = chain_len;
        g_sema_chain_pos = 0;
        g_sema_chain_interp = ip;

        walk_node_for_passes(ip, program, &sp);

        /* restore previous chain state */
        g_sema_chain = saved_chain;
        g_sema_chain_len = saved_len;
        g_sema_chain_pos = saved_pos;
        if (sema_name2) free(sema_name2);
    }
    return 0;
}

/* native cancel() function for runtime hooks */
static Value *native_cancel(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    interp->hook_cancelled = 1;
    return value_incref(XS_NULL_VAL);
}

/* native emit_runtime_hook(node_id, kind, target) for annotate passes */
static Value *native_emit_runtime_hook(Interp *interp, Value **args, int argc) {
    if (argc < 3) return value_incref(XS_NULL_VAL);
    PluginPipeline *pp = (PluginPipeline *)interp->pipeline;
    if (!pp) return value_incref(XS_NULL_VAL);
    int node_id = 0;
    if (args[0] && VAL_TAG(args[0]) == XS_INT) node_id = (int)VAL_INT(args[0]);
    const char *kind = (args[1] && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : "";
    const char *target = (args[2] && VAL_TAG(args[2]) == XS_STR) ? args[2]->s : "";
    pipeline_emit_hook(pp, node_id, kind, target, span_zero());
    return value_incref(XS_NULL_VAL);
}

/* native default(node) for sema override chaining */
static Value *native_default_handler(Interp *interp, Value **args, int argc) {
    /* advance the override chain: call the next handler if one exists */
    if (g_sema_chain && g_sema_chain_pos + 1 < g_sema_chain_len) {
        g_sema_chain_pos++;
        SemaPluginRule *next = g_sema_chain[g_sema_chain_pos];
        if (next->callback)
            return call_value(interp, next->callback, args, argc, "sema_default");
    }
    /* no more handlers in the chain, fall through to built-in */
    (void)interp; (void)args; (void)argc;
    return value_incref(XS_NULL_VAL);
}

/* native __tracer_active() - check if tracer is recording */
static Value *native_tracer_active(Interp *interp, Value **args, int argc) {
    (void)args; (void)argc;
    return xs_bool(interp->tracer != NULL);
}

void interp_setup_tracer_suppress(Interp *i) {
#ifdef XSC_ENABLE_TRACER
    if (i && i->tracer)
        tracer_set_suppress_flag((XSTracer*)i->tracer, &g_in_eval_hook);
#else
    (void)i;
#endif
}

/* native __tracer_write_prov(var_name, json_string) - write rich provenance to trace */
static Value *native_tracer_write_prov(Interp *interp, Value **args, int argc) {
#ifdef XSC_ENABLE_TRACER
    if (!interp->tracer || argc < 2) return xs_null();
    const char *var = (args[0] && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : "?";
    const char *json = (args[1] && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : "{}";
    /* force-write bypasses suppression so explicit plugin provenance goes through */
    tracer_record_provenance_force((XSTracer*)interp->tracer, var, "plugin", json, 0);
#else
    (void)interp; (void)args; (void)argc;
#endif
    return xs_null();
}

/* apply load...with renames */
static const char *apply_load_rename(const char *orig) {
    for (int j = 0; j < g_load_nrenames; j++) {
        if (g_load_rename_keys && g_load_rename_vals &&
            strcmp(g_load_rename_keys[j], orig) == 0) {
            return g_load_rename_vals[j];
        }
    }
    return orig;
}

/* end plugin system */

void interp_exec(Interp *i, Node *stmt) {
    if (!stmt || i->cf.signal) return;
    /* Per-statement resource-limit tick. The interpreter does more work
       per tick than the VM, so one tick per statement keeps budgets
       roughly comparable when the same program runs on both backends. */
    if (xs_limits_tick()) {
        xs_runtime_error(span_zero(), "ResourceLimit", NULL,
                         "%s exceeded", xs_limits_exceeded_name());
        if (i->cf.signal) return;
    }
    i->current_span = stmt->span;
    if (i->coverage && stmt->span.line > 0)
        coverage_record_line(i->coverage, stmt->span.line);

    /* fire before_eval hooks for statement nodes so plugins can track them */
    if (g_has_eval_hooks && g_n_before_eval > 0 && !g_in_eval_hook &&
        (VAL_TAG(stmt) == NODE_LET || VAL_TAG(stmt) == NODE_VAR ||
         VAL_TAG(stmt) == NODE_RETURN || VAL_TAG(stmt) == NODE_THROW)) {
        g_in_eval_hook = 1;
        for (int _h = 0; _h < g_n_before_eval; _h++) {
            EvalHook *hook = &g_before_eval[_h];
            if (!hook->callback) continue;
            if (hook->tag_filter >= 0 && hook->tag_filter != (int)VAL_TAG(stmt)) continue;
            Value *node_map = node_to_xs_map(stmt);
            Value *args[1] = { node_map };
            Value *result = call_value(i, hook->callback, args, 1, "before_eval");
            value_decref(node_map);
            if (result) value_decref(result);
            if (i->cf.signal) { g_in_eval_hook = 0; return; }
        }
        g_in_eval_hook = 0;
    }

    switch (VAL_TAG(stmt)) {
    case NODE_EXPR_STMT: {
        Value *v = EVAL(i, stmt->expr_stmt.expr);
        if (i->last_expr_value) value_decref(i->last_expr_value);
        i->last_expr_value = v ? value_incref(v) : NULL;
        value_decref(v);
        break;
    }

    case NODE_LET:
    case NODE_VAR: {
        Value *val = stmt->let.value ? EVAL(i, stmt->let.value) : value_incref(XS_NULL_VAL);
        if (stmt->let.type_ann) {
            if (!value_matches_typeexpr(val, stmt->let.type_ann)) {
                xs_runtime_error(stmt->span, "type mismatch", NULL,
                    "expected '%s', got '%s'",
                    typeexpr_str(stmt->let.type_ann), value_type_str(val));
                i->cf.signal = CF_PANIC;
                i->cf.value = xs_str("type error");
            }
        }
        /* check where contract before defining */
        if (stmt->let.contract && !i->cf.signal) {
            Env *cenv = env_new(i->env);
            env_define(cenv, stmt->let.name ? stmt->let.name : "_", val, 0);
            Env *saved = i->env;
            i->env = cenv;
            Value *check = EVAL(i, stmt->let.contract);
            i->env = saved;
            int passed = value_truthy(check);
            value_decref(check);
            env_decref(cenv);
            if (!passed) {
                char *vs = value_repr(val);
                char msg[512];
                snprintf(msg, sizeof msg,
                    "contract violation: value %s does not satisfy 'where' constraint for '%s'",
                    vs, stmt->let.name ? stmt->let.name : "<pattern>");
                free(vs);
                i->cf.signal = CF_THROW;
                i->cf.value = xs_str(msg);
            }
        }
        int mutable = (VAL_TAG(stmt) == NODE_VAR) || stmt->let.mutable;
        /* @scoped values opt out of cycle detection: sema has already
         * verified they don't escape, so refcount alone suffices and
         * env_decref at block exit will free them deterministically. */
        if (stmt->let.is_scoped && val) gc_untrack(val);
        if (stmt->let.name) {
            env_define(i->env, stmt->let.name, val, mutable);
            TRACE_STORE(i, stmt->let.name, val);
#ifdef XSC_ENABLE_TRACER
            trace_provenance_for_node(i, stmt->let.name, stmt->let.value);
#endif
            /* pipeline constraint handling: when __pipeline_to_N is defined,
               look up matching __pipeline_from_N and register constraint */
            if (i->pipeline && stmt->let.name &&
                strncmp(stmt->let.name, "__pipeline_to_", 14) == 0) {
                const char *idx_str = stmt->let.name + 14;
                char from_name[64];
                snprintf(from_name, sizeof(from_name), "__pipeline_from_%s", idx_str);
                Value *from_val = env_get(i->env, from_name);
                if (from_val && VAL_TAG(from_val) == XS_STR &&
                    val && VAL_TAG(val) == XS_STR) {
                    pipeline_add_constraint(
                        (PluginPipeline *)i->pipeline, from_val->s, val->s);
                }
            }
        } else if (stmt->let.pattern) {
            bind_pattern(i, stmt->let.pattern, val, i->env, mutable);
        }
        /* after_eval hooks for let/var nodes */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook && !i->cf.signal) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != (int)VAL_TAG(stmt)) continue;
                Value *node_map = node_to_xs_map(stmt);
                Value *hargs[2] = { node_map, val };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
                if (i->cf.signal) { g_in_eval_hook = 0; break; }
            }
            g_in_eval_hook = 0;
        }
        value_decref(val);
        break;
    }

    case NODE_CONST: {
        Value *val = EVAL(i, stmt->const_.value);
        if (stmt->const_.type_ann) {
            if (!value_matches_typeexpr(val, stmt->const_.type_ann)) {
                xs_runtime_error(stmt->span, "type mismatch", NULL,
                    "expected '%s', got '%s'",
                    typeexpr_str(stmt->const_.type_ann), value_type_str(val));
                i->cf.signal = CF_PANIC;
                i->cf.value = xs_str("type error");
            }
        }
        /* check where contract before defining */
        if (stmt->const_.contract && !i->cf.signal) {
            Env *cenv = env_new(i->env);
            env_define(cenv, stmt->const_.name, val, 0);
            Env *saved = i->env;
            i->env = cenv;
            Value *check = EVAL(i, stmt->const_.contract);
            i->env = saved;
            int passed = value_truthy(check);
            value_decref(check);
            env_decref(cenv);
            if (!passed) {
                char *vs = value_repr(val);
                char msg[512];
                snprintf(msg, sizeof msg,
                    "contract violation: value %s does not satisfy 'where' constraint for '%s'",
                    vs, stmt->const_.name);
                free(vs);
                i->cf.signal = CF_THROW;
                i->cf.value = xs_str(msg);
            }
        }
        env_define(i->env, stmt->const_.name, val, 0);
        value_decref(val);
        break;
    }

    case NODE_FN_DECL: {
        if (!stmt->fn_decl.body) break;
        int nparams = stmt->fn_decl.params.len;
        Node **params = nparams ? xs_malloc(nparams * sizeof(Node*)) : NULL;
        Node **defaults = nparams ? xs_calloc(nparams, sizeof(Node*)) : NULL;
        int  *varflags  = nparams ? xs_calloc(nparams, sizeof(int)) : NULL;
        for (int j = 0; j < nparams; j++) {
            Param *pm = &stmt->fn_decl.params.items[j];
            if (pm->pattern) {
                params[j] = pm->pattern;
            } else {
                Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                pn->pat_ident.name    = xs_strdup(pm->name ? pm->name : "_");
                pn->pat_ident.mutable = 0;
                params[j] = pn;
            }
            defaults[j]  = pm->default_val;
            varflags[j]   = pm->variadic;
        }
        XSFunc *fn = func_new_ex(stmt->fn_decl.name, params, nparams,
                               stmt->fn_decl.body, i->env, defaults, varflags);
        fn->is_generator = stmt->fn_decl.is_generator;
        fn->is_async     = stmt->fn_decl.is_async;
        if (stmt->fn_decl.deprecated_msg)
            fn->deprecated_msg = xs_strdup(stmt->fn_decl.deprecated_msg);
        if (nparams > 0) {
            fn->param_type_names = xs_calloc(nparams, sizeof(char*));
            int has_contracts = 0;
            for (int j = 0; j < nparams; j++) {
                Param *pm = &stmt->fn_decl.params.items[j];
                if (pm->type_ann && pm->type_ann->name)
                    fn->param_type_names[j] = xs_strdup(pm->type_ann->name);
                if (pm->contract) has_contracts = 1;
            }
            if (has_contracts) {
                fn->param_contracts = xs_calloc(nparams, sizeof(Node*));
                for (int j = 0; j < nparams; j++) {
                    fn->param_contracts[j] = stmt->fn_decl.params.items[j].contract;
                }
            }
        }
        if (stmt->fn_decl.ret_type && stmt->fn_decl.ret_type->name)
            fn->ret_type_name = xs_strdup(stmt->fn_decl.ret_type->name);
        Value *v = xs_func_new(fn);
        if (stmt->fn_decl.name) {
            Value *existing = env_get(i->env, stmt->fn_decl.name);
            /* hoist_functions already bound this exact decl at top
               level; running the stmt path would wrap it in a single-
               element overload set with itself. Skip when we recognise
               the already-hoisted shape (existing fn whose body and
               name match this stmt). */
            int already_hoisted = 0;
            if (existing && VAL_TAG(existing) == XS_FUNC && existing->fn &&
                existing->fn->body == stmt->fn_decl.body &&
                existing->fn->name && stmt->fn_decl.name &&
                strcmp(existing->fn->name, stmt->fn_decl.name) == 0) {
                already_hoisted = 1;
            }
            /* If hoist wrapped the fn in a @memoize / @retry / @trace /
               @timed dispatcher, the binding is now an XS_MAP wrapper
               carrying the same name; running the stmt path here would
               clobber that wrapper with the raw fn again. */
            if (existing && VAL_TAG(existing) == XS_MAP && existing->map) {
                Value *wk = map_get(existing->map, "_wrap_kind");
                Value *wn = map_get(existing->map, "_wrap_name");
                if (wk && VAL_TAG(wk) == XS_STR &&
                    wn && VAL_TAG(wn) == XS_STR &&
                    stmt->fn_decl.name &&
                    strcmp(wn->s, stmt->fn_decl.name) == 0) {
                    already_hoisted = 1;
                }
            }
            if (already_hoisted) {
                /* nothing to do -- hoist already did it */
            } else if (existing && VAL_TAG(existing) == XS_OVERLOAD) {
                array_push(existing->overload, value_incref(v));
            } else if (existing && (VAL_TAG(existing) == XS_FUNC || VAL_TAG(existing) == XS_NATIVE)) {
                Value *oset = xs_overload_new();
                array_push(oset->overload, value_incref(existing));
                array_push(oset->overload, value_incref(v));
                env_set(i->env, stmt->fn_decl.name, oset);
                value_decref(oset);
            } else {
                env_define(i->env, stmt->fn_decl.name, v, 1);
            }
        }
        /* hoist_functions already registered top-level decorators; this
           path is for nested fn_decls and locally-bound fns where
           hoisting doesn't run. */
        value_decref(v);
        break;
    }

    case NODE_CLASS_DECL: {
        XSClass *cls = xs_calloc(1, sizeof(XSClass));
        cls->name           = xs_strdup(stmt->class_decl.name);
        cls->fields         = map_new();
        cls->methods        = map_new();
        cls->static_methods = map_new();
        cls->refcount       = 1;

        int nbases = stmt->class_decl.nbases;
        cls->nbases = nbases;
        cls->bases  = nbases ? xs_malloc(nbases * sizeof(XSClass*)) : NULL;
        for (int j = 0; j < nbases; j++) {
            Value *base_val = env_get(i->env, stmt->class_decl.bases[j]);
            if (base_val && VAL_TAG(base_val) == XS_CLASS_VAL) {
                cls->bases[j] = base_val->cls;
            } else {
                const char *base_sugg = find_similar_name(i->env, stmt->class_decl.bases[j]);
                if (base_sugg)
                    fprintf(stderr, "xs: error at %s:%d:%d: base class '%s' not found for class '%s' (did you mean '%s'?)\n",
                            stmt->span.file ? stmt->span.file : "<unknown>",
                            stmt->span.line, stmt->span.col,
                            stmt->class_decl.bases[j], stmt->class_decl.name, base_sugg);
                else
                    fprintf(stderr, "xs: error at %s:%d:%d: base class '%s' not found for class '%s'\n",
                            stmt->span.file ? stmt->span.file : "<unknown>",
                            stmt->span.line, stmt->span.col,
                            stmt->class_decl.bases[j], stmt->class_decl.name);
                cls->bases[j] = NULL;
            }
        }

        for (int j = 0; j < nbases; j++) {
            XSClass *base = cls->bases[j];
            if (!base) continue;
            if (base->fields) {
                int nk = 0; char **ks = map_keys(base->fields, &nk);
                for (int k = 0; k < nk; k++) {
                    Value *fv = map_get(base->fields, ks[k]);
                    if (fv) map_set(cls->fields, ks[k], fv);
                    free(ks[k]);
                }
                free(ks);
            }
            if (base->methods) {
                int nk = 0; char **ks = map_keys(base->methods, &nk);
                for (int k = 0; k < nk; k++) {
                    Value *mv = map_get(base->methods, ks[k]);
                    if (mv) map_set(cls->methods, ks[k], mv);
                    free(ks[k]);
                }
                free(ks);
            }
            if (base->static_methods) {
                int nk = 0; char **ks = map_keys(base->static_methods, &nk);
                for (int k = 0; k < nk; k++) {
                    Value *mv = map_get(base->static_methods, ks[k]);
                    if (mv) map_set(cls->static_methods, ks[k], mv);
                    free(ks[k]);
                }
                free(ks);
            }
        }

        for (int j = 0; j < stmt->class_decl.members.len; j++) {
            Node *mem = stmt->class_decl.members.items[j];
            if (VAL_TAG(mem) == NODE_FN_DECL) {
                int np = mem->fn_decl.params.len;
                Node **params = np ? xs_malloc(np*sizeof(Node*)) : NULL;
                for (int k=0;k<np;k++) {
                    Param *pm=&mem->fn_decl.params.items[k];
                    params[k] = pm->pattern ? pm->pattern :
                        ({ Node *pn=node_new(NODE_PAT_IDENT,pm->span);
                           pn->pat_ident.name=xs_strdup(pm->name?pm->name:"_");
                           pn->pat_ident.mutable=0; pn; });
                }
                XSFunc *fn = func_new(mem->fn_decl.name, params, np,
                                       mem->fn_decl.body, i->env);
                Value *fv = xs_func_new(fn);
                if (mem->fn_decl.name) {
                    if (mem->fn_decl.is_static)
                        map_set(cls->static_methods, mem->fn_decl.name, fv);
                    else
                        map_set(cls->methods, mem->fn_decl.name, fv);
                }
                else value_decref(fv);
            } else if (VAL_TAG(mem) == NODE_LET || VAL_TAG(mem) == NODE_VAR) {
                Value *def = mem->let.value ? EVAL(i, mem->let.value) : value_incref(XS_NULL_VAL);
                if (mem->let.name) map_set(cls->fields, mem->let.name, def);
                else value_decref(def);
            }
        }

        Value *cls_val = xs_calloc(1, sizeof(Value));
        cls_val->tag = XS_CLASS_VAL; cls_val->refcount = 1;
        cls_val->cls = cls;
        env_define(i->env, stmt->class_decl.name, cls_val, 1);
        value_decref(cls_val);
        break;
    }

    case NODE_ACTOR_DECL: {
        XSActor *actor = xs_calloc(1, sizeof(XSActor));
        actor->name     = xs_strdup(stmt->actor_decl.name);
        actor->state    = map_new();
        actor->methods  = map_new();
        actor->refcount = 1;

        /* Evaluate default state field values */
        for (int j = 0; j < stmt->actor_decl.state_fields.len; j++) {
            char *fname = stmt->actor_decl.state_fields.items[j].key;
            Node *def   = stmt->actor_decl.state_fields.items[j].val;
            Value *dv   = def ? EVAL(i, def) : value_incref(XS_NULL_VAL);
            map_set(actor->state, fname, dv);
            value_decref(dv);
        }

        /* Process methods: find handle and other methods */
        for (int j = 0; j < stmt->actor_decl.methods.len; j++) {
            Node *m = stmt->actor_decl.methods.items[j];
            if (VAL_TAG(m) != NODE_FN_DECL) continue;
            int np = m->fn_decl.params.len;
            Node **params = np ? xs_malloc(np * sizeof(Node*)) : NULL;
            for (int k = 0; k < np; k++) {
                Param *pm = &m->fn_decl.params.items[k];
                if (pm->pattern) {
                    params[k] = pm->pattern;
                } else {
                    Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                    pn->pat_ident.name    = xs_strdup(pm->name ? pm->name : "_");
                    pn->pat_ident.mutable = 0;
                    params[k] = pn;
                }
            }
            XSFunc *fn = func_new(m->fn_decl.name, params, np, m->fn_decl.body, i->env);
            if (m->fn_decl.name && strcmp(m->fn_decl.name, "handle") == 0) {
                actor->handle_fn = fn;
                fn->refcount++;
            }
            /* Store all methods (including handle) in methods map */
            if (m->fn_decl.name) {
                Value *fv = xs_func_new(fn);
                map_set(actor->methods, m->fn_decl.name, fv);
                value_decref(fv);
            }
        }

        actor->closure = env_incref(i->env);

        Value *actor_val = xs_calloc(1, sizeof(Value));
        actor_val->tag = XS_ACTOR;
        actor_val->refcount = 1;
        actor_val->actor    = actor;
        env_define(i->env, stmt->actor_decl.name, actor_val, 1);
        value_decref(actor_val);
        break;
    }

    case NODE_STRUCT_DECL: {
        XSClass *cls = xs_calloc(1, sizeof(XSClass));
        cls->name           = xs_strdup(stmt->struct_decl.name);
        cls->fields         = map_new();
        cls->methods        = map_new();
        cls->static_methods = map_new();
        cls->refcount       = 1;
        for (int j=0;j<stmt->struct_decl.fields.len;j++) {
            char *fn2 = stmt->struct_decl.fields.items[j].key;
            Value *def = stmt->struct_decl.fields.items[j].val ?
                         EVAL(i, stmt->struct_decl.fields.items[j].val) :
                         value_incref(XS_NULL_VAL);
            if (fn2) map_set(cls->fields, fn2, def);
            else value_decref(def);
        }
        for (int j = 0; j < stmt->struct_decl.n_derives; j++) {
            const char *tname = stmt->struct_decl.derives[j];
            int idx = cls->ntraits++;
            cls->traits = xs_realloc(cls->traits, sizeof(char*) * cls->ntraits);
            cls->traits[idx] = xs_strdup(tname);
            if (strcmp(tname, "Debug") == 0) {
                Value *dbg_fn = xs_native(builtin_debug_to_string);
                map_set(cls->methods, "to_string", dbg_fn);
                map_set(cls->methods, "debug", value_incref(dbg_fn));
            } else if (strcmp(tname, "Clone") == 0) {
                Value *clone_fn = xs_native(builtin_clone);
                map_set(cls->methods, "clone", clone_fn);
            } else if (strcmp(tname, "PartialEq") == 0 || strcmp(tname, "Eq") == 0) {
                Value *eq_fn = xs_native(builtin_struct_eq);
                map_set(cls->methods, "eq", eq_fn);
            }
        }
        Value *cls_val = xs_calloc(1, sizeof(Value));
        cls_val->tag = XS_CLASS_VAL; cls_val->refcount = 1;
        cls_val->cls = cls;
        env_define(i->env, stmt->struct_decl.name, cls_val, 1);
        value_decref(cls_val);
        break;
    }

    case NODE_ENUM_DECL: {
        XSMap *enum_map = map_new();

        for (int j = 0; j < stmt->enum_decl.variants.len; j++) {
            EnumVariant *v = &stmt->enum_decl.variants.items[j];
            if (v->fields.len == 0) {
                Value *vv = xs_str(v->name);
                XSEnum *en = xs_calloc(1, sizeof(XSEnum));
                en->type_name = xs_strdup(stmt->enum_decl.name);
                en->variant   = xs_strdup(v->name);
                en->refcount  = 1;
                Value *ev = xs_calloc(1, sizeof(Value));
                ev->tag = XS_ENUM_VAL; ev->refcount = 1; ev->en = en;
                map_set(enum_map, v->name, ev);
                value_decref(vv);
            } else {
                Value *ctor = make_enum_ctor_native(stmt->enum_decl.name, v->name);
                map_set(enum_map, v->name, ctor);
            }
        }

        Value *mod = xs_module(enum_map);
        env_define(i->env, stmt->enum_decl.name, mod, 1);
        value_decref(mod);
        break;
    }

    case NODE_IMPL_DECL: {
        Value *cls_val = env_get(i->env, stmt->impl_decl.type_name);
        XSClass *cls = NULL;
        if (cls_val && VAL_TAG(cls_val) == XS_CLASS_VAL) {
            cls = cls_val->cls;
        }
        if (cls && stmt->impl_decl.trait_name) {
            int idx = cls->ntraits++;
            cls->traits = xs_realloc(cls->traits, sizeof(char*) * cls->ntraits);
            cls->traits[idx] = xs_strdup(stmt->impl_decl.trait_name);
            Value *trait_val = env_get(i->env, stmt->impl_decl.trait_name);
            if (trait_val && VAL_TAG(trait_val) == XS_MAP) {
                Value *defaults = map_get(trait_val->map, "__defaults__");
                if (defaults && VAL_TAG(defaults) == XS_MAP) {
                    int nk = 0; char **ks = map_keys(defaults->map, &nk);
                    for (int j = 0; j < nk; j++) {
                        int found = 0;
                        for (int m = 0; m < stmt->impl_decl.members.len; m++) {
                            Node *mem = stmt->impl_decl.members.items[m];
                            if (VAL_TAG(mem) == NODE_FN_DECL && mem->fn_decl.name &&
                                strcmp(mem->fn_decl.name, ks[j]) == 0) { found = 1; break; }
                        }
                        if (!found) {
                            Value *dfn = map_get(defaults->map, ks[j]);
                            if (dfn && VAL_TAG(dfn) == XS_FUNC) {
                                map_set(cls->methods, ks[j], value_incref(dfn));
                            }
                        }
                        free(ks[j]);
                    }
                    free(ks);
                }
            }
        }
        XSMap *enum_impl = NULL;
        if (cls_val && (VAL_TAG(cls_val) == XS_MODULE || VAL_TAG(cls_val) == XS_MAP)) {
            Value *impl_val = map_get(cls_val->map, "__impl__");
            if (!impl_val) {
                Value *new_impl = xs_map_new();
                map_set(cls_val->map, "__impl__", new_impl);
                enum_impl = new_impl->map;
                value_decref(new_impl);
            } else {
                enum_impl = impl_val->map;
            }
        }
        for (int j = 0; j < stmt->impl_decl.members.len; j++) {
            Node *mem = stmt->impl_decl.members.items[j];
            if (VAL_TAG(mem) == NODE_FN_DECL && mem->fn_decl.body) {
                int np = mem->fn_decl.params.len;
                Node **params = np ? xs_malloc(np*sizeof(Node*)) : NULL;
                for (int k=0;k<np;k++) {
                    Param *pm=&mem->fn_decl.params.items[k];
                    params[k] = pm->pattern ? pm->pattern :
                        ({ Node *pn=node_new(NODE_PAT_IDENT,pm->span);
                           pn->pat_ident.name=xs_strdup(pm->name?pm->name:"_");
                           pn->pat_ident.mutable=0; pn; });
                }
                XSFunc *fn = func_new(mem->fn_decl.name, params, np,
                                       mem->fn_decl.body, i->env);
                Value *fv = xs_func_new(fn);
                if (cls && mem->fn_decl.name)
                    map_set(cls->methods, mem->fn_decl.name, value_incref(fv));
                if (enum_impl && mem->fn_decl.name)
                    map_set(enum_impl, mem->fn_decl.name, value_incref(fv));
                if (mem->fn_decl.name)
                    env_define(i->env, mem->fn_decl.name, fv, 1);
                value_decref(fv);
            }
        }
        break;
    }

    case NODE_IMPORT: {
        if (stmt->import.nparts == 0) break;
        char *modname = stmt->import.path[0];
        Value *mod = env_get(i->env, modname);
        /* If the existing binding is not a module/map, a builtin function
           is shadowing a stdlib module of the same name. Prefer the module
           when an explicit import is used. */
        if (mod && VAL_TAG(mod) != XS_MAP && VAL_TAG(mod) != XS_MODULE)
            mod = NULL;

        /* phase 3: resolve_import hook chain */
        if (!mod && g_n_resolve_import > 0) {
            for (int _ri = g_n_resolve_import - 1; _ri >= 0; _ri--) {
                if (!g_resolve_import_hooks[_ri]) continue;
                /* build a "previous" function that either chains to the next
                   hook or does the default module lookup */
                Value *name_arg = xs_str(modname);
                /* for the previous fn, use a native that does the default lookup */
                Value *prev_fn = NULL;
                /* find the next active hook or use default */
                int found_prev = 0;
                for (int _p = _ri - 1; _p >= 0; _p--) {
                    if (g_resolve_import_hooks[_p]) {
                        prev_fn = value_incref(g_resolve_import_hooks[_p]);
                        found_prev = 1;
                        break;
                    }
                }
                if (!found_prev) {
                    /* the "previous" when no more hooks just returns null:
                       the caller can check and fall through to default */
                    prev_fn = value_incref(XS_NULL_VAL);
                }
                Value *hook_args[2] = { name_arg, prev_fn };
                Value *result = call_value(i, g_resolve_import_hooks[_ri],
                                           hook_args, 2, "resolve_import");
                value_decref(name_arg);
                if (prev_fn) value_decref(prev_fn);
                if (result && VAL_TAG(result) != XS_NULL &&
                    (VAL_TAG(result) == XS_MAP || VAL_TAG(result) == XS_MODULE)) {
                    mod = result;
                    env_define(i->globals, modname, mod, 1);
                    value_decref(mod);
                    mod = env_get(i->env, modname);
                    break;
                }
                if (result) value_decref(result);
                if (i->cf.signal) break;
            }
        }

        if (!mod) {
            mod = stdlib_load_module(i, modname);
            if (mod) {
                env_define(i->globals, modname, mod, 1);
                value_decref(mod);
                mod = env_get(i->env, modname);
            }
        }
        if (!mod) {
            mod = try_load_xs_module(i, modname);
            if (mod) {
                env_define(i->globals, modname, mod, 1);
                value_decref(mod);
                mod = env_get(i->env, modname);
            }
        }
        if (mod) {
            if (stmt->import.nitems > 0) {
                for (int j=0;j<stmt->import.nitems;j++) {
                    Value *item = NULL;
                    if (VAL_TAG(mod)==XS_MODULE||VAL_TAG(mod)==XS_MAP)
                        item = map_get(mod->map, stmt->import.items[j]);
                    if (item) env_define(i->env, stmt->import.items[j], item, 1);
                }
            } else if (stmt->import.alias) {
                env_define(i->env, stmt->import.alias, mod, 1);
            } else {
                env_define(i->env, modname, mod, 1);
            }
        }
        break;
    }

    case NODE_USE: {
        char resolved[PATH_MAX];
        const char *use_path = stmt->use_.path;
        struct stat st2;

        /* Bare-name `use "X"` (no path separators, no .xs extension)
           resolves through the same fallback chain as `import X`:
           built-in module registry first, then disk, then package dir.
           Without this, native modules like http only worked under
           `import` -- `use "http"` would fail to find http.xs on disk
           even though make_http_module had populated the global at
           startup. Path-shaped strings (`./foo`, `foo.xs`, `a/b`) skip
           the registry to avoid a name in globals shadowing the file
           the author clearly meant. */
        if (!stmt->use_.is_plugin
            && !strchr(use_path, '/')
            && !strstr(use_path, ".xs")) {
            Value *registered = env_get(i->globals, use_path);
            if (registered &&
                (VAL_TAG(registered) == XS_MODULE ||
                 VAL_TAG(registered) == XS_MAP)) {
                /* bind it under the same name (or alias) the user asked for */
                const char *bind_as = stmt->use_.alias ? stmt->use_.alias : use_path;
                env_define(i->env, bind_as, registered, 1);
                if (stmt->use_.import_all && registered->map) {
                    int nk = 0;
                    char **keys = map_keys(registered->map, &nk);
                    for (int j = 0; j < nk; j++) {
                        Value *v = map_get(registered->map, keys[j]);
                        if (v) env_define(i->env, keys[j], v, 1);
                        free(keys[j]);
                    }
                    free(keys);
                }
                break;
            }
        }

        /* resolve relative to current file's directory */
        if (use_path[0] != '/') {
            const char *fn = i->filename ? i->filename : "";
            const char *last_slash = strrchr(fn, '/');
            if (last_slash) {
                int dirlen = (int)(last_slash - fn);
                snprintf(resolved, sizeof(resolved), "%.*s/%s", dirlen, fn, use_path);
            } else {
                snprintf(resolved, sizeof(resolved), "%s", use_path);
            }
        } else {
            snprintf(resolved, sizeof(resolved), "%s", use_path);
        }

        if (stmt->use_.is_plugin) {
            /* `use plugin "path"` is deprecated in favor of the `load "path"`
               syntax from the programmable-pipeline spec. Both still work,
               but `load` is the canonical form going forward. */
            fprintf(stderr, "warning: 'use plugin' is deprecated, use 'load' instead (at %s:%d)\n",
                    stmt->span.file ? stmt->span.file : "<unknown>", stmt->span.line);
            struct stat pst;
            /* if file doesn't exist, try .xs_lib/<name> and xs_lib/<name> */
            if (stat(resolved, &pst) != 0) {
                char lib_try[PATH_MAX];
                static const char *pdirs[] = { ".xs_lib", "xs_lib", NULL };
                static const char *pentries[] = { "plugin.xs", "main.xs", "lib.xs", NULL };
                int found = 0;
                for (int pd = 0; pdirs[pd] && !found; pd++) {
                    for (int pe = 0; pentries[pe] && !found; pe++) {
                        snprintf(lib_try, sizeof(lib_try), "%s/%s/%s", pdirs[pd], use_path, pentries[pe]);
                        if (stat(lib_try, &pst) == 0) {
                            snprintf(resolved, sizeof(resolved), "%s", lib_try);
                            found = 1;
                        }
                    }
                }
            }
            exec_plugin_load(i, stmt, resolved);
            break;
        }

        /* directory import: look for mod.xs or index.xs inside */
        size_t rlen = strlen(resolved);
        if (rlen > 0 && resolved[rlen - 1] == '/') {
            char dir_try[PATH_MAX];
            snprintf(dir_try, sizeof(dir_try), "%.*smod.xs", (int)(sizeof(dir_try)-8), resolved);
            if (stat(dir_try, &st2) == 0) {
                snprintf(resolved, sizeof(resolved), "%s", dir_try);
            } else {
                snprintf(dir_try, sizeof(dir_try), "%.*sindex.xs", (int)(sizeof(dir_try)-10), resolved);
                if (stat(dir_try, &st2) == 0)
                    snprintf(resolved, sizeof(resolved), "%s", dir_try);
            }
        } else if (stat(resolved, &st2) == 0 && S_ISDIR(st2.st_mode)) {
            char dir_try[PATH_MAX];
            snprintf(dir_try, sizeof(dir_try), "%.*s/mod.xs", (int)(sizeof(dir_try)-9), resolved);
            if (stat(dir_try, &st2) == 0) {
                snprintf(resolved, sizeof(resolved), "%s", dir_try);
            } else {
                snprintf(dir_try, sizeof(dir_try), "%.*s/index.xs", (int)(sizeof(dir_try)-11), resolved);
                if (stat(dir_try, &st2) == 0)
                    snprintf(resolved, sizeof(resolved), "%s", dir_try);
            }
        }

        Value *mod = load_xs_module_file(i, resolved);
        /* fallback: try .xs_lib/ and xs_lib/ as package directories */
        if (!mod) {
            const char *bare = use_path;
            /* strip .xs extension if present */
            char bare_buf[PATH_MAX];
            size_t blen = strlen(bare);
            if (blen > 3 && strcmp(bare + blen - 3, ".xs") == 0) {
                snprintf(bare_buf, sizeof(bare_buf), "%.*s", (int)(blen - 3), bare);
                bare = bare_buf;
            }
            mod = try_load_xs_module(i, bare);
        }
        if (!mod) {
            xs_runtime_error(stmt->span, "failed to load module", NULL,
                "could not load '%s'", resolved);
            i->cf.signal = CF_PANIC;
            i->cf.value = xs_str("module load error");
            break;
        }

        if (stmt->use_.import_all) {
            env_define(i->env, stmt->use_.alias, mod, 1);
        } else {
            for (int j = 0; j < stmt->use_.nnames; j++) {
                Value *item = NULL;
                if (VAL_TAG(mod) == XS_MODULE || VAL_TAG(mod) == XS_MAP)
                    item = map_get(mod->map, stmt->use_.names[j]);
                if (item)
                    env_define(i->env, stmt->use_.name_aliases[j], item, 1);
            }
        }
        value_decref(mod);
        break;
    }

    case NODE_LOAD: {
        exec_load_stmt(i, stmt);
        break;
    }

    case NODE_PLUGIN_DECL: {
        PluginPipeline *pp = (PluginPipeline *)i->pipeline;
        if (!pp) break;

        const char *plugin_id = stmt->plugin_decl.name ? stmt->plugin_decl.name : "";

        /* register meta */
        if (stmt->plugin_decl.meta && VAL_TAG(stmt->plugin_decl.meta) == NODE_LIT_MAP) {
            Node *meta = stmt->plugin_decl.meta;
            PluginMeta pm;
            memset(&pm, 0, sizeof(pm));
            pm.id = xs_strdup(plugin_id);
            pm.priority = 50;
            for (int j = 0; j < meta->lit_map.keys.len; j++) {
                Node *k = meta->lit_map.keys.items[j];
                Node *v = meta->lit_map.vals.items[j];
                if (!k || VAL_TAG(k) != NODE_LIT_STRING || !k->lit_string.sval) continue;
                const char *key = k->lit_string.sval;
                if (strcmp(key, "version") == 0 && v && VAL_TAG(v) == NODE_LIT_STRING)
                    pm.version = xs_strdup(v->lit_string.sval ? v->lit_string.sval : "");
                else if (strcmp(key, "id") == 0 && v && VAL_TAG(v) == NODE_LIT_STRING) {
                    free(pm.id);
                    pm.id = xs_strdup(v->lit_string.sval ? v->lit_string.sval : "");
                }
                else if (strcmp(key, "priority") == 0 && v && VAL_TAG(v) == NODE_LIT_INT)
                    pm.priority = (int)v->lit_int.ival;
                else if (strcmp(key, "depends_on") == 0 && v && VAL_TAG(v) == NODE_LIT_ARRAY) {
                    pm.ndepends = v->lit_array.elems.len;
                    pm.depends_on = malloc(pm.ndepends * sizeof(char *));
                    for (int d = 0; d < pm.ndepends; d++) {
                        Node *e = v->lit_array.elems.items[d];
                        pm.depends_on[d] = xs_strdup(
                            (e && VAL_TAG(e) == NODE_LIT_STRING && e->lit_string.sval)
                            ? e->lit_string.sval : "");
                    }
                }
                else if (strcmp(key, "provides") == 0 && v && VAL_TAG(v) == NODE_LIT_ARRAY) {
                    pm.nprovides = v->lit_array.elems.len;
                    pm.provides = malloc(pm.nprovides * sizeof(char *));
                    for (int d = 0; d < pm.nprovides; d++) {
                        Node *e = v->lit_array.elems.items[d];
                        const char *prov = (e && VAL_TAG(e) == NODE_LIT_STRING && e->lit_string.sval)
                            ? e->lit_string.sval : "";
                        pm.provides[d] = xs_strdup(apply_load_rename(prov));
                    }
                }
                else if (strcmp(key, "conflicts_with") == 0 && v && VAL_TAG(v) == NODE_LIT_ARRAY) {
                    pm.nconflicts = v->lit_array.elems.len;
                    pm.conflicts_with = malloc(pm.nconflicts * sizeof(char *));
                    for (int d = 0; d < pm.nconflicts; d++) {
                        Node *e = v->lit_array.elems.items[d];
                        pm.conflicts_with[d] = xs_strdup(
                            (e && VAL_TAG(e) == NODE_LIT_STRING && e->lit_string.sval)
                            ? e->lit_string.sval : "");
                    }
                }
                else if (strcmp(key, "modifies") == 0 && v && VAL_TAG(v) == NODE_LIT_ARRAY) {
                    pm.nmodifies = v->lit_array.elems.len;
                    pm.modifies = malloc(pm.nmodifies * sizeof(char *));
                    for (int d = 0; d < pm.nmodifies; d++) {
                        Node *e = v->lit_array.elems.items[d];
                        pm.modifies[d] = xs_strdup(
                            (e && VAL_TAG(e) == NODE_LIT_STRING && e->lit_string.sval)
                            ? e->lit_string.sval : "");
                    }
                }
            }
            if (!pm.version) pm.version = xs_strdup("0.0.0");
            pipeline_register_meta(pp, &pm);
            /* validate after each registration to catch dependency/conflict errors early */
            if (pipeline_validate(pp) < 0) {
                fprintf(stderr, "plugin '%s': pipeline validation failed\n", plugin_id);
            }
        }

        /* process lexer section: register dynamic tokens */
        if (stmt->plugin_decl.lexer_sec && VAL_TAG(stmt->plugin_decl.lexer_sec) == NODE_BLOCK) {
            Node *blk = stmt->plugin_decl.lexer_sec;
            for (int j = 0; j < blk->block.stmts.len; j++) {
                Node *s = blk->block.stmts.items[j];
                if (VAL_TAG(s) == NODE_LET && s->let.name && strncmp(s->let.name, "__lx_", 5) == 0) {
                    const char *tok_name = apply_load_rename(s->let.name + 5);
                    const char *tok_pattern = "";
                    if (s->let.value && VAL_TAG(s->let.value) == NODE_LIT_STRING && s->let.value->lit_string.sval)
                        tok_pattern = s->let.value->lit_string.sval;
                    int kind = lexer_register_dyn_token(tok_name, tok_pattern);
                    pipeline_register_token(pp, tok_name, tok_pattern, plugin_id);
                    (void)kind;
                    i->needs_reparse = 1;
                }
            }
        }

        /* process runtime section: register hook callbacks */
        if (stmt->plugin_decl.runtime_sec && VAL_TAG(stmt->plugin_decl.runtime_sec) == NODE_BLOCK) {
            Node *blk = stmt->plugin_decl.runtime_sec;
            for (int j = 0; j < blk->block.stmts.len; j++) {
                Node *s = blk->block.stmts.items[j];
                if (VAL_TAG(s) != NODE_LET || !s->let.name) continue;
                const char *nm = s->let.name;
                /* parse __rt_KIND_TARGET */
                RuntimeHookKind hk;
                const char *target = NULL;
                if (strncmp(nm, "__rt_before_", 12) == 0) {
                    hk = RT_HOOK_BEFORE; target = nm + 12;
                } else if (strncmp(nm, "__rt_after_", 11) == 0) {
                    hk = RT_HOOK_AFTER; target = nm + 11;
                } else if (strncmp(nm, "__rt_exec_", 10) == 0) {
                    hk = RT_HOOK_EXEC; target = nm + 10;
                } else continue;

                /* evaluate the body to get a callback value */
                Value *cb = NULL;
                if (s->let.value) {
                    cb = interp_eval(i, s->let.value);
                    if (i->cf.signal) { if (cb) value_decref(cb); break; }
                }
                if (!cb || (VAL_TAG(cb) != XS_FUNC && VAL_TAG(cb) != XS_NATIVE)) {
                    if (cb) value_decref(cb);
                    continue;
                }

                /* also register into the old hook system for dispatch */
                int tag_filter = node_tag_from_string(target);
                if (hk == RT_HOOK_BEFORE && g_n_before_eval < 64) {
                    int idx = g_n_before_eval++;
                    g_before_eval[idx].callback = value_incref(cb);
                    g_before_eval[idx].tag_filter = tag_filter;
                    g_has_eval_hooks = 1;
                } else if (hk == RT_HOOK_AFTER && g_n_after_eval < 64) {
                    int idx = g_n_after_eval++;
                    g_after_eval[idx].callback = value_incref(cb);
                    g_after_eval[idx].tag_filter = tag_filter;
                    g_has_eval_hooks = 1;
                }

                RuntimePluginHook hook;
                memset(&hook, 0, sizeof(hook));
                const char *rt_target = apply_load_rename(target);
                hook.target = xs_strdup(rt_target);
                hook.plugin_id = xs_strdup(plugin_id);
                hook.kind = hk;
                hook.priority = 50;
                hook.callback = cb; /* pipeline takes ownership */
                pipeline_register_runtime_hook(pp, &hook);
                free(hook.target);
                free(hook.plugin_id);
            }
        }

        /* process sema section: register rules (stubs - no dispatch yet) */
        if (stmt->plugin_decl.sema_sec && VAL_TAG(stmt->plugin_decl.sema_sec) == NODE_BLOCK) {
            /* look up meta priority for this plugin */
            int sema_priority = 50;
            for (int mi = 0; mi < pp->nmetas; mi++) {
                if (pp->metas[mi].id && strcmp(pp->metas[mi].id, plugin_id) == 0) {
                    sema_priority = pp->metas[mi].priority;
                    break;
                }
            }

            Node *blk = stmt->plugin_decl.sema_sec;
            for (int j = 0; j < blk->block.stmts.len; j++) {
                Node *s = blk->block.stmts.items[j];
                if (VAL_TAG(s) != NODE_LET || !s->let.name) continue;
                const char *nm = s->let.name;
                if (strncmp(nm, "__sema_", 7) != 0) continue;
                /* parse __sema_KIND_TARGET */
                SemaRuleKind sk;
                const char *target = NULL;
                if (strncmp(nm + 7, "new_", 4) == 0) {
                    sk = SEMA_RULE_NEW; target = nm + 11;
                } else if (strncmp(nm + 7, "override_", 9) == 0) {
                    sk = SEMA_RULE_OVERRIDE; target = nm + 16;
                } else if (strncmp(nm + 7, "exclusive_", 10) == 0) {
                    sk = SEMA_RULE_EXCLUSIVE; target = nm + 17;
                } else continue;

                Value *cb = NULL;
                if (s->let.value) {
                    cb = interp_eval(i, s->let.value);
                    if (i->cf.signal) { if (cb) value_decref(cb); break; }
                }

                SemaPluginRule rule;
                memset(&rule, 0, sizeof(rule));
                const char *sema_target = apply_load_rename(target);
                rule.target = xs_strdup(sema_target);
                rule.plugin_id = xs_strdup(plugin_id);
                rule.kind = sk;
                rule.priority = sema_priority;
                rule.callback = value_incref(cb);
                pipeline_register_sema_rule(pp, &rule);
                free(rule.target);
                free(rule.plugin_id);
            }
        }

        /* process pass sections: extract phase, kind, visitors */
        for (int pi = 0; pi < stmt->plugin_decl.passes.len; pi++) {
            Node *pass_blk = stmt->plugin_decl.passes.items[pi];
            if (!pass_blk || VAL_TAG(pass_blk) != NODE_BLOCK) continue;

            CustomPass cp;
            memset(&cp, 0, sizeof(cp));
            cp.plugin_id = xs_strdup(plugin_id);
            cp.phase_ref = xs_strdup("parser");
            cp.is_after = 1;
            cp.kind = PASS_ANALYZE;
            cp.name = NULL;

            /* temporary arrays for visitors */
            int vcap = 8;
            cp.visitors = malloc(vcap * sizeof(Value *));
            cp.visitor_tags = malloc(vcap * sizeof(int));
            cp.visitor_names = malloc(vcap * sizeof(char *));
            cp.nvisitors = 0;

            for (int pj = 0; pj < pass_blk->block.stmts.len; pj++) {
                Node *ps = pass_blk->block.stmts.items[pj];
                if (!ps || VAL_TAG(ps) != NODE_LET || !ps->let.name) continue;
                const char *pnm = ps->let.name;
                if (strcmp(pnm, "__pass_name") == 0 && ps->let.value &&
                    VAL_TAG(ps->let.value) == NODE_LIT_STRING) {
                    free(cp.name);
                    const char *raw_name = ps->let.value->lit_string.sval ?
                                        ps->let.value->lit_string.sval : "";
                    cp.name = xs_strdup(apply_load_rename(raw_name));
                } else if (strcmp(pnm, "__pass_phase") == 0 && ps->let.value &&
                           VAL_TAG(ps->let.value) == NODE_LIT_STRING) {
                    cp.is_after = (ps->let.value->lit_string.sval &&
                                   strcmp(ps->let.value->lit_string.sval, "after") == 0) ? 1 : 0;
                } else if (strcmp(pnm, "__pass_phase_ref") == 0 && ps->let.value &&
                           VAL_TAG(ps->let.value) == NODE_LIT_STRING) {
                    free(cp.phase_ref);
                    cp.phase_ref = xs_strdup(ps->let.value->lit_string.sval ?
                                             ps->let.value->lit_string.sval : "parser");
                } else if (strcmp(pnm, "__pass_kind") == 0 && ps->let.value &&
                           VAL_TAG(ps->let.value) == NODE_LIT_STRING) {
                    const char *ks = ps->let.value->lit_string.sval;
                    if (ks && strcmp(ks, "annotate") == 0) cp.kind = PASS_ANNOTATE;
                    else if (ks && strcmp(ks, "transform") == 0) cp.kind = PASS_TRANSFORM;
                    else cp.kind = PASS_ANALYZE;
                } else if (strncmp(pnm, "__pass_visit_", 13) == 0) {
                    const char *type_name = pnm + 13;
                    Value *cb = NULL;
                    if (ps->let.value) {
                        cb = interp_eval(i, ps->let.value);
                        if (i->cf.signal) { if (cb) value_decref(cb); break; }
                    }
                    if (cb && (VAL_TAG(cb) == XS_FUNC || VAL_TAG(cb) == XS_NATIVE)) {
                        if (cp.nvisitors >= vcap) {
                            vcap *= 2;
                            cp.visitors = realloc(cp.visitors, vcap * sizeof(Value *));
                            cp.visitor_tags = realloc(cp.visitor_tags, vcap * sizeof(int));
                            cp.visitor_names = realloc(cp.visitor_names, vcap * sizeof(char *));
                        }
                        cp.visitors[cp.nvisitors] = value_incref(cb);
                        int vtag = node_tag_from_string(type_name);
                        cp.visitor_tags[cp.nvisitors] = vtag;
                        cp.visitor_names[cp.nvisitors] = (vtag == -1) ? xs_strdup(type_name) : NULL;
                        cp.nvisitors++;
                    } else {
                        if (cb) value_decref(cb);
                    }
                } else if (strcmp(pnm, "__pass_state") == 0 && ps->let.value) {
                    Value *st = interp_eval(i, ps->let.value);
                    if (i->cf.signal) { if (st) value_decref(st); break; }
                    cp.state = st;
                } else if (strcmp(pnm, "__pass_on_scope_exit") == 0) {
                    Value *cb = NULL;
                    if (ps->let.value) {
                        cb = interp_eval(i, ps->let.value);
                        if (i->cf.signal) { if (cb) value_decref(cb); break; }
                    }
                    cp.on_scope_exit = value_incref(cb);
                }
            }
            if (!cp.name) cp.name = xs_strdup("unnamed");
            pipeline_register_pass(pp, &cp);
            /* free the local cp strings (pipeline made copies via dup_str) */
            free(cp.name);
            free(cp.plugin_id);
            free(cp.phase_ref);
            /* don't free visitors/visitor_tags - pipeline took ownership of these pointers */
        }

        /* process parser section: register extend/production callbacks */
        if (stmt->plugin_decl.parser_sec && VAL_TAG(stmt->plugin_decl.parser_sec) == NODE_BLOCK) {
            Node *blk = stmt->plugin_decl.parser_sec;
            for (int j = 0; j < blk->block.stmts.len; j++) {
                Node *s = blk->block.stmts.items[j];
                if (VAL_TAG(s) != NODE_LET || !s->let.name) continue;
                const char *nm = s->let.name;
                if (strncmp(nm, "__parser_", 9) != 0) continue;
                /* evaluate the callback */
                Value *cb = NULL;
                if (s->let.value) {
                    cb = interp_eval(i, s->let.value);
                    if (i->cf.signal) { if (cb) value_decref(cb); break; }
                }
                if (!cb || (VAL_TAG(cb) != XS_FUNC && VAL_TAG(cb) != XS_NATIVE)) {
                    if (cb) value_decref(cb);
                    continue;
                }
                /* register callback in the appropriate hook system */
                if (strncmp(nm, "__parser_production_", 20) == 0) {
                    /* new production: extract keyword and register */
                    const char *prod_name = apply_load_rename(nm + 20);
                    char kw[64];
                    const char *us = strchr(prod_name, '_');
                    if (us) {
                        int klen = (int)(us - prod_name);
                        if (klen > 63) klen = 63;
                        memcpy(kw, prod_name, klen);
                        kw[klen] = '\0';
                    } else {
                        strncpy(kw, prod_name, 63);
                        kw[63] = '\0';
                    }
                    /* register keyword so parser recognizes it */
                    int dup = 0;
                    for (int k = 0; k < g_n_plugin_keywords; k++) {
                        if (strcmp(g_plugin_keywords[k], kw) == 0) { dup = 1; break; }
                    }
                    if (!dup && g_n_plugin_keywords < MAX_PLUGIN_KEYWORDS) {
                        g_plugin_keywords[g_n_plugin_keywords++] = xs_strdup(kw);
                    }
                    /* store production callback with its keyword and plugin id */
                    if (g_n_parser_productions < 32) {
                        int idx = g_n_parser_productions++;
                        snprintf(g_parser_productions[idx].keyword,
                                 sizeof g_parser_productions[idx].keyword,
                                 "%s", kw);
                        snprintf(g_parser_productions[idx].plugin_id,
                                 sizeof g_parser_productions[idx].plugin_id,
                                 "%s", plugin_id);
                        g_parser_productions[idx].callback = value_incref(cb);
                    }
                    i->needs_reparse = 1;
                } else if (strncmp(nm, "__parser_extend_", 16) == 0) {
                    /* extend existing production: register as syntax handler with target */
                    const char *ext_target = apply_load_rename(nm + 16);
                    if (g_n_syntax_handlers < 16) {
                        int eidx = g_n_syntax_handlers++;
                        g_syntax_handlers[eidx] = value_incref(cb);
                        g_syntax_handler_targets[eidx] = xs_strdup(ext_target);
                    }
                    i->needs_reparse = 1;
                }
                value_decref(cb);
            }
        }
        break;
    }

    case NODE_MODULE_DECL: {
        Env *saved = i->env;
        push_env(i);
        for (int j=0;j<stmt->module_decl.body.len;j++) {
            interp_exec(i, stmt->module_decl.body.items[j]);
            if (i->cf.signal) break;
        }
        XSMap *m = map_new();
        for (int j=0;j<i->env->len;j++)
            map_set(m, i->env->bindings[j].name, value_incref(i->env->bindings[j].value));
        pop_env(i);
        i->env = saved;
        Value *mod = xs_module(m);
        env_define(i->env, stmt->module_decl.name, mod, 1);
        value_decref(mod);
        break;
    }

    case NODE_TYPE_ALIAS: break;
    case NODE_TRAIT_DECL: {
        Value *trait_map = xs_map_new();
        Value *methods_arr = xs_array_new();
        for (int j = 0; j < stmt->trait_decl.n_methods; j++) {
            array_push(methods_arr->arr, xs_str(stmt->trait_decl.method_names[j]));
        }
        map_set(trait_map->map, "__methods__", methods_arr);
        Value *defaults_map = xs_map_new();
        for (int j = 0; j < stmt->trait_decl.methods.len; j++) {
            Node *meth = stmt->trait_decl.methods.items[j];
            if (VAL_TAG(meth) == NODE_FN_DECL && meth->fn_decl.body) {
                int np = meth->fn_decl.params.len;
                Node **params = np ? xs_malloc(np*sizeof(Node*)) : NULL;
                for (int k = 0; k < np; k++) {
                    Param *pm = &meth->fn_decl.params.items[k];
                    params[k] = pm->pattern ? pm->pattern :
                        ({ Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                           pn->pat_ident.name = xs_strdup(pm->name ? pm->name : "_");
                           pn->pat_ident.mutable = 0; pn; });
                }
                XSFunc *fn = func_new(meth->fn_decl.name, params, np,
                                       meth->fn_decl.body, i->env);
                Value *fv = xs_func_new(fn);
                map_set(defaults_map->map, meth->fn_decl.name, fv);
            }
        }
        map_set(trait_map->map, "__defaults__", defaults_map);
        if (stmt->trait_decl.super_trait) {
            map_set(trait_map->map, "__super__", xs_str(stmt->trait_decl.super_trait));
        }
        if (stmt->trait_decl.n_assoc_types > 0) {
            Value *assoc = xs_array_new();
            for (int j = 0; j < stmt->trait_decl.n_assoc_types; j++)
                array_push(assoc->arr, xs_str(stmt->trait_decl.assoc_types[j]));
            map_set(trait_map->map, "__assoc_types__", assoc);
        }
        map_set(trait_map->map, "__name__", xs_str(stmt->trait_decl.name));
        env_define(i->env, stmt->trait_decl.name, trait_map, 1);
        value_decref(trait_map);
        break;
    }
    case NODE_EFFECT_DECL: break;

    case NODE_HANDLE:
    case NODE_PERFORM:
    case NODE_RESUME: {
        Value *v = interp_eval(i, stmt);
        value_decref(v);
        break;
    }

    case NODE_RETURN: {
        if (stmt->ret.value && VAL_TAG(stmt->ret.value) == NODE_CALL) {
            Node *cn = stmt->ret.value;
            Value *callee = EVAL(i, cn->call.callee);
            if (i->cf.signal) { value_decref(callee); break; }
            if (VAL_TAG(callee) == XS_FUNC) {
                int argc = cn->call.args.len;
                Value **args = argc ? xs_malloc(argc * sizeof(Value*)) : NULL;
                int ok = 1;
                for (int j = 0; j < argc; j++) {
                    args[j] = EVAL(i, cn->call.args.items[j]);
                    if (i->cf.signal) {
                        for (int k = 0; k <= j; k++) value_decref(args[k]);
                        free(args); value_decref(callee);
                        ok = 0; break;
                    }
                }
                if (ok) {
                    i->tc_callee = callee;
                    i->tc_args   = args;
                    i->tc_argc   = argc;
                    i->cf.signal = CF_TAIL_CALL;
                    if (i->cf.value) value_decref(i->cf.value);
                    i->cf.value  = NULL;
                    break;
                }
                break;
            }
            value_decref(callee);
        }
        Value *val = stmt->ret.value ? EVAL(i, stmt->ret.value) : value_incref(XS_NULL_VAL);
        /* If evaluating the return expression propagated a throw (or any
           other signal), do not overwrite it with CF_RETURN. */
        if (i->cf.signal) {
            value_decref(val);
            break;
        }
        /* after_eval hooks for return nodes */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_RETURN) continue;
                Value *node_map = node_to_xs_map(stmt);
                Value *hargs[2] = { node_map, val };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_RETURN;
        i->cf.value  = val;
        break;
    }

    case NODE_YIELD: {
        /* yield statement (e.g. yield expr;) */
        Value *val = stmt->yield_.value ? EVAL(i, stmt->yield_.value) : value_incref(XS_NULL_VAL);
        if (i->cf.signal) { value_decref(val); break; }
        if (i->yield_collect) {
            array_push(i->yield_collect->arr, val);
            if (i->yield_limit > 0 && i->yield_collect->arr->len >= i->yield_limit) {
                i->cf.signal = CF_RETURN;
                i->cf.value = value_incref(XS_NULL_VAL);
            }
        } else {
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_YIELD;
            i->cf.value  = val;
        }
        break;
    }

    case NODE_BREAK: {
        Value *val = stmt->brk.value ? EVAL(i, stmt->brk.value) : value_incref(XS_NULL_VAL);
        if (i->cf.value) value_decref(i->cf.value);
        free(i->cf.label); i->cf.label = stmt->brk.label ? xs_strdup(stmt->brk.label) : NULL;
        i->cf.signal = CF_BREAK;
        i->cf.value  = val;
        break;
    }

    case NODE_CONTINUE:
        free(i->cf.label); i->cf.label = stmt->cont.label ? xs_strdup(stmt->cont.label) : NULL;
        i->cf.signal = CF_CONTINUE;
        break;

    case NODE_THROW: {
        Value *val = EVAL(i, stmt->throw_.value);
        /* after_eval hooks for throw nodes (fire before signal so plugins can dump state) */
        if (g_has_eval_hooks && g_n_after_eval > 0 && !g_in_eval_hook) {
            g_in_eval_hook = 1;
            for (int _h = 0; _h < g_n_after_eval; _h++) {
                EvalHook *hook = &g_after_eval[_h];
                if (!hook->callback) continue;
                if (hook->tag_filter >= 0 && hook->tag_filter != NODE_THROW) continue;
                Value *node_map = node_to_xs_map(stmt);
                Value *hargs[2] = { node_map, val };
                Value *hresult = call_value(i, hook->callback, hargs, 2, "after_eval");
                value_decref(node_map);
                if (hresult) value_decref(hresult);
            }
            g_in_eval_hook = 0;
        }
        if (i->cf.value) value_decref(i->cf.value);
        i->cf.signal = CF_THROW;
        i->cf.value  = val;
        break;
    }

    case NODE_DEFER: {
        if (i->defers.len >= i->defers.cap) {
            i->defers.cap = i->defers.cap ? i->defers.cap * 2 : 8;
            i->defers.items = xs_realloc(i->defers.items, i->defers.cap * sizeof(Node*));
        }
        i->defers.items[i->defers.len++] = stmt->defer_.body;
        break;
    }

    case NODE_BLOCK: {
        Value *v = EVAL(i, stmt);
        value_decref(v);
        break;
    }

    case NODE_IF:
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_LOOP:
    case NODE_MATCH:
    case NODE_TRY: {
        Value *v = interp_eval(i, stmt);
        value_decref(v);
        break;
    }

    case NODE_BIND: {
        /* reactive binding: bind name = expr */
        /* start tracking deps */
        int saved_tracking = g_dep_tracking;
        char **saved_names = g_dep_track_names;
        int saved_len = g_dep_track_len;
        int saved_cap = g_dep_track_cap;
        g_dep_tracking = 1;
        g_dep_track_names = NULL;
        g_dep_track_len = 0;
        g_dep_track_cap = 0;

        /* evaluate the expression */
        Value *val = EVAL(i, stmt->bind_decl.expr);

        /* capture deps */
        char **deps = g_dep_track_names;
        int ndeps = g_dep_track_len;

        /* restore tracking state */
        g_dep_tracking = saved_tracking;
        g_dep_track_names = saved_names;
        g_dep_track_len = saved_len;
        g_dep_track_cap = saved_cap;

        /* define as mutable var so reactive updates can overwrite */
        env_define(i->env, stmt->bind_decl.name, val, 1);
        value_decref(val);

        /* register reactive binding */
        env_set_reactive_interp(i);
        env_add_reactive(i->env, stmt->bind_decl.name, stmt->bind_decl.expr,
                         i->env, deps, ndeps);
        break;
    }

    case NODE_TAG_DECL: {
        /* Desugar: tag name(params) { body } -> fn name(params, __block) { body }
           where yield inside body calls __block() */
        int nparams = stmt->tag_decl.params.len + 1;  /* +1 for __block */
        Node **params = xs_malloc(nparams * sizeof(Node*));
        Node **defaults = xs_calloc(nparams, sizeof(Node*));
        int *varflags = xs_calloc(nparams, sizeof(int));
        for (int j = 0; j < stmt->tag_decl.params.len; j++) {
            Param *pm = &stmt->tag_decl.params.items[j];
            if (pm->pattern) {
                params[j] = pm->pattern;
            } else {
                Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                pn->pat_ident.name = xs_strdup(pm->name ? pm->name : "_");
                pn->pat_ident.mutable = 0;
                params[j] = pn;
            }
            defaults[j] = pm->default_val;
            varflags[j] = pm->variadic;
        }
        /* Add implicit __block parameter */
        Node *bp = node_new(NODE_PAT_IDENT, stmt->span);
        bp->pat_ident.name = xs_strdup("__block");
        bp->pat_ident.mutable = 0;
        params[nparams - 1] = bp;
        defaults[nparams - 1] = NULL;
        varflags[nparams - 1] = 0;

        XSFunc *fn = func_new_ex(stmt->tag_decl.name, params, nparams,
                                 stmt->tag_decl.body, i->env, defaults, varflags);
        Value *v = xs_func_new(fn);
        if (stmt->tag_decl.name)
            env_define(i->env, stmt->tag_decl.name, v, 1);
        value_decref(v);
        break;
    }

    default: {
            Value *v = interp_eval(i, stmt);
            value_decref(v);
        }
        break;
    }
}

static void hoist_functions(Interp *i, NodeList *stmts) {
    for (int j = 0; j < stmts->len; j++) {
        Node *stmt = stmts->items[j];
        if (!stmt || VAL_TAG(stmt) != NODE_FN_DECL) continue;
        if (!stmt->fn_decl.body || !stmt->fn_decl.name) continue;

        int nparams = stmt->fn_decl.params.len;
        Node **params = nparams ? xs_malloc(nparams * sizeof(Node*)) : NULL;
        Node **defaults = nparams ? xs_calloc(nparams, sizeof(Node*)) : NULL;
        int  *varflags  = nparams ? xs_calloc(nparams, sizeof(int)) : NULL;
        for (int k = 0; k < nparams; k++) {
            Param *pm = &stmt->fn_decl.params.items[k];
            if (pm->pattern) {
                params[k] = pm->pattern;
            } else {
                Node *pn = node_new(NODE_PAT_IDENT, pm->span);
                pn->pat_ident.name    = xs_strdup(pm->name ? pm->name : "_");
                pn->pat_ident.mutable = 0;
                params[k] = pn;
            }
            defaults[k]  = pm->default_val;
            varflags[k]   = pm->variadic;
        }
        XSFunc *fn = func_new_ex(stmt->fn_decl.name, params, nparams,
                                 stmt->fn_decl.body, i->env, defaults, varflags);
        fn->is_generator = stmt->fn_decl.is_generator;
        fn->is_async     = stmt->fn_decl.is_async;
        if (stmt->fn_decl.deprecated_msg)
            fn->deprecated_msg = xs_strdup(stmt->fn_decl.deprecated_msg);
        if (nparams > 0) {
            fn->param_type_names = xs_calloc(nparams, sizeof(char*));
            int has_contracts = 0;
            for (int k2 = 0; k2 < nparams; k2++) {
                Param *pm2 = &stmt->fn_decl.params.items[k2];
                if (pm2->type_ann && pm2->type_ann->name)
                    fn->param_type_names[k2] = xs_strdup(pm2->type_ann->name);
                if (pm2->contract) has_contracts = 1;
            }
            if (has_contracts) {
                fn->param_contracts = xs_calloc(nparams, sizeof(Node*));
                for (int k2 = 0; k2 < nparams; k2++) {
                    fn->param_contracts[k2] = stmt->fn_decl.params.items[k2].contract;
                }
            }
        }
        if (stmt->fn_decl.ret_type && stmt->fn_decl.ret_type->name)
            fn->ret_type_name = xs_strdup(stmt->fn_decl.ret_type->name);
        Value *v = xs_func_new(fn);
        env_define(i->env, stmt->fn_decl.name, v, 1);
        register_fn_decl_triggers(i, stmt, v);
        /* Wrapping decorators: feed the bound fn through @memoize /
           @retry / @trace / @timed in declaration order, re-binding
           the name to the produced wrapper map. The wrapper is then
           callable via wrap_call_dispatch in call_value. */
        for (int dk = 0; dk < stmt->fn_decl.n_decorators; dk++) {
            Decorator *d = &stmt->fn_decl.decorators[dk];
            if (!parser_decorator_is_wrapping(d->name)) continue;
            Value *cur = env_get(i->env, stmt->fn_decl.name);
            if (!cur) continue;
            char wname[64];
            snprintf(wname, sizeof wname, "__wrap_%s", d->name);
            Value *make = env_get(i->env, wname);
            if (!make) continue;
            int wargc = 1 + d->n_args + 1;
            Value **wargs = xs_calloc(wargc, sizeof(Value*));
            wargs[0] = value_incref(cur);
            for (int a = 0; a < d->n_args; a++)
                wargs[1 + a] = EVAL(i, d->args[a]);
            wargs[1 + d->n_args] = xs_str(stmt->fn_decl.name);
            Value *wrapped = call_value(i, make, wargs, wargc, d->name);
            for (int a = 0; a < wargc; a++)
                if (wargs[a]) value_decref(wargs[a]);
            free(wargs);
            if (wrapped) {
                env_define(i->env, stmt->fn_decl.name, wrapped, 1);
                value_decref(wrapped);
            }
        }
        value_decref(v);
    }
}

void interp_run(Interp *i, Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;
    /* Reset per-invocation counters but keep any caps set by the
       embedder / CLI. */
    xs_limits_reset();
    if (i->last_expr_value) {
        value_decref(i->last_expr_value);
        i->last_expr_value = NULL;
    }
    g_current_interp = i;
    g_plugin_interp = i;
    i->current_program = program;
    hoist_functions(i, &program->program.stmts);
    for (int j = 0; j < program->program.stmts.len; j++) {
        interp_exec(i, program->program.stmts.items[j]);

        /* phase 2: if a plugin registered syntax handlers, re-parse remaining source */
        if (i->needs_reparse) {
        }
        if (i->needs_reparse && i->source && i->filename &&
            (g_n_syntax_handlers > 0 || g_n_plugin_keywords > 0 ||
             g_n_parser_overrides > 0 || g_n_parser_productions > 0)) {
            i->needs_reparse = 0;
            if (i->cf.signal) CF_CLEAR(i);
            Lexer rlex;
            lexer_init(&rlex, i->source, i->filename);
            TokenArray rta = lexer_tokenize(&rlex);
            Parser rp;
            parser_init(&rp, &rta, i->filename);
            Node *reparsed = parser_parse(&rp);
            token_array_free(&rta);
            if (reparsed && !rp.had_error) {
                i->current_program = reparsed;
                hoist_functions(i, &reparsed->program.stmts);
                for (int k = j + 1; k < reparsed->program.stmts.len; k++) {
                    interp_exec(i, reparsed->program.stmts.items[k]);
                    /* nested re-parse: if another plugin registered syntax, re-parse again */
                    if (i->needs_reparse && i->source && i->filename &&
                        (g_n_syntax_handlers > 0 || g_n_plugin_keywords > 0 ||
                         g_n_parser_overrides > 0 || g_n_parser_productions > 0)) {
                        i->needs_reparse = 0;
                        if (i->cf.signal) CF_CLEAR(i);
                        Lexer rlex2;
                        lexer_init(&rlex2, i->source, i->filename);
                        TokenArray rta2 = lexer_tokenize(&rlex2);
                        Parser rp2;
                        parser_init(&rp2, &rta2, i->filename);
                        Node *reparsed2 = parser_parse(&rp2);
                        token_array_free(&rta2);
                        if (reparsed2 && !rp2.had_error) {
                            i->current_program = reparsed2;
                            hoist_functions(i, &reparsed2->program.stmts);
                            for (int m = k + 1; m < reparsed2->program.stmts.len; m++) {
                                interp_exec(i, reparsed2->program.stmts.items[m]);
                                if (i->cf.signal == CF_RETURN) { CF_CLEAR(i); }
                                else if (i->cf.signal == CF_ERROR || i->cf.signal == CF_PANIC) {
                                    goto run_done;
                                } else if (i->cf.signal == CF_THROW) {
                                    if (!g_xs_throw_from_runtime) {
                                        Value *exc = i->cf.value;
                                        char *s = exc ? value_repr(exc) : xs_strdup("<error>");
                                        Node *sn2 = reparsed2->program.stmts.items[m];
                                        fprintf(stderr, "xs: error at %s:%d:%d: unhandled exception: %s\n",
                                                sn2->span.file ? sn2->span.file : "<unknown>",
                                                sn2->span.line, sn2->span.col, s);
                                        free(s);
                                    }
                                    g_xs_throw_from_runtime = 0;
                                    CF_CLEAR(i);
                                } else if (i->cf.signal) {
                                    CF_CLEAR(i);
                                }
                            }
                        }
                        if (reparsed2) { /* don't free -- AST may be referenced */ }
                        goto run_done;
                    }
                    if (i->cf.signal == CF_RETURN) { CF_CLEAR(i); }
                    else if (i->cf.signal == CF_ERROR || i->cf.signal == CF_PANIC) {
                        goto run_done;
                    } else if (i->cf.signal == CF_THROW) {
                        if (!g_xs_throw_from_runtime) {
                            Value *exc = i->cf.value;
                            char *s = exc ? value_repr(exc) : xs_strdup("<error>");
                            Node *sn = reparsed->program.stmts.items[k];
                            fprintf(stderr, "xs: error at %s:%d:%d: unhandled exception: %s\n",
                                    sn->span.file ? sn->span.file : "<unknown>",
                                    sn->span.line, sn->span.col, s);
                            free(s);
                        }
                        g_xs_throw_from_runtime = 0;
                        CF_CLEAR(i);
                    } else if (i->cf.signal) {
                        CF_CLEAR(i);
                    }
                }
                /* don't free reparsed -- AST nodes may be referenced by closures */
                goto run_done;
            }
            if (reparsed) node_free(reparsed);
        }

        if (i->cf.signal == CF_RETURN) { CF_CLEAR(i); }
        else if (i->cf.signal == CF_ERROR || i->cf.signal == CF_PANIC) {
            /* phase 3: on_error hook for panics */
            if (g_n_on_error > 0) {
                Value *err_val = i->cf.value ? value_incref(i->cf.value) : xs_str("<error>");
                CF_CLEAR(i);
                int handled = 0;
                for (int _oe = g_n_on_error - 1; _oe >= 0; _oe--) {
                    if (!g_on_error_hooks[_oe]) continue;
                    Value *prev = value_incref(XS_NULL_VAL);
                    Value *oargs[2] = { err_val, prev };
                    Value *oresult = call_value(i, g_on_error_hooks[_oe], oargs, 2, "on_error");
                    value_decref(prev);
                    if (oresult) value_decref(oresult);
                    if (!i->cf.signal) { handled = 1; break; }
                    CF_CLEAR(i);
                }
                value_decref(err_val);
                if (!handled) {
                    i->cf.signal = CF_PANIC;
                    i->cf.value = xs_str("unhandled error");
                    break;
                }
            } else {
                break;
            }
        } else if (i->cf.signal == CF_THROW) {
            /* phase 3: on_error hook for unhandled throws */
            if (g_n_on_error > 0) {
                Value *exc = i->cf.value ? value_incref(i->cf.value) : xs_str("<error>");
                CF_CLEAR(i);
                int handled = 0;
                for (int _oe = g_n_on_error - 1; _oe >= 0; _oe--) {
                    if (!g_on_error_hooks[_oe]) continue;
                    Value *prev = value_incref(XS_NULL_VAL);
                    Value *oargs[2] = { exc, prev };
                    Value *oresult = call_value(i, g_on_error_hooks[_oe], oargs, 2, "on_error");
                    value_decref(prev);
                    if (oresult) value_decref(oresult);
                    if (!i->cf.signal) { handled = 1; break; }
                    CF_CLEAR(i);
                }
                value_decref(exc);
                if (!handled) {
                    Node *sn = program->program.stmts.items[j];
                    fprintf(stderr, "xs: error at %s:%d:%d: unhandled exception\n",
                            sn->span.file ? sn->span.file : "<unknown>",
                            sn->span.line, sn->span.col);
                }
            } else {
                Value *exc = i->cf.value;
                if (!g_xs_throw_from_runtime) {
                    char *s = exc ? value_repr(exc) : xs_strdup("<error>");
                    Node *sn = program->program.stmts.items[j];
                    fprintf(stderr, "xs: error at %s:%d:%d: unhandled exception: %s\n",
                            sn->span.file ? sn->span.file : "<unknown>",
                            sn->span.line, sn->span.col, s);
                    free(s);
                }
                g_xs_throw_from_runtime = 0;
                if (exc) i->unhandled_exception_value = value_incref(exc);
                CF_CLEAR(i);
                i->had_unhandled_exception = 1;
                break;
            }
        } else if (i->cf.signal) {
            CF_CLEAR(i);
        }
    }
run_done:;
    trigger_fire_on_start(i);
    Value *main_fn = env_get(i->globals, "main");
    if (main_fn && VAL_TAG(main_fn) == XS_FUNC) {
        Value *res = call_value(i, main_fn, NULL, 0, "main");
        if (res) value_decref(res);
    }
    trigger_run_event_loop(i);
    if (i->had_unhandled_exception && i->unhandled_exception_value)
        trigger_fire_on_panic(i, i->unhandled_exception_value);
    trigger_fire_on_exit(i);
    if (i->unhandled_exception_value) {
        value_decref(i->unhandled_exception_value);
        i->unhandled_exception_value = NULL;
    }
}
