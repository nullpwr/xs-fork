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

    case NODE_IMPORT: case NODE_USE: case NODE_EXPORT: case NODE_MODULE_DECL:
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
