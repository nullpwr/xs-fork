/* checker.c -- deep type checker with inference and generics for XS.
 *
 * Implements Hindley-Milner style type inference with unification,
 * constraint-based solving, subtype checking, and union/option types.
 */

#include "types/checker.h"
#include "types/types.h"
#include "semantic/symtable.h"
#include "diagnostic/diagnostic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ----------------------------------------------------------------
 * Primitive singletons
 * ---------------------------------------------------------------- */

static CkType _ck_int    = { CK_INT,    {{0}} };
static CkType _ck_float  = { CK_FLOAT,  {{0}} };
static CkType _ck_bool   = { CK_BOOL,   {{0}} };
static CkType _ck_string = { CK_STRING, {{0}} };
static CkType _ck_null   = { CK_NULL,   {{0}} };
static CkType _ck_any    = { CK_ANY,    {{0}} };
static CkType _ck_never  = { CK_NEVER,  {{0}} };

CkType *ck_int(void)    { return &_ck_int; }
CkType *ck_float(void)  { return &_ck_float; }
CkType *ck_bool(void)   { return &_ck_bool; }
CkType *ck_string(void) { return &_ck_string; }
CkType *ck_null(void)   { return &_ck_null; }
CkType *ck_any(void)    { return &_ck_any; }
CkType *ck_never(void)  { return &_ck_never; }

static int is_singleton(CkType *t) {
    return t == &_ck_int || t == &_ck_float || t == &_ck_bool ||
           t == &_ck_string || t == &_ck_null || t == &_ck_any ||
           t == &_ck_never;
}

/* ----------------------------------------------------------------
 * Constructors
 * ---------------------------------------------------------------- */

CkType *ck_array(CkType *elem) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_ARRAY;
    t->array.elem = elem;
    return t;
}

CkType *ck_map(CkType *key, CkType *val) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_MAP;
    t->map.key = key;
    t->map.val = val;
    return t;
}

CkType *ck_tuple(CkType **elems, int n) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_TUPLE;
    t->tuple.nelems = n;
    t->tuple.elems = xs_malloc(sizeof(CkType *) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) t->tuple.elems[i] = elems[i];
    return t;
}

CkType *ck_function(CkType **params, int nparams, CkType *ret) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_FUNCTION;
    t->func.nparams = nparams;
    t->func.ret = ret;
    t->func.params = xs_malloc(sizeof(CkType *) * (nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++) t->func.params[i] = params[i];
    return t;
}

CkType *ck_struct(const char *name, CkType **fields, char **names, int n) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_STRUCT;
    t->struct_.name = xs_strdup(name);
    t->struct_.nfields = n;
    if (n > 0) {
        t->struct_.fields = xs_malloc(sizeof(CkType *) * n);
        t->struct_.field_names = xs_malloc(sizeof(char *) * n);
        for (int i = 0; i < n; i++) {
            t->struct_.fields[i] = fields ? fields[i] : ck_any();
            t->struct_.field_names[i] = names ? xs_strdup(names[i]) : NULL;
        }
    }
    return t;
}

CkType *ck_enum(const char *name) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_ENUM;
    t->enum_.name = xs_strdup(name);
    t->enum_.variants = NULL;
    t->enum_.nvariants = 0;
    return t;
}

CkType *ck_union(CkType **members, int n) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_UNION;
    t->union_.nmembers = n;
    t->union_.members = xs_malloc(sizeof(CkType *) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) t->union_.members[i] = members[i];
    return t;
}

CkType *ck_option(CkType *inner) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_OPTION;
    t->option.inner = inner;
    return t;
}

CkType *ck_generic(const char *name, int id) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_GENERIC;
    t->generic.name = xs_strdup(name);
    t->generic.id = id;
    return t;
}

CkType *ck_var(CkContext *ctx) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_VAR;
    t->var.id = ++ctx->next_var_id;
    t->var.bound = NULL;
    return t;
}

CkType *ck_trait(const char *name) {
    CkType *t = xs_calloc(1, sizeof(CkType));
    t->kind = CK_TRAIT;
    /* store trait name in the struct_ name field for convenience */
    t->struct_.name = xs_strdup(name);
    t->struct_.nfields = 0;
    return t;
}

/* ----------------------------------------------------------------
 * Type freeing
 * ---------------------------------------------------------------- */

void ck_type_free(CkType *t) {
    if (!t || is_singleton(t)) return;
    switch (t->kind) {
    case CK_ARRAY:
        ck_type_free(t->array.elem);
        break;
    case CK_MAP:
        ck_type_free(t->map.key);
        ck_type_free(t->map.val);
        break;
    case CK_TUPLE:
        for (int i = 0; i < t->tuple.nelems; i++)
            ck_type_free(t->tuple.elems[i]);
        free(t->tuple.elems);
        break;
    case CK_FUNCTION:
        for (int i = 0; i < t->func.nparams; i++)
            ck_type_free(t->func.params[i]);
        free(t->func.params);
        ck_type_free(t->func.ret);
        break;
    case CK_STRUCT:
        free(t->struct_.name);
        for (int i = 0; i < t->struct_.nfields; i++) {
            ck_type_free(t->struct_.fields[i]);
            free(t->struct_.field_names[i]);
        }
        free(t->struct_.fields);
        free(t->struct_.field_names);
        break;
    case CK_ENUM:
        free(t->enum_.name);
        for (int i = 0; i < t->enum_.nvariants; i++) {
            free(t->enum_.variants[i].tag);
            for (int j = 0; j < t->enum_.variants[i].nfields; j++)
                ck_type_free(t->enum_.variants[i].fields[j]);
            free(t->enum_.variants[i].fields);
        }
        free(t->enum_.variants);
        break;
    case CK_UNION:
        for (int i = 0; i < t->union_.nmembers; i++)
            ck_type_free(t->union_.members[i]);
        free(t->union_.members);
        break;
    case CK_OPTION:
        ck_type_free(t->option.inner);
        break;
    case CK_GENERIC:
        free(t->generic.name);
        break;
    case CK_TRAIT:
        free(t->struct_.name);
        break;
    case CK_VAR:
        /* don't free bound, it may be shared */
        break;
    default:
        break;
    }
    free(t);
}

/* ----------------------------------------------------------------
 * Type resolution (follow unification variable chains)
 * ---------------------------------------------------------------- */

CkType *ck_resolve(CkType *t) {
    if (!t) return NULL;
    while (t->kind == CK_VAR && t->var.bound)
        t = t->var.bound;
    return t;
}

/* ----------------------------------------------------------------
 * Type to string
 * ---------------------------------------------------------------- */

static void type_to_buf(CkType *t, char *buf, int sz) {
    if (!t) { snprintf(buf, sz, "?"); return; }
    t = ck_resolve(t);
    switch (t->kind) {
    case CK_INT:    snprintf(buf, sz, "int"); break;
    case CK_FLOAT:  snprintf(buf, sz, "float"); break;
    case CK_BOOL:   snprintf(buf, sz, "bool"); break;
    case CK_STRING: snprintf(buf, sz, "string"); break;
    case CK_NULL:   snprintf(buf, sz, "null"); break;
    case CK_ANY:    snprintf(buf, sz, "any"); break;
    case CK_NEVER:  snprintf(buf, sz, "never"); break;
    case CK_ARRAY: {
        char inner[256];
        type_to_buf(t->array.elem, inner, sizeof inner);
        snprintf(buf, sz, "Array<%s>", inner);
        break;
    }
    case CK_MAP: {
        char kb[128], vb[128];
        type_to_buf(t->map.key, kb, sizeof kb);
        type_to_buf(t->map.val, vb, sizeof vb);
        snprintf(buf, sz, "Map<%s, %s>", kb, vb);
        break;
    }
    case CK_TUPLE: {
        int pos = 0;
        pos += snprintf(buf + pos, sz - pos, "(");
        for (int i = 0; i < t->tuple.nelems && pos < sz - 10; i++) {
            char eb[128];
            type_to_buf(t->tuple.elems[i], eb, sizeof eb);
            if (i > 0) pos += snprintf(buf + pos, sz - pos, ", ");
            pos += snprintf(buf + pos, sz - pos, "%s", eb);
        }
        snprintf(buf + pos, sz - pos, ")");
        break;
    }
    case CK_FUNCTION: {
        int pos = 0;
        pos += snprintf(buf + pos, sz - pos, "(");
        for (int i = 0; i < t->func.nparams && pos < sz - 20; i++) {
            char pb[128];
            type_to_buf(t->func.params[i], pb, sizeof pb);
            if (i > 0) pos += snprintf(buf + pos, sz - pos, ", ");
            pos += snprintf(buf + pos, sz - pos, "%s", pb);
        }
        pos += snprintf(buf + pos, sz - pos, ") -> ");
        char rb[128];
        type_to_buf(t->func.ret, rb, sizeof rb);
        snprintf(buf + pos, sz - pos, "%s", rb);
        break;
    }
    case CK_STRUCT:
        snprintf(buf, sz, "%s", t->struct_.name ? t->struct_.name : "struct");
        break;
    case CK_ENUM:
        snprintf(buf, sz, "%s", t->enum_.name ? t->enum_.name : "enum");
        break;
    case CK_TRAIT:
        snprintf(buf, sz, "%s", t->struct_.name ? t->struct_.name : "trait");
        break;
    case CK_UNION: {
        int pos = 0;
        for (int i = 0; i < t->union_.nmembers && pos < sz - 10; i++) {
            char mb[128];
            type_to_buf(t->union_.members[i], mb, sizeof mb);
            if (i > 0) pos += snprintf(buf + pos, sz - pos, " | ");
            pos += snprintf(buf + pos, sz - pos, "%s", mb);
        }
        break;
    }
    case CK_OPTION: {
        char inner[256];
        type_to_buf(t->option.inner, inner, sizeof inner);
        snprintf(buf, sz, "%s?", inner);
        break;
    }
    case CK_GENERIC:
        snprintf(buf, sz, "%s", t->generic.name ? t->generic.name : "T");
        break;
    case CK_VAR:
        snprintf(buf, sz, "$%d", t->var.id);
        break;
    }
}

const char *ck_type_to_string(CkType *t) {
    static char buf[512];
    type_to_buf(t, buf, sizeof buf);
    return buf;
}

/* thread-safe version that returns allocated string */
static char *type_to_str_alloc(CkType *t) {
    char buf[512];
    type_to_buf(t, buf, sizeof buf);
    return xs_strdup(buf);
}

/* ----------------------------------------------------------------
 * Type environment
 * ---------------------------------------------------------------- */

CkTypeEnv *ck_env_new(CkTypeEnv *parent) {
    CkTypeEnv *env = xs_calloc(1, sizeof(CkTypeEnv));
    env->parent = parent;
    env->cap = 16;
    env->bindings = xs_malloc(sizeof(CkBinding) * env->cap);
    env->nbindings = 0;
    return env;
}

void ck_env_free(CkTypeEnv *env) {
    if (!env) return;
    for (int i = 0; i < env->nbindings; i++)
        free(env->bindings[i].name);
    free(env->bindings);
    free(env);
}

void ck_env_bind(CkTypeEnv *env, const char *name, CkType *type) {
    if (env->nbindings >= env->cap) {
        env->cap *= 2;
        env->bindings = xs_realloc(env->bindings, sizeof(CkBinding) * env->cap);
    }
    env->bindings[env->nbindings].name = xs_strdup(name);
    env->bindings[env->nbindings].type = type;
    env->nbindings++;
}

CkType *ck_env_lookup(CkTypeEnv *env, const char *name) {
    if (!env || !name) return NULL;
    for (int i = env->nbindings - 1; i >= 0; i--) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].type;
    }
    if (env->parent) return ck_env_lookup(env->parent, name);
    return NULL;
}

/* ----------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------- */

void ck_type_error(CkContext *ctx, Span span, const char *fmt, ...) {
    if (ctx->errors.nerrors >= 50) return; /* cap */
    if (ctx->errors.nerrors >= ctx->errors.cap) {
        ctx->errors.cap = ctx->errors.cap ? ctx->errors.cap * 2 : 16;
        ctx->errors.messages = xs_realloc(ctx->errors.messages,
                                          sizeof(char *) * ctx->errors.cap);
        ctx->errors.spans = xs_realloc(ctx->errors.spans,
                                       sizeof(Span) * ctx->errors.cap);
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ctx->errors.messages[ctx->errors.nerrors] = xs_strdup(buf);
    ctx->errors.spans[ctx->errors.nerrors] = span;
    ctx->errors.nerrors++;
}

void ck_type_mismatch(CkContext *ctx, Span span, CkType *expected, CkType *got) {
    char *es = type_to_str_alloc(expected);
    char *gs = type_to_str_alloc(got);
    ck_type_error(ctx, span, "type mismatch: expected '%s', got '%s'", es, gs);
    free(es);
    free(gs);
}

/* ----------------------------------------------------------------
 * Unification
 * ---------------------------------------------------------------- */

/* occurs check: does var id occur in type t? */
static int occurs_in(int var_id, CkType *t) {
    if (!t) return 0;
    t = ck_resolve(t);
    switch (t->kind) {
    case CK_VAR: return t->var.id == var_id;
    case CK_ARRAY: return occurs_in(var_id, t->array.elem);
    case CK_MAP: return occurs_in(var_id, t->map.key) || occurs_in(var_id, t->map.val);
    case CK_TUPLE:
        for (int i = 0; i < t->tuple.nelems; i++)
            if (occurs_in(var_id, t->tuple.elems[i])) return 1;
        return 0;
    case CK_FUNCTION:
        for (int i = 0; i < t->func.nparams; i++)
            if (occurs_in(var_id, t->func.params[i])) return 1;
        return occurs_in(var_id, t->func.ret);
    case CK_OPTION: return occurs_in(var_id, t->option.inner);
    case CK_UNION:
        for (int i = 0; i < t->union_.nmembers; i++)
            if (occurs_in(var_id, t->union_.members[i])) return 1;
        return 0;
    default: return 0;
    }
}

static int types_same_kind(CkType *a, CkType *b) {
    if (!a || !b) return 0;
    return a->kind == b->kind;
}

int ck_unify(CkContext *ctx, CkType *a, CkType *b) {
    a = ck_resolve(a);
    b = ck_resolve(b);
    if (!a || !b) return 1; /* missing type = accept */
    if (a == b) return 1;

    /* any matches everything */
    if (a->kind == CK_ANY || b->kind == CK_ANY) return 1;

    /* never matches anything (bottom type) */
    if (a->kind == CK_NEVER) return 1;
    if (b->kind == CK_NEVER) return 1;

    /* unification variable */
    if (a->kind == CK_VAR) {
        if (occurs_in(a->var.id, b)) return 0; /* infinite type */
        a->var.bound = b;
        return 1;
    }
    if (b->kind == CK_VAR) {
        if (occurs_in(b->var.id, a)) return 0;
        b->var.bound = a;
        return 1;
    }

    /* option: T? unifies with T and with null */
    if (a->kind == CK_OPTION && b->kind == CK_NULL) return 1;
    if (b->kind == CK_OPTION && a->kind == CK_NULL) return 1;
    if (a->kind == CK_OPTION && b->kind == CK_OPTION)
        return ck_unify(ctx, a->option.inner, b->option.inner);
    if (a->kind == CK_OPTION)
        return ck_unify(ctx, a->option.inner, b);
    if (b->kind == CK_OPTION)
        return ck_unify(ctx, a, b->option.inner);

    /* union: check if b is contained in the union */
    if (a->kind == CK_UNION) {
        for (int i = 0; i < a->union_.nmembers; i++) {
            if (ck_unify(ctx, a->union_.members[i], b)) return 1;
        }
        return 0;
    }
    if (b->kind == CK_UNION) {
        for (int i = 0; i < b->union_.nmembers; i++) {
            if (ck_unify(ctx, a, b->union_.members[i])) return 1;
        }
        return 0;
    }

    if (!types_same_kind(a, b)) return 0;

    switch (a->kind) {
    case CK_INT: case CK_FLOAT: case CK_BOOL: case CK_STRING:
    case CK_NULL: case CK_ANY: case CK_NEVER:
        return 1;

    case CK_ARRAY:
        return ck_unify(ctx, a->array.elem, b->array.elem);

    case CK_MAP:
        return ck_unify(ctx, a->map.key, b->map.key) &&
               ck_unify(ctx, a->map.val, b->map.val);

    case CK_TUPLE:
        if (a->tuple.nelems != b->tuple.nelems) return 0;
        for (int i = 0; i < a->tuple.nelems; i++)
            if (!ck_unify(ctx, a->tuple.elems[i], b->tuple.elems[i])) return 0;
        return 1;

    case CK_FUNCTION:
        if (a->func.nparams != b->func.nparams) return 0;
        for (int i = 0; i < a->func.nparams; i++)
            if (!ck_unify(ctx, a->func.params[i], b->func.params[i])) return 0;
        return ck_unify(ctx, a->func.ret, b->func.ret);

    case CK_STRUCT:
        if (!a->struct_.name || !b->struct_.name) return 0;
        return strcmp(a->struct_.name, b->struct_.name) == 0;

    case CK_ENUM:
        if (!a->enum_.name || !b->enum_.name) return 0;
        return strcmp(a->enum_.name, b->enum_.name) == 0;

    case CK_GENERIC:
        return a->generic.id == b->generic.id;

    case CK_OPTION:
        return ck_unify(ctx, a->option.inner, b->option.inner);

    default:
        return 0;
    }
}

/* ----------------------------------------------------------------
 * Subtype checking
 * ---------------------------------------------------------------- */

int ck_subtype(CkType *sub, CkType *super) {
    sub = ck_resolve(sub);
    super = ck_resolve(super);
    if (!sub || !super) return 1;
    if (sub == super) return 1;

    if (super->kind == CK_ANY) return 1;
    if (sub->kind == CK_NEVER) return 1;

    /* T is subtype of T? */
    if (super->kind == CK_OPTION) {
        if (sub->kind == CK_NULL) return 1;
        return ck_subtype(sub, super->option.inner);
    }

    /* T is subtype of T1 | T2 if T is subtype of some member */
    if (super->kind == CK_UNION) {
        for (int i = 0; i < super->union_.nmembers; i++) {
            if (ck_subtype(sub, super->union_.members[i])) return 1;
        }
        return 0;
    }

    /* union sub: all members must be subtypes */
    if (sub->kind == CK_UNION) {
        for (int i = 0; i < sub->union_.nmembers; i++) {
            if (!ck_subtype(sub->union_.members[i], super)) return 0;
        }
        return 1;
    }

    if (sub->kind != super->kind) return 0;

    switch (sub->kind) {
    case CK_ARRAY:
        return ck_subtype(sub->array.elem, super->array.elem);
    case CK_MAP:
        return ck_subtype(sub->map.key, super->map.key) &&
               ck_subtype(sub->map.val, super->map.val);
    case CK_TUPLE:
        if (sub->tuple.nelems != super->tuple.nelems) return 0;
        for (int i = 0; i < sub->tuple.nelems; i++)
            if (!ck_subtype(sub->tuple.elems[i], super->tuple.elems[i])) return 0;
        return 1;
    case CK_FUNCTION:
        if (sub->func.nparams != super->func.nparams) return 0;
        /* contravariant params, covariant return */
        for (int i = 0; i < sub->func.nparams; i++)
            if (!ck_subtype(super->func.params[i], sub->func.params[i])) return 0;
        return ck_subtype(sub->func.ret, super->func.ret);
    case CK_STRUCT:
        if (!sub->struct_.name || !super->struct_.name) return 0;
        return strcmp(sub->struct_.name, super->struct_.name) == 0;
    case CK_ENUM:
        if (!sub->enum_.name || !super->enum_.name) return 0;
        return strcmp(sub->enum_.name, super->enum_.name) == 0;
    default:
        return 1; /* primitives: same kind = same type */
    }
}

/* ----------------------------------------------------------------
 * Instantiation and generalization
 * ---------------------------------------------------------------- */

/* instantiate: replace generic type params with fresh vars */
CkType *ck_instantiate(CkContext *ctx, CkType *scheme) {
    if (!scheme) return NULL;
    scheme = ck_resolve(scheme);

    switch (scheme->kind) {
    case CK_GENERIC:
        return ck_var(ctx);
    case CK_ARRAY:
        return ck_array(ck_instantiate(ctx, scheme->array.elem));
    case CK_MAP:
        return ck_map(ck_instantiate(ctx, scheme->map.key),
                      ck_instantiate(ctx, scheme->map.val));
    case CK_TUPLE: {
        CkType **elems = xs_malloc(sizeof(CkType *) * (scheme->tuple.nelems > 0 ? scheme->tuple.nelems : 1));
        for (int i = 0; i < scheme->tuple.nelems; i++)
            elems[i] = ck_instantiate(ctx, scheme->tuple.elems[i]);
        CkType *t = ck_tuple(elems, scheme->tuple.nelems);
        free(elems);
        return t;
    }
    case CK_FUNCTION: {
        CkType **params = xs_malloc(sizeof(CkType *) * (scheme->func.nparams > 0 ? scheme->func.nparams : 1));
        for (int i = 0; i < scheme->func.nparams; i++)
            params[i] = ck_instantiate(ctx, scheme->func.params[i]);
        CkType *ret = ck_instantiate(ctx, scheme->func.ret);
        CkType *t = ck_function(params, scheme->func.nparams, ret);
        free(params);
        return t;
    }
    case CK_OPTION:
        return ck_option(ck_instantiate(ctx, scheme->option.inner));
    default:
        return scheme;
    }
}

/* generalize: check if vars in t are free (not bound in env).
   For now, a simplified version that just returns the type as-is. */
CkType *ck_generalize(CkTypeEnv *env, CkType *t) {
    (void)env;
    return t;
}

/* ----------------------------------------------------------------
 * Constraint management and solving
 * ---------------------------------------------------------------- */

void ck_add_constraint(CkContext *ctx, CkType *left, CkType *right, Span span) {
    CkConstraintSet *cs = &ctx->constraints;
    if (cs->len >= cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 32;
        cs->items = xs_realloc(cs->items, sizeof(CkConstraint) * cs->cap);
    }
    cs->items[cs->len].left = left;
    cs->items[cs->len].right = right;
    cs->items[cs->len].span = span;
    cs->len++;
}

int ck_solve_constraints(CkContext *ctx) {
    int failed = 0;
    for (int i = 0; i < ctx->constraints.len; i++) {
        CkConstraint *c = &ctx->constraints.items[i];
        if (!ck_unify(ctx, c->left, c->right)) {
            ck_type_mismatch(ctx, c->span, c->left, c->right);
            failed++;
        }
    }
    /* clear constraints after solving */
    ctx->constraints.len = 0;
    return failed == 0;
}

/* ----------------------------------------------------------------
 * Conversion from existing XsType / TypeExpr
 * ---------------------------------------------------------------- */

CkType *ck_from_xstype(XsType *t) {
    if (!t) return ck_any();
    switch (t->kind) {
    case TY_BOOL:   return ck_bool();
    case TY_I8: case TY_I16: case TY_I32: case TY_I64:
    case TY_U8: case TY_U16: case TY_U32: case TY_U64:
        return ck_int();
    case TY_F32: case TY_F64:
        return ck_float();
    case TY_STR:    return ck_string();
    case TY_UNIT:   return ck_null();
    case TY_NEVER:  return ck_never();
    case TY_DYN:    return ck_any();
    case TY_UNKNOWN: return ck_any();
    case TY_ARRAY:
        return ck_array(ck_from_xstype(t->array.inner));
    case TY_TUPLE: {
        CkType **elems = xs_malloc(sizeof(CkType *) * (t->tuple.nelems > 0 ? t->tuple.nelems : 1));
        for (int i = 0; i < t->tuple.nelems; i++)
            elems[i] = ck_from_xstype(t->tuple.elems[i]);
        CkType *r = ck_tuple(elems, t->tuple.nelems);
        free(elems);
        return r;
    }
    case TY_OPTION:
        return ck_option(ck_from_xstype(t->option.inner));
    case TY_FN: {
        CkType **params = xs_malloc(sizeof(CkType *) * (t->fn_.nparams > 0 ? t->fn_.nparams : 1));
        for (int i = 0; i < t->fn_.nparams; i++)
            params[i] = ck_from_xstype(t->fn_.params[i]);
        CkType *ret = ck_from_xstype(t->fn_.ret);
        CkType *r = ck_function(params, t->fn_.nparams, ret);
        free(params);
        return r;
    }
    case TY_NAMED: {
        if (!t->named.name) return ck_any();
        /* check common names */
        if (strcmp(t->named.name, "int") == 0 || strcmp(t->named.name, "i64") == 0 ||
            strcmp(t->named.name, "i32") == 0) return ck_int();
        if (strcmp(t->named.name, "float") == 0 || strcmp(t->named.name, "f64") == 0 ||
            strcmp(t->named.name, "f32") == 0) return ck_float();
        if (strcmp(t->named.name, "str") == 0 || strcmp(t->named.name, "string") == 0)
            return ck_string();
        if (strcmp(t->named.name, "bool") == 0) return ck_bool();
        if (strcmp(t->named.name, "any") == 0 || strcmp(t->named.name, "dyn") == 0)
            return ck_any();
        if (strcmp(t->named.name, "Map") == 0 || strcmp(t->named.name, "map") == 0) {
            CkType *k = t->named.nargs > 0 ? ck_from_xstype(t->named.args[0]) : ck_any();
            CkType *v = t->named.nargs > 1 ? ck_from_xstype(t->named.args[1]) : ck_any();
            return ck_map(k, v);
        }
        if (strcmp(t->named.name, "Array") == 0 || strcmp(t->named.name, "array") == 0) {
            CkType *elem = t->named.nargs > 0 ? ck_from_xstype(t->named.args[0]) : ck_any();
            return ck_array(elem);
        }
        return ck_struct(t->named.name, NULL, NULL, 0);
    }
    case TY_GENERIC:
        return ck_generic(t->generic.name, 0);
    case TY_RESULT: {
        /* model Result<T,E> as a named struct */
        return ck_struct("Result", NULL, NULL, 0);
    }
    default:
        return ck_any();
    }
}

CkType *ck_from_texpr(TypeExpr *te) {
    if (!te) return ck_any();
    switch (te->kind) {
    case TEXPR_NAMED: {
        if (!te->name) return ck_any();
        if (strcmp(te->name, "int") == 0 || strcmp(te->name, "i64") == 0 ||
            strcmp(te->name, "i32") == 0) return ck_int();
        if (strcmp(te->name, "float") == 0 || strcmp(te->name, "f64") == 0 ||
            strcmp(te->name, "f32") == 0) return ck_float();
        if (strcmp(te->name, "str") == 0 || strcmp(te->name, "string") == 0)
            return ck_string();
        if (strcmp(te->name, "bool") == 0) return ck_bool();
        if (strcmp(te->name, "any") == 0 || strcmp(te->name, "dyn") == 0)
            return ck_any();
        if (strcmp(te->name, "never") == 0) return ck_never();
        if (strcmp(te->name, "array") == 0 || strcmp(te->name, "Array") == 0) {
            CkType *elem = te->nargs > 0 ? ck_from_texpr(te->args[0]) : ck_any();
            return ck_array(elem);
        }
        if (strcmp(te->name, "map") == 0 || strcmp(te->name, "Map") == 0) {
            CkType *k = te->nargs > 0 ? ck_from_texpr(te->args[0]) : ck_any();
            CkType *v = te->nargs > 1 ? ck_from_texpr(te->args[1]) : ck_any();
            return ck_map(k, v);
        }
        /* generic type args */
        if (te->nargs > 0) {
            /* treat as struct with type params (simplified) */
            return ck_struct(te->name, NULL, NULL, 0);
        }
        return ck_struct(te->name, NULL, NULL, 0);
    }
    case TEXPR_ARRAY:
        return ck_array(ck_from_texpr(te->inner));
    case TEXPR_OPTION:
        return ck_option(ck_from_texpr(te->inner));
    case TEXPR_TUPLE: {
        CkType **elems = xs_malloc(sizeof(CkType *) * (te->nelems > 0 ? te->nelems : 1));
        for (int i = 0; i < te->nelems; i++)
            elems[i] = ck_from_texpr(te->elems[i]);
        CkType *r = ck_tuple(elems, te->nelems);
        free(elems);
        return r;
    }
    case TEXPR_FN: {
        CkType **params = xs_malloc(sizeof(CkType *) * (te->nparams > 0 ? te->nparams : 1));
        for (int i = 0; i < te->nparams; i++)
            params[i] = ck_from_texpr(te->params[i]);
        CkType *ret = te->ret ? ck_from_texpr(te->ret) : ck_null();
        CkType *r = ck_function(params, te->nparams, ret);
        free(params);
        return r;
    }
    case TEXPR_INFER:
        return ck_any();
    default:
        return ck_any();
    }
}

/* ----------------------------------------------------------------
 * Expression type inference
 * ---------------------------------------------------------------- */

static CkType *check_binop(CkContext *ctx, Node *n) {
    const char *op = n->binop.op;
    CkType *lt = ck_check_expr(ctx, n->binop.left);
    CkType *rt = ck_check_expr(ctx, n->binop.right);

    /* comparison ops always produce bool */
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
        strcmp(op, "&&") == 0 || strcmp(op, "||") == 0 ||
        strcmp(op, "in") == 0 || strcmp(op, "not in") == 0 ||
        strcmp(op, "is") == 0)
        return ck_bool();

    /* arithmetic ops: check both sides are numeric */
    if (strcmp(op, "+") == 0) {
        /* string concatenation */
        lt = ck_resolve(lt);
        rt = ck_resolve(rt);
        if (lt->kind == CK_STRING || rt->kind == CK_STRING)
            return ck_string();
        /* array concat */
        if (lt->kind == CK_ARRAY) return lt;
        return lt; /* numeric */
    }

    if (strcmp(op, "-") == 0 || strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
        strcmp(op, "%") == 0 || strcmp(op, "**") == 0) {
        lt = ck_resolve(lt);
        rt = ck_resolve(rt);
        if (lt->kind == CK_FLOAT || rt->kind == CK_FLOAT)
            return ck_float();
        return ck_int();
    }

    /* bitwise ops */
    if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
        strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)
        return ck_int();

    /* string concat operator */
    if (strcmp(op, "++") == 0)
        return ck_string();

    /* pipe operator */
    if (strcmp(op, "|>") == 0)
        return ck_any(); /* depends on rhs function return type */

    /* null coalescing */
    if (strcmp(op, "??") == 0)
        return lt;

    return lt;
}

static CkType *check_call(CkContext *ctx, Node *n) {
    CkType *callee = ck_check_expr(ctx, n->call.callee);
    callee = ck_resolve(callee);

    /* check each argument */
    for (int i = 0; i < n->call.args.len; i++)
        ck_check_expr(ctx, n->call.args.items[i]);

    if (callee->kind == CK_FUNCTION) {
        /* check arg count */
        int nargs = n->call.args.len;
        int nparams = callee->func.nparams;
        if (nargs > nparams && nparams > 0) {
            /* variadic or error - for now just warn if way off */
        }
        /* unify arg types with param types */
        int limit = nargs < nparams ? nargs : nparams;
        for (int i = 0; i < limit; i++) {
            CkType *arg_ty = ck_check_expr(ctx, n->call.args.items[i]);
            ck_add_constraint(ctx, callee->func.params[i], arg_ty,
                              n->call.args.items[i]->span);
        }
        return callee->func.ret;
    }

    /* if callee is a struct name, return instance of that struct */
    if (callee->kind == CK_STRUCT)
        return callee;

    /* callee is a var that hasn't been resolved yet */
    if (callee->kind == CK_VAR) {
        CkType *ret = ck_var(ctx);
        return ret;
    }

    return ck_any();
}

static CkType *check_field(CkContext *ctx, Node *n) {
    CkType *obj = ck_check_expr(ctx, n->field.obj);
    obj = ck_resolve(obj);

    if (!n->field.name) return ck_any();

    /* tuple index */
    if (obj->kind == CK_TUPLE) {
        char *end;
        long idx = strtol(n->field.name, &end, 10);
        if (*end == '\0' && idx >= 0 && idx < obj->tuple.nelems)
            return obj->tuple.elems[idx];
    }

    /* struct field lookup via symtab */
    if (obj->kind == CK_STRUCT && obj->struct_.name && ctx->symtab) {
        Symbol *sym = sym_lookup(ctx->symtab, obj->struct_.name);
        if (sym && sym->decl && VAL_TAG(sym->decl) == NODE_STRUCT_DECL) {
            NodePairList *fl = &sym->decl->struct_decl.fields;
            TypeExpr **ft = sym->decl->struct_decl.field_types;
            int nft = sym->decl->struct_decl.n_field_types;
            for (int i = 0; i < fl->len; i++) {
                if (fl->items[i].key && strcmp(fl->items[i].key, n->field.name) == 0) {
                    if (ft && i < nft && ft[i])
                        return ck_from_texpr(ft[i]);
                    return ck_any();
                }
            }
        }
        /* named struct field - might not be tracked */
        for (int i = 0; i < obj->struct_.nfields; i++) {
            if (obj->struct_.field_names[i] &&
                strcmp(obj->struct_.field_names[i], n->field.name) == 0)
                return obj->struct_.fields[i];
        }
    }

    /* string methods */
    if (obj->kind == CK_STRING) {
        if (strcmp(n->field.name, "len") == 0) return ck_int();
    }

    /* array methods */
    if (obj->kind == CK_ARRAY) {
        if (strcmp(n->field.name, "len") == 0) return ck_int();
    }

    return ck_any();
}

static CkType *check_method_call(CkContext *ctx, Node *n) {
    CkType *recv = ck_check_expr(ctx, n->method_call.obj);
    recv = ck_resolve(recv);
    const char *method = n->method_call.method;

    for (int i = 0; i < n->method_call.args.len; i++)
        ck_check_expr(ctx, n->method_call.args.items[i]);

    if (!method) return ck_any();

    /* common method return types */
    if (strcmp(method, "len") == 0 || strcmp(method, "count") == 0 ||
        strcmp(method, "index") == 0 || strcmp(method, "find") == 0)
        return ck_int();
    if (strcmp(method, "contains") == 0 || strcmp(method, "is_empty") == 0 ||
        strcmp(method, "starts_with") == 0 || strcmp(method, "ends_with") == 0)
        return ck_bool();
    if (strcmp(method, "to_str") == 0 || strcmp(method, "trim") == 0 ||
        strcmp(method, "upper") == 0 || strcmp(method, "lower") == 0 ||
        strcmp(method, "join") == 0 || strcmp(method, "replace") == 0)
        return ck_string();
    if (strcmp(method, "push") == 0 || strcmp(method, "sort") == 0 ||
        strcmp(method, "reverse") == 0 || strcmp(method, "clear") == 0)
        return recv;
    if (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0)
        return recv;
    if (strcmp(method, "pop") == 0) {
        if (recv->kind == CK_ARRAY) return recv->array.elem;
        return ck_any();
    }

    /* struct method lookup */
    if (recv->kind == CK_STRUCT && recv->struct_.name && ctx->symtab) {
        char qual[256];
        snprintf(qual, sizeof qual, "%s.%s", recv->struct_.name, method);
        Symbol *msym = sym_lookup(ctx->symtab, qual);
        if (msym && msym->type && msym->type->kind == TY_FN && msym->type->fn_.ret) {
            return ck_from_xstype(msym->type->fn_.ret);
        }
    }

    return ck_any();
}

CkType *ck_check_expr(CkContext *ctx, Node *expr) {
    if (!expr) return ck_any();

    switch (VAL_TAG(expr)) {
    case NODE_LIT_INT:    return ck_int();
    case NODE_LIT_BIGINT: return ck_int();
    case NODE_LIT_FLOAT:  return ck_float();
    case NODE_LIT_STRING: return ck_string();
    case NODE_LIT_BOOL:   return ck_bool();
    case NODE_LIT_CHAR:   return ck_string();
    case NODE_LIT_NULL:   return ck_null();
    case NODE_LIT_REGEX:  return ck_struct("Regex", NULL, NULL, 0);

    case NODE_LIT_ARRAY: {
        if (expr->lit_array.elems.len > 0) {
            CkType *elem = ck_check_expr(ctx, expr->lit_array.elems.items[0]);
            for (int i = 1; i < expr->lit_array.elems.len; i++)
                ck_check_expr(ctx, expr->lit_array.elems.items[i]);
            return ck_array(elem);
        }
        return ck_array(ck_any());
    }
    case NODE_LIT_TUPLE: {
        int ne = expr->lit_array.elems.len;
        CkType **elems = xs_malloc(sizeof(CkType *) * (ne > 0 ? ne : 1));
        for (int i = 0; i < ne; i++)
            elems[i] = ck_check_expr(ctx, expr->lit_array.elems.items[i]);
        CkType *t = ck_tuple(elems, ne);
        free(elems);
        return t;
    }
    case NODE_LIT_MAP: {
        CkType *kt = ck_any(), *vt = ck_any();
        for (int i = 0; i < expr->lit_map.keys.len; i++) {
            Node *k = expr->lit_map.keys.items[i];
            if (k && VAL_TAG(k) != NODE_SPREAD && i < expr->lit_map.vals.len) {
                kt = ck_check_expr(ctx, k);
                if (expr->lit_map.vals.items[i])
                    vt = ck_check_expr(ctx, expr->lit_map.vals.items[i]);
                break;
            }
        }
        return ck_map(kt, vt);
    }
    case NODE_IDENT: {
        if (!expr->ident.name) return ck_any();
        /* check local type env first */
        CkType *t = ck_env_lookup(ctx->env, expr->ident.name);
        if (t) return t;
        /* fall back to symtab */
        if (ctx->symtab) {
            Symbol *sym = sym_lookup(ctx->symtab, expr->ident.name);
            if (sym && sym->type) return ck_from_xstype(sym->type);
            if (sym && sym->decl) {
                if (sym->kind == SYM_STRUCT)
                    return ck_struct(expr->ident.name, NULL, NULL, 0);
                if (sym->kind == SYM_ENUM)
                    return ck_enum(expr->ident.name);
            }
        }
        return ck_any();
    }
    case NODE_BINOP:
        return check_binop(ctx, expr);

    case NODE_UNARY:
        if (strcmp(expr->unary.op, "!") == 0) return ck_bool();
        return ck_check_expr(ctx, expr->unary.expr);

    case NODE_CALL:
        return check_call(ctx, expr);

    case NODE_METHOD_CALL:
        return check_method_call(ctx, expr);

    case NODE_FIELD:
        return check_field(ctx, expr);

    case NODE_INDEX: {
        CkType *obj = ck_check_expr(ctx, expr->index.obj);
        ck_check_expr(ctx, expr->index.index);
        obj = ck_resolve(obj);
        if (obj->kind == CK_ARRAY) return obj->array.elem;
        if (obj->kind == CK_MAP) return obj->map.val;
        if (obj->kind == CK_TUPLE && expr->index.index &&
            VAL_TAG(expr->index.index) == NODE_LIT_INT) {
            int64_t idx = expr->index.index->lit_int.ival;
            if (idx >= 0 && idx < obj->tuple.nelems)
                return obj->tuple.elems[idx];
        }
        if (obj->kind == CK_STRING) return ck_string();
        return ck_any();
    }
    case NODE_IF: {
        ck_check_expr(ctx, expr->if_expr.cond);
        CkType *then_ty = ck_any();
        if (expr->if_expr.then) then_ty = ck_check_expr(ctx, expr->if_expr.then);
        if (expr->if_expr.else_branch) {
            CkType *else_ty = ck_check_expr(ctx, expr->if_expr.else_branch);
            /* unify branch types */
            CkType *rt = ck_resolve(then_ty);
            CkType *re = ck_resolve(else_ty);
            if (rt->kind != CK_ANY && re->kind != CK_ANY) {
                ck_add_constraint(ctx, then_ty, else_ty, expr->span);
            }
        }
        return then_ty;
    }
    case NODE_BLOCK: {
        for (int i = 0; i < expr->block.stmts.len; i++)
            ck_check_stmt(ctx, expr->block.stmts.items[i]);
        if (expr->block.expr)
            return ck_check_expr(ctx, expr->block.expr);
        if (expr->block.stmts.len > 0) {
            Node *last = expr->block.stmts.items[expr->block.stmts.len - 1];
            if (last && VAL_TAG(last) == NODE_EXPR_STMT)
                return ck_check_expr(ctx, last->expr_stmt.expr);
        }
        return ck_null();
    }
    case NODE_LAMBDA: {
        CkTypeEnv *inner = ck_env_new(ctx->env);
        CkTypeEnv *old = ctx->env;
        ctx->env = inner;
        int np = expr->lambda.params.len;
        CkType **params = xs_malloc(sizeof(CkType *) * (np > 0 ? np : 1));
        for (int i = 0; i < np; i++) {
            Param *pm = &expr->lambda.params.items[i];
            CkType *pt = pm->type_ann ? ck_from_texpr(pm->type_ann) : ck_var(ctx);
            params[i] = pt;
            if (pm->name) ck_env_bind(inner, pm->name, pt);
        }
        CkType *ret = expr->lambda.body
                     ? ck_check_expr(ctx, expr->lambda.body) : ck_null();
        CkType *fn = ck_function(params, np, ret);
        free(params);
        ctx->env = old;
        ck_env_free(inner);
        return fn;
    }
    case NODE_CAST: {
        if (expr->cast.expr) ck_check_expr(ctx, expr->cast.expr);
        if (expr->cast.type_name) {
            if (strcmp(expr->cast.type_name, "int") == 0 ||
                strcmp(expr->cast.type_name, "i64") == 0 ||
                strcmp(expr->cast.type_name, "i32") == 0) return ck_int();
            if (strcmp(expr->cast.type_name, "float") == 0 ||
                strcmp(expr->cast.type_name, "f64") == 0) return ck_float();
            if (strcmp(expr->cast.type_name, "str") == 0 ||
                strcmp(expr->cast.type_name, "string") == 0) return ck_string();
            if (strcmp(expr->cast.type_name, "bool") == 0) return ck_bool();
            return ck_struct(expr->cast.type_name, NULL, NULL, 0);
        }
        return ck_any();
    }
    case NODE_MATCH: {
        ck_check_expr(ctx, expr->match.subject);
        CkType *result = ck_any();
        for (int i = 0; i < expr->match.arms.len; i++) {
            MatchArm *arm = &expr->match.arms.items[i];
            if (arm->guard) ck_check_expr(ctx, arm->guard);
            if (arm->body) {
                CkType *arm_ty = ck_check_expr(ctx, arm->body);
                if (i == 0) result = arm_ty;
                else {
                    CkType *rr = ck_resolve(result);
                    CkType *ra = ck_resolve(arm_ty);
                    if (rr->kind != CK_ANY && ra->kind != CK_ANY)
                        ck_add_constraint(ctx, result, arm_ty, arm->body->span);
                }
            }
        }
        return result;
    }
    case NODE_RANGE:
        if (expr->range.start) ck_check_expr(ctx, expr->range.start);
        if (expr->range.end) ck_check_expr(ctx, expr->range.end);
        return ck_array(ck_int());

    case NODE_STRUCT_INIT: {
        for (int i = 0; i < expr->struct_init.fields.len; i++) {
            if (expr->struct_init.fields.items[i].val)
                ck_check_expr(ctx, expr->struct_init.fields.items[i].val);
        }
        if (expr->struct_init.rest) ck_check_expr(ctx, expr->struct_init.rest);
        if (expr->struct_init.path)
            return ck_struct(expr->struct_init.path, NULL, NULL, 0);
        return ck_any();
    }
    case NODE_SPREAD:
        if (expr->spread.expr) return ck_check_expr(ctx, expr->spread.expr);
        return ck_any();

    case NODE_INTERP_STRING:
        return ck_string();

    case NODE_SCOPE:
        return ck_any();

    case NODE_AWAIT:
        if (expr->await_.expr) return ck_check_expr(ctx, expr->await_.expr);
        return ck_any();

    case NODE_LIST_COMP: {
        for (int i = 0; i < expr->list_comp.clause_iters.len; i++)
            ck_check_expr(ctx, expr->list_comp.clause_iters.items[i]);
        CkType *elem = expr->list_comp.element
                      ? ck_check_expr(ctx, expr->list_comp.element) : ck_any();
        return ck_array(elem);
    }
    case NODE_MAP_COMP:
        return ck_map(ck_any(), ck_any());

    case NODE_DO_EXPR:
        if (expr->do_expr.body)
            return ck_check_expr(ctx, expr->do_expr.body);
        return ck_null();

    default:
        return ck_any();
    }
}

/* ----------------------------------------------------------------
 * Statement type checking
 * ---------------------------------------------------------------- */

/* return type stack for functions */
#define CK_RET_STACK_MAX 64
static CkType *ck_ret_stack[CK_RET_STACK_MAX];
static int     ck_ret_depth = 0;

static void ck_ret_push(CkType *t) {
    if (ck_ret_depth < CK_RET_STACK_MAX) ck_ret_stack[ck_ret_depth++] = t;
}
static void ck_ret_pop(void) {
    if (ck_ret_depth > 0) ck_ret_depth--;
}
static CkType *ck_ret_current(void) {
    return ck_ret_depth > 0 ? ck_ret_stack[ck_ret_depth - 1] : NULL;
}

void ck_check_stmt(CkContext *ctx, Node *stmt) {
    if (!stmt) return;

    switch (VAL_TAG(stmt)) {
    case NODE_LET: case NODE_VAR: {
        CkType *init_ty = ck_any();
        if (stmt->let.value)
            init_ty = ck_check_expr(ctx, stmt->let.value);
        if (stmt->let.type_ann) {
            CkType *ann = ck_from_texpr(stmt->let.type_ann);
            CkType *ra = ck_resolve(ann);
            CkType *ri = ck_resolve(init_ty);
            if (ra->kind != CK_ANY && ri->kind != CK_ANY && stmt->let.value) {
                ck_add_constraint(ctx, ann, init_ty, stmt->let.value->span);
            }
            if (stmt->let.name)
                ck_env_bind(ctx->env, stmt->let.name, ann);
        } else {
            if (stmt->let.name)
                ck_env_bind(ctx->env, stmt->let.name, init_ty);
        }
        break;
    }
    case NODE_CONST: {
        CkType *init_ty = ck_any();
        if (stmt->const_.value)
            init_ty = ck_check_expr(ctx, stmt->const_.value);
        if (stmt->const_.type_ann) {
            CkType *ann = ck_from_texpr(stmt->const_.type_ann);
            if (stmt->const_.name)
                ck_env_bind(ctx->env, stmt->const_.name, ann);
        } else {
            if (stmt->const_.name)
                ck_env_bind(ctx->env, stmt->const_.name, init_ty);
        }
        break;
    }
    case NODE_RETURN: {
        CkType *expected = ck_ret_current();
        if (stmt->ret.value) {
            CkType *actual = ck_check_expr(ctx, stmt->ret.value);
            if (expected) {
                CkType *re = ck_resolve(expected);
                CkType *ra = ck_resolve(actual);
                if (re->kind != CK_ANY && ra->kind != CK_ANY)
                    ck_add_constraint(ctx, expected, actual, stmt->ret.value->span);
            }
        }
        break;
    }
    case NODE_EXPR_STMT:
        if (stmt->expr_stmt.expr)
            ck_check_expr(ctx, stmt->expr_stmt.expr);
        break;

    case NODE_IF:
        ck_check_expr(ctx, (Node *)stmt);
        break;

    case NODE_WHILE:
        if (stmt->while_loop.cond) ck_check_expr(ctx, stmt->while_loop.cond);
        if (stmt->while_loop.body) ck_check_stmt(ctx, stmt->while_loop.body);
        break;

    case NODE_FOR:
        if (stmt->for_loop.iter) ck_check_expr(ctx, stmt->for_loop.iter);
        if (stmt->for_loop.body) ck_check_stmt(ctx, stmt->for_loop.body);
        break;

    case NODE_LOOP:
        if (stmt->loop.body) ck_check_stmt(ctx, stmt->loop.body);
        break;

    case NODE_BLOCK: {
        CkTypeEnv *inner = ck_env_new(ctx->env);
        CkTypeEnv *old = ctx->env;
        ctx->env = inner;
        for (int i = 0; i < stmt->block.stmts.len; i++)
            ck_check_stmt(ctx, stmt->block.stmts.items[i]);
        if (stmt->block.expr)
            ck_check_expr(ctx, stmt->block.expr);
        ctx->env = old;
        ck_env_free(inner);
        break;
    }
    case NODE_FN_DECL:
        ck_check_fn(ctx, stmt);
        break;

    case NODE_MATCH:
        ck_check_expr(ctx, (Node *)stmt);
        break;

    case NODE_TRY:
        if (stmt->try_.body) ck_check_stmt(ctx, stmt->try_.body);
        for (int i = 0; i < stmt->try_.catch_arms.len; i++) {
            if (stmt->try_.catch_arms.items[i].body)
                ck_check_stmt(ctx, stmt->try_.catch_arms.items[i].body);
        }
        if (stmt->try_.finally_block) ck_check_stmt(ctx, stmt->try_.finally_block);
        break;

    case NODE_DEFER:
        if (stmt->defer_.body) ck_check_stmt(ctx, stmt->defer_.body);
        break;

    case NODE_ASSIGN:
        if (stmt->assign.target) ck_check_expr(ctx, stmt->assign.target);
        if (stmt->assign.value) ck_check_expr(ctx, stmt->assign.value);
        break;

    case NODE_THROW:
        if (stmt->throw_.value) ck_check_expr(ctx, stmt->throw_.value);
        break;

    case NODE_BREAK: case NODE_CONTINUE:
        break;

    case NODE_STRUCT_DECL: {
        /* register struct type in env */
        if (stmt->struct_decl.name) {
            int nf = stmt->struct_decl.fields.len;
            CkType **fields = NULL;
            char **names = NULL;
            if (nf > 0) {
                fields = xs_malloc(sizeof(CkType *) * nf);
                names = xs_malloc(sizeof(char *) * nf);
                for (int i = 0; i < nf; i++) {
                    names[i] = stmt->struct_decl.fields.items[i].key
                             ? xs_strdup(stmt->struct_decl.fields.items[i].key)
                             : NULL;
                    if (stmt->struct_decl.field_types &&
                        i < stmt->struct_decl.n_field_types &&
                        stmt->struct_decl.field_types[i]) {
                        fields[i] = ck_from_texpr(stmt->struct_decl.field_types[i]);
                    } else {
                        fields[i] = ck_any();
                    }
                }
            }
            CkType *st = ck_struct(stmt->struct_decl.name, fields, names, nf);
            ck_env_bind(ctx->env, stmt->struct_decl.name, st);
            if (fields) {
                for (int i = 0; i < nf; i++) free(names[i]);
                free(fields);
                free(names);
            }
        }
        break;
    }
    case NODE_ENUM_DECL: {
        if (stmt->enum_decl.name)
            ck_env_bind(ctx->env, stmt->enum_decl.name, ck_enum(stmt->enum_decl.name));
        break;
    }
    case NODE_TRAIT_DECL: {
        if (stmt->trait_decl.name)
            ck_env_bind(ctx->env, stmt->trait_decl.name, ck_trait(stmt->trait_decl.name));
        break;
    }
    case NODE_IMPL_DECL:
        for (int i = 0; i < stmt->impl_decl.members.len; i++)
            ck_check_stmt(ctx, stmt->impl_decl.members.items[i]);
        break;

    case NODE_CLASS_DECL:
        for (int i = 0; i < stmt->class_decl.members.len; i++)
            ck_check_stmt(ctx, stmt->class_decl.members.items[i]);
        break;

    case NODE_IMPORT: case NODE_USE: case NODE_MODULE_DECL:
    case NODE_TYPE_ALIAS: case NODE_EFFECT_DECL:
        break;

    case NODE_SPAWN:
        if (stmt->spawn_.expr) ck_check_expr(ctx, stmt->spawn_.expr);
        break;

    case NODE_PROGRAM:
        for (int i = 0; i < stmt->program.stmts.len; i++)
            ck_check_stmt(ctx, stmt->program.stmts.items[i]);
        break;

    default:
        /* try as expression */
        ck_check_expr(ctx, stmt);
        break;
    }
}

/* ----------------------------------------------------------------
 * Function checking
 * ---------------------------------------------------------------- */

void ck_check_fn(CkContext *ctx, Node *fn_decl) {
    if (!fn_decl || VAL_TAG(fn_decl) != NODE_FN_DECL) return;

    CkTypeEnv *fn_env = ck_env_new(ctx->env);
    CkTypeEnv *old = ctx->env;
    ctx->env = fn_env;

    /* bind params */
    int np = fn_decl->fn_decl.params.len;
    CkType **param_types = xs_malloc(sizeof(CkType *) * (np > 0 ? np : 1));
    for (int i = 0; i < np; i++) {
        Param *pm = &fn_decl->fn_decl.params.items[i];
        CkType *pt = pm->type_ann ? ck_from_texpr(pm->type_ann) : ck_var(ctx);
        param_types[i] = pt;
        if (pm->name) ck_env_bind(fn_env, pm->name, pt);
    }

    /* return type */
    CkType *ret_type = fn_decl->fn_decl.ret_type
                     ? ck_from_texpr(fn_decl->fn_decl.ret_type)
                     : ck_var(ctx);

    ck_ret_push(ret_type);

    /* bind function itself for recursion */
    if (fn_decl->fn_decl.name) {
        CkType *fn_type = ck_function(param_types, np, ret_type);
        ck_env_bind(fn_env, fn_decl->fn_decl.name, fn_type);
        /* also bind in parent for later lookups */
        ck_env_bind(old, fn_decl->fn_decl.name, fn_type);
    }

    /* check body */
    if (fn_decl->fn_decl.body) {
        ck_check_stmt(ctx, fn_decl->fn_decl.body);
    }

    ck_ret_pop();
    free(param_types);

    /* solve accumulated constraints from this function */
    ck_solve_constraints(ctx);

    ctx->env = old;
    ck_env_free(fn_env);
}

/* ----------------------------------------------------------------
 * Context
 * ---------------------------------------------------------------- */

CkContext *ck_context_new(SymTab *st, SemaCtx *sema, int strict) {
    CkContext *ctx = xs_calloc(1, sizeof(CkContext));
    ctx->env = ck_env_new(NULL);
    ctx->symtab = st;
    ctx->sema = sema;
    ctx->strict = strict;
    ctx->next_var_id = 0;
    ctx->next_generic_id = 0;

    /* seed builtins */
    ck_env_bind(ctx->env, "print", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "println", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "len", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_int()));
    ck_env_bind(ctx->env, "str", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_string()));
    ck_env_bind(ctx->env, "int", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_int()));
    ck_env_bind(ctx->env, "float", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_float()));
    ck_env_bind(ctx->env, "bool", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "range", ck_function(
        (CkType *[]){ ck_int(), ck_int() }, 2, ck_array(ck_int())));
    ck_env_bind(ctx->env, "assert", ck_function(
        (CkType *[]){ ck_bool() }, 1, ck_null()));
    ck_env_bind(ctx->env, "assert_eq", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_null()));
    ck_env_bind(ctx->env, "type", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_string()));
    ck_env_bind(ctx->env, "typeof", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_string()));
    ck_env_bind(ctx->env, "repr", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_string()));
    ck_env_bind(ctx->env, "abs", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "min", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_any()));
    ck_env_bind(ctx->env, "max", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_any()));
    ck_env_bind(ctx->env, "format", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_string()));
    ck_env_bind(ctx->env, "input", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_string()));
    ck_env_bind(ctx->env, "exit", ck_function(
        (CkType *[]){ ck_int() }, 1, ck_never()));
    ck_env_bind(ctx->env, "panic", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_never()));
    ck_env_bind(ctx->env, "copy", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "clone", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "map", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_array(ck_any())));
    ck_env_bind(ctx->env, "filter", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_array(ck_any())));
    ck_env_bind(ctx->env, "reduce", ck_function(
        (CkType *[]){ ck_any(), ck_any(), ck_any() }, 3, ck_any()));
    ck_env_bind(ctx->env, "sorted", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "keys", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_string())));
    ck_env_bind(ctx->env, "values", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "enumerate", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "zip", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_array(ck_any())));
    ck_env_bind(ctx->env, "flatten", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "sum", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "chars", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_array(ck_string())));
    ck_env_bind(ctx->env, "bytes", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_array(ck_int())));
    ck_env_bind(ctx->env, "sqrt", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_float()));
    ck_env_bind(ctx->env, "pow", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_any()));
    ck_env_bind(ctx->env, "floor", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_int()));
    ck_env_bind(ctx->env, "ceil", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_int()));
    ck_env_bind(ctx->env, "round", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_int()));
    ck_env_bind(ctx->env, "log", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_float()));
    ck_env_bind(ctx->env, "sin", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_float()));
    ck_env_bind(ctx->env, "cos", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_float()));
    ck_env_bind(ctx->env, "tan", ck_function(
        (CkType *[]){ ck_float() }, 1, ck_float()));
    ck_env_bind(ctx->env, "Ok", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_struct("Result", NULL, NULL, 0)));
    ck_env_bind(ctx->env, "Err", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_struct("Result", NULL, NULL, 0)));
    ck_env_bind(ctx->env, "Some", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_option(ck_any())));
    ck_env_bind(ctx->env, "None", ck_null());
    ck_env_bind(ctx->env, "true", ck_bool());
    ck_env_bind(ctx->env, "false", ck_bool());
    ck_env_bind(ctx->env, "null", ck_null());
    ck_env_bind(ctx->env, "contains", ck_function(
        (CkType *[]){ ck_any(), ck_any() }, 2, ck_bool()));
    ck_env_bind(ctx->env, "channel", ck_function(
        (CkType *[]){}, 0, ck_any()));
    ck_env_bind(ctx->env, "signal", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "derived", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "todo", ck_function(
        (CkType *[]){}, 0, ck_never()));
    ck_env_bind(ctx->env, "unreachable", ck_function(
        (CkType *[]){}, 0, ck_never()));
    ck_env_bind(ctx->env, "is_null", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_int", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_float", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_str", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_bool", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_array", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "is_fn", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_bool()));
    ck_env_bind(ctx->env, "ord", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_int()));
    ck_env_bind(ctx->env, "chr", ck_function(
        (CkType *[]){ ck_int() }, 1, ck_string()));
    ck_env_bind(ctx->env, "array", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "vec", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));
    ck_env_bind(ctx->env, "clear", ck_function(
        (CkType *[]){}, 0, ck_null()));
    ck_env_bind(ctx->env, "dbg", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_any()));
    ck_env_bind(ctx->env, "pprint", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "type_of", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_string()));
    ck_env_bind(ctx->env, "eprint", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "eprintln", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "print_no_nl", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_null()));
    ck_env_bind(ctx->env, "sprintf", ck_function(
        (CkType *[]){ ck_string() }, 1, ck_string()));
    ck_env_bind(ctx->env, "entries", ck_function(
        (CkType *[]){ ck_any() }, 1, ck_array(ck_any())));

    return ctx;
}

void ck_context_free(CkContext *ctx) {
    if (!ctx) return;
    ck_env_free(ctx->env);
    free(ctx->constraints.items);
    for (int i = 0; i < ctx->errors.nerrors; i++)
        free(ctx->errors.messages[i]);
    free(ctx->errors.messages);
    free(ctx->errors.spans);
    free(ctx);
}

/* ----------------------------------------------------------------
 * Top-level entry point
 * ---------------------------------------------------------------- */

void ck_check_program(Node *program, SymTab *st, SemaCtx *sema, int strict) {
    if (!program) return;

    CkContext *ctx = ck_context_new(st, sema, strict);
    ck_ret_depth = 0;

    /* first pass: register all top-level declarations */
    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len; i++) {
            Node *s = program->program.stmts.items[i];
            if (!s) continue;
            if (VAL_TAG(s) == NODE_FN_DECL && s->fn_decl.name) {
                /* pre-register function type */
                int np = s->fn_decl.params.len;
                CkType **pts = xs_malloc(sizeof(CkType *) * (np > 0 ? np : 1));
                for (int j = 0; j < np; j++) {
                    Param *pm = &s->fn_decl.params.items[j];
                    pts[j] = pm->type_ann ? ck_from_texpr(pm->type_ann) : ck_any();
                }
                CkType *ret = s->fn_decl.ret_type
                            ? ck_from_texpr(s->fn_decl.ret_type) : ck_any();
                ck_env_bind(ctx->env, s->fn_decl.name,
                            ck_function(pts, np, ret));
                free(pts);
            }
            if (VAL_TAG(s) == NODE_STRUCT_DECL && s->struct_decl.name)
                ck_env_bind(ctx->env, s->struct_decl.name,
                            ck_struct(s->struct_decl.name, NULL, NULL, 0));
            if (VAL_TAG(s) == NODE_ENUM_DECL && s->enum_decl.name)
                ck_env_bind(ctx->env, s->enum_decl.name,
                            ck_enum(s->enum_decl.name));
            if (VAL_TAG(s) == NODE_TRAIT_DECL && s->trait_decl.name)
                ck_env_bind(ctx->env, s->trait_decl.name,
                            ck_trait(s->trait_decl.name));
        }
    }

    /* second pass: check all statements */
    if (VAL_TAG(program) == NODE_PROGRAM) {
        for (int i = 0; i < program->program.stmts.len; i++)
            ck_check_stmt(ctx, program->program.stmts.items[i]);
    } else {
        ck_check_stmt(ctx, program);
    }

    /* solve remaining constraints */
    ck_solve_constraints(ctx);

    /* emit errors through the diagnostic system if available */
    if (sema && sema->diag) {
        for (int i = 0; i < ctx->errors.nerrors; i++) {
            Diagnostic *d = diag_new(DIAG_WARNING, DIAG_PHASE_SEMANTIC, "T0020",
                                     "%s", ctx->errors.messages[i]);
            diag_annotate(d, ctx->errors.spans[i], 1, "%s", ctx->errors.messages[i]);
            diag_emit(sema->diag, d);
        }
    }

    ck_context_free(ctx);
}

/* ----------------------------------------------------------------
 * Extended constraint types
 *
 * Beyond simple equality constraints, the deep checker supports:
 * - HasField: a type must have a field with a given name and type
 * - HasMethod: a type must have a method with signature
 * - ImplementsTrait: a type must implement a given trait
 * - Subtype: one type must be a subtype of another
 * ---------------------------------------------------------------- */

typedef enum {
    CK_CONSTR_EQUAL,
    CK_CONSTR_SUBTYPE,
    CK_CONSTR_HAS_FIELD,
    CK_CONSTR_HAS_METHOD,
    CK_CONSTR_IMPLEMENTS_TRAIT,
} CkConstraintKind;

typedef struct {
    CkConstraintKind kind;
    CkType *left;
    CkType *right;
    char   *field_name;
    char   *trait_name;
    Span    span;
    char   *origin_desc;
} CkExtConstraint;

typedef struct {
    CkExtConstraint *items;
    int len, cap;
} CkExtConstraintSet;

static void ck_ext_add(CkExtConstraintSet *cs, CkConstraintKind kind,
                       CkType *left, CkType *right,
                       const char *field_name, const char *trait_name,
                       Span span, const char *origin) {
    if (cs->len >= cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 32;
        cs->items = xs_realloc(cs->items, sizeof(CkExtConstraint) * cs->cap);
    }
    CkExtConstraint *c = &cs->items[cs->len++];
    c->kind = kind;
    c->left = left;
    c->right = right;
    c->field_name = field_name ? xs_strdup(field_name) : NULL;
    c->trait_name = trait_name ? xs_strdup(trait_name) : NULL;
    c->span = span;
    c->origin_desc = origin ? xs_strdup(origin) : NULL;
}

static void ck_ext_free(CkExtConstraintSet *cs) {
    for (int i = 0; i < cs->len; i++) {
        free(cs->items[i].field_name);
        free(cs->items[i].trait_name);
        free(cs->items[i].origin_desc);
    }
    free(cs->items);
}

/* ----------------------------------------------------------------
 * Trait registry
 *
 * Track which types implement which traits, so we can verify
 * trait bounds on generic type parameters.
 * ---------------------------------------------------------------- */

typedef struct {
    char *type_name;
    char *trait_name;
} TraitImpl;

typedef struct {
    TraitImpl *items;
    int len, cap;
} TraitRegistry;

static void trait_reg_init(TraitRegistry *reg) {
    reg->items = NULL;
    reg->len = 0;
    reg->cap = 0;
}

static void trait_reg_free(TraitRegistry *reg) {
    for (int i = 0; i < reg->len; i++) {
        free(reg->items[i].type_name);
        free(reg->items[i].trait_name);
    }
    free(reg->items);
}

static void trait_reg_add(TraitRegistry *reg, const char *type_name,
                          const char *trait_name) {
    if (reg->len >= reg->cap) {
        reg->cap = reg->cap ? reg->cap * 2 : 16;
        reg->items = xs_realloc(reg->items, sizeof(TraitImpl) * reg->cap);
    }
    reg->items[reg->len].type_name = xs_strdup(type_name);
    reg->items[reg->len].trait_name = xs_strdup(trait_name);
    reg->len++;
}

static int trait_reg_check(TraitRegistry *reg, const char *type_name,
                           const char *trait_name) {
    for (int i = 0; i < reg->len; i++) {
        if (strcmp(reg->items[i].type_name, type_name) == 0 &&
            strcmp(reg->items[i].trait_name, trait_name) == 0)
            return 1;
    }
    /* built-in trait impls */
    if (strcmp(trait_name, "Display") == 0 || strcmp(trait_name, "Debug") == 0)
        return 1; /* all types can be displayed/debugged */
    if (strcmp(trait_name, "Eq") == 0 || strcmp(trait_name, "Hash") == 0) {
        if (strcmp(type_name, "int") == 0 || strcmp(type_name, "string") == 0 ||
            strcmp(type_name, "bool") == 0 || strcmp(type_name, "float") == 0)
            return 1;
    }
    if (strcmp(trait_name, "Ord") == 0 || strcmp(trait_name, "PartialOrd") == 0) {
        if (strcmp(type_name, "int") == 0 || strcmp(type_name, "float") == 0 ||
            strcmp(type_name, "string") == 0)
            return 1;
    }
    if (strcmp(trait_name, "Add") == 0 || strcmp(trait_name, "Sub") == 0 ||
        strcmp(trait_name, "Mul") == 0 || strcmp(trait_name, "Div") == 0) {
        if (strcmp(type_name, "int") == 0 || strcmp(type_name, "float") == 0)
            return 1;
    }
    if (strcmp(trait_name, "Iterator") == 0) {
        if (strcmp(type_name, "Array") == 0 || strcmp(type_name, "Range") == 0)
            return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Structural subtype checking for structs
 *
 * A struct A is structurally a subtype of B if A has all fields
 * that B has, and each field type in A is a subtype of the
 * corresponding field type in B.
 * ---------------------------------------------------------------- */

static int ck_structural_subtype(CkType *sub, CkType *super) {
    if (!sub || !super) return 0;
    sub = ck_resolve(sub);
    super = ck_resolve(super);

    if (sub->kind != CK_STRUCT || super->kind != CK_STRUCT)
        return 0;

    /* nominal match first */
    if (sub->struct_.name && super->struct_.name &&
        strcmp(sub->struct_.name, super->struct_.name) == 0)
        return 1;

    /* structural: super must have fewer or equal fields, all present in sub */
    for (int i = 0; i < super->struct_.nfields; i++) {
        char *fname = super->struct_.field_names[i];
        if (!fname) continue;

        int found = 0;
        for (int j = 0; j < sub->struct_.nfields; j++) {
            if (sub->struct_.field_names[j] &&
                strcmp(sub->struct_.field_names[j], fname) == 0) {
                if (!ck_subtype(sub->struct_.fields[j], super->struct_.fields[i]))
                    return 0;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------
 * Fix suggestions
 *
 * When a type error is detected, try to suggest a fix.
 * ---------------------------------------------------------------- */

static const char *suggest_fix(CkType *expected, CkType *got) {
    expected = ck_resolve(expected);
    got = ck_resolve(got);
    if (!expected || !got) return NULL;

    /* int where float expected: suggest adding .0 or using float() */
    if (expected->kind == CK_FLOAT && got->kind == CK_INT)
        return "try converting with float() or using a float literal (e.g. 1.0)";

    /* float where int expected */
    if (expected->kind == CK_INT && got->kind == CK_FLOAT)
        return "try converting with int() or using floor()/ceil()";

    /* string where int/float expected */
    if ((expected->kind == CK_INT || expected->kind == CK_FLOAT) &&
        got->kind == CK_STRING)
        return "try converting with int() or float()";

    /* T where T? expected: already OK (widening), but T? where T... */
    if (expected->kind != CK_OPTION && got->kind == CK_OPTION)
        return "value might be null, consider adding a null check or using ??";

    /* array where single element expected */
    if (expected->kind != CK_ARRAY && got->kind == CK_ARRAY)
        return "did you mean to index into the array? (e.g. arr[0])";

    /* single element where array expected */
    if (expected->kind == CK_ARRAY && got->kind != CK_ARRAY &&
        got->kind != CK_NULL)
        return "try wrapping the value in an array: [value]";

    return NULL;
}

/* ----------------------------------------------------------------
 * Extended error reporting with suggestions
 * ---------------------------------------------------------------- */

static void ck_type_mismatch_suggest(CkContext *ctx, Span span,
                                     CkType *expected, CkType *got,
                                     const char *origin) {
    char *es = type_to_str_alloc(expected);
    char *gs = type_to_str_alloc(got);
    const char *fix = suggest_fix(expected, got);

    if (origin && fix) {
        ck_type_error(ctx, span,
                      "type mismatch in %s: expected '%s', got '%s'. Hint: %s",
                      origin, es, gs, fix);
    } else if (origin) {
        ck_type_error(ctx, span,
                      "type mismatch in %s: expected '%s', got '%s'",
                      origin, es, gs);
    } else if (fix) {
        ck_type_error(ctx, span,
                      "type mismatch: expected '%s', got '%s'. Hint: %s",
                      es, gs, fix);
    } else {
        ck_type_error(ctx, span,
                      "type mismatch: expected '%s', got '%s'", es, gs);
    }
    free(es);
    free(gs);
}

/* ----------------------------------------------------------------
 * Extended constraint solving
 * ---------------------------------------------------------------- */

static int ck_solve_ext_constraints(CkContext *ctx, CkExtConstraintSet *ecs,
                                    TraitRegistry *traits) {
    int failed = 0;
    for (int i = 0; i < ecs->len; i++) {
        CkExtConstraint *c = &ecs->items[i];
        switch (c->kind) {
        case CK_CONSTR_EQUAL:
            if (!ck_unify(ctx, c->left, c->right)) {
                ck_type_mismatch_suggest(ctx, c->span, c->left, c->right,
                                         c->origin_desc);
                failed++;
            }
            break;

        case CK_CONSTR_SUBTYPE:
            if (!ck_subtype(c->left, c->right) &&
                !ck_structural_subtype(c->left, c->right)) {
                ck_type_mismatch_suggest(ctx, c->span, c->right, c->left,
                                         c->origin_desc);
                failed++;
            }
            break;

        case CK_CONSTR_HAS_FIELD: {
            CkType *t = ck_resolve(c->left);
            if (t->kind == CK_STRUCT && c->field_name) {
                int found = 0;
                for (int j = 0; j < t->struct_.nfields; j++) {
                    if (t->struct_.field_names[j] &&
                        strcmp(t->struct_.field_names[j], c->field_name) == 0) {
                        if (c->right && !ck_unify(ctx, t->struct_.fields[j], c->right)) {
                            char *es = type_to_str_alloc(c->right);
                            char *gs = type_to_str_alloc(t->struct_.fields[j]);
                            ck_type_error(ctx, c->span,
                                          "field '%s' has type '%s', expected '%s'",
                                          c->field_name, gs, es);
                            free(es);
                            free(gs);
                            failed++;
                        }
                        found = 1;
                        break;
                    }
                }
                if (!found && ctx->strict) {
                    char *ts = type_to_str_alloc(t);
                    ck_type_error(ctx, c->span,
                                  "type '%s' has no field '%s'", ts, c->field_name);
                    free(ts);
                    failed++;
                }
            }
            break;
        }
        case CK_CONSTR_HAS_METHOD: {
            CkType *t = ck_resolve(c->left);
            if (t->kind == CK_STRUCT && c->field_name && ctx->strict) {
                /* look up method in symtab */
                int found = 0;
                if (ctx->symtab && t->struct_.name) {
                    char qual[256];
                    snprintf(qual, sizeof qual, "%s.%s",
                             t->struct_.name, c->field_name);
                    Symbol *msym = sym_lookup(ctx->symtab, qual);
                    if (msym) found = 1;
                }
                if (!found) {
                    char *ts = type_to_str_alloc(t);
                    ck_type_error(ctx, c->span,
                                  "type '%s' has no method '%s'", ts, c->field_name);
                    free(ts);
                    failed++;
                }
            }
            break;
        }
        case CK_CONSTR_IMPLEMENTS_TRAIT: {
            CkType *t = ck_resolve(c->left);
            if (t->kind == CK_STRUCT && t->struct_.name && c->trait_name) {
                if (!trait_reg_check(traits, t->struct_.name, c->trait_name) &&
                    ctx->strict) {
                    ck_type_error(ctx, c->span,
                                  "type '%s' does not implement trait '%s'",
                                  t->struct_.name, c->trait_name);
                    failed++;
                }
            }
            break;
        }
        }
    }
    return failed == 0;
}

/* ----------------------------------------------------------------
 * Type checking for patterns
 *
 * Patterns in match arms, let destructuring, and for loops need
 * their own type inference logic.
 * ---------------------------------------------------------------- */

static CkType *ck_check_pattern(CkContext *ctx, Node *pattern, CkType *subject) {
    if (!pattern) return ck_any();

    switch (VAL_TAG(pattern)) {
    case NODE_PAT_WILD:
        return subject ? subject : ck_any();

    case NODE_PAT_IDENT: {
        CkType *ty = subject ? subject : ck_any();
        if (pattern->pat_ident.name)
            ck_env_bind(ctx->env, pattern->pat_ident.name, ty);
        return ty;
    }
    case NODE_PAT_LIT: {
        CkType *lit_ty;
        switch (pattern->pat_lit.tag) {
        case 0: lit_ty = ck_int(); break;
        case 1: lit_ty = ck_float(); break;
        case 2: lit_ty = ck_string(); break;
        case 3: lit_ty = ck_bool(); break;
        case 4: lit_ty = ck_null(); break;
        default: lit_ty = ck_any(); break;
        }
        if (subject) ck_add_constraint(ctx, subject, lit_ty, pattern->span);
        return lit_ty;
    }
    case NODE_PAT_TUPLE: {
        int ne = pattern->pat_tuple.elems.len;
        CkType **elems = xs_malloc(sizeof(CkType *) * (ne > 0 ? ne : 1));
        CkType *subj = subject ? ck_resolve(subject) : NULL;
        for (int i = 0; i < ne; i++) {
            CkType *elem_ty = NULL;
            if (subj && subj->kind == CK_TUPLE && i < subj->tuple.nelems)
                elem_ty = subj->tuple.elems[i];
            elems[i] = ck_check_pattern(ctx, pattern->pat_tuple.elems.items[i],
                                        elem_ty);
        }
        CkType *t = ck_tuple(elems, ne);
        free(elems);
        return t;
    }
    case NODE_PAT_STRUCT: {
        CkType *st_ty = ck_any();
        if (pattern->pat_struct.path) {
            st_ty = ck_env_lookup(ctx->env, pattern->pat_struct.path);
            if (!st_ty) st_ty = ck_struct(pattern->pat_struct.path, NULL, NULL, 0);
        }
        for (int i = 0; i < pattern->pat_struct.fields.len; i++) {
            Node *sub_pat = pattern->pat_struct.fields.items[i].val;
            ck_check_pattern(ctx, sub_pat, NULL);
        }
        return st_ty;
    }
    case NODE_PAT_ENUM: {
        if (pattern->pat_enum.path)
            return ck_env_lookup(ctx->env, pattern->pat_enum.path);
        return ck_any();
    }
    case NODE_PAT_OR:
        ck_check_pattern(ctx, pattern->pat_or.left, subject);
        ck_check_pattern(ctx, pattern->pat_or.right, subject);
        return subject ? subject : ck_any();

    case NODE_PAT_RANGE:
        return ck_int();

    case NODE_PAT_SLICE: {
        CkType *elem = ck_any();
        if (subject) {
            CkType *s = ck_resolve(subject);
            if (s->kind == CK_ARRAY) elem = s->array.elem;
        }
        for (int i = 0; i < pattern->pat_slice.elems.len; i++)
            ck_check_pattern(ctx, pattern->pat_slice.elems.items[i], elem);
        if (pattern->pat_slice.rest)
            ck_env_bind(ctx->env, pattern->pat_slice.rest, subject ? subject : ck_array(elem));
        return subject ? subject : ck_array(elem);
    }
    case NODE_PAT_GUARD:
        ck_check_pattern(ctx, pattern->pat_guard.pattern, subject);
        if (pattern->pat_guard.guard) {
            CkType *gt = ck_check_expr(ctx, pattern->pat_guard.guard);
            CkType *rg = ck_resolve(gt);
            if (rg->kind != CK_BOOL && rg->kind != CK_ANY)
                ck_type_error(ctx, pattern->pat_guard.guard->span,
                              "guard expression must be bool, got '%s'",
                              ck_type_to_string(gt));
        }
        return subject ? subject : ck_any();

    case NODE_PAT_CAPTURE:
        if (pattern->pat_capture.name) {
            CkType *inner = ck_check_pattern(ctx, pattern->pat_capture.pattern, subject);
            ck_env_bind(ctx->env, pattern->pat_capture.name, inner);
            return inner;
        }
        return ck_check_pattern(ctx, pattern->pat_capture.pattern, subject);

    case NODE_PAT_EXPR:
        if (pattern->pat_expr.expr)
            return ck_check_expr(ctx, pattern->pat_expr.expr);
        return ck_any();

    default:
        return ck_any();
    }
}

/* ----------------------------------------------------------------
 * Type checking for impl blocks
 *
 * Verify that impl blocks provide all required methods from traits,
 * with matching signatures.
 * ---------------------------------------------------------------- */

static void ck_check_impl(CkContext *ctx, Node *impl, TraitRegistry *traits) {
    if (!impl || VAL_TAG(impl) != NODE_IMPL_DECL) return;

    const char *type_name = impl->impl_decl.type_name;
    const char *trait_name = impl->impl_decl.trait_name;

    if (type_name && trait_name)
        trait_reg_add(traits, type_name, trait_name);

    /* check each method in the impl block */
    CkTypeEnv *impl_env = ck_env_new(ctx->env);
    CkTypeEnv *old = ctx->env;
    ctx->env = impl_env;

    /* bind self as the implementing type */
    if (type_name) {
        CkType *self_type = ck_env_lookup(ctx->env, type_name);
        if (!self_type) self_type = ck_struct(type_name, NULL, NULL, 0);
        ck_env_bind(impl_env, "self", self_type);
        ck_env_bind(impl_env, "Self", self_type);
    }

    for (int i = 0; i < impl->impl_decl.members.len; i++) {
        Node *member = impl->impl_decl.members.items[i];
        if (member && VAL_TAG(member) == NODE_FN_DECL)
            ck_check_fn(ctx, member);
    }

    ctx->env = old;
    ck_env_free(impl_env);
}

/* ----------------------------------------------------------------
 * Type inference for for-loop patterns
 *
 * Extract the element type from the iterator expression and bind
 * it to the loop variable.
 * ---------------------------------------------------------------- */

static CkType *ck_infer_iter_elem(CkContext *ctx, Node *iter) {
    if (!iter) return ck_any();
    CkType *iter_ty = ck_check_expr(ctx, iter);
    CkType *resolved = ck_resolve(iter_ty);

    if (resolved->kind == CK_ARRAY) return resolved->array.elem;
    if (resolved->kind == CK_MAP) {
        /* iterating a map yields (key, value) tuples */
        CkType *elems[2] = { resolved->map.key, resolved->map.val };
        return ck_tuple(elems, 2);
    }
    if (resolved->kind == CK_STRING) return ck_string();

    return ck_any();
}

/* ----------------------------------------------------------------
 * Number literal type narrowing
 *
 * Determine the narrowest integer type that fits a given value,
 * for more precise type inference.
 * ---------------------------------------------------------------- */

static CkType *ck_narrow_int(int64_t val) {
    (void)val;
    /* always use the general int type in XS */
    return ck_int();
}

static CkType *ck_narrow_float(double val) {
    (void)val;
    return ck_float();
}

/* ----------------------------------------------------------------
 * Binary type promotion rules
 *
 * When mixing int and float in arithmetic, the result is float.
 * When mixing different numeric types, promote to the wider one.
 * ---------------------------------------------------------------- */

static CkType *ck_promote_numeric(CkType *a, CkType *b) {
    a = ck_resolve(a);
    b = ck_resolve(b);
    if (!a || !b) return ck_any();

    if (a->kind == CK_FLOAT || b->kind == CK_FLOAT)
        return ck_float();
    if (a->kind == CK_INT && b->kind == CK_INT)
        return ck_int();
    return ck_any();
}

/* ----------------------------------------------------------------
 * Exhaustiveness checking for match expressions
 *
 * A simplified check: if the subject is an enum, verify that all
 * variants are covered. For primitive types, check if there's a
 * wildcard arm.
 * ---------------------------------------------------------------- */

static int ck_match_has_wildcard(Node *match_expr) {
    for (int i = 0; i < match_expr->match.arms.len; i++) {
        Node *pat = match_expr->match.arms.items[i].pattern;
        if (pat && VAL_TAG(pat) == NODE_PAT_WILD) return 1;
        if (pat && VAL_TAG(pat) == NODE_PAT_IDENT) return 1;
    }
    return 0;
}

static void ck_check_match_exhaustive(CkContext *ctx, Node *match_expr) {
    if (ck_match_has_wildcard(match_expr)) return;

    CkType *subject_ty = ck_check_expr(ctx, match_expr->match.subject);
    CkType *st = ck_resolve(subject_ty);

    if (st->kind == CK_ENUM && st->enum_.name && ctx->symtab) {
        Symbol *sym = sym_lookup(ctx->symtab, st->enum_.name);
        if (sym && sym->decl && VAL_TAG(sym->decl) == NODE_ENUM_DECL) {
            int nvariants = sym->decl->enum_decl.variants.len;
            int ncovered = 0;
            for (int v = 0; v < nvariants; v++) {
                const char *vname = sym->decl->enum_decl.variants.items[v].name;
                for (int a = 0; a < match_expr->match.arms.len; a++) {
                    Node *pat = match_expr->match.arms.items[a].pattern;
                    if (pat && VAL_TAG(pat) == NODE_PAT_ENUM && pat->pat_enum.path &&
                        strcmp(pat->pat_enum.path, vname) == 0) {
                        ncovered++;
                        break;
                    }
                }
            }
            if (ncovered < nvariants && ctx->strict) {
                ck_type_error(ctx, match_expr->span,
                              "non-exhaustive match on enum '%s': %d of %d variants covered",
                              st->enum_.name, ncovered, nvariants);
            }
        }
    } else if (st->kind == CK_BOOL) {
        int has_true = 0, has_false = 0;
        for (int a = 0; a < match_expr->match.arms.len; a++) {
            Node *pat = match_expr->match.arms.items[a].pattern;
            if (pat && VAL_TAG(pat) == NODE_PAT_LIT && pat->pat_lit.tag == 3) {
                if (pat->pat_lit.bval) has_true = 1;
                else has_false = 1;
            }
        }
        if ((!has_true || !has_false) && ctx->strict)
            ck_type_error(ctx, match_expr->span,
                          "non-exhaustive match on bool: missing %s case",
                          !has_true ? "true" : "false");
    }
}

/* ----------------------------------------------------------------
 * Type checking for generics with bounds
 *
 * When a function has type parameters with bounds like <T: Ord>,
 * verify that at each call site the concrete type satisfies the bound.
 * ---------------------------------------------------------------- */

static void ck_check_generic_bounds(CkContext *ctx, Node *fn_decl,
                                    CkType **arg_types, int nargs,
                                    TraitRegistry *traits) {
    if (!fn_decl || VAL_TAG(fn_decl) != NODE_FN_DECL) return;
    if (fn_decl->fn_decl.n_type_params <= 0) return;

    for (int i = 0; i < fn_decl->fn_decl.n_type_params; i++) {
        char *tp_name = fn_decl->fn_decl.type_params[i];
        TypeExpr *bound = (fn_decl->fn_decl.type_bounds &&
                           i < fn_decl->fn_decl.n_type_params)
                        ? fn_decl->fn_decl.type_bounds[i] : NULL;
        if (!tp_name || !bound) continue;

        /* find which argument maps to this type parameter */
        for (int j = 0; j < fn_decl->fn_decl.params.len && j < nargs; j++) {
            Param *pm = &fn_decl->fn_decl.params.items[j];
            if (pm->type_ann && pm->type_ann->kind == TEXPR_NAMED &&
                pm->type_ann->name && strcmp(pm->type_ann->name, tp_name) == 0) {
                CkType *at = ck_resolve(arg_types[j]);
                if (at->kind == CK_STRUCT && at->struct_.name && bound->name) {
                    if (!trait_reg_check(traits, at->struct_.name, bound->name)) {
                        ck_type_error(ctx, fn_decl->span,
                                      "type '%s' does not satisfy bound '%s' for generic '%s'",
                                      at->struct_.name, bound->name, tp_name);
                    }
                }
            }
        }
    }
}

/* ----------------------------------------------------------------
 * Type compatibility matrix
 *
 * Quick lookup table for whether two primitive types are compatible
 * in various contexts (assignment, comparison, arithmetic).
 * ---------------------------------------------------------------- */

typedef enum {
    COMPAT_NONE = 0,
    COMPAT_EXACT = 1,
    COMPAT_COERCE = 2,
    COMPAT_WIDEN = 3,
} CompatLevel;

static CompatLevel ck_prim_compat(CkTypeKind a, CkTypeKind b) {
    if (a == b) return COMPAT_EXACT;
    if (a == CK_ANY || b == CK_ANY) return COMPAT_COERCE;
    if (a == CK_NEVER || b == CK_NEVER) return COMPAT_COERCE;

    /* int <-> float coercion */
    if ((a == CK_INT && b == CK_FLOAT) || (a == CK_FLOAT && b == CK_INT))
        return COMPAT_WIDEN;

    /* null compatible with option */
    if ((a == CK_NULL && b == CK_OPTION) || (a == CK_OPTION && b == CK_NULL))
        return COMPAT_COERCE;

    return COMPAT_NONE;
}

/* ----------------------------------------------------------------
 * Unused variable detection
 *
 * Walk the type environment and find bindings that were never
 * referenced. Report as warnings.
 * ---------------------------------------------------------------- */

typedef struct {
    char *name;
    Span  span;
    int   used;
} UsageEntry;

typedef struct {
    UsageEntry *items;
    int len, cap;
} UsageTracker;

static void usage_init(UsageTracker *ut) {
    ut->items = NULL;
    ut->len = 0;
    ut->cap = 0;
}

static void usage_free(UsageTracker *ut) {
    for (int i = 0; i < ut->len; i++)
        free(ut->items[i].name);
    free(ut->items);
}

static void usage_add(UsageTracker *ut, const char *name, Span span) {
    if (!name || name[0] == '_') return; /* skip underscore-prefixed */
    if (ut->len >= ut->cap) {
        ut->cap = ut->cap ? ut->cap * 2 : 32;
        ut->items = xs_realloc(ut->items, sizeof(UsageEntry) * ut->cap);
    }
    ut->items[ut->len].name = xs_strdup(name);
    ut->items[ut->len].span = span;
    ut->items[ut->len].used = 0;
    ut->len++;
}

static void usage_mark(UsageTracker *ut, const char *name) {
    for (int i = ut->len - 1; i >= 0; i--) {
        if (strcmp(ut->items[i].name, name) == 0) {
            ut->items[i].used = 1;
            return;
        }
    }
}

static void usage_report(UsageTracker *ut, CkContext *ctx) {
    for (int i = 0; i < ut->len; i++) {
        if (!ut->items[i].used && ctx->strict) {
            ck_type_error(ctx, ut->items[i].span,
                          "unused variable '%s'", ut->items[i].name);
        }
    }
}

/* ----------------------------------------------------------------
 * Return path analysis
 *
 * In strict mode, check that all code paths in a function return
 * a value when the function has a non-void return type.
 * ---------------------------------------------------------------- */

static int ck_all_paths_return(Node *body) {
    if (!body) return 0;

    switch (VAL_TAG(body)) {
    case NODE_RETURN:
        return 1;
    case NODE_BLOCK: {
        if (body->block.expr) return 1; /* trailing expression counts */
        for (int i = body->block.stmts.len - 1; i >= 0; i--) {
            Node *s = body->block.stmts.items[i];
            if (!s) continue;
            if (VAL_TAG(s) == NODE_RETURN) return 1;
            if (VAL_TAG(s) == NODE_IF) {
                int then_ret = s->if_expr.then
                             ? ck_all_paths_return(s->if_expr.then) : 0;
                int else_ret = s->if_expr.else_branch
                             ? ck_all_paths_return(s->if_expr.else_branch) : 0;
                if (then_ret && else_ret) return 1;
            }
            if (VAL_TAG(s) == NODE_MATCH) {
                int all_arms = 1;
                for (int j = 0; j < s->match.arms.len; j++) {
                    if (!ck_all_paths_return(s->match.arms.items[j].body)) {
                        all_arms = 0;
                        break;
                    }
                }
                if (all_arms && s->match.arms.len > 0) return 1;
            }
            if (VAL_TAG(s) == NODE_THROW) return 1;
            if (VAL_TAG(s) == NODE_EXPR_STMT && i == body->block.stmts.len - 1)
                return 1;
            break;
        }
        return 0;
    }
    case NODE_IF: {
        int then_ret = body->if_expr.then
                     ? ck_all_paths_return(body->if_expr.then) : 0;
        int else_ret = body->if_expr.else_branch
                     ? ck_all_paths_return(body->if_expr.else_branch) : 0;
        return then_ret && else_ret;
    }
    case NODE_MATCH: {
        for (int j = 0; j < body->match.arms.len; j++) {
            if (!ck_all_paths_return(body->match.arms.items[j].body))
                return 0;
        }
        return body->match.arms.len > 0;
    }
    case NODE_THROW:
        return 1;
    case NODE_EXPR_STMT:
        return 1;
    default:
        return 0;
    }
}

static void ck_check_return_paths(CkContext *ctx, Node *fn_decl) {
    if (!fn_decl || VAL_TAG(fn_decl) != NODE_FN_DECL) return;
    if (!fn_decl->fn_decl.ret_type) return; /* no declared return type */
    if (!fn_decl->fn_decl.body) return;
    if (!ctx->strict) return;

    CkType *ret = ck_from_texpr(fn_decl->fn_decl.ret_type);
    CkType *r = ck_resolve(ret);
    if (r->kind == CK_NULL || r->kind == CK_ANY || r->kind == CK_NEVER)
        return;

    if (!ck_all_paths_return(fn_decl->fn_decl.body)) {
        ck_type_error(ctx, fn_decl->span,
                      "function '%s' declared to return '%s' but not all paths return a value",
                      fn_decl->fn_decl.name ? fn_decl->fn_decl.name : "<anonymous>",
                      ck_type_to_string(ret));
    }
}

/* ----------------------------------------------------------------
 * Recursive type detection
 *
 * Check for directly recursive struct definitions (struct that
 * contains itself as a non-optional field), which would have
 * infinite size.
 * ---------------------------------------------------------------- */

static int ck_is_recursive_struct(CkContext *ctx, const char *name,
                                  const char *field_type_name, int depth) {
    if (depth > 20) return 0;
    if (!name || !field_type_name) return 0;
    if (strcmp(name, field_type_name) == 0) return 1;

    /* look up field_type_name to see if it transitively contains name */
    if (ctx->symtab) {
        Symbol *sym = sym_lookup(ctx->symtab, field_type_name);
        if (sym && sym->decl && VAL_TAG(sym->decl) == NODE_STRUCT_DECL) {
            Node *sd = sym->decl;
            for (int i = 0; i < sd->struct_decl.fields.len; i++) {
                TypeExpr *ft = (sd->struct_decl.field_types &&
                                i < sd->struct_decl.n_field_types)
                             ? sd->struct_decl.field_types[i] : NULL;
                if (ft && ft->kind == TEXPR_NAMED && ft->name) {
                    if (ck_is_recursive_struct(ctx, name, ft->name, depth + 1))
                        return 1;
                }
            }
        }
    }
    return 0;
}

static void ck_check_recursive_types(CkContext *ctx, Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return;

    for (int i = 0; i < program->program.stmts.len; i++) {
        Node *s = program->program.stmts.items[i];
        if (!s || VAL_TAG(s) != NODE_STRUCT_DECL || !s->struct_decl.name) continue;

        for (int j = 0; j < s->struct_decl.fields.len; j++) {
            TypeExpr *ft = (s->struct_decl.field_types &&
                            j < s->struct_decl.n_field_types)
                         ? s->struct_decl.field_types[j] : NULL;
            if (!ft) continue;
            /* direct recursion with non-optional type is invalid */
            if (ft->kind == TEXPR_NAMED && ft->name &&
                ft->kind != TEXPR_OPTION) {
                if (ck_is_recursive_struct(ctx, s->struct_decl.name,
                                           ft->name, 0)) {
                    ck_type_error(ctx, s->span,
                                  "recursive struct '%s' contains itself through field '%s'. "
                                  "Use an optional type (T?) to break the cycle",
                                  s->struct_decl.name,
                                  s->struct_decl.fields.items[j].key
                                  ? s->struct_decl.fields.items[j].key : "?");
                }
            }
        }
    }
}
