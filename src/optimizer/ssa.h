/* ssa.h -- SSA intermediate representation for advanced optimization. */
#ifndef SSA_H
#define SSA_H

#include "core/ast.h"

/* SSA type tags for inferred types */
#define SSA_TYPE_ANY    0
#define SSA_TYPE_INT    1
#define SSA_TYPE_FLOAT  2
#define SSA_TYPE_BOOL   3
#define SSA_TYPE_STRING 4
#define SSA_TYPE_NULL   5

typedef enum {
    SSA_CONST,
    SSA_LOAD,
    SSA_STORE,
    SSA_BINOP,
    SSA_UNOP,
    SSA_CALL,
    SSA_BRANCH,
    SSA_JUMP,
    SSA_PHI,
    SSA_RETURN,
    SSA_PARAM,
} SSAOpKind;

typedef struct SSAInstr {
    int         id;
    SSAOpKind   op;
    int         type;
    int         dead;       /* marked dead by DCE */
    union {
        struct { int64_t ival; double fval; char *sval; int is_float; int is_bool; int bval; } konst;
        struct { char *name; int version; } load;
        struct { char *name; int val_id; } store;
        struct { int left, right; char op[8]; } binop;
        struct { char op[4]; int operand; } unop;
        struct { int callee; int *args; int nargs; char *name; } call;
        struct { int cond; int true_bb; int false_bb; } branch;
        struct { int target_bb; } jump;
        struct { int *sources; int *blocks; int nsources; } phi;
        struct { int value; int has_value; } ret;
        struct { int index; char *name; } param;
    };
    struct SSAInstr *next;
} SSAInstr;

typedef struct {
    int         id;
    SSAInstr   *first;
    SSAInstr   *last;
    int        *preds;
    int         npreds;
    int         pred_cap;
    int        *succs;
    int         nsuccs;
    int         succ_cap;
    int         sealed;
} BasicBlock;

typedef struct {
    BasicBlock *blocks;
    int         nblocks;
    int         block_cap;
    int         next_id;
    char      **param_names;
    int         nparams;
} SSAFunction;

/* Build SSA from a function declaration AST node */
SSAFunction *ssa_build(Node *fn_body, const char **param_names, int nparams);

/* Convert SSA back to an AST block */
Node *ssa_to_ast(SSAFunction *ssa);

/* Free SSA IR */
void ssa_free(SSAFunction *ssa);

/* type propagation pass */
void ssa_propagate_types(SSAFunction *ssa);

/* type specialization: replace generic ops with typed ones */
void ssa_type_specialize(SSAFunction *ssa);

/* global value numbering */
void ssa_gvn(SSAFunction *ssa);

/* copy propagation */
void ssa_copy_propagate(SSAFunction *ssa);

/* sparse conditional constant propagation */
void ssa_sccp(SSAFunction *ssa);

/* dead code elimination on SSA */
void ssa_dce(SSAFunction *ssa);

/* inline small calls (requires the program-level function list) */
void ssa_inline_calls(SSAFunction *ssa, Node *program);

#endif /* SSA_H */
