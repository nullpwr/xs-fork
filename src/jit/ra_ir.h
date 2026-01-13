/* Tier-2 JIT: bytecode -> IR -> register allocation -> x86-64.
 *
 * This replaces the per-opcode dispatch template JIT for protos whose
 * bytecode fits within a supported subset. The pipeline:
 *
 *   1. ralow_lower:   bytecode -> linear IR + basic blocks
 *   2. ralow_liveness: per-block use/def + iterative live-in/live-out
 *   3. ralow_alloc:    linear-scan register allocation with spilling
 *   4. ralow_codegen:  emit x86-64 using the allocation
 *
 * Values flow as SSA-ish virtual registers. At VM-visible boundaries
 * (CALL, RETURN, throws, deopt) registers are flushed to vm->sp so the
 * rest of the runtime sees the stack in its expected shape. */

#ifndef XS_JIT_RA_IR_H
#define XS_JIT_RA_IR_H

#include <stdint.h>
#include "core/xs.h"
#include "vm/bytecode.h"

/* IR opcode set. These are a strict subset of the bytecode opcodes --
 * a proto that contains any opcode outside the supported set falls
 * back to the template JIT. Keeping the set small keeps the compiler
 * small; we can grow it opcode by opcode as wins are measured. */
typedef enum {
    /* pure value production */
    IR_CONST,        /* dst = consts[imm] (incref'd copy pushed later) */
    IR_LOAD_LOCAL,   /* dst = frame->base[imm] (incref'd) */
    IR_LOAD_GLOBAL,  /* dst = global via IC; imm = const index of name */
    IR_LOAD_UP,      /* dst = *(CL->upvalues[imm]->ptr), incref'd */
    IR_PUSH_NULL,
    IR_PUSH_TRUE,
    IR_PUSH_FALSE,

    /* Create a new closure from an inner proto descriptor. imm is the
     * const-index of an XS_INT whose value is the inner[] array index
     * (matches the OP_MAKE_CLOSURE encoding). Slow-path dispatches to
     * vm_step_jit; the JIT handles the "it's a rare op" case by just
     * running the interpreter on that one instruction. */
    IR_MAKE_CLOSURE,

    /* Containers, field access, methods, and literal construction. All
     * of these dispatch through vm_step_jit -- the JIT just keeps them
     * inside the tier-2 subset instead of bailing the whole proto to
     * tier 1 on any aggregate op. imm is set for documentation (and to
     * make future inline fast paths easy to wire up) but the codegen
     * relies only on operand flushing + interpreter step + optional
     * result pop:
     *
     *   IR_INDEX_GET   src1=container, src2=index   -> dst
     *   IR_INDEX_SET   src1=container, src2=index, call_args[0]=value -> -
     *   IR_LOAD_FIELD  src1=object     (imm = name const)  -> dst
     *   IR_STORE_FIELD src1=object, src2=value (imm=name)  -> -
     *   IR_MAKE_RANGE  src1=start,    src2=end     (imm = inclusive) -> dst
     *   IR_MAKE_ARRAY  call_args[0..n-1] = items  (imm=n)  -> dst
     *   IR_MAKE_TUPLE  call_args[0..n-1] = items  (imm=n)  -> dst
     *   IR_MAKE_MAP    call_args[0..2n-1] = k0,v0,k1,v1,.. (imm=n pairs) -> dst
     *   IR_METHOD_CALL src1=receiver, call_args[0..argc-1]=args (imm=argc) -> dst
     */
    IR_INDEX_GET, IR_INDEX_SET,
    IR_LOAD_FIELD, IR_STORE_FIELD,
    IR_MAKE_RANGE, IR_MAKE_ARRAY, IR_MAKE_TUPLE, IR_MAKE_MAP,
    IR_METHOD_CALL,

    /* Generic interpreter-step dispatch. src1/src2/call_args supply
     * the operands to flush onto vm->sp in order, and dst (if >= 0)
     * receives the produced result. Used for any bytecode whose
     * semantics are tied up in the interpreter (e.g. arithmetic dunder
     * dispatch, class/enum/module construction, trace hooks) but which
     * still advances frame->ip normally. */
    IR_VM_STEP,

    /* Same operand flushing as IR_VM_STEP, but emits a post-step check
     * on frame->ip. If the interpreter's step left frame->ip at any
     * address other than the natural next bytecode instruction --
     * THROW unwinding, TAIL_CALL replacing the frame, YIELD / AWAIT /
     * SPAWN / EFFECT_* suspending -- the emitted code jumps to a deopt
     * trampoline that writes back local_vregs and returns from the
     * jit_entry so tier2_run_until / the interpreter can pick up
     * wherever the op left the frame. */
    IR_VM_STEP_CF,

    /* pure value consumption */
    IR_STORE_LOCAL,  /* frame->base[imm] = src1 (decref old) */
    IR_STORE_UP,     /* *(CL->upvalues[imm]->ptr) = src1, decref old */
    IR_POP,          /* decref src1 */

    /* binops: dst = src1 op src2, SMI fast path + slow fallback */
    IR_ADD, IR_SUB, IR_MUL,
    IR_DIV, IR_MOD,              /* integer div/mod with div-by-zero guard */
    IR_BAND, IR_BOR, IR_BXOR,    /* bitwise on raw SMI bits (no untag needed) */
    IR_SHL, IR_SHR,              /* shift by SMI count */
    IR_LT, IR_GT, IR_LE, IR_GE, IR_EQ, IR_NE,

    /* unary ops on a single value */
    IR_NEG,   /* arithmetic negate (SMI fast path + overflow-to-slow) */
    IR_NOT,   /* logical not -- always slow path (truthy test) */
    IR_BNOT,  /* bitwise complement */

    /* control flow */
    IR_JUMP,         /* unconditional jump to imm = block index */
    IR_JIF_FALSE,    /* if !src1 jump to imm else fall through */
    IR_JIF_TRUE,     /* if src1 jump to imm else fall through */

    /* Fused compare-and-branch. Produced by the lowerer's peephole when
     * a CMP's sole consumer is the JIF immediately after. Saves the
     * TRUE/FALSE heap singleton round-trip on the SMI hot path.
     *
     *   op   = IR_CMP_BR
     *   src1 = left operand
     *   src2 = right operand
     *   dst  = -1  (no produced value)
     *   imm  = target block index (JIF's jump target)
     *   call_args[0] = packed (cmp_kind << 1) | branch_if_false
     *       cmp_kind: 0=LT 1=GT 2=LE 3=GE 4=EQ 5=NE
     *       branch_if_false: 0 for JIF_TRUE origin, 1 for JIF_FALSE
     */
    IR_CMP_BR,

    /* Emits no code; left behind by the CMP_BR peephole where the
     * original JIF instruction used to sit, keeping instruction indices
     * (and therefore block ranges) stable. */
    IR_NOP,

    /* calls: src1 = callee, then srcs[] = args (argc of them) */
    IR_CALL,

    /* return src1 */
    IR_RETURN,

    /* misc */
    IR_DUP,          /* dst = src1 (incref) */
    IR_MOVE,         /* dst = src1 (no refcount change, ownership transfer);
                      * used only by the self-inliner to splice caller args
                      * into inlined-callee locals and to route inlined
                      * RETURN values into the outer CALL's destination. */

    IR__MAX
} IROp;

#define IR_MAX_CALL_ARGS 16   /* anything above this falls back to template JIT */

typedef int16_t IRVReg;   /* virtual register id; -1 = no operand */

typedef struct {
    IROp     op;
    IRVReg   dst;                     /* -1 if op has no output */
    IRVReg   src1;                    /* primary input or -1 */
    IRVReg   src2;                    /* secondary input or -1 */
    int32_t  imm;                     /* const index, slot, block, or argc */
    IRVReg   call_args[IR_MAX_CALL_ARGS]; /* used only by IR_CALL */
    int      bc_offset;               /* original bytecode instruction index -- for deopt */
} IRInst;

/* A basic block is a maximal straight-line sequence of IR instructions.
 * Control flow enters at `start` and leaves at `end - 1` which is a
 * branch/jump/return. Fall-through blocks are connected via the
 * branch's imm (conditional) and through the next block in index order
 * (unconditional). */
typedef struct {
    int      start;       /* first IR inst index */
    int      end;         /* one past last IR inst index */
    int      succ[2];     /* up to 2 successor block indices; -1 = no succ */
    int      n_succ;
    /* liveness */
    uint64_t *use;        /* bitset, one bit per vreg */
    uint64_t *def;
    uint64_t *live_in;
    uint64_t *live_out;
} IRBlock;

typedef struct {
    IRInst   *insts;
    int       n_insts, cap_insts;
    IRBlock  *blocks;
    int       n_blocks, cap_blocks;
    int       n_vregs;
    int       n_locals;
    int       n_consts;        /* mirror of proto->chunk.nconsts */
    XSProto  *proto;           /* original proto we're compiling */
    /* Each local slot gets a long-lived vreg (`local_vregs[slot]`)
     * that the allocator keeps live across the whole function, so
     * LOAD_LOCAL becomes a reg copy rather than a memory chase. See
     * ra_codegen.c for the shadow-model refcount discipline. NULL if
     * the proto has 0 locals. */
    IRVReg   *local_vregs;
    /* Per-slot flag: 1 if the slot is ever STORE_LOCAL'd. Read-only
     * slots skip the incref/writeback/decref dance entirely and just
     * borrow frame->base[slot]'s reference for the function body;
     * the caller's +1 on that slot stays in place throughout, so
     * teardown cleans it up without our help. */
    int8_t   *local_written;
} IRFunc;

/* Linear-scan register allocation output. For each vreg either:
 *   - assigned a physical x86-64 register (index into phys_regs[])
 *   - spilled to a numbered slot on the native stack (frame-relative)
 */
typedef struct {
    int8_t *reg;      /* one entry per vreg: physical reg index, or -1 if spilled */
    int8_t *spill;    /* one entry per vreg: spill slot index, or -1 if in reg */
    int     n_spill_slots;
} IRAlloc;

/* Phase 1: lower bytecode into IR + basic blocks. Returns NULL if the
 * proto uses any opcode outside the supported set, signalling the
 * caller to fall back to the template JIT. */
IRFunc *ralow_lower(XSProto *proto);
void    irfunc_free(IRFunc *f);

/* Phase 2: compute liveness. Populates block use/def/live_in/live_out. */
void ralow_liveness(IRFunc *f);

/* Phase 3: linear-scan register allocation. */
IRAlloc *ralow_alloc(IRFunc *f);
void     iralloc_free(IRAlloc *a);

/* Phase 4: emit x86-64 machine code using the allocation. Caller
 * provides the JIT code buffer. Returns the entry address, or NULL on
 * emission failure (buffer overflow, unsupported construct). */
typedef struct XSJIT XSJIT;
void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a);

#endif /* XS_JIT_RA_IR_H */
