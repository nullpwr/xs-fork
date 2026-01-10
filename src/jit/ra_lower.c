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
#include <stdlib.h>
#include <string.h>

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
} VRegStack;

static void vstack_push(VRegStack *s, IRVReg r) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = xs_realloc(s->v, (size_t)s->cap * sizeof(IRVReg));
    }
    s->v[s->len++] = r;
}
static IRVReg vstack_pop(VRegStack *s) {
    return s->len > 0 ? s->v[--s->len] : (IRVReg)-1;
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
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
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
        case OP_ADD: return IR_ADD;
        case OP_SUB: return IR_SUB;
        case OP_MUL: return IR_MUL;
        case OP_LT:  return IR_LT;
        case OP_GT:  return IR_GT;
        case OP_LTE: return IR_LE;
        case OP_GTE: return IR_GE;
        case OP_EQ:  return IR_EQ;
        case OP_NEQ: return IR_NE;
        default:     return IR__MAX;
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
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_LT:  case OP_GT:  case OP_LTE:
            case OP_GTE: case OP_EQ:  case OP_NEQ: {
                IRVReg b = vstack_pop(&vs);
                IRVReg a = vstack_pop(&vs);
                IRVReg d = new_vreg(f);
                ir_emit(f, ir_binop_for(op), d, a, b, 0, pc);
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
    free(vs.v);

    /* Scan the lowered IR for STORE_LOCAL targets. Slots that are
     * never stored stay in "borrow" mode -- the codegen skips the
     * prologue incref, the RETURN writeback, and the err-exit
     * decref for them. */
    for (int i = 0; i < f->n_insts; i++) {
        IRInst *in = &f->insts[i];
        if (in->op == IR_STORE_LOCAL && in->imm >= 0 && in->imm < f->n_locals)
            f->local_written[in->imm] = 1;
    }

    return f;
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
