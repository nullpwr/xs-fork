/* Phase 1 of the register-allocating JIT pipeline: turn bytecode into
 * a linear IR with basic blocks. The lowering simulates the bytecode
 * stack at compile time: each PUSH creates a new vreg, each POP
 * consumes one. Branch targets start new basic blocks.
 *
 * If any bytecode op is outside the supported set, the lowerer bails
 * (returns NULL) so the caller can fall back to the template JIT. */

#include "jit/ra_ir.h"
#include "core/xs.h"
#include "vm/bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ir_try_inline_self(IRFunc *f);
static void ir_fuse_cmp_branch(IRFunc *f);

/* --- small growable array for the IR instruction stream --- */

static int ir_emit(IRFunc *f, IROp op, IRVReg dst, IRVReg src1,
                   IRVReg src2, int32_t imm, int bc_off) {
    if (f->n_insts == f->cap_insts) {
        f->cap_insts = f->cap_insts ? f->cap_insts * 2 : 64;
        f->insts = xs_realloc(f->insts, (size_t)f->cap_insts * sizeof(IRInst));
    }
    IRInst *in = &f->insts[f->n_insts];
    in->op = op;
    in->dst = dst;
    in->src1 = src1;
    in->src2 = src2;
    in->imm = imm;
    in->bc_offset = bc_off;
    for (int i = 0; i < IR_MAX_CALL_ARGS; i++) in->call_args[i] = -1;
    return f->n_insts++;
}

static IRVReg new_vreg(IRFunc *f) {
    return (IRVReg)(f->n_vregs++);
}

/* --- compile-time vreg stack --- */

typedef struct {
    IRVReg *v;
    int     len, cap;
    int     underflow;  /* set if pop was ever called while empty */
} VRegStack;

static void vstack_push(VRegStack *s, IRVReg r) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = xs_realloc(s->v, (size_t)s->cap * sizeof(IRVReg));
    }
    s->v[s->len++] = r;
}
static IRVReg vstack_pop(VRegStack *s) {
    if (s->len > 0) return s->v[--s->len];
    s->underflow = 1;
    return (IRVReg)-1;
}

/* --- supported-opcode check --- */

static int op_supported(Opcode op) {
    switch (op) {
        case OP_NOP:
        case OP_PUSH_CONST:
        case OP_PUSH_NULL:
        case OP_PUSH_TRUE:
        case OP_PUSH_FALSE:
        case OP_POP:
        case OP_DUP:
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_LOAD_GLOBAL:
        case OP_LOAD_UPVALUE:
        case OP_STORE_UPVALUE:
        case OP_MAKE_CLOSURE:
        case OP_LOAD_FIELD:
        case OP_STORE_FIELD:
        case OP_INDEX_GET:
        case OP_INDEX_SET:
        case OP_MAKE_RANGE:
        case OP_MAKE_ARRAY:
        case OP_MAKE_TUPLE:
        case OP_MAKE_MAP:
        case OP_METHOD_CALL:
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_NEG:
        case OP_NOT:
        case OP_BAND:
        case OP_BOR:
        case OP_BXOR:
        case OP_BNOT:
        case OP_SHL:
        case OP_SHR:
        case OP_LT:
        case OP_GT:
        case OP_LTE:
        case OP_GTE:
        case OP_EQ:
        case OP_NEQ:
        case OP_JUMP:
        case OP_LOOP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_RETURN:
        case OP_CALL:
            return 1;
        default:
            return 0;
    }
}

/* Map bytecode comparison opcode to its IR equivalent. */
static IROp ir_binop_for(Opcode op) {
    switch (op) {
        case OP_ADD:  return IR_ADD;
        case OP_SUB:  return IR_SUB;
        case OP_MUL:  return IR_MUL;
        case OP_DIV:  return IR_DIV;
        case OP_MOD:  return IR_MOD;
        case OP_BAND: return IR_BAND;
        case OP_BOR:  return IR_BOR;
        case OP_BXOR: return IR_BXOR;
        case OP_SHL:  return IR_SHL;
        case OP_SHR:  return IR_SHR;
        case OP_LT:   return IR_LT;
        case OP_GT:   return IR_GT;
        case OP_LTE:  return IR_LE;
        case OP_GTE:  return IR_GE;
        case OP_EQ:   return IR_EQ;
        case OP_NEQ:  return IR_NE;
        default:      return IR__MAX;
    }
}

/* Unary op bytecode -> IR. */
static IROp ir_unop_for(Opcode op) {
    switch (op) {
        case OP_NEG:  return IR_NEG;
        case OP_NOT:  return IR_NOT;
        case OP_BNOT: return IR_BNOT;
        default:      return IR__MAX;
    }
}

/* --- basic block construction ---
 *
 * Pass 1: scan bytecode, mark each PC that's a branch target. Every
 * marked PC is the start of a new basic block. PC 0 is implicitly a
 * block start. After a branch, the fall-through PC is also a block
 * start. We collect block starts into a sorted array. */

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int *find_block_starts(XSProto *p, int *n_out) {
    int cap = 16, n = 0;
    int *starts = xs_malloc(cap * sizeof(int));
    #define ADD(pc) do { \
        if (n == cap) { cap *= 2; starts = xs_realloc(starts, cap * sizeof(int)); } \
        starts[n++] = (pc); \
    } while (0)
    ADD(0);
    for (int i = 0; i < p->chunk.len; i++) {
        Instruction ins = p->chunk.code[i];
        Opcode op = INSTR_OPCODE(ins);
        if (op == OP_JUMP || op == OP_LOOP) {
            int tgt = i + 1 + (int)INSTR_sBx(ins);
            ADD(tgt);
            if (i + 1 < p->chunk.len) ADD(i + 1);
        } else if (op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE) {
            int tgt = i + 1 + (int)INSTR_sBx(ins);
            ADD(tgt);
            ADD(i + 1);
        } else if (op == OP_RETURN && i + 1 < p->chunk.len) {
            ADD(i + 1);
        }
    }
    #undef ADD
    qsort(starts, n, sizeof(int), cmp_int);
    /* dedupe */
    int w = 1;
    for (int r = 1; r < n; r++)
        if (starts[r] != starts[r - 1]) starts[w++] = starts[r];
    *n_out = w;
    return starts;
}

/* Given a PC, find its block index via the sorted `block_starts`
 * array. If the PC is exactly a block start, that's the block;
 * otherwise we find the block that contains it. For our CFG edges the
 * PC is always a block start (jumps / fall-throughs to block
 * boundaries), so a simple linear scan keeps the code small. */
static int pc_to_block(const int *starts, int n, int pc) {
    for (int i = 0; i < n; i++) if (starts[i] == pc) return i;
    return -1;
}

/* --- lowering ---
 *
 * For each block we re-simulate the bytecode stack starting empty.
 * This is correct for code without stack-leaking-across-blocks (which
 * our supported bytecode avoids), and keeps the lowerer simple. */

IRFunc *ralow_lower(XSProto *proto) {
    /* Bail out on unsupported opcodes or things that make the simple
     * per-block stack simulation unsafe (try/catch, generators, etc).
     * The bail is represented by returning NULL -- caller uses the
     * template JIT instead. */
    if (proto->is_generator) return NULL;
    for (int i = 0; i < proto->chunk.len; i++) {
        Opcode op = INSTR_OPCODE(proto->chunk.code[i]);
        if (!op_supported(op)) return NULL;
        if (op == OP_CALL) {
            int argc = INSTR_C(proto->chunk.code[i]);
            if (argc > IR_MAX_CALL_ARGS) return NULL;
        }
        if (op == OP_METHOD_CALL) {
            int argc = INSTR_A(proto->chunk.code[i]);
            if (argc > IR_MAX_CALL_ARGS) return NULL;
        }
        if (op == OP_MAKE_ARRAY || op == OP_MAKE_TUPLE) {
            int n = INSTR_C(proto->chunk.code[i]);
            if (n > IR_MAX_CALL_ARGS) return NULL;
        }
        if (op == OP_MAKE_MAP) {
            int n = INSTR_C(proto->chunk.code[i]);
            if (n * 2 > IR_MAX_CALL_ARGS) return NULL;
        }
    }

    IRFunc *f = xs_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->proto = proto;
    f->n_locals = proto->nlocals;
    f->n_consts = proto->chunk.nconsts;

    /* Pre-allocate one vreg per local slot. The allocator later
     * overrides these ranges to span the full function so LOAD_LOCAL
     * can be emitted as a reg copy. local_written is populated after
     * lowering below by scanning for IR_STORE_LOCAL. */
    if (f->n_locals > 0) {
        f->local_vregs = xs_malloc((size_t)f->n_locals * sizeof(IRVReg));
        f->local_written = xs_calloc((size_t)f->n_locals, sizeof(int8_t));
        for (int i = 0; i < f->n_locals; i++)
            f->local_vregs[i] = new_vreg(f);
    }

    /* Find block starts and build the block table. Each block's IR
     * range is populated as we lower. */
    int n_starts = 0;
    int *starts = find_block_starts(proto, &n_starts);

    f->cap_blocks = n_starts;
    f->n_blocks = n_starts;
    f->blocks = xs_calloc((size_t)n_starts, sizeof(IRBlock));
    for (int i = 0; i < n_starts; i++) {
        f->blocks[i].succ[0] = -1;
        f->blocks[i].succ[1] = -1;
    }

    VRegStack vs = {0};

    for (int bi = 0; bi < n_starts; bi++) {
        int pc_lo = starts[bi];
        int pc_hi = (bi + 1 < n_starts) ? starts[bi + 1] : proto->chunk.len;
        f->blocks[bi].start = f->n_insts;
        vs.len = 0;  /* reset per-block stack simulation */

        for (int pc = pc_lo; pc < pc_hi; pc++) {
            Instruction ins = proto->chunk.code[pc];
            Opcode op = INSTR_OPCODE(ins);
            int bx = (int)INSTR_Bx(ins);
            int sbx = (int)INSTR_sBx(ins);
            int c = (int)INSTR_C(ins);

            switch (op) {
            case OP_NOP:
                break;
            case OP_PUSH_CONST: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_CONST, d, -1, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_PUSH_NULL: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_PUSH_NULL, d, -1, -1, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_PUSH_TRUE: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_PUSH_TRUE, d, -1, -1, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_PUSH_FALSE: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_PUSH_FALSE, d, -1, -1, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_POP: {
                IRVReg v = vstack_pop(&vs);
                ir_emit(f, IR_POP, -1, v, -1, 0, pc);
                break;
            }
            case OP_DUP: {
                IRVReg top = vs.len > 0 ? vs.v[vs.len - 1] : -1;
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_DUP, d, top, -1, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_LOAD_LOCAL: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_LOAD_LOCAL, d, -1, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_STORE_LOCAL: {
                IRVReg v = vstack_pop(&vs);
                ir_emit(f, IR_STORE_LOCAL, -1, v, -1, bx, pc);
                break;
            }
            case OP_LOAD_GLOBAL: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_LOAD_GLOBAL, d, -1, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_LOAD_UPVALUE: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_LOAD_UP, d, -1, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_STORE_UPVALUE: {
                IRVReg v = vstack_pop(&vs);
                ir_emit(f, IR_STORE_UP, -1, v, -1, bx, pc);
                break;
            }
            case OP_MAKE_CLOSURE: {
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_MAKE_CLOSURE, d, -1, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD:
            case OP_BAND: case OP_BOR: case OP_BXOR:
            case OP_SHL: case OP_SHR:
            case OP_LT:  case OP_GT:  case OP_LTE:
            case OP_GTE: case OP_EQ:  case OP_NEQ: {
                IRVReg b = vstack_pop(&vs);
                IRVReg a = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                ir_emit(f, ir_binop_for(op), d, a, b, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_NEG: case OP_NOT: case OP_BNOT: {
                IRVReg a = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                ir_emit(f, ir_unop_for(op), d, a, -1, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_JUMP: case OP_LOOP: {
                int tgt = pc + 1 + sbx;
                int tb = pc_to_block(starts, n_starts, tgt);
                ir_emit(f, IR_JUMP, -1, -1, -1, tb, pc);
                f->blocks[bi].succ[0] = tb;
                f->blocks[bi].n_succ = 1;
                goto end_block;
            }
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_TRUE: {
                IRVReg cond = vstack_pop(&vs);
                int tgt = pc + 1 + sbx;
                int tb = pc_to_block(starts, n_starts, tgt);
                int fb = pc_to_block(starts, n_starts, pc + 1);
                ir_emit(f, op == OP_JUMP_IF_FALSE ? IR_JIF_FALSE : IR_JIF_TRUE,
                        -1, cond, -1, tb, pc);
                f->blocks[bi].succ[0] = tb;
                f->blocks[bi].succ[1] = fb;
                f->blocks[bi].n_succ = 2;
                goto end_block;
            }
            case OP_RETURN: {
                IRVReg v = vstack_pop(&vs);
                ir_emit(f, IR_RETURN, -1, v, -1, 0, pc);
                f->blocks[bi].n_succ = 0;
                goto end_block;
            }
            case OP_CALL: {
                int argc = c;
                IRVReg args[IR_MAX_CALL_ARGS];
                for (int i = argc - 1; i >= 0; i--) args[i] = vstack_pop(&vs);
                IRVReg callee = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                int idx = ir_emit(f, IR_CALL, d, callee, -1, argc, pc);
                for (int i = 0; i < argc; i++) f->insts[idx].call_args[i] = args[i];
                vstack_push(&vs, d);
                break;
            }
            case OP_INDEX_GET: {
                IRVReg idx = vstack_pop(&vs);
                IRVReg col = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_INDEX_GET, d, col, idx, 0, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_INDEX_SET: {
                IRVReg val = vstack_pop(&vs);
                IRVReg idx = vstack_pop(&vs);
                IRVReg col = vstack_pop(&vs);
                int iidx = ir_emit(f, IR_INDEX_SET, -1, col, idx, 0, pc);
                f->insts[iidx].call_args[0] = val;
                break;
            }
            case OP_LOAD_FIELD: {
                IRVReg obj = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                ir_emit(f, IR_LOAD_FIELD, d, obj, -1, bx, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_STORE_FIELD: {
                IRVReg val = vstack_pop(&vs);
                IRVReg obj = vstack_pop(&vs);
                ir_emit(f, IR_STORE_FIELD, -1, obj, val, bx, pc);
                break;
            }
            case OP_MAKE_RANGE: {
                IRVReg end = vstack_pop(&vs);
                IRVReg start = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                /* A=inclusive; bytecode gives it via INSTR_A, stash in imm. */
                int incl = (int)INSTR_A(ins);
                ir_emit(f, IR_MAKE_RANGE, d, start, end, incl, pc);
                vstack_push(&vs, d);
                break;
            }
            case OP_MAKE_ARRAY:
            case OP_MAKE_TUPLE: {
                int n = c;
                IRVReg items[IR_MAX_CALL_ARGS];
                for (int i = n - 1; i >= 0; i--) items[i] = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                IROp oir = op == OP_MAKE_ARRAY ? IR_MAKE_ARRAY : IR_MAKE_TUPLE;
                int idx = ir_emit(f, oir, d, -1, -1, n, pc);
                for (int i = 0; i < n; i++) f->insts[idx].call_args[i] = items[i];
                vstack_push(&vs, d);
                break;
            }
            case OP_MAKE_MAP: {
                int npairs = c;
                IRVReg entries[IR_MAX_CALL_ARGS];
                for (int i = npairs * 2 - 1; i >= 0; i--) entries[i] = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                int idx = ir_emit(f, IR_MAKE_MAP, d, -1, -1, npairs, pc);
                for (int i = 0; i < npairs * 2; i++) f->insts[idx].call_args[i] = entries[i];
                vstack_push(&vs, d);
                break;
            }
            case OP_METHOD_CALL: {
                int argc = (int)INSTR_A(ins);
                IRVReg args[IR_MAX_CALL_ARGS];
                for (int i = argc - 1; i >= 0; i--) args[i] = vstack_pop(&vs);
                IRVReg recv = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                int idx = ir_emit(f, IR_METHOD_CALL, d, recv, -1, argc, pc);
                for (int i = 0; i < argc; i++) f->insts[idx].call_args[i] = args[i];
                vstack_push(&vs, d);
                break;
            }
            default:
                /* Shouldn't reach here -- op_supported() filtered these. */
                free(starts);
                free(vs.v);
                irfunc_free(f);
                return NULL;
            }
        }
        /* Block ended by falling off -- link to the next block if any. */
        if (f->blocks[bi].n_succ == 0 && bi + 1 < n_starts) {
            /* Check last emitted op: if it's a terminator, leave succ alone;
             * else, add fall-through. */
            int last_idx = f->n_insts - 1;
            if (last_idx < f->blocks[bi].start ||
                (f->insts[last_idx].op != IR_JUMP &&
                 f->insts[last_idx].op != IR_JIF_FALSE &&
                 f->insts[last_idx].op != IR_JIF_TRUE &&
                 f->insts[last_idx].op != IR_RETURN)) {
                /* synthetic fall-through jump */
                ir_emit(f, IR_JUMP, -1, -1, -1, bi + 1, pc_hi - 1);
                f->blocks[bi].succ[0] = bi + 1;
                f->blocks[bi].n_succ = 1;
            }
        }
    end_block:
        f->blocks[bi].end = f->n_insts;
    }
    free(starts);
    int underflowed = vs.underflow;
    free(vs.v);

    /* Per-block stack simulation assumes each block starts with an
     * empty operand stack. Compiler patterns like `a and b` leak values
     * across branches (the DUP before JUMP_IF_FALSE stays live on the
     * interpreter stack into the merge block), so our per-block
     * re-simulation hits a pop on an empty stack and manufactures a
     * -1 vreg. Rather than lower that to garbage code, bail out
     * cleanly so the template JIT handles the proto. */
    if (underflowed) {
        irfunc_free(f);
        return NULL;
    }

    /* Scan the lowered IR for STORE_LOCAL targets. Slots that are
     * never stored stay in "borrow" mode -- the codegen skips the
     * prologue incref, the RETURN writeback, and the err-exit
     * decref for them. */
    for (int i = 0; i < f->n_insts; i++) {
        IRInst *in = &f->insts[i];
        if (in->op == IR_STORE_LOCAL && in->imm >= 0 && in->imm < f->n_locals)
            f->local_written[in->imm] = 1;
    }

    /* Attempt one level of self-recursive inlining. Controlled by
     * XS_JIT_INLINE (default on; set to 0/n/N to disable). */
    {
        const char *off = getenv("XS_JIT_INLINE");
        if (!off || (off[0] != '0' && off[0] != 'n' && off[0] != 'N'))
            ir_try_inline_self(f);
    }

    /* Fuse CMP + JIF pairs into IR_CMP_BR so the SMI hot path can go
     * straight from `cmp` to `jcc` without materialising a TRUE/FALSE
     * heap singleton. Runs AFTER the self-inliner so clone bodies get
     * fused too. Toggled by XS_JIT_CMPBR (default on). */
    {
        const char *off = getenv("XS_JIT_CMPBR");
        if (!off || (off[0] != '0' && off[0] != 'n' && off[0] != 'N'))
            ir_fuse_cmp_branch(f);
    }

    return f;
}

/* Is `op` one of the six comparison ops lowered from OP_LT .. OP_NEQ? */
static int ir_is_cmp(IROp op) {
    return op == IR_LT || op == IR_GT || op == IR_LE ||
           op == IR_GE || op == IR_EQ || op == IR_NE;
}

/* Encode an IR cmp op as the 3-bit kind stored in IR_CMP_BR's
 * call_args[0]. Matches the switch in ra_codegen.c. */
static int ir_cmp_kind(IROp op) {
    switch (op) {
    case IR_LT: return 0;
    case IR_GT: return 1;
    case IR_LE: return 2;
    case IR_GE: return 3;
    case IR_EQ: return 4;
    case IR_NE: return 5;
    default:    return -1;
    }
}

/* Peephole: rewrite an in-block pair
 *
 *     <CMP>   dst=t, src1=a, src2=b
 *     JIF_*   src1=t, imm=target
 *
 * as
 *
 *     CMP_BR  src1=a, src2=b, imm=target, call_args[0]=packed
 *     NOP
 *
 * The vreg `t` is produced by the CMP and consumed by the JIF. Under
 * tier-2's per-block stack simulation (see ralow_lower) each CMP's dst
 * is pushed and immediately popped by the following consumer, so it
 * cannot be live across a block boundary. The JIF's presence after the
 * CMP is therefore sufficient proof that `t` has no other consumer.
 *
 * We also require the pair to sit in the same block (same block range)
 * and the JIF to be the block's terminator. Both invariants hold by
 * construction of the lowerer: JIF_* always terminates its block. */
static void ir_fuse_cmp_branch(IRFunc *f) {
    if (!f) return;
    for (int bi = 0; bi < f->n_blocks; bi++) {
        IRBlock *b = &f->blocks[bi];
        if (b->end - b->start < 2) continue;
        for (int ii = b->start; ii + 1 < b->end; ii++) {
            IRInst *cmp = &f->insts[ii];
            IRInst *jif = &f->insts[ii + 1];
            if (!ir_is_cmp(cmp->op)) continue;
            if (jif->op != IR_JIF_FALSE && jif->op != IR_JIF_TRUE) continue;
            if (cmp->dst < 0 || jif->src1 != cmp->dst) continue;

            int kind = ir_cmp_kind(cmp->op);
            int branch_if_false = (jif->op == IR_JIF_FALSE) ? 1 : 0;

            IRInst merged;
            memset(&merged, 0, sizeof merged);
            merged.op = IR_CMP_BR;
            merged.dst = -1;
            merged.src1 = cmp->src1;
            merged.src2 = cmp->src2;
            merged.imm = jif->imm;
            for (int k = 0; k < IR_MAX_CALL_ARGS; k++) merged.call_args[k] = -1;
            merged.call_args[0] = (IRVReg)((kind << 1) | branch_if_false);
            merged.bc_offset = cmp->bc_offset;
            *cmp = merged;

            IRInst nop;
            memset(&nop, 0, sizeof nop);
            nop.op = IR_NOP;
            nop.dst = -1;
            nop.src1 = -1;
            nop.src2 = -1;
            for (int k = 0; k < IR_MAX_CALL_ARGS; k++) nop.call_args[k] = -1;
            nop.bc_offset = jif->bc_offset;
            *jif = nop;
        }
    }
}

/* ================================================================
 *         Self-recursion inliner (single-level, no guard).
 *
 * Splices a clone of the proto's IR at every CALL site whose callee
 * is a LOAD_GLOBAL of the proto's own name. Safe without a runtime
 * guard because tier-2 already rejects protos containing
 * OP_STORE_GLOBAL (so `globals[self_name]` cannot change during
 * execution of our body), and we additionally require that every
 * CALL in the body is itself a self-call -- meaning no external
 * callee can perturb globals either.
 *
 * Strategy per call site at instruction index `ci` inside block B:
 *   1. Split B at `ci`: keep [B.start, ci] and overwrite `ci` with
 *      an IR_JUMP to the clone's entry block. Move [ci+1, B.end)
 *      into a fresh `B_post` block that inherits B's original succs.
 *   2. Emit a clone of each original block with all vregs remapped
 *      to fresh IDs. Inject at the top of the clone entry the arg
 *      plumbing (IR_MOVE from caller args to the clone's fresh
 *      local_vregs, plus IR_PUSH_NULL for slots beyond arity).
 *   3. Rewrite each cloned IR_RETURN as IR_MOVE(outer_dst, ret_val)
 *      followed by IR_JUMP to B_post. Those blocks point at B_post
 *      as their sole successor.
 * ================================================================ */

static void ensure_inst_cap(IRFunc *f, int need) {
    if (f->cap_insts >= need) return;
    int n = f->cap_insts ? f->cap_insts * 2 : 64;
    while (n < need) n *= 2;
    f->insts = xs_realloc(f->insts, (size_t)n * sizeof(IRInst));
    f->cap_insts = n;
}
static void ensure_block_cap(IRFunc *f, int need) {
    if (f->cap_blocks >= need) return;
    int n = f->cap_blocks ? f->cap_blocks * 2 : 16;
    while (n < need) n *= 2;
    f->blocks = xs_realloc(f->blocks, (size_t)n * sizeof(IRBlock));
    f->cap_blocks = n;
}

static int ir_callees_are_all_self(IRFunc *f, int up_to) {
    if (!f->proto || !f->proto->name) return 0;
    for (int i = 0; i < up_to; i++) {
        IRInst *in = &f->insts[i];
        if (in->op != IR_CALL) continue;
        IRVReg callee = in->src1;
        int di = -1;
        for (int j = i - 1; j >= 0; j--) {
            if (f->insts[j].dst == callee) { di = j; break; }
        }
        if (di < 0 || f->insts[di].op != IR_LOAD_GLOBAL) return 0;
        int ci = f->insts[di].imm;
        if (ci < 0 || ci >= f->proto->chunk.nconsts) return 0;
        Value *nv = f->proto->chunk.consts[ci];
        if (!nv || VAL_TAG(nv) != XS_STR || !nv->s) return 0;
        if (strcmp(nv->s, f->proto->name) != 0) return 0;
    }
    return 1;
}

/* Inline a single CALL at `ci`. Uses the snapshot captured before
 * any mutation so we always clone the ORIGINAL body even when
 * several inline passes run back-to-back. */
static void ir_inline_one(IRFunc *f, int ci,
                           const IRInst *orig_insts, int orig_n_insts,
                           const IRBlock *orig_blocks, int orig_n_blocks,
                           int orig_n_vregs, int orig_n_locals,
                           const IRVReg *orig_local_vregs) {
    (void)orig_n_insts;

    /* Capture call details from the live IR (it may have already
     * been mutated by an earlier rewrite of this same site, which
     * shouldn't happen but be defensive). */
    IRInst call = f->insts[ci];
    IRVReg dst_outer = call.dst;
    int argc = call.imm;
    IRVReg args[IR_MAX_CALL_ARGS];
    for (int i = 0; i < argc; i++) args[i] = call.call_args[i];

    /* Find the block that contains ci. */
    int B_idx = -1;
    for (int b = 0; b < f->n_blocks; b++) {
        if (f->blocks[b].start <= ci && ci < f->blocks[b].end) {
            B_idx = b; break;
        }
    }
    if (B_idx < 0) return;

    IRBlock B_orig = f->blocks[B_idx];

    /* Allocate per-vreg remap. Original vreg ids 0..orig_n_vregs-1
     * map to fresh ids starting at f->n_vregs. Original local_vregs
     * get their fresh-id slots overwritten below so the cloned
     * LOAD_LOCAL / STORE_LOCAL pick up the clone's own locals. */
    IRVReg *vmap = xs_calloc((size_t)orig_n_vregs, sizeof(IRVReg));
    for (int v = 0; v < orig_n_vregs; v++) vmap[v] = (IRVReg)(f->n_vregs++);

    IRVReg *clone_locals = xs_malloc((size_t)orig_n_locals * sizeof(IRVReg));
    for (int i = 0; i < orig_n_locals; i++) {
        clone_locals[i] = (IRVReg)(f->n_vregs++);
        if (orig_local_vregs && orig_local_vregs[i] >= 0
            && orig_local_vregs[i] < orig_n_vregs)
            vmap[orig_local_vregs[i]] = clone_locals[i];
    }

    /* Reserve block slots: one B_post plus orig_n_blocks clones. */
    int B_post_idx = f->n_blocks;
    int clone_base = f->n_blocks + 1;
    int new_n_blocks = f->n_blocks + 1 + orig_n_blocks;
    ensure_block_cap(f, new_n_blocks);
    for (int b = B_post_idx; b < new_n_blocks; b++) {
        memset(&f->blocks[b], 0, sizeof(IRBlock));
        f->blocks[b].succ[0] = f->blocks[b].succ[1] = -1;
    }
    f->n_blocks = new_n_blocks;

    /* Copy the post-CALL slice of B into B_post. */
    int post_count = B_orig.end - (ci + 1);
    ensure_inst_cap(f, f->n_insts + post_count);
    int post_start = f->n_insts;
    for (int i = 0; i < post_count; i++)
        f->insts[f->n_insts++] = f->insts[ci + 1 + i];
    f->blocks[B_post_idx].start = post_start;
    f->blocks[B_post_idx].end = f->n_insts;
    f->blocks[B_post_idx].succ[0] = B_orig.succ[0];
    f->blocks[B_post_idx].succ[1] = B_orig.succ[1];
    f->blocks[B_post_idx].n_succ = B_orig.n_succ;

    /* Truncate B and overwrite the CALL slot with a jump to clone
     * entry. The original LOAD_GLOBAL that produced the callee sits
     * earlier in B and becomes dead -- harmless, DCE-friendly. */
    f->blocks[B_idx].end = ci + 1;
    f->blocks[B_idx].succ[0] = clone_base;
    f->blocks[B_idx].succ[1] = -1;
    f->blocks[B_idx].n_succ = 1;
    {
        IRInst *ji = &f->insts[ci];
        memset(ji, 0, sizeof(IRInst));
        ji->op = IR_JUMP;
        ji->dst = -1; ji->src1 = -1; ji->src2 = -1;
        ji->imm = clone_base;
        ji->bc_offset = call.bc_offset;
        for (int k = 0; k < IR_MAX_CALL_ARGS; k++) ji->call_args[k] = -1;
    }

    /* Clone each original block into positions [clone_base, clone_base+orig_n_blocks). */
    for (int b = 0; b < orig_n_blocks; b++) {
        IRBlock src_b = orig_blocks[b];
        int dst_bi = clone_base + b;
        f->blocks[dst_bi].start = f->n_insts;

        if (b == 0) {
            /* Prepend arg setup: IR_MOVE args into clone locals, and
             * IR_PUSH_NULL for locals past arity. */
            for (int i = 0; i < argc; i++) {
                ensure_inst_cap(f, f->n_insts + 1);
                IRInst *ni = &f->insts[f->n_insts++];
                memset(ni, 0, sizeof(IRInst));
                ni->op = IR_MOVE;
                ni->dst = clone_locals[i];
                ni->src1 = args[i];
                ni->src2 = -1;
                ni->bc_offset = call.bc_offset;
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) ni->call_args[k] = -1;
            }
            for (int i = argc; i < orig_n_locals; i++) {
                ensure_inst_cap(f, f->n_insts + 1);
                IRInst *ni = &f->insts[f->n_insts++];
                memset(ni, 0, sizeof(IRInst));
                ni->op = IR_PUSH_NULL;
                ni->dst = clone_locals[i];
                ni->src1 = -1; ni->src2 = -1;
                ni->bc_offset = call.bc_offset;
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) ni->call_args[k] = -1;
            }
        }

        int src_ends_on_return = 0;
        for (int i = src_b.start; i < src_b.end; i++) {
            IRInst src_in = orig_insts[i];  /* value-copy from snapshot */
            ensure_inst_cap(f, f->n_insts + 2);
            IRInst *ni = &f->insts[f->n_insts++];
            *ni = src_in;

            if (ni->dst >= 0 && ni->dst < orig_n_vregs) ni->dst = vmap[ni->dst];
            if (ni->src1 >= 0 && ni->src1 < orig_n_vregs) ni->src1 = vmap[ni->src1];
            if (ni->src2 >= 0 && ni->src2 < orig_n_vregs) ni->src2 = vmap[ni->src2];
            for (int k = 0; k < IR_MAX_CALL_ARGS; k++)
                if (ni->call_args[k] >= 0 && ni->call_args[k] < orig_n_vregs)
                    ni->call_args[k] = vmap[ni->call_args[k]];

            if (ni->op == IR_JUMP || ni->op == IR_JIF_FALSE || ni->op == IR_JIF_TRUE) {
                if (ni->imm >= 0 && ni->imm < orig_n_blocks)
                    ni->imm = ni->imm + clone_base;
            } else if (ni->op == IR_LOAD_LOCAL && ni->imm >= 0
                       && ni->imm < orig_n_locals) {
                /* Cloned LOAD_LOCAL's imm still refers to the CALLER's
                 * slot index; if we leave it as LOAD_LOCAL, codegen
                 * would read f->local_vregs[imm] (the caller's local)
                 * instead of the clone's fresh local. Rewrite into
                 * IR_DUP from the clone's own local_vreg. */
                ni->op = IR_DUP;
                ni->src1 = clone_locals[ni->imm];
                ni->src2 = -1;
                ni->imm = 0;
            } else if (ni->op == IR_STORE_LOCAL && ni->imm >= 0
                       && ni->imm < orig_n_locals) {
                /* Cloned STORE_LOCAL has to decref the clone's own
                 * current local-vreg value and then take src1. Model
                 * it as IR_POP(clone_local) + IR_MOVE(clone_local <- src1).
                 * Clone locals are initialized to null in the entry
                 * block so the first pop is safe. */
                IRVReg new_val = ni->src1;
                ni->op = IR_POP;
                ni->src1 = clone_locals[ni->imm];
                ni->src2 = -1;
                ni->dst = -1;
                ni->imm = 0;
                IRInst *nj = &f->insts[f->n_insts++];
                memset(nj, 0, sizeof(IRInst));
                nj->op = IR_MOVE;
                nj->dst = clone_locals[src_in.imm];
                nj->src1 = new_val;
                nj->src2 = -1;
                nj->bc_offset = src_in.bc_offset;
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) nj->call_args[k] = -1;
            } else if (ni->op == IR_RETURN) {
                /* Rewrite into IR_MOVE + IR_JUMP. The current ni
                 * slot becomes the MOVE; append a JUMP after it. */
                IRVReg ret_val = ni->src1;
                ni->op = IR_MOVE;
                ni->dst = dst_outer;
                ni->src1 = ret_val;
                ni->src2 = -1;
                IRInst *nj = &f->insts[f->n_insts++];
                memset(nj, 0, sizeof(IRInst));
                nj->op = IR_JUMP;
                nj->dst = -1; nj->src1 = -1; nj->src2 = -1;
                nj->imm = B_post_idx;
                nj->bc_offset = src_in.bc_offset;
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) nj->call_args[k] = -1;
                src_ends_on_return = 1;
            }
        }
        f->blocks[dst_bi].end = f->n_insts;

        if (src_ends_on_return) {
            f->blocks[dst_bi].succ[0] = B_post_idx;
            f->blocks[dst_bi].succ[1] = -1;
            f->blocks[dst_bi].n_succ = 1;
        } else {
            f->blocks[dst_bi].n_succ = src_b.n_succ;
            for (int s = 0; s < src_b.n_succ; s++) {
                int succ = src_b.succ[s];
                f->blocks[dst_bi].succ[s] =
                    (succ >= 0 && succ < orig_n_blocks) ? (succ + clone_base) : succ;
            }
        }
    }

    free(vmap);
    free(clone_locals);
}

static int ir_try_inline_self(IRFunc *f) {
    if (!f || !f->proto || !f->proto->name) return 0;

    int orig_n_insts = f->n_insts;
    int orig_n_blocks = f->n_blocks;
    int orig_n_vregs = f->n_vregs;
    int orig_n_locals = f->n_locals;

    /* Safety: every CALL must target self. */
    if (!ir_callees_are_all_self(f, orig_n_insts)) return 0;

    /* Count CALL sites and apply a budget so we don't blow up tiny
     * helper functions into megabytes of code. */
    int n_calls = 0;
    int call_positions[8];
    for (int i = 0; i < orig_n_insts; i++) {
        if (f->insts[i].op != IR_CALL) continue;
        if (n_calls >= 8) return 0;
        call_positions[n_calls++] = i;
    }
    if (n_calls == 0) return 0;
    if (orig_n_insts > 80) return 0;
    if (n_calls * orig_n_insts > 800) return 0;

    /* Snapshot original IR -- the in-place rewrite mutates both
     * blocks (we truncate B, add new succs) and insts (we overwrite
     * the CALL, copy tail to B_post, append clone body). Cloning
     * from the snapshot keeps subsequent sites correct. */
    IRInst  *snap_insts = xs_malloc((size_t)orig_n_insts * sizeof(IRInst));
    IRBlock *snap_blocks = xs_malloc((size_t)orig_n_blocks * sizeof(IRBlock));
    memcpy(snap_insts,  f->insts,  (size_t)orig_n_insts  * sizeof(IRInst));
    memcpy(snap_blocks, f->blocks, (size_t)orig_n_blocks * sizeof(IRBlock));
    IRVReg *snap_locals = NULL;
    if (orig_n_locals > 0 && f->local_vregs) {
        snap_locals = xs_malloc((size_t)orig_n_locals * sizeof(IRVReg));
        memcpy(snap_locals, f->local_vregs,
               (size_t)orig_n_locals * sizeof(IRVReg));
    }

    /* Process call sites in reverse so earlier positions remain
     * valid (later ones get rewritten first). */
    for (int p = n_calls - 1; p >= 0; p--) {
        ir_inline_one(f, call_positions[p],
                      snap_insts, orig_n_insts,
                      snap_blocks, orig_n_blocks,
                      orig_n_vregs, orig_n_locals, snap_locals);
    }

    /* Rescan local_written for any slots that newly gained a store
     * via the inlined copies. The inliner adds LOAD_LOCAL / STORE_LOCAL
     * that still reference the ORIGINAL local_vregs (caller's) only
     * when our safety check permits, which it doesn't here -- those
     * ops end up retargeted at clone_locals through vmap -- so the
     * caller's local_written flags don't need to change. */

    free(snap_insts);
    free(snap_blocks);
    free(snap_locals);
    return 1;
}

void irfunc_free(IRFunc *f) {
    if (!f) return;
    if (f->blocks) {
        for (int i = 0; i < f->n_blocks; i++) {
            free(f->blocks[i].use);
            free(f->blocks[i].def);
            free(f->blocks[i].live_in);
            free(f->blocks[i].live_out);
        }
        free(f->blocks);
    }
    free(f->insts);
    free(f->local_vregs);
    free(f->local_written);
    free(f);
}
