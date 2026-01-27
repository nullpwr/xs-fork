/* Phase 4 of the register-allocating JIT pipeline on AArch64.
 *
 * Mirrors ra_codegen.c (x86-64) op for op, sharing the same IR coming
 * out of ra_lower / ra_live / ra_alloc. Emits 32-bit ARM64 instructions
 * directly into the XSJIT code buffer (same buffer as the x86 path, so
 * jit_compile's arch dispatch picks whichever target this build is
 * for). Only active when __aarch64__.
 *
 * Register assignments (AAPCS64):
 *   x0                        scratch, first arg, return value
 *   x1..x7                    scratch / arg passing
 *   x8                        indirect result
 *   x9..x15                   scratch
 *   x16, x17                  linker scratch (avoided)
 *   x18                       platform reserved (never touched)
 *   x19                       VM*                    (pinned, callee-saved)
 *   x20                       current CallFrame*     (refreshed after calls)
 *   x21, x22, x23             vreg storage           (callee-saved)
 *   x24..x28                  callee-saved but unused
 *   x29 (FP), x30 (LR)        frame pointer / link register
 *
 * Spill slots live at [x29 - disp], matching the x86 path's rbp-relative
 * layout. Frame size is reserved by a single sub-sp in the prologue and
 * released in the epilogue. */

#include "jit/ra_ir.h"
#include "jit/jit.h"
#include "vm/vm.h"
#include "vm/bytecode.h"
#include "core/xs.h"
#include "core/value.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#define RA_ARM64_HAS_MMAP 1
#else
#define RA_ARM64_HAS_MMAP 0
#endif

#if !RA_ARM64_HAS_MMAP
void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a) {
    (void)j; (void)f; (void)a;
    return NULL;
}
#else

/* VM layout mirrors the x86 codegen's constants exactly. */
#define VM_OFF_SP            16
#define VM_OFF_FRAMES        24
#define VM_OFF_FRAME_COUNT   36
#define FRAME_OFF_IP          8
#define FRAME_OFF_BASE       16
#define FRAME_SIZE         1608
#define VAL_OFF_REFCOUNT      4

#define X_VM    19
#define X_FRAME 20
#define X_SPILL0 21  /* 1st vreg phys reg */
#define X_SPILL1 22
#define X_SPILL2 23

/* AArch64 condition codes (low 4 bits of cond field). */
#define CC_EQ  0x0
#define CC_NE  0x1
#define CC_CS  0x2
#define CC_CC  0x3
#define CC_MI  0x4
#define CC_PL  0x5
#define CC_VS  0x6
#define CC_VC  0x7
#define CC_HI  0x8
#define CC_LS  0x9
#define CC_GE  0xA
#define CC_LT  0xB
#define CC_GT  0xC
#define CC_LE  0xD
#define CC_AL  0xE

/* Each vreg is assigned one of {X21, X22, X23} by the allocator. */
static int phys_to_arm(int phys) {
    switch (phys) {
    case 0: return X_SPILL0;
    case 1: return X_SPILL1;
    case 2: return X_SPILL2;
    default: return X_SPILL0;
    }
}

/* Frame layout, positive offsets from x29 (= SP at function entry
 * minus sub_amt). AArch64's convention is opposite to x86's rbp --
 * the FP sits at the LOW address of the frame, so everything lives
 * at positive offsets:
 *
 *   [x29 +  0] saved x29
 *   [x29 +  8] saved x30 (LR)
 *   [x29 + 16] saved x19   -- X_VM
 *   [x29 + 24] saved x20   -- X_FRAME
 *   [x29 + 32] saved x21   -- vreg phys 0
 *   [x29 + 40] saved x22   -- vreg phys 1
 *   [x29 + 48] saved x23   -- vreg phys 2
 *   [x29 + 56] saved x24   -- unused (paired for STP)
 *   [x29 + 64] RC stash    -- scratch word reused by IR_CALL etc.
 *   [x29 + 72..] spill slots (8 B each)
 *
 * Earlier revisions mirrored the x86 layout blindly with negative
 * offsets, which wrote spill data *below* our frame and corrupted
 * the caller's stack; this is the fix. */
#define RC_STASH_DISP 64
static int32_t spill_disp(int slot) {
    return 72 + 8 * slot;
}

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    int      overflow;
} Emitter;

static void emit_init(Emitter *e, uint8_t *buf, size_t cap, size_t start) {
    e->buf = buf; e->cap = cap; e->pos = start; e->overflow = 0;
}

static inline void emit_u32(Emitter *e, uint32_t v) {
    if (e->pos + 4 > e->cap) { e->overflow = 1; return; }
    e->buf[e->pos + 0] = (uint8_t)v;
    e->buf[e->pos + 1] = (uint8_t)(v >> 8);
    e->buf[e->pos + 2] = (uint8_t)(v >> 16);
    e->buf[e->pos + 3] = (uint8_t)(v >> 24);
    e->pos += 4;
}

/* --- raw ARM64 instruction encoders (all 32-bit, little-endian) --- */

/* MOVZ xd, #imm, LSL #(shift*16) */
static void a_movz(Emitter *e, int rd, uint16_t imm, int shift) {
    emit_u32(e, 0xD2800000u | ((uint32_t)(shift & 3) << 21) |
                 ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
}
/* MOVK xd, #imm, LSL #(shift*16) */
static void a_movk(Emitter *e, int rd, uint16_t imm, int shift) {
    emit_u32(e, 0xF2800000u | ((uint32_t)(shift & 3) << 21) |
                 ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
}
/* Materialise a 64-bit literal in rd via MOVZ + up to 3 MOVKs. */
static void a_mov_imm64(Emitter *e, int rd, uint64_t imm) {
    a_movz(e, rd, (uint16_t)(imm & 0xFFFF), 0);
    if ((imm >> 16) & 0xFFFF) a_movk(e, rd, (uint16_t)((imm >> 16) & 0xFFFF), 1);
    if ((imm >> 32) & 0xFFFF) a_movk(e, rd, (uint16_t)((imm >> 32) & 0xFFFF), 2);
    if ((imm >> 48) & 0xFFFF) a_movk(e, rd, (uint16_t)((imm >> 48) & 0xFFFF), 3);
}
/* MOV xd, xn  (register to register) via ORR xd, xzr, xn. */
static void a_mov_reg(Emitter *e, int rd, int rn) {
    emit_u32(e, 0xAA0003E0u | ((uint32_t)(rn & 31) << 16) | (uint32_t)(rd & 31));
}
/* LDR xt, [xn, #imm]  (64-bit load, unsigned imm12 in units of 8 bytes). */
static void a_ldr_off(Emitter *e, int rt, int rn, int off) {
    /* Off in bytes; encoded / 8. If off doesn't fit in 12-bit scaled imm,
     * materialise through x16 (scratch). */
    if (off >= 0 && (off & 7) == 0 && (off / 8) < 4096) {
        uint32_t imm12 = (uint32_t)(off / 8);
        emit_u32(e, 0xF9400000u | (imm12 << 10) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)off);
        /* LDR xt, [xn, x16] */
        emit_u32(e, 0xF8606800u |
                     ((uint32_t)16u << 16) |
                     ((uint32_t)(rn & 31) << 5) |
                     (uint32_t)(rt & 31));
    }
}
/* STR xt, [xn, #imm] */
static void a_str_off(Emitter *e, int rt, int rn, int off) {
    if (off >= 0 && (off & 7) == 0 && (off / 8) < 4096) {
        uint32_t imm12 = (uint32_t)(off / 8);
        emit_u32(e, 0xF9000000u | (imm12 << 10) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)off);
        emit_u32(e, 0xF8206800u |
                     ((uint32_t)16u << 16) |
                     ((uint32_t)(rn & 31) << 5) |
                     (uint32_t)(rt & 31));
    }
}
/* LDR wt, [xn, #imm]  (32-bit load). */
static void a_ldr_w_off(Emitter *e, int rt, int rn, int off) {
    if (off >= 0 && (off & 3) == 0 && (off / 4) < 4096) {
        uint32_t imm12 = (uint32_t)(off / 4);
        emit_u32(e, 0xB9400000u | (imm12 << 10) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)off);
        emit_u32(e, 0xB8606800u |
                     ((uint32_t)16u << 16) |
                     ((uint32_t)(rn & 31) << 5) |
                     (uint32_t)(rt & 31));
    }
}
/* ADD xd, xn, #imm12 (unshifted). */
static void a_add_imm(Emitter *e, int rd, int rn, int imm) {
    if (imm >= 0 && imm < 4096) {
        emit_u32(e, 0x91000000u | ((uint32_t)imm << 10) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)imm);
        emit_u32(e, 0x8B000000u | ((uint32_t)16u << 16) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
    }
}
/* SUB xd, xn, #imm12. */
static void a_sub_imm(Emitter *e, int rd, int rn, int imm) {
    if (imm >= 0 && imm < 4096) {
        emit_u32(e, 0xD1000000u | ((uint32_t)imm << 10) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)imm);
        emit_u32(e, 0xCB000000u | ((uint32_t)16u << 16) |
                     ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
    }
}
/* ADD xd, xn, xm. */
static void a_add_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x8B000000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* SUB xd, xn, xm. */
static void a_sub_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0xCB000000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* MUL xd, xn, xm  (MADD xd, xn, xm, xzr). */
static void a_mul_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x9B007C00u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* SDIV xd, xn, xm. */
static void a_sdiv_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x9AC00C00u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* MSUB xd, xn, xm, xa  (used to synthesise modulo: xd = xa - xn*xm). */
static void a_msub_reg(Emitter *e, int rd, int rn, int rm, int ra) {
    emit_u32(e, 0x9B008000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(ra & 31) << 10) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* AND xd, xn, xm. */
static void a_and_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x8A000000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* ORR xd, xn, xm. */
static void a_orr_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0xAA000000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* EOR xd, xn, xm. */
static void a_eor_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0xCA000000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* MVN xd, xm   (ORN xd, xzr, xm). */
static void a_mvn_reg(Emitter *e, int rd, int rm) {
    emit_u32(e, 0xAA2003E0u | ((uint32_t)(rm & 31) << 16) | (uint32_t)(rd & 31));
}
/* NEG xd, xm   (SUB xd, xzr, xm). */
static void a_neg_reg(Emitter *e, int rd, int rm) {
    emit_u32(e, 0xCB0003E0u | ((uint32_t)(rm & 31) << 16) | (uint32_t)(rd & 31));
}
/* LSL xd, xn, xm (UDF via LSLV). */
static void a_lsl_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x9AC02000u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* ASR xd, xn, xm. */
static void a_asr_reg(Emitter *e, int rd, int rn, int rm) {
    emit_u32(e, 0x9AC02800u | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
}
/* CMP xn, xm  (SUBS xzr, xn, xm). */
static void a_cmp_reg(Emitter *e, int rn, int rm) {
    emit_u32(e, 0xEB00001Fu | ((uint32_t)(rm & 31) << 16) |
                 ((uint32_t)(rn & 31) << 5));
}
/* CMP xn, #imm12. */
static void a_cmp_imm(Emitter *e, int rn, int imm) {
    if (imm >= 0 && imm < 4096) {
        emit_u32(e, 0xF100001Fu | ((uint32_t)imm << 10) |
                     ((uint32_t)(rn & 31) << 5));
    } else {
        a_mov_imm64(e, 16, (uint64_t)(int64_t)imm);
        a_cmp_reg(e, rn, 16);
    }
}
/* TST xn, #imm (via AND xzr, xn, #imm). Only used here for #1 (the SMI
 * tag bit), so we hand-encode that specific case. */
static void a_tst_bit0(Emitter *e, int rn) {
    /* ANDS xzr, xn, #1 — immediate encoding N=1, immr=0, imms=0 -> #1. */
    emit_u32(e, 0xF240001Fu | ((uint32_t)(rn & 31) << 5));
}
/* CSET xd, cond  (conditional set: xd = cond ? 1 : 0). Implemented via
 * CSINC xd, xzr, xzr, <inv(cond)>. */
static void a_cset(Emitter *e, int rd, int cond) {
    emit_u32(e, 0x9A9F07E0u | (((uint32_t)(cond ^ 1) & 0xF) << 12) |
                 (uint32_t)(rd & 31));
}
/* B.cond relative (patched later with target_offset - patch_pc) / 4. */
static size_t a_bcond_ph(Emitter *e, int cond) {
    size_t pos = e->pos;
    emit_u32(e, 0x54000000u | (uint32_t)(cond & 0xF));
    return pos;
}
/* B relative, 26-bit signed imm (scaled by 4). */
static size_t a_b_ph(Emitter *e) {
    size_t pos = e->pos;
    emit_u32(e, 0x14000000u);
    return pos;
}
/* BR xn  (absolute register jump, used by jump tables -- we just jump to a patched landing later). */
static void a_blr(Emitter *e, int rn) {
    emit_u32(e, 0xD63F0000u | ((uint32_t)(rn & 31) << 5));
}
static void a_ret(Emitter *e) {
    emit_u32(e, 0xD65F03C0u);
}
/* Absolute call: materialise target in x16, BLR x16. */
static void a_call_abs(Emitter *e, void *fn) {
    a_mov_imm64(e, 16, (uint64_t)(uintptr_t)fn);
    a_blr(e, 16);
}

/* Patch a B.cond whose target is target_pc into the slot at patch_pc. */
static void patch_bcond(Emitter *e, size_t patch_pc, size_t target_pc) {
    int64_t diff = ((int64_t)target_pc - (int64_t)patch_pc) / 4;
    uint32_t w = (uint32_t)e->buf[patch_pc]
               | ((uint32_t)e->buf[patch_pc + 1] << 8)
               | ((uint32_t)e->buf[patch_pc + 2] << 16)
               | ((uint32_t)e->buf[patch_pc + 3] << 24);
    /* B.cond's offset lives in bits [23:5]. */
    w = (w & ~(0x7FFFFu << 5)) | (((uint32_t)(diff & 0x7FFFF)) << 5);
    e->buf[patch_pc] = (uint8_t)w;
    e->buf[patch_pc + 1] = (uint8_t)(w >> 8);
    e->buf[patch_pc + 2] = (uint8_t)(w >> 16);
    e->buf[patch_pc + 3] = (uint8_t)(w >> 24);
}
static void patch_b(Emitter *e, size_t patch_pc, size_t target_pc) {
    int64_t diff = ((int64_t)target_pc - (int64_t)patch_pc) / 4;
    uint32_t w = 0x14000000u | ((uint32_t)(diff & 0x3FFFFFF));
    e->buf[patch_pc] = (uint8_t)w;
    e->buf[patch_pc + 1] = (uint8_t)(w >> 8);
    e->buf[patch_pc + 2] = (uint8_t)(w >> 16);
    e->buf[patch_pc + 3] = (uint8_t)(w >> 24);
}

/* --- vreg load/store --- */

static void emit_load_vreg(Emitter *e, IRVReg v, IRAlloc *a, int dst) {
    if (v < 0) return;
    int8_t phys = a->reg[v];
    if (phys >= 0) {
        int src = phys_to_arm(phys);
        if (src != dst) a_mov_reg(e, dst, src);
    } else {
        /* LDR xt, [x29, #disp]  -- 12-bit unsigned-imm scaled by 8,
         * which reaches up to 32760 bytes. Spill slots all live above
         * the saved-reg block so disp is always positive and aligned. */
        a_ldr_off(e, dst, 29, spill_disp(a->spill[v]));
    }
}

static void emit_store_vreg(Emitter *e, IRVReg v, IRAlloc *a, int src) {
    if (v < 0) return;
    int8_t phys = a->reg[v];
    if (phys >= 0) {
        int dst = phys_to_arm(phys);
        if (dst != src) a_mov_reg(e, dst, src);
    } else {
        a_str_off(e, src, 29, spill_disp(a->spill[v]));
    }
}

/* Inline value_incref. Skips SMIs and NULLs. */
static void emit_inline_incref_x0(Emitter *e) {
    /* TST x0, #1 ; B.NE +4 instrs */
    a_tst_bit0(e, 0);
    size_t j_smi = a_bcond_ph(e, CC_NE);
    /* CBZ x0, +3 instrs */
    size_t cbz_pc = e->pos;
    emit_u32(e, 0xB4000000u | (uint32_t)(0u & 31));  /* CBZ x0, #imm — patched */
    /* LDR w1, [x0, #VAL_OFF_REFCOUNT] */
    a_ldr_w_off(e, 1, 0, VAL_OFF_REFCOUNT);
    /* ADD w1, w1, #1 */
    emit_u32(e, 0x11000421u);  /* ADD w1, w1, #1 */
    /* STR w1, [x0, #VAL_OFF_REFCOUNT] */
    emit_u32(e, 0xB9000001u | ((uint32_t)(VAL_OFF_REFCOUNT / 4) << 10) |
                 ((uint32_t)0u << 5) | (uint32_t)1u);
    /* Patch CBZ to land here. */
    {
        int64_t diff = ((int64_t)e->pos - (int64_t)cbz_pc) / 4;
        uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5);
        e->buf[cbz_pc] = (uint8_t)w;
        e->buf[cbz_pc + 1] = (uint8_t)(w >> 8);
        e->buf[cbz_pc + 2] = (uint8_t)(w >> 16);
        e->buf[cbz_pc + 3] = (uint8_t)(w >> 24);
    }
    patch_bcond(e, j_smi, e->pos);
}

/* --- vm->sp push / pop --- */

static void emit_vmsp_push_x0(Emitter *e) {
    /* LDR x1, [x19, #VM_OFF_SP] ; STR x0, [x1] ; ADD x1, x1, #8 ; STR x1, [x19, #VM_OFF_SP] */
    a_ldr_off(e, 1, X_VM, VM_OFF_SP);
    emit_u32(e, 0xF9000020u);  /* STR x0, [x1] */
    a_add_imm(e, 1, 1, 8);
    a_str_off(e, 1, X_VM, VM_OFF_SP);
}

static void emit_vmsp_pop_x0(Emitter *e) {
    /* LDR x1, [x19, #VM_OFF_SP] ; SUB x1, x1, #8 ; STR x1, [x19, #VM_OFF_SP] ; LDR x0, [x1] */
    a_ldr_off(e, 1, X_VM, VM_OFF_SP);
    a_sub_imm(e, 1, 1, 8);
    a_str_off(e, 1, X_VM, VM_OFF_SP);
    emit_u32(e, 0xF9400020u);  /* LDR x0, [x1] */
}

/* Refresh x20 = &vm->frames[vm->frame_count - 1]. */
static void emit_refresh_frame(Emitter *e) {
    /* LDR w1, [x19, #VM_OFF_FRAME_COUNT] ; SUB w1, w1, #1 ;
     * MOV x2, #FRAME_SIZE ; MUL x1, x1, x2 ;
     * LDR x2, [x19, #VM_OFF_FRAMES] ; ADD x20, x2, x1 */
    a_ldr_w_off(e, 1, X_VM, VM_OFF_FRAME_COUNT);
    /* SUB w1, w1, #1 */
    emit_u32(e, 0x51000421u);
    a_mov_imm64(e, 2, (uint64_t)FRAME_SIZE);
    a_mul_reg(e, 1, 1, 2);
    a_ldr_off(e, 2, X_VM, VM_OFF_FRAMES);
    a_add_reg(e, X_FRAME, 2, 1);
}

/* Forward-branch fixups. */
typedef struct { size_t patch; int target_block; } Fixup;

void *ralow_codegen(XSJIT *j, IRFunc *f, IRAlloc *a) {
    if (!j || !j->available || !f) return NULL;

    /* Budget: ~64 bytes per IR inst (16 ARM64 instructions) + 1 KiB
     * prologue/epilogue slack. */
    size_t est = (size_t)f->n_insts * 64 + 1024;
    if (j->code_used + est > j->code_size) return NULL;

    Emitter em;
    emit_init(&em, j->code, j->code_size, j->code_used);
    void *entry = (void *)(j->code + em.pos);

    int n_spill = a->n_spill_slots;
    /* 64 B saved regs + 8 B RC stash + 8 B per spill slot, padded to 16. */
    int32_t sub_amt = 72 + 8 * n_spill;
    if (sub_amt & 15) sub_amt += 16 - (sub_amt & 15);
    /* SUB sp, sp, #sub_amt takes a 12-bit imm; if we ever need more we
     * could shift or materialise in x16, but keeping the simple form
     * means bailing for very wide functions. */
    if (sub_amt > 4080) {
        free(NULL); return NULL;
    }

    /* SUB sp, sp, #sub_amt  (sp-form imm12, sh=0). */
    emit_u32(&em, 0xD10003FFu | ((uint32_t)(sub_amt & 0xFFFu) << 10));
    /* STP x29, x30, [sp, #0] */
    emit_u32(&em, 0xA9000000u | 29u | (30u << 10) | (31u << 5));
    /* STP x19, x20, [sp, #16] */
    emit_u32(&em, 0xA9000000u | 19u | (20u << 10) | (31u << 5) | ((uint32_t)(16/8) << 15));
    /* STP x21, x22, [sp, #32] */
    emit_u32(&em, 0xA9000000u | 21u | (22u << 10) | (31u << 5) | ((uint32_t)(32/8) << 15));
    /* STP x23, x24, [sp, #48] */
    emit_u32(&em, 0xA9000000u | 23u | (24u << 10) | (31u << 5) | ((uint32_t)(48/8) << 15));
    /* ADD x29, sp, #0   (MOV x29, sp but sp-form). */
    emit_u32(&em, 0x910003FDu);

    /* X_VM = X0 (first arg) */
    a_mov_reg(&em, X_VM, 0);
    emit_refresh_frame(&em);

    /* Load each local from frame->base[i] into its persistent vreg. */
    for (int i = 0; i < f->n_locals; i++) {
        /* x0 = frame->base */
        a_ldr_off(&em, 0, X_FRAME, FRAME_OFF_BASE);
        /* x0 = [x0 + i*8] */
        a_ldr_off(&em, 0, 0, i * 8);
        if (f->local_written && f->local_written[i])
            emit_inline_incref_x0(&em);
        emit_store_vreg(&em, f->local_vregs[i], a, 0);
    }

    /* Pin single_step = 1 on the VM so vm_step_jit uses the fast exit. */
    {
        int32_t off = (int32_t)offsetof(VM, single_step);
        a_mov_imm64(&em, 0, 1);
        /* STR w0, [x19, #off] */
        if (off >= 0 && (off & 3) == 0 && (off / 4) < 4096) {
            emit_u32(&em, 0xB9000000u | ((uint32_t)(off / 4) << 10) |
                           ((uint32_t)X_VM << 5) | 0u);
        } else {
            a_mov_imm64(&em, 16, (uint64_t)(int64_t)off);
            emit_u32(&em, 0xB8206800u | ((uint32_t)16u << 16) |
                           ((uint32_t)X_VM << 5) | 0u);
        }
    }

    size_t *block_pos = xs_calloc((size_t)f->n_blocks, sizeof(size_t));
    for (int i = 0; i < f->n_blocks; i++) block_pos[i] = (size_t)-1;
    Fixup *fixups = xs_malloc(1024 * sizeof(Fixup));
    int n_fixups = 0, cap_fixups = 1024;

    size_t *err_exit_patches = xs_malloc(256 * sizeof(size_t));
    int n_err = 0, cap_err = 256;
    size_t *ok_exit_patches = xs_malloc(256 * sizeof(size_t));
    int n_ok = 0, cap_ok = 256;
    size_t *deopt_exit_patches = xs_malloc(256 * sizeof(size_t));
    int n_deopt = 0, cap_deopt = 256;

    #define ADD_FIXUP(p, tb) do { \
        if (n_fixups == cap_fixups) { cap_fixups *= 2; fixups = xs_realloc(fixups, (size_t)cap_fixups * sizeof(Fixup)); } \
        fixups[n_fixups].patch = (p); fixups[n_fixups].target_block = (tb); n_fixups++; \
    } while (0)

    #define ADD_ERR_EXIT(p) do { \
        if (n_err == cap_err) { cap_err *= 2; err_exit_patches = xs_realloc(err_exit_patches, (size_t)cap_err * sizeof(size_t)); } \
        err_exit_patches[n_err++] = (p); \
    } while (0)
    #define ADD_OK_EXIT(p) do { \
        if (n_ok == cap_ok) { cap_ok *= 2; ok_exit_patches = xs_realloc(ok_exit_patches, (size_t)cap_ok * sizeof(size_t)); } \
        ok_exit_patches[n_ok++] = (p); \
    } while (0)
    #define ADD_DEOPT_EXIT(p) do { \
        if (n_deopt == cap_deopt) { cap_deopt *= 2; deopt_exit_patches = xs_realloc(deopt_exit_patches, (size_t)cap_deopt * sizeof(size_t)); } \
        deopt_exit_patches[n_deopt++] = (p); \
    } while (0)

    XSProto *proto = f->proto;
    Instruction *code_base = proto->chunk.code;

    for (int bi = 0; bi < f->n_blocks; bi++) {
        IRBlock *blk = &f->blocks[bi];
        block_pos[bi] = em.pos;

        for (int ii = blk->start; ii < blk->end; ii++) {
            IRInst *in = &f->insts[ii];
            int bc_off = in->bc_offset;

            switch (in->op) {
            case IR_NOP:
                break;

            case IR_CONST: {
                Value *cv = proto->chunk.consts[in->imm];
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)cv);
                emit_inline_incref_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }
            case IR_PUSH_NULL:
                a_call_abs(&em, (void *)(uintptr_t)xs_null);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            case IR_PUSH_TRUE:
                a_mov_imm64(&em, 0, 1);
                a_call_abs(&em, (void *)(uintptr_t)xs_bool);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            case IR_PUSH_FALSE:
                a_mov_imm64(&em, 0, 0);
                a_call_abs(&em, (void *)(uintptr_t)xs_bool);
                emit_store_vreg(&em, in->dst, a, 0);
                break;

            case IR_LOAD_LOCAL: {
                emit_load_vreg(&em, f->local_vregs[in->imm], a, 0);
                emit_inline_incref_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }
            case IR_LOAD_GLOBAL: {
                /* Full slow-path dispatch to vm_load_global_ic; the inline
                 * IC exists on x86 but isn't ported here yet. */
                a_mov_reg(&em, 0, X_VM);
                a_mov_imm64(&em, 1, (uint64_t)(uint32_t)bc_off);
                a_mov_imm64(&em, 2, (uint64_t)(uint32_t)in->imm);
                a_call_abs(&em, (void *)(uintptr_t)vm_load_global_ic);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }
            case IR_LOAD_UP: {
                /* frame->closure_val -> v->cl -> upvalues[imm] -> ptr -> *ptr. */
                a_ldr_off(&em, 0, X_FRAME, 0);   /* closure_val */
                a_ldr_off(&em, 0, 0, 8);         /* v->cl */
                a_ldr_off(&em, 0, 0, 8);         /* cl->upvalues */
                a_ldr_off(&em, 0, 0, in->imm * 8);
                a_ldr_off(&em, 0, 0, 0);         /* uv->ptr */
                a_ldr_off(&em, 0, 0, 0);         /* *ptr */
                emit_inline_incref_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }
            case IR_DUP: {
                emit_load_vreg(&em, in->src1, a, 0);
                emit_inline_incref_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }
            case IR_MOVE: {
                emit_load_vreg(&em, in->src1, a, 0);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }

            case IR_POP: {
                emit_load_vreg(&em, in->src1, a, 0);
                a_call_abs(&em, (void *)(uintptr_t)value_decref);
                break;
            }
            case IR_STORE_LOCAL: {
                IRVReg lv = f->local_vregs[in->imm];
                emit_load_vreg(&em, lv, a, 0);
                /* CBZ x0, skip_decref */
                size_t cbz = em.pos;
                emit_u32(&em, 0xB4000000u | 0u);
                a_call_abs(&em, (void *)(uintptr_t)value_decref);
                {
                    int64_t diff = ((int64_t)em.pos - (int64_t)cbz) / 4;
                    uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5);
                    em.buf[cbz] = (uint8_t)w;
                    em.buf[cbz + 1] = (uint8_t)(w >> 8);
                    em.buf[cbz + 2] = (uint8_t)(w >> 16);
                    em.buf[cbz + 3] = (uint8_t)(w >> 24);
                }
                emit_load_vreg(&em, in->src1, a, 0);
                emit_store_vreg(&em, lv, a, 0);
                break;
            }
            case IR_STORE_UP: {
                /* Reach &uv->ptr, load ptr into x2, old value into x3,
                 * store new into *ptr, decref old. */
                emit_load_vreg(&em, in->src1, a, 4);  /* x4 = new value */
                a_ldr_off(&em, 0, X_FRAME, 0);
                a_ldr_off(&em, 0, 0, 8);
                a_ldr_off(&em, 0, 0, 8);
                a_ldr_off(&em, 0, 0, in->imm * 8);
                a_ldr_off(&em, 2, 0, 0);  /* x2 = uv->ptr */
                a_ldr_off(&em, 3, 2, 0);  /* x3 = *ptr (old) */
                emit_u32(&em, 0xF9000044u);  /* STR x4, [x2] */
                /* if (old) value_decref(old) */
                /* CBZ x3, +2 */
                size_t cbz = em.pos;
                emit_u32(&em, 0xB4000000u | 3u);
                a_mov_reg(&em, 0, 3);
                a_call_abs(&em, (void *)(uintptr_t)value_decref);
                {
                    int64_t diff = ((int64_t)em.pos - (int64_t)cbz) / 4;
                    uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5) | 3u;
                    em.buf[cbz] = (uint8_t)w;
                    em.buf[cbz + 1] = (uint8_t)(w >> 8);
                    em.buf[cbz + 2] = (uint8_t)(w >> 16);
                    em.buf[cbz + 3] = (uint8_t)(w >> 24);
                }
                break;
            }

            /* Binops: dispatch through vm_step_jit (no arm64 SMI fast
             * path yet; the infrastructure is here, add per-op inline
             * fast paths as the arm64 target matures). */
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            case IR_MOD: case IR_BAND: case IR_BOR: case IR_BXOR:
            case IR_SHL: case IR_SHR:
            case IR_LT: case IR_GT: case IR_LE: case IR_GE:
            case IR_EQ: case IR_NE:
            case IR_CMP_BR: {
                /* Flush both operands to vm->sp, set frame->ip, dispatch. */
                emit_load_vreg(&em, in->src1, a, 0);
                emit_vmsp_push_x0(&em);
                emit_load_vreg(&em, in->src2, a, 0);
                emit_vmsp_push_x0(&em);
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)(code_base + bc_off));
                a_str_off(&em, 0, X_FRAME, FRAME_OFF_IP);
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                /* CBNZ w0, err */
                size_t j_err = em.pos;
                emit_u32(&em, 0x35000000u | 0u);
                ADD_ERR_EXIT(j_err);
                if (in->op == IR_CMP_BR) {
                    /* Slow path: interpreter step produced a bool on
                     * the VM stack. Pop and branch based on truthiness. */
                    int packed = (int)in->call_args[0];
                    int branch_if_false = packed & 1;
                    emit_vmsp_pop_x0(&em);
                    /* Call value_truthy(x0). Returns bool in w0. */
                    a_call_abs(&em, (void *)(uintptr_t)value_truthy);
                    /* CBZ/CBNZ w0, target */
                    if (branch_if_false) {
                        size_t p = em.pos;
                        emit_u32(&em, 0x34000000u | 0u); /* CBZ w0 */
                        ADD_FIXUP(p, in->imm);
                    } else {
                        size_t p = em.pos;
                        emit_u32(&em, 0x35000000u | 0u); /* CBNZ w0 */
                        ADD_FIXUP(p, in->imm);
                    }
                } else if (in->dst >= 0) {
                    emit_vmsp_pop_x0(&em);
                    emit_store_vreg(&em, in->dst, a, 0);
                }
                break;
            }

            case IR_NEG:
            case IR_NOT:
            case IR_BNOT: {
                emit_load_vreg(&em, in->src1, a, 0);
                emit_vmsp_push_x0(&em);
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)(code_base + bc_off));
                a_str_off(&em, 0, X_FRAME, FRAME_OFF_IP);
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                size_t j_err = em.pos;
                emit_u32(&em, 0x35000000u | 0u);
                ADD_ERR_EXIT(j_err);
                emit_vmsp_pop_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                break;
            }

            case IR_JUMP: {
                size_t p = a_b_ph(&em);
                ADD_FIXUP(p, in->imm);
                break;
            }
            case IR_JIF_FALSE:
            case IR_JIF_TRUE: {
                emit_load_vreg(&em, in->src1, a, 0);
                a_call_abs(&em, (void *)(uintptr_t)value_truthy);
                if (in->op == IR_JIF_FALSE) {
                    size_t p = em.pos;
                    emit_u32(&em, 0x34000000u | 0u); /* CBZ w0 */
                    ADD_FIXUP(p, in->imm);
                } else {
                    size_t p = em.pos;
                    emit_u32(&em, 0x35000000u | 0u); /* CBNZ w0 */
                    ADD_FIXUP(p, in->imm);
                }
                break;
            }

            case IR_RETURN: {
                /* Writeback written locals, flush result onto vm->sp,
                 * call vm_return_fast, then exit via ok_exit. */
                for (int si = 0; si < f->n_locals; si++) {
                    if (!(f->local_written && f->local_written[si])) continue;
                    IRVReg lv = f->local_vregs[si];
                    a_ldr_off(&em, 1, X_FRAME, FRAME_OFF_BASE);
                    a_ldr_off(&em, 0, 1, si * 8);
                    emit_load_vreg(&em, lv, a, 2);
                    a_str_off(&em, 2, 1, si * 8);
                    /* if (old) decref(old) */
                    size_t cbz = em.pos;
                    emit_u32(&em, 0xB4000000u | 0u);
                    a_call_abs(&em, (void *)(uintptr_t)value_decref);
                    {
                        int64_t diff = ((int64_t)em.pos - (int64_t)cbz) / 4;
                        uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5);
                        em.buf[cbz] = (uint8_t)w;
                        em.buf[cbz + 1] = (uint8_t)(w >> 8);
                        em.buf[cbz + 2] = (uint8_t)(w >> 16);
                        em.buf[cbz + 3] = (uint8_t)(w >> 24);
                    }
                }
                emit_load_vreg(&em, in->src1, a, 0);
                emit_vmsp_push_x0(&em);
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)(code_base + bc_off));
                a_str_off(&em, 0, X_FRAME, FRAME_OFF_IP);
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_return_fast);
                /* CBNZ w0, slow */
                size_t j_slow = em.pos;
                emit_u32(&em, 0x35000000u | 0u);
                size_t p_ok = a_b_ph(&em);
                ADD_OK_EXIT(p_ok);
                patch_bcond(&em, j_slow, em.pos);  /* CBNZ uses same offset shape */
                /* Slow: step through vm_step_jit to handle return. */
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                size_t p_ex = a_b_ph(&em);
                ADD_OK_EXIT(p_ex);
                break;
            }

            case IR_CALL: {
                /* Mirror the x86 path: flush callee + args to vm->sp,
                 * stash baseline frame_count in our scratch word, then
                 * try vm_call_closure_fast. On success drive any pushed
                 * inner frame to completion via tier2_run_until. On
                 * failure fall back to vm_step_jit on OP_CALL (handles
                 * natives, overloads, variadics). */
                int argc = in->imm;
                emit_load_vreg(&em, in->src1, a, 0);
                emit_vmsp_push_x0(&em);
                for (int k = 0; k < argc; k++) {
                    emit_load_vreg(&em, in->call_args[k], a, 0);
                    emit_vmsp_push_x0(&em);
                }
                /* w0 = vm->frame_count ; store to [x29 + RC_STASH_DISP] */
                a_ldr_w_off(&em, 0, X_VM, VM_OFF_FRAME_COUNT);
                emit_u32(&em, 0xB9000000u | ((uint32_t)(RC_STASH_DISP / 4) << 10) |
                               ((uint32_t)29u << 5) | 0u);
                /* frame->ip = code_base + bc_off. */
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)(code_base + bc_off));
                a_str_off(&em, 0, X_FRAME, FRAME_OFF_IP);
                /* vm_call_closure_fast(vm, argc). */
                a_mov_reg(&em, 0, X_VM);
                a_mov_imm64(&em, 1, (uint64_t)(uint32_t)argc);
                a_call_abs(&em, (void *)(uintptr_t)vm_call_closure_fast);
                /* CBNZ w0, .slow */
                size_t j_slow = em.pos;
                emit_u32(&em, 0x35000000u | 0u);

                /* Fast path: if a residual frame sits above our baseline
                 * (callee had no jit_entry and fast-path only pushed),
                 * drain it via tier2_run_until. */
                {
                    /* w1 = baseline ; w2 = current fc */
                    emit_u32(&em, 0xB9400000u | ((uint32_t)(RC_STASH_DISP / 4) << 10) |
                                   ((uint32_t)29u << 5) | 1u);
                    a_ldr_w_off(&em, 2, X_VM, VM_OFF_FRAME_COUNT);
                    /* CMP w1, w2  (SUBS wzr, w1, w2) */
                    emit_u32(&em, 0x6B00001Fu | ((uint32_t)2u << 16) |
                                   ((uint32_t)1u << 5));
                    size_t j_nd = a_bcond_ph(&em, CC_GE);
                    a_mov_reg(&em, 0, X_VM);
                    emit_u32(&em, 0xB9400000u | ((uint32_t)(RC_STASH_DISP / 4) << 10) |
                                   ((uint32_t)29u << 5) | 1u);
                    a_call_abs(&em, (void *)(uintptr_t)tier2_run_until);
                    size_t j_err1 = em.pos;
                    emit_u32(&em, 0x35000000u | 0u);
                    ADD_ERR_EXIT(j_err1);
                    patch_bcond(&em, j_nd, em.pos);
                }
                emit_refresh_frame(&em);
                emit_vmsp_pop_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);
                size_t j_join = a_b_ph(&em);

                /* Slow path: full OP_CALL via vm_step_jit. */
                patch_bcond(&em, j_slow, em.pos);
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                size_t j_err2 = em.pos;
                emit_u32(&em, 0x35000000u | 0u);
                ADD_ERR_EXIT(j_err2);
                {
                    emit_u32(&em, 0xB9400000u | ((uint32_t)(RC_STASH_DISP / 4) << 10) |
                                   ((uint32_t)29u << 5) | 1u);
                    a_ldr_w_off(&em, 2, X_VM, VM_OFF_FRAME_COUNT);
                    emit_u32(&em, 0x6B00001Fu | ((uint32_t)2u << 16) |
                                   ((uint32_t)1u << 5));
                    size_t j_nd2 = a_bcond_ph(&em, CC_GE);
                    a_mov_reg(&em, 0, X_VM);
                    emit_u32(&em, 0xB9400000u | ((uint32_t)(RC_STASH_DISP / 4) << 10) |
                                   ((uint32_t)29u << 5) | 1u);
                    a_call_abs(&em, (void *)(uintptr_t)tier2_run_until);
                    size_t j_err3 = em.pos;
                    emit_u32(&em, 0x35000000u | 0u);
                    ADD_ERR_EXIT(j_err3);
                    patch_bcond(&em, j_nd2, em.pos);
                }
                emit_refresh_frame(&em);
                emit_vmsp_pop_x0(&em);
                emit_store_vreg(&em, in->dst, a, 0);

                patch_b(&em, j_join, em.pos);
                break;
            }

            /* Closure construction / container ops / generic dispatch —
             * all take the flush-and-step path. No frame is pushed by
             * these ops, so popping a result after vm_step_jit returns
             * is safe. */
            case IR_MAKE_CLOSURE:
            case IR_INDEX_GET:
            case IR_INDEX_SET:
            case IR_LOAD_FIELD:
            case IR_STORE_FIELD:
            case IR_MAKE_RANGE:
            case IR_MAKE_ARRAY:
            case IR_MAKE_TUPLE:
            case IR_MAKE_MAP:
            case IR_METHOD_CALL:
            case IR_VM_STEP:
            case IR_VM_STEP_CF: {
                if (in->src1 >= 0) {
                    emit_load_vreg(&em, in->src1, a, 0);
                    emit_vmsp_push_x0(&em);
                }
                if (in->src2 >= 0) {
                    emit_load_vreg(&em, in->src2, a, 0);
                    emit_vmsp_push_x0(&em);
                }
                for (int k = 0; k < IR_MAX_CALL_ARGS; k++) {
                    if (in->call_args[k] < 0) break;
                    emit_load_vreg(&em, in->call_args[k], a, 0);
                    emit_vmsp_push_x0(&em);
                }
                a_mov_imm64(&em, 0, (uint64_t)(uintptr_t)(code_base + bc_off));
                a_str_off(&em, 0, X_FRAME, FRAME_OFF_IP);
                a_mov_reg(&em, 0, X_VM);
                a_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
                size_t j_err = em.pos;
                emit_u32(&em, 0x35000000u | 0u);
                ADD_ERR_EXIT(j_err);
                if (in->dst >= 0) {
                    emit_vmsp_pop_x0(&em);
                    emit_store_vreg(&em, in->dst, a, 0);
                }
                if (in->op == IR_VM_STEP_CF) {
                    size_t j_deopt = a_b_ph(&em);
                    ADD_DEOPT_EXIT(j_deopt);
                }
                /* Refresh x20 since a nested call may have grown
                 * vm->frames and invalidated our cached pointer. */
                emit_refresh_frame(&em);
                break;
            }

            default:
                /* Unsupported -- bail out of arm64 tier-2. */
                free(block_pos); free(fixups);
                free(err_exit_patches); free(ok_exit_patches); free(deopt_exit_patches);
                return NULL;
            }

            if (em.overflow) {
                free(block_pos); free(fixups);
                free(err_exit_patches); free(ok_exit_patches); free(deopt_exit_patches);
                return NULL;
            }
        }
    }

    /* --- Exit epilogues --- */
    size_t err_exit_pos = em.pos;
    for (int si = 0; si < f->n_locals; si++) {
        if (!(f->local_written && f->local_written[si])) continue;
        IRVReg lv = f->local_vregs[si];
        emit_load_vreg(&em, lv, a, 0);
        size_t cbz = em.pos;
        emit_u32(&em, 0xB4000000u | 0u);
        a_call_abs(&em, (void *)(uintptr_t)value_decref);
        {
            int64_t diff = ((int64_t)em.pos - (int64_t)cbz) / 4;
            uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5);
            em.buf[cbz] = (uint8_t)w;
            em.buf[cbz + 1] = (uint8_t)(w >> 8);
            em.buf[cbz + 2] = (uint8_t)(w >> 16);
            em.buf[cbz + 3] = (uint8_t)(w >> 24);
        }
    }

    size_t deopt_exit_pos = em.pos;
    for (int si = 0; si < f->n_locals; si++) {
        if (!(f->local_written && f->local_written[si])) continue;
        IRVReg lv = f->local_vregs[si];
        a_ldr_off(&em, 1, X_FRAME, FRAME_OFF_BASE);
        a_ldr_off(&em, 0, 1, si * 8);
        emit_load_vreg(&em, lv, a, 2);
        a_str_off(&em, 2, 1, si * 8);
        size_t cbz = em.pos;
        emit_u32(&em, 0xB4000000u | 0u);
        a_call_abs(&em, (void *)(uintptr_t)value_decref);
        {
            int64_t diff = ((int64_t)em.pos - (int64_t)cbz) / 4;
            uint32_t w = 0xB4000000u | (((uint32_t)(diff & 0x7FFFF)) << 5);
            em.buf[cbz] = (uint8_t)w;
            em.buf[cbz + 1] = (uint8_t)(w >> 8);
            em.buf[cbz + 2] = (uint8_t)(w >> 16);
            em.buf[cbz + 3] = (uint8_t)(w >> 24);
        }
    }
    /* MOV w0, #0 ; restore; ret
     * (err_exit flows into deopt_exit writeback then here, returning
     * 0 -- the vm state carries the actual error via vm->error.) */
    a_mov_imm64(&em, 0, 0);
    /* LDP x23, x24, [sp, #48] */
    emit_u32(&em, 0xA9400000u | 23u | (24u << 10) | (31u << 5) | ((uint32_t)(48/8) << 15));
    emit_u32(&em, 0xA9400000u | 21u | (22u << 10) | (31u << 5) | ((uint32_t)(32/8) << 15));
    emit_u32(&em, 0xA9400000u | 19u | (20u << 10) | (31u << 5) | ((uint32_t)(16/8) << 15));
    emit_u32(&em, 0xA9400000u | 29u | (30u << 10) | (31u << 5));
    /* ADD sp, sp, #sub_amt */
    emit_u32(&em, 0x910003FFu | ((uint32_t)(sub_amt & 0xFFFu) << 10));
    a_ret(&em);

    size_t ok_exit_pos = em.pos;
    /* MOV w1, w0 ; MOV w0, #0 ; CMP w1, #0 ; CSET w0, LT.  Matches x86:
     * negative rc -> 1, zero/positive -> 0. */
    a_mov_reg(&em, 1, 0);
    a_mov_imm64(&em, 0, 0);
    a_cmp_imm(&em, 1, 0);
    a_cset(&em, 0, CC_LT);
    emit_u32(&em, 0xA9400000u | 23u | (24u << 10) | (31u << 5) | ((uint32_t)(48/8) << 15));
    emit_u32(&em, 0xA9400000u | 21u | (22u << 10) | (31u << 5) | ((uint32_t)(32/8) << 15));
    emit_u32(&em, 0xA9400000u | 19u | (20u << 10) | (31u << 5) | ((uint32_t)(16/8) << 15));
    emit_u32(&em, 0xA9400000u | 29u | (30u << 10) | (31u << 5));
    emit_u32(&em, 0x910003FFu | ((uint32_t)(sub_amt & 0xFFFu) << 10));
    a_ret(&em);

    for (int i = 0; i < n_fixups; i++) {
        int tb = fixups[i].target_block;
        if (tb < 0 || tb >= f->n_blocks || block_pos[tb] == (size_t)-1) {
            free(block_pos); free(fixups);
            free(err_exit_patches); free(ok_exit_patches); free(deopt_exit_patches);
            return NULL;
        }
        /* fixups[i].patch holds the instruction address; distinguish B
         * from B.cond / CBZ / CBNZ by opcode bits we just wrote. */
        uint32_t w = (uint32_t)em.buf[fixups[i].patch]
                   | ((uint32_t)em.buf[fixups[i].patch + 1] << 8)
                   | ((uint32_t)em.buf[fixups[i].patch + 2] << 16)
                   | ((uint32_t)em.buf[fixups[i].patch + 3] << 24);
        if ((w & 0xFC000000u) == 0x14000000u) {
            patch_b(&em, fixups[i].patch, block_pos[tb]);
        } else {
            patch_bcond(&em, fixups[i].patch, block_pos[tb]);
        }
    }
    for (int i = 0; i < n_err; i++)   patch_bcond(&em, err_exit_patches[i], err_exit_pos);
    for (int i = 0; i < n_ok; i++)    patch_b(&em, ok_exit_patches[i], ok_exit_pos);
    for (int i = 0; i < n_deopt; i++) patch_b(&em, deopt_exit_patches[i], deopt_exit_pos);

    free(block_pos); free(fixups);
    free(err_exit_patches); free(ok_exit_patches); free(deopt_exit_patches);

    if (em.overflow) return NULL;
    j->code_used = em.pos;
    /* Flush icache / dcache for the emitted region so the CPU sees it. */
    __builtin___clear_cache((char *)entry, (char *)(j->code + em.pos));
    return entry;
}

#endif /* RA_ARM64_HAS_MMAP */

#endif /* __aarch64__ */
