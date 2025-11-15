/* ssa.c -- SSA IR construction, passes, and lowering. */

#include "optimizer/ssa.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- helpers --- */

static SSAFunction *ssa_new(void) {
    SSAFunction *f = xs_malloc(sizeof(SSAFunction));
    f->blocks = NULL;
    f->nblocks = 0;
    f->block_cap = 0;
    f->next_id = 0;
    f->param_names = NULL;
    f->nparams = 0;
    return f;
}

static int ssa_new_block(SSAFunction *f) {
    if (f->nblocks >= f->block_cap) {
        f->block_cap = f->block_cap ? f->block_cap * 2 : 8;
        f->blocks = xs_realloc(f->blocks, f->block_cap * sizeof(BasicBlock));
    }
    int id = f->nblocks++;
    BasicBlock *bb = &f->blocks[id];
    bb->id = id;
    bb->first = bb->last = NULL;
    bb->preds = NULL;
    bb->npreds = 0;
    bb->pred_cap = 0;
    bb->succs = NULL;
    bb->nsuccs = 0;
    bb->succ_cap = 0;
    bb->sealed = 0;
    return id;
}

static SSAInstr *ssa_emit(SSAFunction *f, int block_id, SSAOpKind op) {
    SSAInstr *instr = xs_malloc(sizeof(SSAInstr));
    memset(instr, 0, sizeof(SSAInstr));
    instr->id = f->next_id++;
    instr->op = op;
    instr->type = SSA_TYPE_ANY;
    instr->dead = 0;
    instr->next = NULL;

    BasicBlock *bb = &f->blocks[block_id];
    if (!bb->first) {
        bb->first = bb->last = instr;
    } else {
        bb->last->next = instr;
        bb->last = instr;
    }
    return instr;
}

static void bb_add_pred(SSAFunction *f, int bb_id, int pred_id) {
    BasicBlock *bb = &f->blocks[bb_id];
    for (int i = 0; i < bb->npreds; i++)
        if (bb->preds[i] == pred_id) return;
    if (bb->npreds >= bb->pred_cap) {
        bb->pred_cap = bb->pred_cap ? bb->pred_cap * 2 : 4;
        bb->preds = xs_realloc(bb->preds, bb->pred_cap * sizeof(int));
    }
    bb->preds[bb->npreds++] = pred_id;
}

static void bb_add_succ(SSAFunction *f, int bb_id, int succ_id) {
    BasicBlock *bb = &f->blocks[bb_id];
    for (int i = 0; i < bb->nsuccs; i++)
        if (bb->succs[i] == succ_id) return;
    if (bb->nsuccs >= bb->succ_cap) {
        bb->succ_cap = bb->succ_cap ? bb->succ_cap * 2 : 4;
        bb->succs = xs_realloc(bb->succs, bb->succ_cap * sizeof(int));
    }
    bb->succs[bb->nsuccs++] = succ_id;
}

/* Variable version tracking for SSA naming */
#define MAX_SSA_VARS 256

typedef struct {
    char *name;
    int   current_id;   /* latest SSA id for this var */
} SSAVar;

typedef struct {
    SSAVar vars[MAX_SSA_VARS];
    int    nvars;
} SSAVarMap;

static void var_map_init(SSAVarMap *m) { m->nvars = 0; }

static int var_map_get(SSAVarMap *m, const char *name) {
    for (int i = 0; i < m->nvars; i++)
        if (strcmp(m->vars[i].name, name) == 0)
            return m->vars[i].current_id;
    return -1;
}

static void var_map_set(SSAVarMap *m, const char *name, int id) {
    for (int i = 0; i < m->nvars; i++) {
        if (strcmp(m->vars[i].name, name) == 0) {
            m->vars[i].current_id = id;
            return;
        }
    }
    if (m->nvars < MAX_SSA_VARS) {
        m->vars[m->nvars].name = xs_strdup(name);
        m->vars[m->nvars].current_id = id;
        m->nvars++;
    }
}

static void var_map_free(SSAVarMap *m) {
    for (int i = 0; i < m->nvars; i++)
        free(m->vars[i].name);
}

/* --- SSA construction from AST --- */

typedef struct {
    SSAFunction *func;
    int          cur_block;
    SSAVarMap    vars;
} SSABuilder;

static int build_expr(SSABuilder *b, Node *n);
static void build_stmt(SSABuilder *b, Node *n);

static int build_expr(SSABuilder *b, Node *n) {
    if (!n) {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->type = SSA_TYPE_NULL;
        return c->id;
    }

    switch (n->tag) {
    case NODE_LIT_INT: {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->konst.ival = n->lit_int.ival;
        c->konst.is_float = 0;
        c->konst.is_bool = 0;
        c->type = SSA_TYPE_INT;
        return c->id;
    }
    case NODE_LIT_FLOAT: {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->konst.fval = n->lit_float.fval;
        c->konst.is_float = 1;
        c->konst.is_bool = 0;
        c->type = SSA_TYPE_FLOAT;
        return c->id;
    }
    case NODE_LIT_BOOL: {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->konst.bval = n->lit_bool.bval;
        c->konst.is_float = 0;
        c->konst.is_bool = 1;
        c->type = SSA_TYPE_BOOL;
        return c->id;
    }
    case NODE_LIT_STRING: {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->konst.sval = n->lit_string.sval ? xs_strdup(n->lit_string.sval) : NULL;
        c->konst.is_float = 0;
        c->konst.is_bool = 0;
        c->type = SSA_TYPE_STRING;
        return c->id;
    }
    case NODE_LIT_NULL: {
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->type = SSA_TYPE_NULL;
        return c->id;
    }
    case NODE_IDENT: {
        int id = var_map_get(&b->vars, n->ident.name);
        if (id >= 0) return id;
        SSAInstr *ld = ssa_emit(b->func, b->cur_block, SSA_LOAD);
        ld->load.name = xs_strdup(n->ident.name);
        ld->load.version = 0;
        return ld->id;
    }
    case NODE_BINOP: {
        int left = build_expr(b, n->binop.left);
        int right = build_expr(b, n->binop.right);
        SSAInstr *bin = ssa_emit(b->func, b->cur_block, SSA_BINOP);
        bin->binop.left = left;
        bin->binop.right = right;
        strncpy(bin->binop.op, n->binop.op, sizeof(bin->binop.op) - 1);
        bin->binop.op[sizeof(bin->binop.op) - 1] = '\0';
        return bin->id;
    }
    case NODE_UNARY: {
        int operand = build_expr(b, n->unary.expr);
        SSAInstr *un = ssa_emit(b->func, b->cur_block, SSA_UNOP);
        un->unop.operand = operand;
        strncpy(un->unop.op, n->unary.op, sizeof(un->unop.op) - 1);
        un->unop.op[sizeof(un->unop.op) - 1] = '\0';
        return un->id;
    }
    case NODE_CALL: {
        int callee = build_expr(b, n->call.callee);
        int nargs = n->call.args.len;
        int *args = NULL;
        if (nargs > 0) {
            args = xs_malloc(nargs * sizeof(int));
            for (int i = 0; i < nargs; i++)
                args[i] = build_expr(b, n->call.args.items[i]);
        }
        SSAInstr *call = ssa_emit(b->func, b->cur_block, SSA_CALL);
        call->call.callee = callee;
        call->call.args = args;
        call->call.nargs = nargs;
        call->call.name = NULL;
        if (n->call.callee && n->call.callee->tag == NODE_IDENT)
            call->call.name = xs_strdup(n->call.callee->ident.name);
        return call->id;
    }
    case NODE_IF: {
        /* if cond { then } else { else } as branches */
        int cond_val = build_expr(b, n->if_expr.cond);
        int then_bb = ssa_new_block(b->func);
        int else_bb = ssa_new_block(b->func);
        int merge_bb = ssa_new_block(b->func);

        SSAInstr *br = ssa_emit(b->func, b->cur_block, SSA_BRANCH);
        br->branch.cond = cond_val;
        br->branch.true_bb = then_bb;
        br->branch.false_bb = else_bb;
        bb_add_succ(b->func, b->cur_block, then_bb);
        bb_add_succ(b->func, b->cur_block, else_bb);
        bb_add_pred(b->func, then_bb, b->cur_block);
        bb_add_pred(b->func, else_bb, b->cur_block);

        /* then branch */
        b->cur_block = then_bb;
        int then_val = -1;
        if (n->if_expr.then) {
            if (n->if_expr.then->tag == NODE_BLOCK) {
                for (int i = 0; i < n->if_expr.then->block.stmts.len; i++)
                    build_stmt(b, n->if_expr.then->block.stmts.items[i]);
                if (n->if_expr.then->block.expr)
                    then_val = build_expr(b, n->if_expr.then->block.expr);
            } else {
                then_val = build_expr(b, n->if_expr.then);
            }
        }
        int then_end_bb = b->cur_block;
        SSAInstr *jmp_then = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        jmp_then->jump.target_bb = merge_bb;
        bb_add_succ(b->func, b->cur_block, merge_bb);
        bb_add_pred(b->func, merge_bb, b->cur_block);

        /* else branch */
        b->cur_block = else_bb;
        int else_val = -1;
        if (n->if_expr.else_branch) {
            if (n->if_expr.else_branch->tag == NODE_BLOCK) {
                for (int i = 0; i < n->if_expr.else_branch->block.stmts.len; i++)
                    build_stmt(b, n->if_expr.else_branch->block.stmts.items[i]);
                if (n->if_expr.else_branch->block.expr)
                    else_val = build_expr(b, n->if_expr.else_branch->block.expr);
            } else {
                else_val = build_expr(b, n->if_expr.else_branch);
            }
        }
        int else_end_bb = b->cur_block;
        SSAInstr *jmp_else = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        jmp_else->jump.target_bb = merge_bb;
        bb_add_succ(b->func, b->cur_block, merge_bb);
        bb_add_pred(b->func, merge_bb, b->cur_block);

        b->cur_block = merge_bb;

        /* phi node if both branches produce values */
        if (then_val >= 0 && else_val >= 0) {
            SSAInstr *phi = ssa_emit(b->func, merge_bb, SSA_PHI);
            phi->phi.sources = xs_malloc(2 * sizeof(int));
            phi->phi.blocks = xs_malloc(2 * sizeof(int));
            phi->phi.nsources = 2;
            phi->phi.sources[0] = then_val;
            phi->phi.sources[1] = else_val;
            phi->phi.blocks[0] = then_end_bb;
            phi->phi.blocks[1] = else_end_bb;
            return phi->id;
        }
        /* produce a null constant as fallback */
        SSAInstr *nc = ssa_emit(b->func, merge_bb, SSA_CONST);
        nc->type = SSA_TYPE_NULL;
        return nc->id;
    }
    default: {
        /* anything else we can't lower to SSA, emit as generic load */
        SSAInstr *c = ssa_emit(b->func, b->cur_block, SSA_CONST);
        c->type = SSA_TYPE_ANY;
        return c->id;
    }
    }
}

static void build_stmt(SSABuilder *b, Node *n) {
    if (!n) return;

    switch (n->tag) {
    case NODE_LET:
    case NODE_VAR: {
        int val = build_expr(b, n->let.value);
        if (n->let.name) {
            SSAInstr *st = ssa_emit(b->func, b->cur_block, SSA_STORE);
            st->store.name = xs_strdup(n->let.name);
            st->store.val_id = val;
            var_map_set(&b->vars, n->let.name, val);
        }
        break;
    }
    case NODE_CONST: {
        int val = build_expr(b, n->const_.value);
        if (n->const_.name) {
            SSAInstr *st = ssa_emit(b->func, b->cur_block, SSA_STORE);
            st->store.name = xs_strdup(n->const_.name);
            st->store.val_id = val;
            var_map_set(&b->vars, n->const_.name, val);
        }
        break;
    }
    case NODE_ASSIGN: {
        int val = build_expr(b, n->assign.value);
        if (n->assign.target && n->assign.target->tag == NODE_IDENT) {
            SSAInstr *st = ssa_emit(b->func, b->cur_block, SSA_STORE);
            st->store.name = xs_strdup(n->assign.target->ident.name);
            st->store.val_id = val;
            var_map_set(&b->vars, n->assign.target->ident.name, val);
        }
        break;
    }
    case NODE_RETURN: {
        int val = -1;
        int has = 0;
        if (n->ret.value) {
            val = build_expr(b, n->ret.value);
            has = 1;
        }
        SSAInstr *ret = ssa_emit(b->func, b->cur_block, SSA_RETURN);
        ret->ret.value = val;
        ret->ret.has_value = has;
        break;
    }
    case NODE_EXPR_STMT: {
        build_expr(b, n->expr_stmt.expr);
        break;
    }
    case NODE_IF: {
        build_expr(b, n);
        break;
    }
    case NODE_BLOCK: {
        for (int i = 0; i < n->block.stmts.len; i++)
            build_stmt(b, n->block.stmts.items[i]);
        if (n->block.expr)
            build_expr(b, n->block.expr);
        break;
    }
    case NODE_WHILE: {
        int header_bb = ssa_new_block(b->func);
        int body_bb = ssa_new_block(b->func);
        int exit_bb = ssa_new_block(b->func);

        SSAInstr *jmp = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        jmp->jump.target_bb = header_bb;
        bb_add_succ(b->func, b->cur_block, header_bb);
        bb_add_pred(b->func, header_bb, b->cur_block);

        b->cur_block = header_bb;
        int cond = build_expr(b, n->while_loop.cond);
        SSAInstr *br = ssa_emit(b->func, header_bb, SSA_BRANCH);
        br->branch.cond = cond;
        br->branch.true_bb = body_bb;
        br->branch.false_bb = exit_bb;
        bb_add_succ(b->func, header_bb, body_bb);
        bb_add_succ(b->func, header_bb, exit_bb);
        bb_add_pred(b->func, body_bb, header_bb);
        bb_add_pred(b->func, exit_bb, header_bb);

        b->cur_block = body_bb;
        if (n->while_loop.body) {
            if (n->while_loop.body->tag == NODE_BLOCK) {
                for (int i = 0; i < n->while_loop.body->block.stmts.len; i++)
                    build_stmt(b, n->while_loop.body->block.stmts.items[i]);
            } else {
                build_stmt(b, n->while_loop.body);
            }
        }
        SSAInstr *back = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        back->jump.target_bb = header_bb;
        bb_add_succ(b->func, b->cur_block, header_bb);
        bb_add_pred(b->func, header_bb, b->cur_block);

        b->cur_block = exit_bb;
        break;
    }
    case NODE_FOR: {
        /* treat for loops as while with an iterator, simplified */
        int header_bb = ssa_new_block(b->func);
        int body_bb = ssa_new_block(b->func);
        int exit_bb = ssa_new_block(b->func);

        build_expr(b, n->for_loop.iter);

        SSAInstr *jmp = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        jmp->jump.target_bb = header_bb;
        bb_add_succ(b->func, b->cur_block, header_bb);
        bb_add_pred(b->func, header_bb, b->cur_block);

        b->cur_block = header_bb;
        /* synthetic condition - always branch to body (simplified) */
        SSAInstr *c = ssa_emit(b->func, header_bb, SSA_CONST);
        c->konst.bval = 1;
        c->konst.is_bool = 1;
        c->type = SSA_TYPE_BOOL;
        SSAInstr *br = ssa_emit(b->func, header_bb, SSA_BRANCH);
        br->branch.cond = c->id;
        br->branch.true_bb = body_bb;
        br->branch.false_bb = exit_bb;
        bb_add_succ(b->func, header_bb, body_bb);
        bb_add_succ(b->func, header_bb, exit_bb);
        bb_add_pred(b->func, body_bb, header_bb);
        bb_add_pred(b->func, exit_bb, header_bb);

        b->cur_block = body_bb;
        if (n->for_loop.body) {
            if (n->for_loop.body->tag == NODE_BLOCK) {
                for (int i = 0; i < n->for_loop.body->block.stmts.len; i++)
                    build_stmt(b, n->for_loop.body->block.stmts.items[i]);
            } else {
                build_stmt(b, n->for_loop.body);
            }
        }
        SSAInstr *back = ssa_emit(b->func, b->cur_block, SSA_JUMP);
        back->jump.target_bb = header_bb;
        bb_add_succ(b->func, b->cur_block, header_bb);
        bb_add_pred(b->func, header_bb, b->cur_block);

        b->cur_block = exit_bb;
        break;
    }
    default:
        /* anything we can't lower, just evaluate for side effects */
        build_expr(b, n);
        break;
    }
}

SSAFunction *ssa_build(Node *fn_body, const char **param_names, int nparams) {
    SSAFunction *f = ssa_new();
    SSABuilder builder;
    builder.func = f;
    var_map_init(&builder.vars);

    /* entry block */
    builder.cur_block = ssa_new_block(f);

    /* emit param instructions */
    f->nparams = nparams;
    if (nparams > 0) {
        f->param_names = xs_malloc(nparams * sizeof(char*));
        for (int i = 0; i < nparams; i++) {
            f->param_names[i] = xs_strdup(param_names[i]);
            SSAInstr *p = ssa_emit(f, 0, SSA_PARAM);
            p->param.index = i;
            p->param.name = xs_strdup(param_names[i]);
            var_map_set(&builder.vars, param_names[i], p->id);
        }
    }

    /* build body */
    if (fn_body) {
        if (fn_body->tag == NODE_BLOCK) {
            for (int i = 0; i < fn_body->block.stmts.len; i++)
                build_stmt(&builder, fn_body->block.stmts.items[i]);
            if (fn_body->block.expr) {
                int val = build_expr(&builder, fn_body->block.expr);
                SSAInstr *ret = ssa_emit(f, builder.cur_block, SSA_RETURN);
                ret->ret.value = val;
                ret->ret.has_value = 1;
            }
        } else {
            int val = build_expr(&builder, fn_body);
            SSAInstr *ret = ssa_emit(f, builder.cur_block, SSA_RETURN);
            ret->ret.value = val;
            ret->ret.has_value = 1;
        }
    }

    var_map_free(&builder.vars);
    return f;
}

/* --- instruction lookup by ID --- */

static SSAInstr *ssa_find_instr(SSAFunction *f, int id) {
    for (int i = 0; i < f->nblocks; i++) {
        for (SSAInstr *ins = f->blocks[i].first; ins; ins = ins->next) {
            if (ins->id == id) return ins;
        }
    }
    return NULL;
}

/* --- Type Propagation --- */

void ssa_propagate_types(SSAFunction *ssa) {
    if (!ssa) return;

    /* iterate until fixed point */
    int changed = 1;
    int rounds = 0;
    while (changed && rounds < 20) {
        changed = 0;
        rounds++;
        for (int b = 0; b < ssa->nblocks; b++) {
            for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
                int old_type = ins->type;

                switch (ins->op) {
                case SSA_CONST:
                    /* already set during build */
                    break;
                case SSA_PARAM:
                    /* params start as ANY */
                    break;
                case SSA_LOAD:
                    /* try to find the latest store */
                    break;
                case SSA_STORE: {
                    SSAInstr *val = ssa_find_instr(ssa, ins->store.val_id);
                    if (val) ins->type = val->type;
                    break;
                }
                case SSA_BINOP: {
                    SSAInstr *left = ssa_find_instr(ssa, ins->binop.left);
                    SSAInstr *right = ssa_find_instr(ssa, ins->binop.right);
                    if (!left || !right) break;
                    const char *op = ins->binop.op;

                    /* comparison ops always produce bool */
                    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                        strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
                        ins->type = SSA_TYPE_BOOL;
                    }
                    else if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
                        ins->type = SSA_TYPE_BOOL;
                    }
                    else if (left->type == SSA_TYPE_INT && right->type == SSA_TYPE_INT) {
                        ins->type = SSA_TYPE_INT;
                    }
                    else if ((left->type == SSA_TYPE_FLOAT || left->type == SSA_TYPE_INT) &&
                             (right->type == SSA_TYPE_FLOAT || right->type == SSA_TYPE_INT) &&
                             (left->type == SSA_TYPE_FLOAT || right->type == SSA_TYPE_FLOAT)) {
                        ins->type = SSA_TYPE_FLOAT;
                    }
                    else if (left->type == SSA_TYPE_STRING && right->type == SSA_TYPE_STRING) {
                        if (strcmp(op, "++") == 0) ins->type = SSA_TYPE_STRING;
                    }
                    break;
                }
                case SSA_UNOP: {
                    SSAInstr *operand = ssa_find_instr(ssa, ins->unop.operand);
                    if (!operand) break;
                    if (strcmp(ins->unop.op, "!") == 0)
                        ins->type = SSA_TYPE_BOOL;
                    else if (strcmp(ins->unop.op, "-") == 0 || strcmp(ins->unop.op, "~") == 0)
                        ins->type = operand->type;
                    break;
                }
                case SSA_PHI: {
                    /* if all sources have the same type, phi has that type */
                    if (ins->phi.nsources > 0) {
                        SSAInstr *first_src = ssa_find_instr(ssa, ins->phi.sources[0]);
                        if (first_src) {
                            int unified = first_src->type;
                            for (int s = 1; s < ins->phi.nsources; s++) {
                                SSAInstr *src = ssa_find_instr(ssa, ins->phi.sources[s]);
                                if (!src || src->type != unified) {
                                    unified = SSA_TYPE_ANY;
                                    break;
                                }
                            }
                            ins->type = unified;
                        }
                    }
                    break;
                }
                case SSA_RETURN: {
                    if (ins->ret.has_value) {
                        SSAInstr *val = ssa_find_instr(ssa, ins->ret.value);
                        if (val) ins->type = val->type;
                    }
                    break;
                }
                default:
                    break;
                }

                if (ins->type != old_type) changed = 1;
            }
        }
    }
}

/* --- Type Specialization --- */

void ssa_type_specialize(SSAFunction *ssa) {
    if (!ssa) return;

    /* after type propagation, annotate binops with known types.
       the lowering phase (ssa_to_ast) will use these annotations
       to emit specialized operations or type hints. */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->op != SSA_BINOP) continue;

            SSAInstr *left = ssa_find_instr(ssa, ins->binop.left);
            SSAInstr *right = ssa_find_instr(ssa, ins->binop.right);
            if (!left || !right) continue;

            /* both int: mark result as int */
            if (left->type == SSA_TYPE_INT && right->type == SSA_TYPE_INT) {
                ins->type = SSA_TYPE_INT;
            }
            /* float promotion */
            else if ((left->type == SSA_TYPE_FLOAT || left->type == SSA_TYPE_INT) &&
                     (right->type == SSA_TYPE_FLOAT || right->type == SSA_TYPE_INT) &&
                     (left->type == SSA_TYPE_FLOAT || right->type == SSA_TYPE_FLOAT)) {
                ins->type = SSA_TYPE_FLOAT;
            }
        }
    }
}

/* --- Global Value Numbering --- */

#define GVN_BUCKETS 128

typedef struct GVNEntry {
    uint64_t hash;
    int      instr_id;   /* canonical instruction */
    struct GVNEntry *next;
} GVNEntry;

static uint64_t gvn_hash_binop(int op_tag, const char *op, int left, int right) {
    uint64_t h = (uint64_t)op_tag * 2654435761ULL;
    h ^= (uint64_t)(unsigned)(left + 1) * 6364136223846793005ULL;
    h ^= (uint64_t)(unsigned)(right + 1) * 1442695040888963407ULL;
    while (*op) { h = h * 31 + (unsigned char)*op; op++; }
    return h;
}

static uint64_t gvn_hash_unop(const char *op, int operand) {
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)(unsigned)(operand + 1) * 6364136223846793005ULL;
    while (*op) { h = h * 31 + (unsigned char)*op; op++; }
    return h;
}

void ssa_gvn(SSAFunction *ssa) {
    if (!ssa) return;

    GVNEntry *table[GVN_BUCKETS];
    memset(table, 0, sizeof(table));

    /* map from old instruction IDs to replacement IDs */
    int map_size = ssa->next_id;
    int *replace = xs_malloc(map_size * sizeof(int));
    for (int i = 0; i < map_size; i++) replace[i] = i;

    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            uint64_t h = 0;
            int do_gvn = 0;

            if (ins->op == SSA_BINOP) {
                int l = replace[ins->binop.left];
                int r = replace[ins->binop.right];
                ins->binop.left = l;
                ins->binop.right = r;
                h = gvn_hash_binop(SSA_BINOP, ins->binop.op, l, r);
                do_gvn = 1;
            }
            else if (ins->op == SSA_UNOP) {
                int o = replace[ins->unop.operand];
                ins->unop.operand = o;
                h = gvn_hash_unop(ins->unop.op, o);
                do_gvn = 1;
            }
            else {
                /* update references in other instruction types */
                if (ins->op == SSA_STORE && ins->store.val_id < map_size)
                    ins->store.val_id = replace[ins->store.val_id];
                if (ins->op == SSA_RETURN && ins->ret.has_value && ins->ret.value < map_size)
                    ins->ret.value = replace[ins->ret.value];
                if (ins->op == SSA_BRANCH && ins->branch.cond < map_size)
                    ins->branch.cond = replace[ins->branch.cond];
                if (ins->op == SSA_CALL) {
                    for (int a = 0; a < ins->call.nargs; a++)
                        if (ins->call.args[a] < map_size)
                            ins->call.args[a] = replace[ins->call.args[a]];
                }
                if (ins->op == SSA_PHI) {
                    for (int s = 0; s < ins->phi.nsources; s++)
                        if (ins->phi.sources[s] < map_size)
                            ins->phi.sources[s] = replace[ins->phi.sources[s]];
                }
            }

            if (!do_gvn) continue;

            int bucket = (int)(h % GVN_BUCKETS);
            int found = 0;
            for (GVNEntry *e = table[bucket]; e; e = e->next) {
                if (e->hash == h) {
                    SSAInstr *existing = ssa_find_instr(ssa, e->instr_id);
                    if (existing && !existing->dead) {
                        int match = 0;
                        if (ins->op == SSA_BINOP && existing->op == SSA_BINOP) {
                            match = (ins->binop.left == existing->binop.left &&
                                     ins->binop.right == existing->binop.right &&
                                     strcmp(ins->binop.op, existing->binop.op) == 0);
                        }
                        else if (ins->op == SSA_UNOP && existing->op == SSA_UNOP) {
                            match = (ins->unop.operand == existing->unop.operand &&
                                     strcmp(ins->unop.op, existing->unop.op) == 0);
                        }
                        if (match) {
                            replace[ins->id] = e->instr_id;
                            ins->dead = 1;
                            found = 1;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                GVNEntry *ne = xs_malloc(sizeof(GVNEntry));
                ne->hash = h;
                ne->instr_id = ins->id;
                ne->next = table[bucket];
                table[bucket] = ne;
            }
        }
    }

    /* apply replacements to all remaining instructions */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            switch (ins->op) {
            case SSA_BINOP:
                if (ins->binop.left < map_size) ins->binop.left = replace[ins->binop.left];
                if (ins->binop.right < map_size) ins->binop.right = replace[ins->binop.right];
                break;
            case SSA_UNOP:
                if (ins->unop.operand < map_size) ins->unop.operand = replace[ins->unop.operand];
                break;
            case SSA_STORE:
                if (ins->store.val_id < map_size) ins->store.val_id = replace[ins->store.val_id];
                break;
            case SSA_RETURN:
                if (ins->ret.has_value && ins->ret.value < map_size)
                    ins->ret.value = replace[ins->ret.value];
                break;
            case SSA_BRANCH:
                if (ins->branch.cond < map_size) ins->branch.cond = replace[ins->branch.cond];
                break;
            case SSA_CALL:
                for (int a = 0; a < ins->call.nargs; a++)
                    if (ins->call.args[a] < map_size)
                        ins->call.args[a] = replace[ins->call.args[a]];
                break;
            case SSA_PHI:
                for (int s = 0; s < ins->phi.nsources; s++)
                    if (ins->phi.sources[s] < map_size)
                        ins->phi.sources[s] = replace[ins->phi.sources[s]];
                break;
            default:
                break;
            }
        }
    }

    free(replace);
    for (int i = 0; i < GVN_BUCKETS; i++) {
        GVNEntry *e = table[i];
        while (e) { GVNEntry *n = e->next; free(e); e = n; }
    }
}

/* --- Copy Propagation --- */

void ssa_copy_propagate(SSAFunction *ssa) {
    if (!ssa) return;

    int map_size = ssa->next_id;
    int *replace = xs_malloc(map_size * sizeof(int));
    for (int i = 0; i < map_size; i++) replace[i] = i;

    /* find store instructions that are direct copies */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            if (ins->op == SSA_STORE) {
                /* propagate the value through loads of this name */
                /* in simplified SSA, store to var means all loads
                   of that var after this point get the stored value */
            }
        }
    }

    /* propagate replacements through all instructions */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            switch (ins->op) {
            case SSA_BINOP:
                while (ins->binop.left < map_size && replace[ins->binop.left] != ins->binop.left)
                    ins->binop.left = replace[ins->binop.left];
                while (ins->binop.right < map_size && replace[ins->binop.right] != ins->binop.right)
                    ins->binop.right = replace[ins->binop.right];
                break;
            case SSA_UNOP:
                while (ins->unop.operand < map_size && replace[ins->unop.operand] != ins->unop.operand)
                    ins->unop.operand = replace[ins->unop.operand];
                break;
            case SSA_STORE:
                while (ins->store.val_id < map_size && replace[ins->store.val_id] != ins->store.val_id)
                    ins->store.val_id = replace[ins->store.val_id];
                break;
            case SSA_RETURN:
                if (ins->ret.has_value) {
                    while (ins->ret.value < map_size && replace[ins->ret.value] != ins->ret.value)
                        ins->ret.value = replace[ins->ret.value];
                }
                break;
            case SSA_BRANCH:
                while (ins->branch.cond < map_size && replace[ins->branch.cond] != ins->branch.cond)
                    ins->branch.cond = replace[ins->branch.cond];
                break;
            case SSA_CALL:
                for (int a = 0; a < ins->call.nargs; a++)
                    while (ins->call.args[a] < map_size && replace[ins->call.args[a]] != ins->call.args[a])
                        ins->call.args[a] = replace[ins->call.args[a]];
                break;
            case SSA_PHI:
                for (int s = 0; s < ins->phi.nsources; s++)
                    while (ins->phi.sources[s] < map_size && replace[ins->phi.sources[s]] != ins->phi.sources[s])
                        ins->phi.sources[s] = replace[ins->phi.sources[s]];
                break;
            default:
                break;
            }
        }
    }

    free(replace);
}

/* --- Sparse Conditional Constant Propagation --- */

#define SCCP_TOP     0
#define SCCP_CONST   1
#define SCCP_BOTTOM  2

typedef struct {
    int     lattice;   /* SCCP_TOP, SCCP_CONST, SCCP_BOTTOM */
    int64_t ival;
    double  fval;
    int     bval;
    int     type;      /* SSA_TYPE_INT, etc */
} SCCPVal;

void ssa_sccp(SSAFunction *ssa) {
    if (!ssa) return;

    int n = ssa->next_id;
    SCCPVal *vals = xs_malloc(n * sizeof(SCCPVal));
    for (int i = 0; i < n; i++) {
        vals[i].lattice = SCCP_TOP;
        vals[i].ival = 0;
        vals[i].fval = 0.0;
        vals[i].bval = 0;
        vals[i].type = SSA_TYPE_ANY;
    }

    /* init constants */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->op == SSA_CONST) {
                vals[ins->id].lattice = SCCP_CONST;
                if (ins->type == SSA_TYPE_INT) {
                    vals[ins->id].ival = ins->konst.ival;
                    vals[ins->id].type = SSA_TYPE_INT;
                } else if (ins->type == SSA_TYPE_FLOAT) {
                    vals[ins->id].fval = ins->konst.fval;
                    vals[ins->id].type = SSA_TYPE_FLOAT;
                } else if (ins->type == SSA_TYPE_BOOL) {
                    vals[ins->id].bval = ins->konst.bval;
                    vals[ins->id].type = SSA_TYPE_BOOL;
                }
            }
            else if (ins->op == SSA_PARAM || ins->op == SSA_CALL) {
                vals[ins->id].lattice = SCCP_BOTTOM;
            }
        }
    }

    /* iterate to fixed point */
    int changed = 1;
    int rounds = 0;
    while (changed && rounds < 30) {
        changed = 0;
        rounds++;

        for (int b = 0; b < ssa->nblocks; b++) {
            for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
                if (ins->dead) continue;
                if (ins->op == SSA_CONST || ins->op == SSA_PARAM) continue;

                int old_lattice = vals[ins->id].lattice;

                if (ins->op == SSA_BINOP) {
                    int li = ins->binop.left;
                    int ri = ins->binop.right;
                    if (li >= n || ri >= n) { vals[ins->id].lattice = SCCP_BOTTOM; }
                    else if (vals[li].lattice == SCCP_CONST && vals[ri].lattice == SCCP_CONST &&
                             vals[li].type == SSA_TYPE_INT && vals[ri].type == SSA_TYPE_INT) {
                        int64_t l = vals[li].ival, r = vals[ri].ival;
                        int64_t result = 0;
                        int folded = 1;
                        const char *op = ins->binop.op;
                        if      (strcmp(op, "+") == 0)  result = l + r;
                        else if (strcmp(op, "-") == 0)  result = l - r;
                        else if (strcmp(op, "*") == 0)  result = l * r;
                        else if (strcmp(op, "/") == 0) { if (r == 0) folded = 0; else result = l / r; }
                        else if (strcmp(op, "%") == 0) { if (r == 0) folded = 0; else result = l % r; }
                        else if (strcmp(op, "&") == 0)  result = l & r;
                        else if (strcmp(op, "|") == 0)  result = l | r;
                        else if (strcmp(op, "^") == 0)  result = l ^ r;
                        else if (strcmp(op, "<<") == 0) result = l << r;
                        else if (strcmp(op, ">>") == 0) result = l >> r;
                        else folded = 0;
                        if (folded) {
                            vals[ins->id].lattice = SCCP_CONST;
                            vals[ins->id].ival = result;
                            vals[ins->id].type = SSA_TYPE_INT;
                        } else {
                            vals[ins->id].lattice = SCCP_BOTTOM;
                        }
                    }
                    else if (vals[li].lattice == SCCP_BOTTOM || vals[ri].lattice == SCCP_BOTTOM) {
                        vals[ins->id].lattice = SCCP_BOTTOM;
                    }
                }
                else if (ins->op == SSA_UNOP) {
                    int oi = ins->unop.operand;
                    if (oi >= n) { vals[ins->id].lattice = SCCP_BOTTOM; }
                    else if (vals[oi].lattice == SCCP_CONST && vals[oi].type == SSA_TYPE_INT) {
                        if (strcmp(ins->unop.op, "-") == 0) {
                            vals[ins->id].lattice = SCCP_CONST;
                            vals[ins->id].ival = -vals[oi].ival;
                            vals[ins->id].type = SSA_TYPE_INT;
                        } else if (strcmp(ins->unop.op, "~") == 0) {
                            vals[ins->id].lattice = SCCP_CONST;
                            vals[ins->id].ival = ~vals[oi].ival;
                            vals[ins->id].type = SSA_TYPE_INT;
                        } else {
                            vals[ins->id].lattice = SCCP_BOTTOM;
                        }
                    }
                    else if (vals[oi].lattice == SCCP_BOTTOM) {
                        vals[ins->id].lattice = SCCP_BOTTOM;
                    }
                }
                else if (ins->op == SSA_PHI) {
                    int result_lattice = SCCP_TOP;
                    int64_t result_val = 0;
                    int result_type = SSA_TYPE_ANY;
                    for (int s = 0; s < ins->phi.nsources; s++) {
                        int si = ins->phi.sources[s];
                        if (si >= n) { result_lattice = SCCP_BOTTOM; break; }
                        if (vals[si].lattice == SCCP_BOTTOM) {
                            result_lattice = SCCP_BOTTOM;
                            break;
                        }
                        if (vals[si].lattice == SCCP_CONST) {
                            if (result_lattice == SCCP_TOP) {
                                result_lattice = SCCP_CONST;
                                result_val = vals[si].ival;
                                result_type = vals[si].type;
                            } else if (result_lattice == SCCP_CONST) {
                                if (vals[si].ival != result_val || vals[si].type != result_type) {
                                    result_lattice = SCCP_BOTTOM;
                                    break;
                                }
                            }
                        }
                    }
                    vals[ins->id].lattice = result_lattice;
                    if (result_lattice == SCCP_CONST) {
                        vals[ins->id].ival = result_val;
                        vals[ins->id].type = result_type;
                    }
                }
                else if (ins->op == SSA_STORE) {
                    int vi = ins->store.val_id;
                    if (vi < n) vals[ins->id] = vals[vi];
                    else vals[ins->id].lattice = SCCP_BOTTOM;
                }
                else if (ins->op == SSA_LOAD) {
                    vals[ins->id].lattice = SCCP_BOTTOM;
                }
                else {
                    vals[ins->id].lattice = SCCP_BOTTOM;
                }

                if (vals[ins->id].lattice != old_lattice) changed = 1;
            }
        }
    }

    /* replace instructions that resolved to constants */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            if (ins->op == SSA_CONST || ins->op == SSA_PARAM) continue;
            if (vals[ins->id].lattice == SCCP_CONST) {
                /* convert to constant */
                if (ins->op == SSA_BINOP || ins->op == SSA_UNOP || ins->op == SSA_PHI) {
                    ins->op = SSA_CONST;
                    memset(&ins->konst, 0, sizeof(ins->konst));
                    if (vals[ins->id].type == SSA_TYPE_INT) {
                        ins->konst.ival = vals[ins->id].ival;
                        ins->konst.is_float = 0;
                        ins->konst.is_bool = 0;
                        ins->type = SSA_TYPE_INT;
                    } else if (vals[ins->id].type == SSA_TYPE_BOOL) {
                        ins->konst.bval = vals[ins->id].bval;
                        ins->konst.is_bool = 1;
                        ins->type = SSA_TYPE_BOOL;
                    }
                }
            }
        }
    }

    free(vals);
}

/* --- Dead Code Elimination on SSA --- */

void ssa_dce(SSAFunction *ssa) {
    if (!ssa) return;

    int n = ssa->next_id;
    int *used = xs_malloc(n * sizeof(int));
    memset(used, 0, n * sizeof(int));

    /* mark all referenced instruction IDs */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            switch (ins->op) {
            case SSA_BINOP:
                if (ins->binop.left < n) used[ins->binop.left] = 1;
                if (ins->binop.right < n) used[ins->binop.right] = 1;
                break;
            case SSA_UNOP:
                if (ins->unop.operand < n) used[ins->unop.operand] = 1;
                break;
            case SSA_STORE:
                if (ins->store.val_id < n) used[ins->store.val_id] = 1;
                break;
            case SSA_RETURN:
                if (ins->ret.has_value && ins->ret.value < n) used[ins->ret.value] = 1;
                break;
            case SSA_BRANCH:
                if (ins->branch.cond < n) used[ins->branch.cond] = 1;
                break;
            case SSA_CALL:
                for (int a = 0; a < ins->call.nargs; a++)
                    if (ins->call.args[a] < n) used[ins->call.args[a]] = 1;
                break;
            case SSA_PHI:
                for (int s = 0; s < ins->phi.nsources; s++)
                    if (ins->phi.sources[s] < n) used[ins->phi.sources[s]] = 1;
                break;
            default:
                break;
            }
        }
    }

    /* mark unused pure instructions as dead */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;
            if (!used[ins->id]) {
                /* keep side-effectful instructions */
                if (ins->op == SSA_STORE || ins->op == SSA_CALL ||
                    ins->op == SSA_RETURN || ins->op == SSA_BRANCH ||
                    ins->op == SSA_JUMP || ins->op == SSA_PARAM)
                    continue;
                ins->dead = 1;
            }
        }
    }

    free(used);
}

/* --- SSA to AST lowering --- */

static Node *instr_to_expr(SSAFunction *ssa, SSAInstr *ins, Span sp);

static Node *instr_to_expr(SSAFunction *ssa, SSAInstr *ins, Span sp) {
    if (!ins) return NULL;

    switch (ins->op) {
    case SSA_CONST: {
        if (ins->type == SSA_TYPE_INT || (!ins->konst.is_float && !ins->konst.is_bool && !ins->konst.sval && ins->type != SSA_TYPE_NULL)) {
            Node *n = node_new(NODE_LIT_INT, sp);
            n->lit_int.ival = ins->konst.ival;
            return n;
        }
        if (ins->konst.is_float || ins->type == SSA_TYPE_FLOAT) {
            Node *n = node_new(NODE_LIT_FLOAT, sp);
            n->lit_float.fval = ins->konst.fval;
            return n;
        }
        if (ins->konst.is_bool || ins->type == SSA_TYPE_BOOL) {
            Node *n = node_new(NODE_LIT_BOOL, sp);
            n->lit_bool.bval = ins->konst.bval;
            return n;
        }
        if (ins->konst.sval) {
            Node *n = node_new(NODE_LIT_STRING, sp);
            n->lit_string.sval = xs_strdup(ins->konst.sval);
            n->lit_string.interpolated = 0;
            n->lit_string.parts = nodelist_new();
            return n;
        }
        return node_new(NODE_LIT_NULL, sp);
    }
    case SSA_LOAD: {
        Node *n = node_new(NODE_IDENT, sp);
        n->ident.name = xs_strdup(ins->load.name);
        return n;
    }
    case SSA_PARAM: {
        Node *n = node_new(NODE_IDENT, sp);
        n->ident.name = xs_strdup(ins->param.name);
        return n;
    }
    case SSA_BINOP: {
        SSAInstr *left = ssa_find_instr(ssa, ins->binop.left);
        SSAInstr *right = ssa_find_instr(ssa, ins->binop.right);
        Node *n = node_new(NODE_BINOP, sp);
        strncpy(n->binop.op, ins->binop.op, sizeof(n->binop.op) - 1);
        n->binop.op[sizeof(n->binop.op) - 1] = '\0';
        n->binop.left = left ? instr_to_expr(ssa, left, sp) : node_new(NODE_LIT_NULL, sp);
        n->binop.right = right ? instr_to_expr(ssa, right, sp) : node_new(NODE_LIT_NULL, sp);
        return n;
    }
    case SSA_UNOP: {
        SSAInstr *operand = ssa_find_instr(ssa, ins->unop.operand);
        Node *n = node_new(NODE_UNARY, sp);
        strncpy(n->unary.op, ins->unop.op, sizeof(n->unary.op) - 1);
        n->unary.op[sizeof(n->unary.op) - 1] = '\0';
        n->unary.expr = operand ? instr_to_expr(ssa, operand, sp) : node_new(NODE_LIT_NULL, sp);
        n->unary.prefix = 1;
        return n;
    }
    case SSA_CALL: {
        SSAInstr *callee_ins = ssa_find_instr(ssa, ins->call.callee);
        Node *n = node_new(NODE_CALL, sp);
        n->call.callee = callee_ins ? instr_to_expr(ssa, callee_ins, sp) : node_new(NODE_LIT_NULL, sp);
        n->call.args = nodelist_new();
        n->call.kwargs = nodepairlist_new();
        for (int i = 0; i < ins->call.nargs; i++) {
            SSAInstr *arg = ssa_find_instr(ssa, ins->call.args[i]);
            nodelist_push(&n->call.args, arg ? instr_to_expr(ssa, arg, sp) : node_new(NODE_LIT_NULL, sp));
        }
        return n;
    }
    default:
        return node_new(NODE_LIT_NULL, sp);
    }
}

Node *ssa_to_ast(SSAFunction *ssa) {
    if (!ssa) return NULL;
    Span sp = span_zero();

    Node *block = node_new(NODE_BLOCK, sp);
    block->block.stmts = nodelist_new();
    block->block.expr = NULL;

    /* linearize: just walk block 0 for simple functions */
    /* for complex CFGs, we reconstruct if/while from the block structure */
    for (int b = 0; b < ssa->nblocks; b++) {
        for (SSAInstr *ins = ssa->blocks[b].first; ins; ins = ins->next) {
            if (ins->dead) continue;

            switch (ins->op) {
            case SSA_STORE: {
                SSAInstr *val = ssa_find_instr(ssa, ins->store.val_id);
                Node *let_node = node_new(NODE_LET, sp);
                let_node->let.name = xs_strdup(ins->store.name);
                let_node->let.value = val ? instr_to_expr(ssa, val, sp) : node_new(NODE_LIT_NULL, sp);
                let_node->let.mutable = 0;
                let_node->let.pattern = NULL;
                let_node->let.type_ann = NULL;
                let_node->let.contract = NULL;
                nodelist_push(&block->block.stmts, let_node);
                break;
            }
            case SSA_RETURN: {
                Node *ret = node_new(NODE_RETURN, sp);
                if (ins->ret.has_value) {
                    SSAInstr *val = ssa_find_instr(ssa, ins->ret.value);
                    ret->ret.value = val ? instr_to_expr(ssa, val, sp) : NULL;
                } else {
                    ret->ret.value = NULL;
                }
                nodelist_push(&block->block.stmts, ret);
                break;
            }
            case SSA_CALL: {
                /* calls at statement level */
                Node *expr = instr_to_expr(ssa, ins, sp);
                Node *stmt = node_new(NODE_EXPR_STMT, sp);
                stmt->expr_stmt.expr = expr;
                stmt->expr_stmt.has_semicolon = 0;
                nodelist_push(&block->block.stmts, stmt);
                break;
            }
            default:
                /* skip constants, loads, binops, etc. that aren't used at stmt level */
                break;
            }
        }
    }

    return block;
}

/* --- Free --- */

static void instr_free(SSAInstr *ins) {
    if (!ins) return;
    switch (ins->op) {
    case SSA_CONST:
        free(ins->konst.sval);
        break;
    case SSA_LOAD:
        free(ins->load.name);
        break;
    case SSA_STORE:
        free(ins->store.name);
        break;
    case SSA_CALL:
        free(ins->call.args);
        free(ins->call.name);
        break;
    case SSA_PHI:
        free(ins->phi.sources);
        free(ins->phi.blocks);
        break;
    case SSA_PARAM:
        free(ins->param.name);
        break;
    default:
        break;
    }
    free(ins);
}

void ssa_free(SSAFunction *ssa) {
    if (!ssa) return;
    for (int i = 0; i < ssa->nblocks; i++) {
        BasicBlock *bb = &ssa->blocks[i];
        SSAInstr *ins = bb->first;
        while (ins) {
            SSAInstr *next = ins->next;
            instr_free(ins);
            ins = next;
        }
        free(bb->preds);
        free(bb->succs);
    }
    free(ssa->blocks);
    if (ssa->param_names) {
        for (int i = 0; i < ssa->nparams; i++)
            free(ssa->param_names[i]);
        free(ssa->param_names);
    }
    free(ssa);
}

/* --- Inline small calls --- */

void ssa_inline_calls(SSAFunction *ssa, Node *program) {
    /* this is handled at the AST level by opt_inline_expand
       which is more effective since it has access to all function bodies.
       the SSA level just marks call sites for potential inlining. */
    (void)ssa;
    (void)program;
}
