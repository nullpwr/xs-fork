/* checker.h -- deep type checker with inference and generics */
#ifndef XS_CHECKER_H
#define XS_CHECKER_H

#include "core/ast.h"
#include "types/types.h"
#include "semantic/symtable.h"
#include "semantic/sema.h"

/* --- extended type representation for the deep checker --- */

typedef enum {
    CK_INT, CK_FLOAT, CK_BOOL, CK_STRING, CK_NULL,
    CK_ARRAY,      /* Array<T> */
    CK_MAP,        /* Map<K, V> */
    CK_TUPLE,      /* (T1, T2, ...) */
    CK_FUNCTION,   /* (T1, T2) -> R */
    CK_STRUCT,     /* named struct type */
    CK_ENUM,       /* named enum type */
    CK_TRAIT,      /* trait type */
    CK_UNION,      /* T1 | T2 (union type) */
    CK_OPTION,     /* T? (nullable) */
    CK_GENERIC,    /* generic parameter T */
    CK_VAR,        /* unification variable (for inference) */
    CK_ANY,        /* dynamic type */
    CK_NEVER,      /* never returns */
} CkTypeKind;

typedef struct CkType CkType;
struct CkType {
    CkTypeKind kind;
    union {
        struct { CkType *elem; } array;
        struct { CkType *key; CkType *val; } map;
        struct { CkType **elems; int nelems; } tuple;
        struct { CkType **params; int nparams; CkType *ret; } func;
        struct {
            char *name;
            CkType **fields;
            char **field_names;
            int nfields;
        } struct_;
        struct {
            char *name;
            struct { char *tag; CkType **fields; int nfields; } *variants;
            int nvariants;
        } enum_;
        struct { CkType **members; int nmembers; } union_;
        struct { CkType *inner; } option;
        struct { char *name; int id; } generic;
        struct { int id; CkType *bound; } var;
    };
};

/* --- type environment --- */

typedef struct CkBinding {
    char   *name;
    CkType *type;
} CkBinding;

typedef struct CkTypeEnv {
    CkBinding *bindings;
    int nbindings, cap;
    struct CkTypeEnv *parent;
} CkTypeEnv;

/* --- type constraints --- */

typedef struct {
    CkType *left;
    CkType *right;
    Span    span;
} CkConstraint;

typedef struct {
    CkConstraint *items;
    int len, cap;
} CkConstraintSet;

/* --- error tracking --- */

typedef struct {
    char **messages;
    Span  *spans;
    int    nerrors, cap;
} CkErrorList;

/* --- checker context --- */

typedef struct {
    CkTypeEnv    *env;
    CkConstraintSet constraints;
    CkErrorList   errors;
    SymTab       *symtab;
    SemaCtx      *sema;
    int           next_var_id;
    int           next_generic_id;
    int           strict;
} CkContext;

/* --- constructors --- */

CkType *ck_int(void);
CkType *ck_float(void);
CkType *ck_bool(void);
CkType *ck_string(void);
CkType *ck_null(void);
CkType *ck_any(void);
CkType *ck_never(void);
CkType *ck_array(CkType *elem);
CkType *ck_map(CkType *key, CkType *val);
CkType *ck_tuple(CkType **elems, int n);
CkType *ck_function(CkType **params, int nparams, CkType *ret);
CkType *ck_struct(const char *name, CkType **fields, char **names, int n);
CkType *ck_enum(const char *name);
CkType *ck_union(CkType **members, int n);
CkType *ck_option(CkType *inner);
CkType *ck_generic(const char *name, int id);
CkType *ck_var(CkContext *ctx);
CkType *ck_trait(const char *name);

/* --- type operations --- */

int      ck_unify(CkContext *ctx, CkType *a, CkType *b);
CkType  *ck_instantiate(CkContext *ctx, CkType *scheme);
CkType  *ck_generalize(CkTypeEnv *env, CkType *t);
int      ck_subtype(CkType *sub, CkType *super);
CkType  *ck_resolve(CkType *t);

/* --- type checking --- */

CkType *ck_check_expr(CkContext *ctx, Node *expr);
void    ck_check_stmt(CkContext *ctx, Node *stmt);
void    ck_check_fn(CkContext *ctx, Node *fn_decl);
void    ck_check_program(Node *program, SymTab *st, SemaCtx *sema, int strict);

/* --- constraint solving --- */

int  ck_solve_constraints(CkContext *ctx);
void ck_add_constraint(CkContext *ctx, CkType *left, CkType *right, Span span);

/* --- type env --- */

CkTypeEnv *ck_env_new(CkTypeEnv *parent);
void       ck_env_free(CkTypeEnv *env);
void       ck_env_bind(CkTypeEnv *env, const char *name, CkType *type);
CkType    *ck_env_lookup(CkTypeEnv *env, const char *name);

/* --- conversion / display --- */

CkType     *ck_from_xstype(XsType *t);
CkType     *ck_from_texpr(TypeExpr *te);
const char *ck_type_to_string(CkType *t);
void        ck_type_free(CkType *t);

/* --- error reporting --- */

void ck_type_error(CkContext *ctx, Span span, const char *fmt, ...);
void ck_type_mismatch(CkContext *ctx, Span span, CkType *expected, CkType *got);

/* --- context --- */

CkContext *ck_context_new(SymTab *st, SemaCtx *sema, int strict);
void       ck_context_free(CkContext *ctx);

#endif /* XS_CHECKER_H */
