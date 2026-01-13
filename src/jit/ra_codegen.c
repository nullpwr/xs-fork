/* Phase 4 of the register-allocating JIT pipeline: walk the IR one
 * block at a time and emit x86-64, honouring the vreg assignments in
 * IRAlloc.
 *
 * Calling convention matches the tier-1 JIT: int entry(VM *vm).
 *   r12 = VM*                         (kept across the whole body)
 *   r13 = current CallFrame*          (refreshed after calls)
 *   rbx, r14, r15 = allocatable for vregs
 *   rax/rcx/rdx/rsi/rdi/r8-r11 = scratch / arg passing
 *
 * Vreg locations: each vreg either lives permanently in one of the
 * three callee-saved regs, or in a numbered spill slot on the native
 * stack at [rbp - 48 - 8*slot]. Reads/writes go through tiny load/
 * store helpers that pick the right form.
 *
 * Slow paths (non-SMI arithmetic, truthy tests, etc.) delegate to
 * vm_step_jit after flushing the IR-level operand vregs to the VM
 * stack, so behaviour stays identical to --vm. Calls go through the
 * existing vm_call_closure_fast / vm_return_fast helpers plus a mini
 * dispatcher that runs the pushed inner frame to completion. */

#include "jit/ra_ir.h"
#include "jit/jit.h"
#include "vm/vm.h"
#include "vm/bytecode.h"
#include "core/xs.h"
#include "core/value.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)

#ifndef JIT_HAS_MMAP
#  if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#    define JIT_HAS_MMAP 1
#  else
#    define JIT_HAS_MMAP 0
#  endif
#endif

#if !JIT_HAS_MMAP
void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a) {
    (void)j; (void)f; (void)a;
    return NULL;
}
#else

/* --- VM layout offsets (must stay in sync with vm.h / bytecode.h).
 * The tier-1 JIT asserts these via a regression test; we reuse the
 * same numbers rather than duplicating the probe. */
#define VM_OFF_SP            16
#define VM_OFF_FRAMES        24
#define VM_OFF_FRAME_COUNT   36
#define FRAME_OFF_IP          8
#define FRAME_OFF_BASE       16
#define FRAME_SIZE         1608
#define VAL_OFF_REFCOUNT      4

/* --- tiny emitter --- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    int      overflow;
} Emitter;

static void emit_init(Emitter *e, uint8_t *buf, size_t cap, size_t start) {
    e->buf = buf; e->cap = cap; e->pos = start; e->overflow = 0;
}

static inline void emit_byte(Emitter *e, uint8_t b) {
    if (e->pos < e->cap) e->buf[e->pos++] = b;
    else e->overflow = 1;
}
static inline void emit_u32(Emitter *e, uint32_t v) {
    emit_byte(e, (uint8_t)v);
    emit_byte(e, (uint8_t)(v >> 8));
    emit_byte(e, (uint8_t)(v >> 16));
    emit_byte(e, (uint8_t)(v >> 24));
}
static inline void emit_u64(Emitter *e, uint64_t v) {
    emit_u32(e, (uint32_t)v);
    emit_u32(e, (uint32_t)(v >> 32));
}
static inline void emit_i32(Emitter *e, int32_t v) { emit_u32(e, (uint32_t)v); }
static inline void emit_modrm(Emitter *e, uint8_t mod, uint8_t reg, uint8_t rm) {
    emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}
static inline uint8_t rex(int w, int r_ext, int x_ext, int b_ext) {
    return (uint8_t)(0x40 | (w ? 8 : 0) | (r_ext ? 4 : 0) | (x_ext ? 2 : 0) | (b_ext ? 1 : 0));
}

/* Register encoding: low 3 bits; high bit goes in REX.R/B. */
#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
#define R8  8
#define R9  9
#define R12 12
#define R13 13
#define R14 14
#define R15 15

static void emit_mov_reg_imm64(Emitter *e, int reg, uint64_t imm) {
    emit_byte(e, rex(1, 0, 0, reg >= 8));
    emit_byte(e, (uint8_t)(0xB8 + (reg & 7)));
    emit_u64(e, imm);
}

/* mov dst, src (64-bit reg-reg). */
static void emit_mov_rr(Emitter *e, int dst, int src) {
    emit_byte(e, rex(1, src >= 8, 0, dst >= 8));
    emit_byte(e, 0x89);
    emit_modrm(e, 3, (uint8_t)(src & 7), (uint8_t)(dst & 7));
}

/* push reg64. */
static void emit_push_reg(Emitter *e, int reg) {
    if (reg >= 8) emit_byte(e, 0x41);
    emit_byte(e, (uint8_t)(0x50 + (reg & 7)));
}
static void emit_pop_reg(Emitter *e, int reg) {
    if (reg >= 8) emit_byte(e, 0x41);
    emit_byte(e, (uint8_t)(0x58 + (reg & 7)));
}

/* add reg64, imm8 / imm32. */
static void emit_add_reg_imm32(Emitter *e, int reg, int32_t imm) {
    emit_byte(e, rex(1, 0, 0, reg >= 8));
    if (imm >= -128 && imm <= 127) {
        emit_byte(e, 0x83);
        emit_modrm(e, 3, 0, (uint8_t)(reg & 7));
        emit_byte(e, (uint8_t)(int8_t)imm);
    } else {
        emit_byte(e, 0x81);
        emit_modrm(e, 3, 0, (uint8_t)(reg & 7));
        emit_i32(e, imm);
    }
}
static void emit_sub_reg_imm32(Emitter *e, int reg, int32_t imm) {
    emit_byte(e, rex(1, 0, 0, reg >= 8));
    if (imm >= -128 && imm <= 127) {
        emit_byte(e, 0x83);
        emit_modrm(e, 3, 5, (uint8_t)(reg & 7));
        emit_byte(e, (uint8_t)(int8_t)imm);
    } else {
        emit_byte(e, 0x81);
        emit_modrm(e, 3, 5, (uint8_t)(reg & 7));
        emit_i32(e, imm);
    }
}

static void emit_ret(Emitter *e) { emit_byte(e, 0xC3); }

/* mov reg_dst, [base + disp]   (64-bit load). base must be RBP/RSP/any
 * low reg we use here; displacements use disp8 or disp32 as needed. */
static void emit_mov_reg_mem_rbp(Emitter *e, int dst, int32_t disp) {
    int ext = (dst >= 8);
    emit_byte(e, rex(1, ext, 0, 0));
    emit_byte(e, 0x8B);
    if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, (uint8_t)(dst & 7), RBP);
        emit_byte(e, (uint8_t)(int8_t)disp);
    } else {
        emit_modrm(e, 2, (uint8_t)(dst & 7), RBP);
        emit_i32(e, disp);
    }
}
static void emit_mov_mem_rbp_reg(Emitter *e, int32_t disp, int src) {
    int ext = (src >= 8);
    emit_byte(e, rex(1, ext, 0, 0));
    emit_byte(e, 0x89);
    if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, (uint8_t)(src & 7), RBP);
        emit_byte(e, (uint8_t)(int8_t)disp);
    } else {
        emit_modrm(e, 2, (uint8_t)(src & 7), RBP);
        emit_i32(e, disp);
    }
}

/* mov reg_dst, [r12 + disp]  (r12-relative load). disp8 path; disps
 * here are the small fixed VM_OFF_* offsets. */
static void emit_mov_reg_mem_r12(Emitter *e, int dst, uint8_t disp) {
    emit_byte(e, rex(1, dst >= 8, 0, 1));
    emit_byte(e, 0x8B);
    emit_modrm(e, 1, (uint8_t)(dst & 7), RSP);  /* SIB form for r12 */
    emit_byte(e, 0x24);
    emit_byte(e, disp);
}
static void emit_mov_mem_r12_reg(Emitter *e, uint8_t disp, int src) {
    emit_byte(e, rex(1, src >= 8, 0, 1));
    emit_byte(e, 0x89);
    emit_modrm(e, 1, (uint8_t)(src & 7), RSP);
    emit_byte(e, 0x24);
    emit_byte(e, disp);
}

/* mov reg_dst, [r13 + disp8] */
static void emit_mov_reg_mem_r13(Emitter *e, int dst, uint8_t disp) {
    emit_byte(e, rex(1, dst >= 8, 0, 1));
    emit_byte(e, 0x8B);
    emit_modrm(e, 1, (uint8_t)(dst & 7), RBP);  /* r13 uses RBP rm */
    emit_byte(e, disp);
}
static void emit_mov_mem_r13_reg(Emitter *e, uint8_t disp, int src) {
    emit_byte(e, rex(1, src >= 8, 0, 1));
    emit_byte(e, 0x89);
    emit_modrm(e, 1, (uint8_t)(src & 7), RBP);
    emit_byte(e, disp);
}

/* mov reg, [reg + disp]  (general ptr deref). base must be low (0-7). */
static void emit_mov_reg_mem(Emitter *e, int dst, int base, int32_t disp) {
    emit_byte(e, rex(1, dst >= 8, 0, base >= 8));
    emit_byte(e, 0x8B);
    uint8_t b = (uint8_t)(base & 7);
    if (disp == 0 && b != RBP) {
        emit_modrm(e, 0, (uint8_t)(dst & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
    } else if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, (uint8_t)(dst & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
        emit_byte(e, (uint8_t)(int8_t)disp);
    } else {
        emit_modrm(e, 2, (uint8_t)(dst & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
        emit_i32(e, disp);
    }
}
static void emit_mov_mem_reg(Emitter *e, int base, int32_t disp, int src) {
    emit_byte(e, rex(1, src >= 8, 0, base >= 8));
    emit_byte(e, 0x89);
    uint8_t b = (uint8_t)(base & 7);
    if (disp == 0 && b != RBP) {
        emit_modrm(e, 0, (uint8_t)(src & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
    } else if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, (uint8_t)(src & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
        emit_byte(e, (uint8_t)(int8_t)disp);
    } else {
        emit_modrm(e, 2, (uint8_t)(src & 7), b);
        if (b == RSP) emit_byte(e, 0x24);
        emit_i32(e, disp);
    }
}

/* test reg, reg  (64-bit). */
static void emit_test_rr(Emitter *e, int a, int b) {
    emit_byte(e, rex(1, b >= 8, 0, a >= 8));
    emit_byte(e, 0x85);
    emit_modrm(e, 3, (uint8_t)(b & 7), (uint8_t)(a & 7));
}

static void emit_test_eax_eax(Emitter *e) {
    emit_byte(e, 0x85); emit_modrm(e, 3, RAX, RAX);
}

/* call abs: mov rax, imm64; call rax. */
static void emit_call_abs(Emitter *e, void *fn) {
    emit_mov_reg_imm64(e, RAX, (uint64_t)(uintptr_t)fn);
    emit_byte(e, 0xFF); emit_modrm(e, 3, 2, RAX);
}

/* jmp/jcc rel32: emit opcode + 4-byte placeholder, return offset of
 * displacement to patch later. */
static size_t emit_jmp_rel32_ph(Emitter *e) {
    emit_byte(e, 0xE9);
    size_t p = e->pos; emit_u32(e, 0); return p;
}
static size_t emit_jcc_rel32_ph(Emitter *e, uint8_t cc) {
    emit_byte(e, 0x0F); emit_byte(e, (uint8_t)(0x80 | cc));
    size_t p = e->pos; emit_u32(e, 0); return p;
}
static void patch_rel32(Emitter *e, size_t patch_pos, size_t target_pos) {
    int32_t rel = (int32_t)((long)target_pos - (long)(patch_pos + 4));
    if (patch_pos + 4 > e->cap) { e->overflow = 1; return; }
    e->buf[patch_pos + 0] = (uint8_t)rel;
    e->buf[patch_pos + 1] = (uint8_t)(rel >> 8);
    e->buf[patch_pos + 2] = (uint8_t)(rel >> 16);
    e->buf[patch_pos + 3] = (uint8_t)(rel >> 24);
}

/* cc codes used here */
#define CC_O  0x0  /* overflow */
#define CC_Z  0x4  /* equal */
#define CC_NZ 0x5  /* not equal */
#define CC_L  0xC  /* signed less */
#define CC_GE 0xD
#define CC_LE 0xE
#define CC_G  0xF

/* --- vreg access plumbing --- */

/* Map phys reg index (as recorded by the allocator) to the x86 reg
 * number we're using for that slot. */
static int phys_to_x86(int8_t phys_idx) {
    switch (phys_idx) {
        case 0: return RBX;  /* RA_PHYS_RBX */
        case 1: return R14;  /* RA_PHYS_R14 */
        case 2: return R15;  /* RA_PHYS_R15 */
        default: return -1;
    }
}

/* Stack slot i lives at rbp - (56 + 8*i). The slot at rbp-48 is
 * reserved by the prologue as a scratch word used by the error-exit
 * epilogue to stash the return code across the per-local decref
 * loop (without clobbering any spilled local). See the file header
 * for the full frame layout. */
#define RC_STASH_DISP (-48)
static int32_t spill_disp(int slot) { return -56 - 8 * slot; }

/* Load the current value of vreg v into x86 register `dst`. */
static void emit_load_vreg(Emitter *e, IRVReg v, IRAlloc *a, int dst) {
    if (v < 0) return;
    int8_t phys = a->reg[v];
    if (phys >= 0) {
        int src = phys_to_x86(phys);
        if (src != dst) emit_mov_rr(e, dst, src);
    } else {
        int slot = a->spill[v];
        emit_mov_reg_mem_rbp(e, dst, spill_disp(slot));
    }
}

/* Store x86 register `src` into vreg v's home. */
static void emit_store_vreg(Emitter *e, IRVReg v, IRAlloc *a, int src) {
    if (v < 0) return;
    int8_t phys = a->reg[v];
    if (phys >= 0) {
        int dst = phys_to_x86(phys);
        if (dst != src) emit_mov_rr(e, dst, src);
    } else {
        int slot = a->spill[v];
        emit_mov_mem_rbp_reg(e, spill_disp(slot), src);
    }
}

/* --- inline incref/decref --- */

/* Inline value_incref(rax). Skips SMIs (low bit = 1) and NULLs. */
static void emit_inline_incref_rax(Emitter *e) {
    /* test al, 1 */
    emit_byte(e, 0xA8); emit_byte(e, 0x01);
    /* jnz +9  (skip null-check + inc when SMI) */
    emit_byte(e, 0x75); emit_byte(e, 0x09);
    /* test rax, rax */
    emit_test_rr(e, RAX, RAX);
    /* jz +4 */
    emit_byte(e, 0x74); emit_byte(e, 0x04);
    /* add dword [rax + VAL_OFF_REFCOUNT], 1 */
    emit_byte(e, 0x83); emit_byte(e, 0x40);
    emit_byte(e, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(e, 0x01);
}

/* Call value_decref(reg). Clobbers rax/rdi/etc (standard C call). */
static void emit_decref_call(Emitter *e, int src) {
    if (src != RDI) emit_mov_rr(e, RDI, src);
    emit_call_abs(e, (void *)(uintptr_t)value_decref);
}

/* --- VM-sp push/pop helpers (work with vm->sp through r12). --- */

/* [vm->sp] = rax; vm->sp += 8. Uses rcx as scratch. */
static void emit_vmsp_push_rax(Emitter *e) {
    emit_mov_reg_mem_r12(e, RCX, VM_OFF_SP);
    /* mov [rcx], rax */
    emit_byte(e, 0x48); emit_byte(e, 0x89); emit_modrm(e, 0, RAX, RCX);
    /* add rcx, 8 */
    emit_add_reg_imm32(e, RCX, 8);
    emit_mov_mem_r12_reg(e, VM_OFF_SP, RCX);
}

/* rax = *(--vm->sp). Uses rcx. */
static void emit_vmsp_pop_rax(Emitter *e) {
    emit_mov_reg_mem_r12(e, RCX, VM_OFF_SP);
    emit_sub_reg_imm32(e, RCX, 8);
    emit_mov_mem_r12_reg(e, VM_OFF_SP, RCX);
    /* mov rax, [rcx] */
    emit_byte(e, 0x48); emit_byte(e, 0x8B); emit_modrm(e, 0, RAX, RCX);
}

/* Refresh r13 := &vm->frames[frame_count - 1]. Clobbers rax, rcx. */
static void emit_refresh_r13(Emitter *e) {
    /* movsxd rcx, dword [r12 + 36]  -- REX.W + REX.B (no REX.R) so
     * the dst register is RCX. The tier-1 JIT's emit_refresh_frame
     * has the same comment but emits the REX.R variant (loading R9)
     * and only happens to work because RCX is already the frame
     * count from a preceding inlined mov. We can't rely on that
     * here. */
    emit_byte(e, 0x49); emit_byte(e, 0x63); emit_byte(e, 0x4C);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_FRAME_COUNT);
    /* dec rcx */
    emit_byte(e, 0x48); emit_byte(e, 0xFF); emit_byte(e, 0xC9);
    /* imul rcx, rcx, FRAME_SIZE */
    emit_byte(e, 0x48); emit_byte(e, 0x69); emit_byte(e, 0xC9);
    emit_i32(e, FRAME_SIZE);
    /* mov rax, [r12 + VM_OFF_FRAMES] */
    emit_byte(e, 0x49); emit_byte(e, 0x8B); emit_byte(e, 0x44);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_FRAMES);
    /* lea r13, [rax + rcx] */
    emit_byte(e, 0x4C); emit_byte(e, 0x8D); emit_byte(e, 0x2C);
    emit_byte(e, 0x08);  /* SIB: base=rax, index=rcx, scale=1 */
}

/* --- tier-2 mini dispatcher for inner frames (see file comment) ---
 * After OP_CALL pushes a frame, the emitted code calls this to drive
 * the pushed frame to completion. If the callee proto has its own
 * tier-2 entry we invoke that directly so recursive calls stay in
 * native code; otherwise we fall back to stepping through the
 * interpreter. Returns 0 on normal completion, nonzero on error /
 * top-level finish. */
static int tier2_run_until(VM *vm, int target_fc) {
    int rc = 0;
    int saved_ss = vm->single_step;
    while (vm->frame_count > target_fc) {
        CallFrame *top = &vm->frames[vm->frame_count - 1];
        XSProto *proto = NULL;
        if (top->closure_val && VAL_TAG(top->closure_val) == XS_CLOSURE
            && top->closure_val->cl)
            proto = top->closure_val->cl->proto;
        if (proto && proto->jit_entry) {
            int (*fn)(VM *) = (int (*)(VM *))proto->jit_entry;
            rc = fn(vm);
        } else {
            vm->single_step = 1;
            rc = vm_step_jit(vm);
        }
        if (rc != 0) break;
    }
    vm->single_step = saved_ss;
    return rc;
}

/* ==============================================================
 *                       CODEGEN DRIVER
 * ============================================================== */

/* Forward-branch fixups. Each pending branch points to a rel32 slot
 * that needs to be patched once we know the target block's machine
 * offset. */
typedef struct { size_t patch; int target_block; } Fixup;

void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a) {
    if (!j || !j->available || !f) return NULL;

    /* A rough budget: per IR inst we never emit more than ~96 bytes.
     * A generous 128 B/inst ceiling keeps the math simple and bails
     * cleanly on oversized functions. */
    size_t est = (size_t)f->n_insts * 128 + 512;
    if (j->code_used + est > j->code_size) return NULL;

    Emitter em;
    emit_init(&em, j->code, j->code_size, j->code_used);
    void *entry = (void *)(j->code + em.pos);

    /* ----- prologue -----
     * See file header for the frame layout. x = 8 + 16 * (n_spill/2)
     * keeps rsp 16-aligned for every inner C call. */
    int n_spill = a->n_spill_slots;
    /* Layout: 8 B reserved scratch (RC_STASH_DISP) + 8*n_spill for
     * spill slots + optional 8 B alignment pad. Total must leave rsp
     * 16-aligned after the 5 earlier pushes (sp % 16 == 8 at that
     * point), so sub_amt % 16 must equal 8. */
    int32_t sub_amt = 8 + 8 * n_spill + ((n_spill & 1) ? 8 : 0);

    emit_push_reg(&em, RBP);
    emit_mov_rr(&em, RBP, RSP);
    emit_push_reg(&em, RBX);
    emit_push_reg(&em, R12);
    emit_push_reg(&em, R13);
    emit_push_reg(&em, R14);
    emit_push_reg(&em, R15);
    emit_sub_reg_imm32(&em, RSP, sub_amt);

    /* r12 = rdi (vm). */
    emit_mov_rr(&em, R12, RDI);
    /* r13 = current frame pointer. */
    emit_refresh_r13(&em);

    /* Materialise each local into its long-lived vreg. For slots
     * that the function writes, both the slot and our vreg own the
     * value (add a +1 via incref); IR_RETURN later transfers the
     * vreg back into the slot. For read-only slots we just borrow --
     * no incref, no writeback, no decref at exit -- since the slot's
     * original +1 is untouched and the frame teardown reclaims it. */
    for (int i = 0; i < f->n_locals; i++) {
        emit_mov_reg_mem_r13(&em, RAX, FRAME_OFF_BASE);
        emit_mov_reg_mem(&em, RAX, RAX, i * 8);
        if (f->local_written && f->local_written[i])
            emit_inline_incref_rax(&em);
        emit_store_vreg(&em, f->local_vregs[i], a, RAX);
    }

    /* Pin single_step=1 so vm_step_jit slow paths take the fast
     * per-opcode exit rather than the full dispatch loop. */
    /* mov dword [r12 + offsetof(VM, single_step)], 1 */
    {
        int32_t off = (int32_t)offsetof(VM, single_step);
        if (off >= -128 && off <= 127) {
            emit_byte(&em, 0x41); emit_byte(&em, 0xC7);
            emit_byte(&em, 0x44); emit_byte(&em, 0x24);
            emit_byte(&em, (uint8_t)off);
            emit_i32(&em, 1);
        } else {
            emit_byte(&em, 0x41); emit_byte(&em, 0xC7);
            emit_byte(&em, 0x84); emit_byte(&em, 0x24);
            emit_i32(&em, off);
            emit_i32(&em, 1);
        }
    }

    /* Per-block start offsets and pending branch fixups. */
    size_t *block_pos = xs_calloc((size_t)f->n_blocks, sizeof(size_t));
    for (int i = 0; i < f->n_blocks; i++) block_pos[i] = (size_t)-1;
    Fixup *fixups = xs_malloc(1024 * sizeof(Fixup));
    int n_fixups = 0, cap_fixups = 1024;

    /* Two shared exit epilogues:
     *   err_exit -- locals still hold our +1 reference; decref each
     *               before restoring callee-saved regs.
     *   ok_exit  -- IR_RETURN already wrote locals back into the
     *               frame and vm_return_fast disposed of them, so
     *               skip the decref loop. */
    size_t *err_exit_patches = xs_malloc(256 * sizeof(size_t));
    int n_err = 0;
    size_t *ok_exit_patches = xs_malloc(256 * sizeof(size_t));
    int n_ok = 0;

    #define ADD_FIXUP(patch_off, tgt_block) do { \
        if (n_fixups == cap_fixups) { \
            cap_fixups *= 2; \
            fixups = xs_realloc(fixups, (size_t)cap_fixups * sizeof(Fixup)); \
        } \
        fixups[n_fixups].patch = (patch_off); \
        fixups[n_fixups].target_block = (tgt_block); \
        n_fixups++; \
    } while (0)

    #define ADD_ERR_EXIT(patch_off) do { \
        err_exit_patches[n_err++] = (patch_off); \
    } while (0)

    #define ADD_OK_EXIT(patch_off) do { \
        ok_exit_patches[n_ok++] = (patch_off); \
    } while (0)

    /* ---- per-op emitters ---- */

    /* Slow-path dispatch: flush operand vregs to vm->sp, set
     * frame->ip to the bytecode instruction at bc_off, call
     * vm_step_jit, then pop the produced result back into the dst
     * vreg. Used as the fallback for non-SMI arithmetic and truthy
     * tests. */
    XSProto *proto = f->proto;
    Instruction *code_base = proto->chunk.code;

    /* --- driver: one block at a time --- */
    for (int bi = 0; bi < f->n_blocks; bi++) {
        IRBlock *blk = &f->blocks[bi];
        block_pos[bi] = em.pos;

        for (int ii = blk->start; ii < blk->end; ii++) {
            IRInst *in = &f->insts[ii];
            int bc_off = in->bc_offset;

            switch (in->op) {

            /* -------- PURE PUSHES -------- */
            case IR_CONST: {
                /* Embed the Value* directly. Consts stay alive as long
                 * as the proto, which outlives any JITed code here. */
                Value *cv = proto->chunk.consts[in->imm];
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)cv);
                emit_inline_incref_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }
            case IR_PUSH_NULL:
                emit_call_abs(&em, (void *)(uintptr_t)xs_null);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            case IR_PUSH_TRUE:
                /* mov edi, 1; call xs_bool */
                emit_byte(&em, 0xBF); emit_u32(&em, 1);
                emit_call_abs(&em, (void *)(uintptr_t)xs_bool);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            case IR_PUSH_FALSE:
                emit_byte(&em, 0xBF); emit_u32(&em, 0);
                emit_call_abs(&em, (void *)(uintptr_t)xs_bool);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;

            case IR_LOAD_LOCAL: {
                /* rax = local_vreg[imm]; incref(rax); dst = rax. No
                 * memory access if both local_vreg and dst are in
                 * phys regs (one reg-reg move + the SMI-skip incref). */
                emit_load_vreg(&em, f->local_vregs[in->imm], a, RAX);
                emit_inline_incref_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            case IR_LOAD_GLOBAL: {
                /* Inlined monomorphic IC check. The slow path
                 *   vm_load_global_ic(vm, bc_off, const_idx)
                 * is only called when chk->ic isn't allocated yet, the
                 * cache entry is empty, or the stored version is stale.
                 * On the hot path (repeated loads of an unchanged global)
                 * the emitted sequence is just a handful of loads + an
                 * inline incref with no C calls.
                 *
                 * &chk->ic and &chk->ic_version are stable addresses for
                 * the proto's lifetime and live at least as long as the
                 * JITed code, so we embed them as immediates. */
                XSChunk *chk = &proto->chunk;
                uint64_t ic_slot_addr  = (uint64_t)(uintptr_t)&chk->ic;
                uint64_t icv_slot_addr = (uint64_t)(uintptr_t)&chk->ic_version;
                int32_t  gv_off = (int32_t)offsetof(VM, global_version);

                /* RAX = chk->ic  (load pointer through the stable slot) */
                emit_mov_reg_imm64(&em, RAX, ic_slot_addr);
                /* mov rax, [rax] */
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                emit_test_rr(&em, RAX, RAX);
                size_t j_miss1 = emit_jcc_rel32_ph(&em, CC_Z);

                /* RCX = chk->ic[bc_off] */
                {
                    int32_t off = (int32_t)bc_off * 8;
                    if (off >= -128 && off <= 127) {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x48);
                        emit_byte(&em, (uint8_t)(int8_t)off);
                    } else {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x88);
                        emit_i32(&em, off);
                    }
                }
                /* test rcx, rcx */
                emit_byte(&em, 0x48); emit_byte(&em, 0x85); emit_byte(&em, 0xC9);
                size_t j_miss2 = emit_jcc_rel32_ph(&em, CC_Z);

                /* RDX = chk->ic_version (array base), then chk->ic_version[bc_off]. */
                emit_mov_reg_imm64(&em, RDX, icv_slot_addr);
                /* mov rdx, [rdx] */
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x12);
                {
                    int32_t off = (int32_t)bc_off * 8;
                    if (off >= -128 && off <= 127) {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x52);
                        emit_byte(&em, (uint8_t)(int8_t)off);
                    } else {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x92);
                        emit_i32(&em, off);
                    }
                }

                /* RSI = vm->global_version */
                if (gv_off >= -128 && gv_off <= 127) {
                    emit_byte(&em, 0x49); emit_byte(&em, 0x8B); emit_byte(&em, 0x74);
                    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)(int8_t)gv_off);
                } else {
                    emit_byte(&em, 0x49); emit_byte(&em, 0x8B); emit_byte(&em, 0xB4);
                    emit_byte(&em, 0x24); emit_i32(&em, gv_off);
                }
                /* cmp rdx, rsi */
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xF2);
                size_t j_miss3 = emit_jcc_rel32_ph(&em, CC_NZ);

                /* Cache hit: the IC owns a +1 on rcx; borrow it and
                 * add our own +1 so the caller gets a proper owned
                 * value to store into the dst vreg. */
                emit_mov_rr(&em, RAX, RCX);
                emit_inline_incref_rax(&em);
                size_t j_done_lg = emit_jmp_rel32_ph(&em);

                /* Miss: vm_load_global_ic returns an incref'd value and
                 * repopulates the cache with a fresh version tag. */
                patch_rel32(&em, j_miss1, em.pos);
                patch_rel32(&em, j_miss2, em.pos);
                patch_rel32(&em, j_miss3, em.pos);
                emit_mov_rr(&em, RDI, R12);
                emit_byte(&em, 0xBE); emit_u32(&em, (uint32_t)bc_off);
                emit_byte(&em, 0xBA); emit_u32(&em, (uint32_t)in->imm);
                emit_call_abs(&em, (void *)(uintptr_t)vm_load_global_ic);

                patch_rel32(&em, j_done_lg, em.pos);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            case IR_DUP: {
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_inline_incref_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            case IR_MOVE: {
                /* Ownership transfer -- no refcount bump. Emitted only
                 * by the inliner to wire caller args into inlined
                 * callee locals and to route an inlined RETURN value
                 * into the outer CALL's destination. */
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            /* -------- STACK CONSUMERS -------- */
            case IR_POP: {
                emit_load_vreg(&em, in->src1, a, RDI);
                emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                break;
            }

            case IR_STORE_LOCAL: {
                /* Shadow model: local_vreg holds the authoritative
                 * current value (+1), while frame->base[slot] still
                 * owns its original value until IR_RETURN writes back.
                 * Sequence: decref the value currently in local_vreg
                 * (releases vreg's +1), then transfer src1's +1 into
                 * local_vreg. frame->base[slot] is intentionally left
                 * stale; RETURN and error exits resolve it later. */
                IRVReg lv = f->local_vregs[in->imm];
                /* rax = local_vreg */
                emit_load_vreg(&em, lv, a, RAX);
                /* test rax, rax ; jz skip_decref */
                emit_test_rr(&em, RAX, RAX);
                size_t jz = emit_jcc_rel32_ph(&em, CC_Z);
                emit_mov_rr(&em, RDI, RAX);
                emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                patch_rel32(&em, jz, em.pos);
                /* local_vreg = src1 */
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_store_vreg(&em, lv, a, RAX);
                break;
            }

            /* -------- BINOPS (SMI fast + vm_step slow) -------- */
            /* Strategy:
             *   load a -> rcx, b -> rax
             *   check both SMI; if not, go to slow path
             *   inline op using SMI identities
             *   if overflow (where applicable), fall to slow
             *   store result to dst
             * Slow path: push a, b to vm->sp, set frame->ip, call
             * vm_step_jit, pop result. Slow path emits dst store
             * then joins the done label. */

            case IR_ADD: case IR_SUB: case IR_MUL:
            case IR_DIV: case IR_MOD:
            case IR_BAND: case IR_BOR: case IR_BXOR:
            case IR_SHL:  case IR_SHR:
            case IR_LT:  case IR_GT:  case IR_LE:
            case IR_GE:  case IR_EQ:  case IR_NE: {
                /* Load both operands. b -> rax, a -> rcx (matches tier-1 convention). */
                emit_load_vreg(&em, in->src1, a, RCX);
                emit_load_vreg(&em, in->src2, a, RAX);

                /* both-SMI check: (a & b) & 1 */
                /* mov rdx, rcx; and rdx, rax; test dl, 1 */
                emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xCA);
                emit_byte(&em, 0x48); emit_byte(&em, 0x21); emit_byte(&em, 0xC2);
                emit_byte(&em, 0xF6); emit_byte(&em, 0xC2); emit_byte(&em, 0x01);
                /* jz slow */
                size_t jz_slow = emit_jcc_rel32_ph(&em, CC_Z);

                /* --- SMI fast body. Result lands in rcx. --- */
                int jo_slow_list[8]; int n_jo = 0;
                switch (in->op) {
                case IR_ADD: {
                    /* sub rcx, 1; add rcx, rax; jo slow */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xE9); emit_byte(&em, 0x01);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x01); emit_byte(&em, 0xC1);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, CC_O);
                    break;
                }
                case IR_SUB: {
                    /* sub rcx, rax; jo slow; add rcx, 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x29); emit_byte(&em, 0xC1);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, CC_O);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xC1); emit_byte(&em, 0x01);
                    break;
                }
                case IR_MUL: {
                    /* sar rcx, 1; sar rax, 1; imul rcx, rax; jo; add rcx, rcx; jo; or rcx, 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF9);
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF8);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x0F); emit_byte(&em, 0xAF); emit_byte(&em, 0xC8);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, CC_O);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x01); emit_byte(&em, 0xC9);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, CC_O);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xC9); emit_byte(&em, 0x01);
                    break;
                }
                case IR_DIV: case IR_MOD: {
                    /* SMI 0 is encoded as 0x1, so "divisor == 0" is a plain
                     * cmp rax, 1. We keep the div-by-zero path in the
                     * interpreter to reuse its runtime_error reporting. */
                    /* cmp rax, 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xF8); emit_byte(&em, 0x01);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, CC_Z);
                    /* sar rcx, 1 ; sar rax, 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF9);
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF8);
                    /* Save divisor in r8 before clobbering rax/rdx with idiv. */
                    /* mov r8, rax */
                    emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0xC0);
                    /* mov rax, rcx */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xC8);
                    /* cqo  (sign-extend rax into rdx:rax) */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x99);
                    /* idiv r8  (quotient in rax, remainder in rdx) */
                    emit_byte(&em, 0x49); emit_byte(&em, 0xF7); emit_byte(&em, 0xF8);
                    if (in->op == IR_DIV) {
                        /* SMI range covers [-2^62, 2^62-1], so |q| <= 2^62
                         * and retag via lea cannot overflow on quotients
                         * of SMI-range operands. */
                        /* lea rcx, [rax + rax + 1] */
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8D); emit_byte(&em, 0x4C);
                        emit_byte(&em, 0x00); emit_byte(&em, 0x01);
                    } else {
                        /* Math modulo: if (rem != 0 && signs(rem, b) differ) rem += b. */
                        /* mov rcx, rdx */
                        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xD1);
                        /* test rcx, rcx ; jz .retag */
                        emit_byte(&em, 0x48); emit_byte(&em, 0x85); emit_byte(&em, 0xC9);
                        /* jz +imm8 -- short jump over the adjustment block */
                        emit_byte(&em, 0x74); size_t jz_retag = em.pos; emit_byte(&em, 0x00);
                        /* mov r9, rcx ; xor r9, r8 ; test r9, r9 ; jns .retag */
                        emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0xC9);
                        emit_byte(&em, 0x4D); emit_byte(&em, 0x31); emit_byte(&em, 0xC1);
                        emit_byte(&em, 0x4D); emit_byte(&em, 0x85); emit_byte(&em, 0xC9);
                        emit_byte(&em, 0x79); size_t jns_retag = em.pos; emit_byte(&em, 0x00);
                        /* add rcx, r8 */
                        emit_byte(&em, 0x4C); emit_byte(&em, 0x01); emit_byte(&em, 0xC1);
                        /* retag label: patch both forward-jumps to here */
                        size_t retag_pos = em.pos;
                        em.buf[jz_retag] = (uint8_t)(retag_pos - jz_retag - 1);
                        em.buf[jns_retag] = (uint8_t)(retag_pos - jns_retag - 1);
                        /* lea rcx, [rcx + rcx + 1] */
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8D); emit_byte(&em, 0x4C);
                        emit_byte(&em, 0x09); emit_byte(&em, 0x01);
                    }
                    break;
                }
                case IR_BAND: {
                    /* and rcx, rax -- both SMI-tagged so low bit stays 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x21); emit_byte(&em, 0xC1);
                    break;
                }
                case IR_BOR: {
                    /* or rcx, rax */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x09); emit_byte(&em, 0xC1);
                    break;
                }
                case IR_BXOR: {
                    /* xor rcx, rax -- low bit goes to 0, retag with or rcx, 1 */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x31); emit_byte(&em, 0xC1);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xC9); emit_byte(&em, 0x01);
                    break;
                }
                case IR_SHL: case IR_SHR: {
                    /* Match interp: shift count is masked to low 6 bits
                     * anyway via VAL_INT(b) & 63, so any b is "valid" -- but
                     * we still bail if it's outside [0,62] to keep the
                     * fast path simple (avoids retagging negative counts). */
                    /* sar rax, 1 (untag b); cmp rax, 62; ja slow */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF8);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xF8); emit_byte(&em, 62);
                    jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, 0x7);  /* JA */
                    /* sar rcx, 1 (untag a) */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xD1); emit_byte(&em, 0xF9);
                    /* mov r8, rcx  (save a) ; mov rcx, rax  (cl = count) */
                    emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0xC8);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xC1);
                    if (in->op == IR_SHL) {
                        /* shl r8, cl */
                        emit_byte(&em, 0x49); emit_byte(&em, 0xD3); emit_byte(&em, 0xE0);
                        /* Overflow if the shifted value's top two bits differ from its sign:
                         *   mov rax, r8 ; sar rax, 62 ; inc rax ; cmp rax, 1 ; ja slow.
                         * After inc, valid values (rax in {-1,0}) become {0,1} so
                         * unsigned >1 catches both overflow directions. */
                        emit_byte(&em, 0x4C); emit_byte(&em, 0x89); emit_byte(&em, 0xC0);
                        emit_byte(&em, 0x48); emit_byte(&em, 0xC1); emit_byte(&em, 0xF8); emit_byte(&em, 62);
                        emit_byte(&em, 0x48); emit_byte(&em, 0xFF); emit_byte(&em, 0xC0);
                        emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xF8); emit_byte(&em, 0x01);
                        jo_slow_list[n_jo++] = (int)emit_jcc_rel32_ph(&em, 0x7); /* JA */
                    } else {
                        /* sar r8, cl */
                        emit_byte(&em, 0x49); emit_byte(&em, 0xD3); emit_byte(&em, 0xF8);
                    }
                    /* lea rcx, [r8 + r8 + 1]  (retag). REX.W|X|B = 0x4B
                     * because both the index and base registers extend
                     * into R8. */
                    emit_byte(&em, 0x4B); emit_byte(&em, 0x8D); emit_byte(&em, 0x4C);
                    emit_byte(&em, 0x00); emit_byte(&em, 0x01);
                    break;
                }
                case IR_LT: case IR_GT: case IR_LE: case IR_GE:
                case IR_EQ: case IR_NE: {
                    /* cmp rcx, rax ; emit jcc per op, set rcx to TRUE/FALSE singleton */
                    uint8_t cc_take;
                    switch (in->op) {
                    case IR_LT: cc_take = CC_L; break;
                    case IR_GT: cc_take = CC_G; break;
                    case IR_LE: cc_take = CC_LE; break;
                    case IR_GE: cc_take = CC_GE; break;
                    case IR_EQ: cc_take = CC_Z; break;
                    case IR_NE: cc_take = CC_NZ; break;
                    default:    cc_take = CC_Z;
                    }
                    /* cmp rcx, rax */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC1);
                    /* jcc +12 -> true branch */
                    emit_byte(&em, (uint8_t)(0x70 | cc_take));
                    emit_byte(&em, 12);
                    /* mov rax, &XS_FALSE_VAL */
                    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
                    /* jmp +10 */
                    emit_byte(&em, 0xEB); emit_byte(&em, 10);
                    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
                    /* mov rax, [rax] */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                    /* incref */
                    emit_inline_incref_rax(&em);
                    /* mov rcx, rax */
                    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xC1);
                    break;
                }
                default: break;
                }
                /* Fast path done: store result in rcx to dst. */
                emit_store_vreg(&em, in->dst, a, RCX);
                size_t jmp_done = emit_jmp_rel32_ph(&em);

                /* --- float fast path (ADD/SUB/MUL/DIV and compares) ---
                 * Routes the "both-SMI check failed" jump to here first
                 * so numeric code over boxed XS_FLOAT pairs stays out of
                 * the interpreter. Bitwise ops and MOD have no float
                 * analogue we want to speculate on, and on those ops
                 * `jz_slow` still goes straight to the interpreter. */
                int is_float_arith = (in->op == IR_ADD || in->op == IR_SUB ||
                                      in->op == IR_MUL || in->op == IR_DIV);
                int is_float_cmp = (in->op == IR_LT || in->op == IR_GT ||
                                    in->op == IR_LE || in->op == IR_GE ||
                                    in->op == IR_EQ || in->op == IR_NE);
                int has_float_path = is_float_arith || is_float_cmp;

                size_t float_slow_jumps[8]; int n_fslow = 0;
                int has_float_done_jump = 0;
                size_t float_done_jump = 0;
                if (has_float_path) {
                    size_t float_entry = em.pos;
                    patch_rel32(&em, jz_slow, float_entry);

                    /* Reload operands so the float path is independent
                     * of whatever the SMI fast body did to rcx/rax. */
                    emit_load_vreg(&em, in->src1, a, RCX);
                    emit_load_vreg(&em, in->src2, a, RAX);

                    /* Reject NULL and non-FLOAT heap values. The tag
                     * lives at offset 0 of the Value struct; XS_FLOAT=4. */
                    emit_test_rr(&em, RCX, RCX);
                    float_slow_jumps[n_fslow++] = emit_jcc_rel32_ph(&em, CC_Z);
                    emit_test_rr(&em, RAX, RAX);
                    float_slow_jumps[n_fslow++] = emit_jcc_rel32_ph(&em, CC_Z);
                    /* cmp dword [rcx], 4 */
                    emit_byte(&em, 0x83); emit_byte(&em, 0x39); emit_byte(&em, 0x04);
                    float_slow_jumps[n_fslow++] = emit_jcc_rel32_ph(&em, CC_NZ);
                    /* cmp dword [rax], 4 */
                    emit_byte(&em, 0x83); emit_byte(&em, 0x38); emit_byte(&em, 0x04);
                    float_slow_jumps[n_fslow++] = emit_jcc_rel32_ph(&em, CC_NZ);

                    /* movsd xmm0, [rcx + 8]; movsd xmm1, [rax + 8] */
                    emit_byte(&em, 0xF2); emit_byte(&em, 0x0F); emit_byte(&em, 0x10);
                    emit_byte(&em, 0x41); emit_byte(&em, 0x08);
                    emit_byte(&em, 0xF2); emit_byte(&em, 0x0F); emit_byte(&em, 0x10);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x08);

                    if (is_float_arith) {
                        /* <op>sd xmm0, xmm1: opcode depends on the IR op. */
                        uint8_t fop;
                        switch (in->op) {
                        case IR_ADD: fop = 0x58; break;
                        case IR_SUB: fop = 0x5C; break;
                        case IR_MUL: fop = 0x59; break;
                        case IR_DIV: fop = 0x5E; break;
                        default:     fop = 0x58; break;
                        }
                        emit_byte(&em, 0xF2); emit_byte(&em, 0x0F);
                        emit_byte(&em, fop); emit_byte(&em, 0xC1);

                        /* Box the xmm0 result: xs_float(double) takes
                         * its arg in xmm0 and returns a fresh +1 Value*
                         * in rax. */
                        emit_call_abs(&em, (void *)(uintptr_t)xs_float);

                        /* Preserve the boxed result across the two
                         * operand decrefs. Pushes unalign rsp by 8, so
                         * pair them with a sub 8 to stay 16-aligned at
                         * each call. */
                        emit_push_reg(&em, RAX);
                        emit_sub_reg_imm32(&em, RSP, 8);
                        emit_load_vreg(&em, in->src1, a, RDI);
                        emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                        emit_load_vreg(&em, in->src2, a, RDI);
                        emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                        emit_add_reg_imm32(&em, RSP, 8);
                        emit_pop_reg(&em, RCX);
                    } else {
                        /* ucomisd xmm0, xmm1: sets ZF/PF/CF. PF=1 means
                         * unordered (NaN). Bail to slow path on NaN so
                         * we match the interpreter's NaN handling. */
                        emit_byte(&em, 0x66); emit_byte(&em, 0x0F);
                        emit_byte(&em, 0x2E); emit_byte(&em, 0xC1);
                        /* jp slow  (parity = unordered) */
                        float_slow_jumps[n_fslow++] =
                            emit_jcc_rel32_ph(&em, 0xA); /* JP */
                        /* Map compare → cc. ucomisd uses unsigned
                         * semantics (CF/ZF): below=CF, equal=ZF,
                         * above=neither. */
                        uint8_t cc_f;
                        switch (in->op) {
                        case IR_LT: cc_f = 0x2; break; /* JB  */
                        case IR_GT: cc_f = 0x7; break; /* JA  */
                        case IR_LE: cc_f = 0x6; break; /* JBE */
                        case IR_GE: cc_f = 0x3; break; /* JAE */
                        case IR_EQ: cc_f = 0x4; break; /* JE  */
                        case IR_NE: cc_f = 0x5; break; /* JNE */
                        default:    cc_f = 0x4; break;
                        }
                        /* Materialise TRUE/FALSE singleton, matching
                         * the SMI compare path so downstream code that
                         * doesn't use CMP_BR still sees consistent
                         * Value* output. */
                        emit_byte(&em, (uint8_t)(0x70 | cc_f));
                        emit_byte(&em, 12);
                        emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
                        emit_byte(&em, 0xEB); emit_byte(&em, 10);
                        emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                        emit_inline_incref_rax(&em);

                        /* Save result across the operand decrefs. */
                        emit_push_reg(&em, RAX);
                        emit_sub_reg_imm32(&em, RSP, 8);
                        emit_load_vreg(&em, in->src1, a, RDI);
                        emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                        emit_load_vreg(&em, in->src2, a, RDI);
                        emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                        emit_add_reg_imm32(&em, RSP, 8);
                        emit_pop_reg(&em, RCX);
                    }

                    emit_store_vreg(&em, in->dst, a, RCX);
                    float_done_jump = emit_jmp_rel32_ph(&em);
                    has_float_done_jump = 1;
                }

                /* --- slow path --- */
                size_t slow_pos = em.pos;
                if (!has_float_path) patch_rel32(&em, jz_slow, slow_pos);
                for (int i = 0; i < n_jo; i++) patch_rel32(&em, (size_t)jo_slow_list[i], slow_pos);
                for (int i = 0; i < n_fslow; i++)
                    patch_rel32(&em, float_slow_jumps[i], slow_pos);
                /* Push a then b onto vm->sp (transferring ownership). */
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_load_vreg(&em, in->src2, a, RAX);
                emit_vmsp_push_rax(&em);
                /* Set frame->ip = code_base + bc_off. */
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                /* Call vm_step_jit(vm). */
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                /* On nonzero, exit JIT with whatever rc we got. */
                emit_test_eax_eax(&em);
                size_t j_err = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err);
                /* Pop result back into dst. */
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);

                /* done label */
                patch_rel32(&em, jmp_done, em.pos);
                if (has_float_done_jump) patch_rel32(&em, float_done_jump, em.pos);
                break;
            }

            case IR_NEG:
            case IR_BNOT: {
                /* Single-operand SMI fast path. rcx holds src1 on entry;
                 * if it's not tagged as SMI we go through vm_step_jit. */
                emit_load_vreg(&em, in->src1, a, RCX);
                /* test cl, 1 ; jz slow  (non-SMI) */
                emit_byte(&em, 0xF6); emit_byte(&em, 0xC1); emit_byte(&em, 0x01);
                size_t jz_unslow = emit_jcc_rel32_ph(&em, CC_Z);

                int jo_un = -1;
                if (in->op == IR_NEG) {
                    /* neg rcx ; add rcx, 2 ; jo slow.  SMI encoding
                     * stashes the tag in bit 0, so neg+add-2 is the
                     * algebraic identity; overflow occurs only when the
                     * input is SMI_MIN (-2^62). */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xF7); emit_byte(&em, 0xD9);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xC1); emit_byte(&em, 0x02);
                    jo_un = (int)emit_jcc_rel32_ph(&em, CC_O);
                } else {
                    /* not rcx ; or rcx, 1  -- bitwise complement of the
                     * SMI. Low bit flips during NOT; the OR puts the tag
                     * back so the result is a valid SMI. No overflow. */
                    emit_byte(&em, 0x48); emit_byte(&em, 0xF7); emit_byte(&em, 0xD1);
                    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xC9); emit_byte(&em, 0x01);
                }

                /* Fast path: store result, jump to done. */
                emit_store_vreg(&em, in->dst, a, RCX);
                size_t jmp_done_un = emit_jmp_rel32_ph(&em);

                /* Slow path. */
                size_t slow_pos = em.pos;
                patch_rel32(&em, jz_unslow, slow_pos);
                if (jo_un >= 0) patch_rel32(&em, (size_t)jo_un, slow_pos);
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_err_un = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err_un);
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);

                patch_rel32(&em, jmp_done_un, em.pos);
                break;
            }

            case IR_NOT: {
                /* Logical not. Inlining the SMI + singleton case is
                 * tempting, but the full semantics (truthy on arbitrary
                 * heap values, produce an incref'd TRUE/FALSE singleton)
                 * match what vm_step_jit already does, and NOT isn't
                 * typically hot. Flush + step + pop. */
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_err_not = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err_not);
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            /* -------- CONTROL FLOW -------- */
            case IR_JUMP: {
                size_t p = emit_jmp_rel32_ph(&em);
                ADD_FIXUP(p, in->imm);
                break;
            }
            case IR_JIF_FALSE:
            case IR_JIF_TRUE: {
                /* Inline the common tier-2 cond shapes -- SMI and bool
                 * singleton -- so the hot path is a handful of reg
                 * ops with no C calls. Tier-2's own IR_LT/IR_EQ/etc.
                 * produce exactly XS_TRUE_VAL / XS_FALSE_VAL, and
                 * numeric conditions are SMIs; anything else (strings,
                 * maps, bigints, ...) falls through to the full
                 * value_truthy + value_decref dance. */
                emit_load_vreg(&em, in->src1, a, RDI);

                /* SMI fast path: low bit of cond set means it's a
                 * tagged immediate int. Truthy iff pointer bits !=
                 * 0x1 (the encoded SMI 0). */
                /* test dil, 1 */
                emit_byte(&em, 0x40); emit_byte(&em, 0xF6);
                emit_byte(&em, 0xC7); emit_byte(&em, 0x01);
                size_t j_heap = emit_jcc_rel32_ph(&em, CC_Z);
                /* cmp rdi, 1 ; setnz cl ; movzx ecx, cl */
                emit_byte(&em, 0x48); emit_byte(&em, 0x83);
                emit_byte(&em, 0xFF); emit_byte(&em, 0x01);
                emit_byte(&em, 0x0F); emit_byte(&em, 0x95); emit_byte(&em, 0xC1);
                emit_byte(&em, 0x0F); emit_byte(&em, 0xB6); emit_byte(&em, 0xC9);
                size_t j_test_smi = emit_jmp_rel32_ph(&em);

                /* Heap path. First check XS_FALSE_VAL singleton. */
                patch_rel32(&em, j_heap, em.pos);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
                /* mov rax, [rax] */
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                /* cmp rdi, rax */
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC7);
                size_t j_not_false = emit_jcc_rel32_ph(&em, CC_NZ);
                /* Matched FALSE singleton: inline decref and set ecx = 0. */
                /* sub dword [rdi+4], 1  -- singletons never hit 0 so no free */
                emit_byte(&em, 0x83); emit_byte(&em, 0x6F);
                emit_byte(&em, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(&em, 0x01);
                /* xor ecx, ecx */
                emit_byte(&em, 0x31); emit_byte(&em, 0xC9);
                size_t j_test_false = emit_jmp_rel32_ph(&em);

                patch_rel32(&em, j_not_false, em.pos);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC7);
                size_t j_slow = emit_jcc_rel32_ph(&em, CC_NZ);
                /* Matched TRUE singleton. */
                emit_byte(&em, 0x83); emit_byte(&em, 0x6F);
                emit_byte(&em, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(&em, 0x01);
                /* mov ecx, 1 */
                emit_byte(&em, 0xB9); emit_u32(&em, 1);
                size_t j_test_true = emit_jmp_rel32_ph(&em);

                /* Slow path: full value_truthy + value_decref. */
                patch_rel32(&em, j_slow, em.pos);
                emit_push_reg(&em, RDI);
                emit_sub_reg_imm32(&em, RSP, 8);
                emit_call_abs(&em, (void *)(uintptr_t)value_truthy);
                emit_byte(&em, 0x89); emit_byte(&em, 0xC1);
                emit_add_reg_imm32(&em, RSP, 8);
                emit_pop_reg(&em, RDI);
                emit_call_abs(&em, (void *)(uintptr_t)value_decref);

                /* Converge: ecx is the truthy boolean. */
                size_t test_pos = em.pos;
                patch_rel32(&em, j_test_smi,   test_pos);
                patch_rel32(&em, j_test_false, test_pos);
                patch_rel32(&em, j_test_true,  test_pos);
                emit_byte(&em, 0x85); emit_byte(&em, 0xC9);  /* test ecx, ecx */
                uint8_t cc = (in->op == IR_JIF_FALSE) ? CC_Z : CC_NZ;
                size_t p = emit_jcc_rel32_ph(&em, cc);
                ADD_FIXUP(p, in->imm);
                break;
            }

            case IR_NOP:
                /* Peephole residue from IR_CMP_BR fusion. Emits nothing;
                 * keeps instruction indices stable for liveness / regalloc. */
                break;

            case IR_LOAD_UP: {
                /* Inline pointer chase:
                 *   frame->closure_val  -> [r13 + 0]
                 *     ->cl              -> [rax + 8]   (v->cl)
                 *     ->upvalues        -> [rax + 8]   (XSClosure::upvalues)
                 *     [imm]             -> [rax + imm*8]
                 *     ->ptr             -> [rax + 0]   (Upvalue::ptr)
                 *     *ptr              -> [rax]       (the live Value*)
                 * Then inline incref so SMIs skip the refcount write and
                 * store the owned result into dst. */
                emit_mov_reg_mem_r13(&em, RAX, 0);       /* rax = closure_val */
                emit_mov_reg_mem(&em, RAX, RAX, 8);      /* rax = v->cl */
                emit_mov_reg_mem(&em, RAX, RAX, 8);      /* rax = cl->upvalues */
                {
                    int32_t off = (int32_t)in->imm * 8;
                    if (off >= -128 && off <= 127) {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x40);
                        emit_byte(&em, (uint8_t)(int8_t)off);
                    } else {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x80);
                        emit_i32(&em, off);
                    }
                }
                emit_mov_reg_mem(&em, RAX, RAX, 0);      /* rax = uv->ptr */
                emit_mov_reg_mem(&em, RAX, RAX, 0);      /* rax = *uv->ptr */
                emit_inline_incref_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            case IR_STORE_UP: {
                /* Ownership transfer: src1 carries +1 (tier-2 vreg
                 * convention). After this op that +1 lives in *uv->ptr,
                 * and the Value previously stored there is decref'd.
                 *
                 *   new <- src1
                 *   uv_ptr_cell = &uv->ptr (rsi); live Value** is [rsi]
                 *   *uv_ptr_cell = new  (transferring src1's +1)
                 *   decref old
                 *
                 * We spill the incoming value on rcx because decref
                 * clobbers the scratch regs. */
                emit_load_vreg(&em, in->src1, a, RCX);   /* rcx = new_value */
                emit_mov_reg_mem_r13(&em, RAX, 0);
                emit_mov_reg_mem(&em, RAX, RAX, 8);
                emit_mov_reg_mem(&em, RAX, RAX, 8);
                {
                    int32_t off = (int32_t)in->imm * 8;
                    if (off >= -128 && off <= 127) {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x40);
                        emit_byte(&em, (uint8_t)(int8_t)off);
                    } else {
                        emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x80);
                        emit_i32(&em, off);
                    }
                }
                /* rax = Upvalue*.  rsi = uv->ptr (Value**). */
                emit_mov_reg_mem(&em, RSI, RAX, 0);
                /* rdi = *rsi = old Value* */
                emit_mov_reg_mem(&em, RDI, RSI, 0);
                /* *rsi = rcx  (store new value, ownership transfers) */
                /* mov [rsi], rcx */
                emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x0E);

                /* decref old if not NULL */
                emit_test_rr(&em, RDI, RDI);
                size_t j_skip_dec = emit_jcc_rel32_ph(&em, CC_Z);
                emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                patch_rel32(&em, j_skip_dec, em.pos);
                break;
            }

            case IR_MAKE_CLOSURE: {
                /* Rare op; dispatch through vm_step_jit so the full
                 * upvalue-capture dance lives in one place. The bytecode
                 * doesn't consume any VM-stack inputs (all captures come
                 * from frame->base and CL->upvalues) so we don't need to
                 * flush operand vregs to vm->sp beforehand -- we only
                 * pop the single new value off afterwards. */
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_err_mc = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err_mc);
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                break;
            }

            /* All the aggregate / container / field ops fall through a
             * single template: flush the input vregs onto vm->sp in the
             * order the interpreter expects, set frame->ip, invoke
             * vm_step_jit to run exactly one opcode, propagate errors,
             * then pop the produced result into dst (if any). The tier-2
             * JIT only wants these inside its subset so surrounding
             * arithmetic keeps the regalloc win -- the ops themselves
             * still go through the interpreter.
             *
             * src1 is always pushed first (when present). For
             * non-aggregate ops that's the only push. Aggregate ops
             * (ARRAY / TUPLE / MAP / METHOD_CALL / INDEX_SET) then push
             * each non-(-1) entry of call_args in order. src2 slots in
             * between when it's set (IR_INDEX_GET, IR_MAKE_RANGE,
             * IR_STORE_FIELD). IR_INDEX_SET pushes src1, src2, then
             * call_args[0]. */
            case IR_INDEX_GET:
            case IR_INDEX_SET:
            case IR_LOAD_FIELD:
            case IR_STORE_FIELD:
            case IR_MAKE_RANGE:
            case IR_MAKE_ARRAY:
            case IR_MAKE_TUPLE:
            case IR_MAKE_MAP:
            case IR_METHOD_CALL: {
                if (in->src1 >= 0) {
                    emit_load_vreg(&em, in->src1, a, RAX);
                    emit_vmsp_push_rax(&em);
                }
                if (in->src2 >= 0) {
                    emit_load_vreg(&em, in->src2, a, RAX);
                    emit_vmsp_push_rax(&em);
                }
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) {
                    if (in->call_args[k] < 0) break;
                    emit_load_vreg(&em, in->call_args[k], a, RAX);
                    emit_vmsp_push_rax(&em);
                }

                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_err_agg = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err_agg);
                if (in->dst >= 0) {
                    emit_vmsp_pop_rax(&em);
                    emit_store_vreg(&em, in->dst, a, RAX);
                }
                break;
            }

            case IR_CMP_BR: {
                /* Fused compare-and-branch. On the SMI hot path the
                 * existing CMP + JIF pair materialises a TRUE/FALSE heap
                 * singleton, stores it to a vreg, then loads/decrefs it
                 * in the branch -- wasted cycles. Here we skip the
                 * singleton entirely and go straight from `cmp` to `jcc`.
                 *
                 * Slow path replicates the CMP slow path (vm_step_jit on
                 * OP_LT etc.) followed by the inlined truthy + branch
                 * from IR_JIF, so behaviour is byte-identical to the
                 * unfused sequence. */
                int packed = (int)in->call_args[0];
                int kind = (packed >> 1) & 0x7;
                int branch_if_false = packed & 1;

                /* Load a -> rcx, b -> rax (same convention as IR_<CMP>). */
                emit_load_vreg(&em, in->src1, a, RCX);
                emit_load_vreg(&em, in->src2, a, RAX);

                /* Both-SMI check: mov rdx, rcx; and rdx, rax; test dl, 1 */
                emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xCA);
                emit_byte(&em, 0x48); emit_byte(&em, 0x21); emit_byte(&em, 0xC2);
                emit_byte(&em, 0xF6); emit_byte(&em, 0xC2); emit_byte(&em, 0x01);
                size_t jz_slow_cb = emit_jcc_rel32_ph(&em, CC_Z);

                /* SMI fast path: cmp rcx, rax; then jcc to branch target. */
                uint8_t cc_true;
                switch (kind) {
                case 0: cc_true = CC_L;  break; /* LT */
                case 1: cc_true = CC_G;  break; /* GT */
                case 2: cc_true = CC_LE; break; /* LE */
                case 3: cc_true = CC_GE; break; /* GE */
                case 4: cc_true = CC_Z;  break; /* EQ */
                case 5: cc_true = CC_NZ; break; /* NE */
                default: cc_true = CC_Z; break;
                }
                /* `branch_if_false` means "take the jump when the
                 * relation is FALSE", so invert the condition. The
                 * encodings are paired (cc ^ 1 flips sense for every
                 * relation we use here). */
                uint8_t cc_branch = branch_if_false ? (uint8_t)(cc_true ^ 1)
                                                    : cc_true;
                /* cmp rcx, rax */
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC1);
                size_t p_fast = emit_jcc_rel32_ph(&em, cc_branch);
                ADD_FIXUP(p_fast, in->imm);
                /* Fall-through (fast path, branch not taken) jumps over
                 * the slow path to the converged "done" point. */
                size_t j_done_fast = emit_jmp_rel32_ph(&em);

                /* --- slow path --- */
                size_t slow_pos = em.pos;
                patch_rel32(&em, jz_slow_cb, slow_pos);
                /* Push a then b onto vm->sp, set ip, call vm_step_jit. */
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_load_vreg(&em, in->src2, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_err_cb = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_err_cb);

                /* Pop the bool result into rdi for inlined truthy test. */
                emit_vmsp_pop_rax(&em);
                emit_mov_rr(&em, RDI, RAX);

                /* -- inlined truthy test, mirrors the IR_JIF_* body --
                 * Produces ecx = 0/1 covering SMI, TRUE/FALSE singleton,
                 * and the fully general value_truthy fallback. */

                /* test dil, 1  (SMI tag?) */
                emit_byte(&em, 0x40); emit_byte(&em, 0xF6);
                emit_byte(&em, 0xC7); emit_byte(&em, 0x01);
                size_t j_heap_cb = emit_jcc_rel32_ph(&em, CC_Z);
                /* cmp rdi, 1 ; setnz cl ; movzx ecx, cl -- SMI 0 is falsy */
                emit_byte(&em, 0x48); emit_byte(&em, 0x83);
                emit_byte(&em, 0xFF); emit_byte(&em, 0x01);
                emit_byte(&em, 0x0F); emit_byte(&em, 0x95); emit_byte(&em, 0xC1);
                emit_byte(&em, 0x0F); emit_byte(&em, 0xB6); emit_byte(&em, 0xC9);
                size_t j_test_smi_cb = emit_jmp_rel32_ph(&em);

                /* Heap path: check XS_FALSE_VAL then XS_TRUE_VAL singletons. */
                patch_rel32(&em, j_heap_cb, em.pos);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC7);
                size_t j_not_false_cb = emit_jcc_rel32_ph(&em, CC_NZ);
                /* Matched FALSE: refcount-- on singleton (never frees), ecx=0. */
                emit_byte(&em, 0x83); emit_byte(&em, 0x6F);
                emit_byte(&em, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(&em, 0x01);
                emit_byte(&em, 0x31); emit_byte(&em, 0xC9);
                size_t j_test_false_cb = emit_jmp_rel32_ph(&em);

                patch_rel32(&em, j_not_false_cb, em.pos);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
                emit_byte(&em, 0x48); emit_byte(&em, 0x8B); emit_byte(&em, 0x00);
                emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xC7);
                size_t j_vtslow_cb = emit_jcc_rel32_ph(&em, CC_NZ);
                emit_byte(&em, 0x83); emit_byte(&em, 0x6F);
                emit_byte(&em, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(&em, 0x01);
                emit_byte(&em, 0xB9); emit_u32(&em, 1);
                size_t j_test_true_cb = emit_jmp_rel32_ph(&em);

                /* Full value_truthy + value_decref fallback. */
                patch_rel32(&em, j_vtslow_cb, em.pos);
                emit_push_reg(&em, RDI);
                emit_sub_reg_imm32(&em, RSP, 8);
                emit_call_abs(&em, (void *)(uintptr_t)value_truthy);
                emit_byte(&em, 0x89); emit_byte(&em, 0xC1);
                emit_add_reg_imm32(&em, RSP, 8);
                emit_pop_reg(&em, RDI);
                emit_call_abs(&em, (void *)(uintptr_t)value_decref);

                /* Converge truthy-test tails; test + branch. */
                size_t test_pos_cb = em.pos;
                patch_rel32(&em, j_test_smi_cb,   test_pos_cb);
                patch_rel32(&em, j_test_false_cb, test_pos_cb);
                patch_rel32(&em, j_test_true_cb,  test_pos_cb);
                emit_byte(&em, 0x85); emit_byte(&em, 0xC9);  /* test ecx, ecx */
                uint8_t cc_slow = branch_if_false ? CC_Z : CC_NZ;
                size_t p_slow = emit_jcc_rel32_ph(&em, cc_slow);
                ADD_FIXUP(p_slow, in->imm);

                /* Done: fast-path (branch not taken) fall-through lands here.
                 * Slow-path (branch not taken) also falls through to here. */
                patch_rel32(&em, j_done_fast, em.pos);
                break;
            }

            case IR_RETURN: {
                /* Writeback each local_vreg into frame->base[slot]
                 * so the frame teardown sees the up-to-date values.
                 * For every slot: decref the stale value still sitting
                 * there (added a +1 at prologue -- no one has touched
                 * it since), then move local_vreg in (ownership
                 * transferred; no incref). After this, local_vregs
                 * are logically consumed and the ok-exit epilogue
                 * skips their extra decref. */
                for (int si = 0; si < f->n_locals; si++) {
                    if (!(f->local_written && f->local_written[si]))
                        continue;  /* borrowed: slot still has original value */
                    IRVReg lv = f->local_vregs[si];
                    emit_mov_reg_mem_r13(&em, RSI, FRAME_OFF_BASE);
                    emit_mov_reg_mem(&em, RAX, RSI, si * 8);
                    emit_load_vreg(&em, lv, a, RCX);
                    emit_mov_mem_reg(&em, RSI, si * 8, RCX);
                    emit_test_rr(&em, RAX, RAX);
                    size_t jz = emit_jcc_rel32_ph(&em, CC_Z);
                    emit_mov_rr(&em, RDI, RAX);
                    emit_call_abs(&em, (void *)(uintptr_t)value_decref);
                    patch_rel32(&em, jz, em.pos);
                }

                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_return_fast);
                emit_test_eax_eax(&em);
                /* jnz .slow */
                size_t jnz_slow = emit_jcc_rel32_ph(&em, CC_NZ);
                /* Success: frame torn down, locals already written
                 * back and freed by teardown. Use ok_exit path which
                 * skips the local-vreg decref loop. */
                size_t p_ok = emit_jmp_rel32_ph(&em);
                ADD_OK_EXIT(p_ok);
                /* Slow path: vm_step_jit executes OP_RETURN in full. */
                patch_rel32(&em, jnz_slow, em.pos);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                size_t p_ex = emit_jmp_rel32_ph(&em);
                ADD_OK_EXIT(p_ex);
                break;
            }

            case IR_CALL: {
                /* Flush callee + args to vm->sp in order. After that,
                 * vm_call_closure_fast picks them up with the standard
                 * OP_CALL layout. If it returns 0, drive inner frames
                 * via tier2_run_until and then pop the result. If it
                 * returns 1, fall back to vm_step_jit at the OP_CALL
                 * instruction (rare types: overloads, varargs, native
                 * bindings). */
                /* Layout: flush callee + args onto vm->sp, stash the
                 * pre-call frame_count at [rbp + RC_STASH_DISP], set
                 * frame->ip, call vm_call_closure_fast. That helper
                 * now auto-invokes proto->jit_entry when the callee
                 * has one, so in the common case the frame has already
                 * been pushed, run, and torn down by the time it
                 * returns. We still check frame_count to catch the
                 * residual-pushed-frame case (no jit_entry, fast path
                 * didn't recurse). The slow path (return 1) routes
                 * through vm_step_jit which handles builtins /
                 * overloads / variadics. */
                int argc = in->imm;
                emit_load_vreg(&em, in->src1, a, RAX);
                emit_vmsp_push_rax(&em);
                for (int k = 0; k < argc; k++) {
                    emit_load_vreg(&em, in->call_args[k], a, RAX);
                    emit_vmsp_push_rax(&em);
                }
                /* mov ecx, [r12 + VM_OFF_FRAME_COUNT] */
                emit_byte(&em, 0x41); emit_byte(&em, 0x8B); emit_byte(&em, 0x4C);
                emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAME_COUNT);
                /* mov [rbp + RC_STASH_DISP], ecx  -- stash baseline_fc */
                emit_byte(&em, 0x89); emit_byte(&em, 0x4D);
                emit_byte(&em, (uint8_t)(int8_t)RC_STASH_DISP);

                /* frame->ip = code_base + bc_off (for helpers that
                 * need to locate the current instruction). */
                emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)(code_base + bc_off));
                emit_mov_mem_r13_reg(&em, FRAME_OFF_IP, RAX);

                /* Try the closure fast path. */
                emit_mov_rr(&em, RDI, R12);
                emit_byte(&em, 0xBE); emit_u32(&em, (uint32_t)argc);
                emit_call_abs(&em, (void *)(uintptr_t)vm_call_closure_fast);
                emit_test_eax_eax(&em);
                size_t jnz_slow = emit_jcc_rel32_ph(&em, CC_NZ);

                /* Fast path: eax == 0. */
                /* If a frame still sits above baseline (callee had no
                 * jit_entry and vm_call_closure_fast only pushed it),
                 * drain it via tier2_run_until. */
                {
                    /* mov esi, [rbp + RC_STASH_DISP] */
                    emit_byte(&em, 0x8B); emit_byte(&em, 0x75);
                    emit_byte(&em, (uint8_t)(int8_t)RC_STASH_DISP);
                    /* cmp esi, [r12 + VM_OFF_FRAME_COUNT] */
                    emit_byte(&em, 0x41); emit_byte(&em, 0x3B); emit_byte(&em, 0x74);
                    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAME_COUNT);
                    /* jge .no_drain  (esi >= fc means fc <= baseline) */
                    size_t j_no_drain = emit_jcc_rel32_ph(&em, CC_GE);
                    emit_mov_rr(&em, RDI, R12);
                    emit_call_abs(&em, (void *)(uintptr_t)tier2_run_until);
                    emit_test_eax_eax(&em);
                    size_t j_err1 = emit_jcc_rel32_ph(&em, CC_NZ);
                    ADD_ERR_EXIT(j_err1);
                    patch_rel32(&em, j_no_drain, em.pos);
                }
                emit_refresh_r13(&em);
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);
                size_t j_join = emit_jmp_rel32_ph(&em);

                /* Slow path: vm_step_jit runs the full OP_CALL (may
                 * push a frame for an overload / variadic closure, or
                 * simply invoke a builtin in place). */
                patch_rel32(&em, jnz_slow, em.pos);
                emit_mov_rr(&em, RDI, R12);
                emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                emit_test_eax_eax(&em);
                size_t j_ex2 = emit_jcc_rel32_ph(&em, CC_NZ);
                ADD_ERR_EXIT(j_ex2);
                {
                    emit_byte(&em, 0x8B); emit_byte(&em, 0x75);
                    emit_byte(&em, (uint8_t)(int8_t)RC_STASH_DISP);
                    emit_byte(&em, 0x41); emit_byte(&em, 0x3B); emit_byte(&em, 0x74);
                    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAME_COUNT);
                    size_t j_no_drain2 = emit_jcc_rel32_ph(&em, CC_GE);
                    emit_mov_rr(&em, RDI, R12);
                    emit_call_abs(&em, (void *)(uintptr_t)tier2_run_until);
                    emit_test_eax_eax(&em);
                    size_t j_err3 = emit_jcc_rel32_ph(&em, CC_NZ);
                    ADD_ERR_EXIT(j_err3);
                    patch_rel32(&em, j_no_drain2, em.pos);
                }
                emit_refresh_r13(&em);
                emit_vmsp_pop_rax(&em);
                emit_store_vreg(&em, in->dst, a, RAX);

                patch_rel32(&em, j_join, em.pos);
                break;
            }

            default:
                /* Unreachable -- ra_lower ensured every op is supported. */
                free(block_pos); free(fixups); free(err_exit_patches);
                return NULL;
            }

            if (em.overflow) {
                free(block_pos); free(fixups); free(err_exit_patches);
                return NULL;
            }
        }
    }

    /* ----- shared exit epilogues -----
     * err_exit walks every local_vreg, decref-ing (the allocator
     * handed out the same storage for the life of the function so
     * these reads/writes are stable). eax is preserved across the
     * loop so the original step-jit return code propagates. ok_exit
     * skips the decref; both converge on the common restore+ret. */
    size_t err_exit_pos = em.pos;
    /* Preserve eax across decref calls: stash in rbx? But rbx is a
     * local_vreg home. Use a caller-saved reg: rsi survives our
     * value_decref calls? No -- rsi is caller-saved and clobbered.
     * Stash into a stack slot we already reserved. The first spill
     * slot lives at [rbp - 48]; if n_spill_slots == 0 we still have
     * the alignment pad there. */
    /* mov [rbp - 48], eax  (store 32-bit rc) */
    emit_byte(&em, 0x89); emit_byte(&em, 0x45); emit_byte(&em, (uint8_t)(int8_t)-48);
    for (int si = 0; si < f->n_locals; si++) {
        if (!(f->local_written && f->local_written[si]))
            continue;  /* borrowed slot: no vreg ownership to release */
        IRVReg lv = f->local_vregs[si];
        emit_load_vreg(&em, lv, a, RDI);
        emit_test_rr(&em, RDI, RDI);
        size_t jz = emit_jcc_rel32_ph(&em, CC_Z);
        emit_call_abs(&em, (void *)(uintptr_t)value_decref);
        patch_rel32(&em, jz, em.pos);
    }
    /* eax = [rbp - 48] */
    emit_byte(&em, 0x8B); emit_byte(&em, 0x45); emit_byte(&em, (uint8_t)(int8_t)-48);

    size_t ok_exit_pos = em.pos;
    /* Convert vm_step_jit's tri-state return code to the caller's
     * 0/1 convention (matches tier-1 epilogue): positive/zero rcs
     * are "program done" / "continue" both mapped to 0, negative rcs
     * (errors) map to 1. */
    /* mov ecx, eax */
    emit_byte(&em, 0x89); emit_byte(&em, 0xC1);
    /* xor eax, eax */
    emit_byte(&em, 0x31); emit_byte(&em, 0xC0);
    /* test ecx, ecx */
    emit_byte(&em, 0x85); emit_byte(&em, 0xC9);
    /* jns +5 */
    emit_byte(&em, 0x79); emit_byte(&em, 0x05);
    /* mov eax, 1 */
    emit_byte(&em, 0xB8); emit_u32(&em, 1);
    emit_add_reg_imm32(&em, RSP, sub_amt);
    emit_pop_reg(&em, R15);
    emit_pop_reg(&em, R14);
    emit_pop_reg(&em, R13);
    emit_pop_reg(&em, R12);
    emit_pop_reg(&em, RBX);
    emit_pop_reg(&em, RBP);
    emit_ret(&em);

    /* Patch all block-target fixups. */
    for (int i = 0; i < n_fixups; i++) {
        int tb = fixups[i].target_block;
        if (tb < 0 || tb >= f->n_blocks || block_pos[tb] == (size_t)-1) {
            free(block_pos); free(fixups);
            free(err_exit_patches); free(ok_exit_patches);
            return NULL;
        }
        patch_rel32(&em, fixups[i].patch, block_pos[tb]);
    }
    /* Error exits walk through the decref loop; ok exits skip it. */
    for (int i = 0; i < n_err; i++) {
        patch_rel32(&em, err_exit_patches[i], err_exit_pos);
    }
    for (int i = 0; i < n_ok; i++) {
        patch_rel32(&em, ok_exit_patches[i], ok_exit_pos);
    }

    free(block_pos); free(fixups);
    free(err_exit_patches); free(ok_exit_patches);

    if (em.overflow) return NULL;

    if (getenv("XS_JIT_TIER2_DUMP")) {
        FILE *fp = fopen("/tmp/tier2.bin", "wb");
        if (fp) {
            size_t n = em.pos - (size_t)((uint8_t *)entry - j->code);
            fwrite(entry, 1, n, fp); fclose(fp);
            fprintf(stderr, "[tier2] %zuB at %p (proto=%s nlocals=%d n_spill=%d sub_amt=%d)\n",
                    n, entry, proto->name ? proto->name : "<top>",
                    f->n_locals, n_spill, sub_amt);
        }
    }

    j->code_used = em.pos;
    return entry;
}

#endif /* JIT_HAS_MMAP */
#else  /* !__x86_64__ */
void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a) {
    (void)j; (void)f; (void)a;
    return NULL;
}
#endif
