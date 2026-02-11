/* arm64.c - ARM64/AArch64 instruction encoder and code generator for XS JIT.
 *
 * Encodes all major ARM64 instruction classes: data processing, memory,
 * branching, floating-point, and SIMD. Includes a linear-scan register
 * allocator, function prologue/epilogue generation, and compilation
 * from XS bytecode to native ARM64 machine code.
 *
 * Only active when XSC_ENABLE_JIT is defined; guarded by JIT_ARM64
 * which is set on aarch64 targets.
 */

#ifdef XSC_ENABLE_JIT

#include "jit/arm64.h"
#include "core/xs.h"
#include "core/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Code buffer management
 * ---------------------------------------------------------------- */

void arm64_init(ARM64Code *c, int initial_cap) {
    c->code = xs_malloc(initial_cap);
    c->len = 0;
    c->cap = initial_cap;
    c->overflow = 0;
}

void arm64_free(ARM64Code *c) {
    free(c->code);
    c->code = NULL;
    c->len = 0;
    c->cap = 0;
}

void arm64_emit32(ARM64Code *c, uint32_t instr) {
    if (c->len + 4 > c->cap) {
        if (c->cap > (1 << 24)) {
            c->overflow = 1;
            return;
        }
        c->cap *= 2;
        c->code = xs_realloc(c->code, c->cap);
    }
    c->code[c->len + 0] = (uint8_t)(instr & 0xff);
    c->code[c->len + 1] = (uint8_t)((instr >> 8) & 0xff);
    c->code[c->len + 2] = (uint8_t)((instr >> 16) & 0xff);
    c->code[c->len + 3] = (uint8_t)((instr >> 24) & 0xff);
    c->len += 4;
}

/* ----------------------------------------------------------------
 * Data Processing - Immediate
 * ---------------------------------------------------------------- */

/* MOVZ: move wide with zero (16-bit immediate at shift position) */
void arm64_movz(ARM64Code *c, int rd, uint16_t imm, int shift) {
    /* sf=1, opc=10, hw=shift/16 */
    int hw = shift / 16;
    uint32_t enc = (1u << 31) | (0x2u << 29) | (0x25u << 23)
                 | ((uint32_t)hw << 21) | ((uint32_t)imm << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MOVK: move wide with keep */
void arm64_movk(ARM64Code *c, int rd, uint16_t imm, int shift) {
    int hw = shift / 16;
    uint32_t enc = (1u << 31) | (0x3u << 29) | (0x25u << 23)
                 | ((uint32_t)hw << 21) | ((uint32_t)imm << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MOVN: move wide with NOT */
void arm64_movn(ARM64Code *c, int rd, uint16_t imm, int shift) {
    int hw = shift / 16;
    uint32_t enc = (1u << 31) | (0x0u << 29) | (0x25u << 23)
                 | ((uint32_t)hw << 21) | ((uint32_t)imm << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* load arbitrary 64-bit immediate into register */
void arm64_mov_imm(ARM64Code *c, int rd, int64_t imm) {
    uint64_t uimm = (uint64_t)imm;

    /* small non-negative: single MOVZ */
    if (uimm <= 0xffff) {
        arm64_movz(c, rd, (uint16_t)uimm, 0);
        return;
    }

    /* small negative: MOVN */
    if (imm < 0 && imm >= -0x10000) {
        arm64_movn(c, rd, (uint16_t)(~uimm & 0xffff), 0);
        return;
    }

    /* fits in 32 bits */
    if (uimm <= 0xffffffff) {
        arm64_movz(c, rd, (uint16_t)(uimm & 0xffff), 0);
        if (uimm >> 16)
            arm64_movk(c, rd, (uint16_t)((uimm >> 16) & 0xffff), 16);
        return;
    }

    /* full 64-bit */
    arm64_movz(c, rd, (uint16_t)(uimm & 0xffff), 0);
    if ((uimm >> 16) & 0xffff)
        arm64_movk(c, rd, (uint16_t)((uimm >> 16) & 0xffff), 16);
    if ((uimm >> 32) & 0xffff)
        arm64_movk(c, rd, (uint16_t)((uimm >> 32) & 0xffff), 32);
    if ((uimm >> 48) & 0xffff)
        arm64_movk(c, rd, (uint16_t)((uimm >> 48) & 0xffff), 48);
}

/* MOV reg, reg (encoded as ORR rd, xzr, rm) */
void arm64_mov_reg(ARM64Code *c, int rd, int rs) {
    if (rd == rs) return;
    /* ORR (shifted register) X: sf=1, opc=01, shift=00, N=0, imm6=0 */
    uint32_t enc = (1u << 31) | (0x15u << 24)
                 | ((uint32_t)(rs & 0x1f) << 16)
                 | ((uint32_t)(A64_XZR & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ADD immediate (64-bit) */
void arm64_add_imm(ARM64Code *c, int rd, int rn, int imm) {
    int sh = 0;
    int uimm = imm;
    if (uimm < 0) {
        arm64_sub_imm(c, rd, rn, -uimm);
        return;
    }
    if (uimm > 0xfff && (uimm & 0xfff) == 0 && (uimm >> 12) <= 0xfff) {
        sh = 1;
        uimm >>= 12;
    }
    uint32_t enc = (1u << 31) | (0x11u << 24) | ((uint32_t)sh << 22)
                 | ((uint32_t)(uimm & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ADD register (64-bit) */
void arm64_add_reg(ARM64Code *c, int rd, int rn, int rm) {
    /* sf=1, op=0, S=0, shift=00, imm6=0 */
    uint32_t enc = (1u << 31) | (0x0Bu << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* SUB immediate */
void arm64_sub_imm(ARM64Code *c, int rd, int rn, int imm) {
    int sh = 0;
    int uimm = imm;
    if (uimm < 0) {
        arm64_add_imm(c, rd, rn, -uimm);
        return;
    }
    if (uimm > 0xfff && (uimm & 0xfff) == 0 && (uimm >> 12) <= 0xfff) {
        sh = 1;
        uimm >>= 12;
    }
    uint32_t enc = (1u << 31) | (0x51u << 24) | ((uint32_t)sh << 22)
                 | ((uint32_t)(uimm & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* SUB register */
void arm64_sub_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x4Bu << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MUL (encoded as MADD rd, rn, rm, xzr) */
void arm64_mul(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD8u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(A64_XZR & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* SDIV */
void arm64_sdiv(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD6u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x3u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* UDIV */
void arm64_udiv(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD6u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x2u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MSUB rd, rn, rm, ra: rd = ra - rn*rm */
void arm64_msub(ARM64Code *c, int rd, int rn, int rm, int ra) {
    uint32_t enc = (1u << 31) | (0xD8u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (1u << 15)
                 | ((uint32_t)(ra & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MADD rd, rn, rm, ra: rd = ra + rn*rm */
void arm64_madd(ARM64Code *c, int rd, int rn, int rm, int ra) {
    uint32_t enc = (1u << 31) | (0xD8u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(ra & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Logical operations
 * ---------------------------------------------------------------- */

/* AND register */
void arm64_and_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x0Au << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ORR register */
void arm64_orr_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x2Au << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* EOR register */
void arm64_eor_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x4Au << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ORN register */
void arm64_orn_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x2Au << 24) | (1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* MVN (encoded as ORN rd, xzr, rm) */
void arm64_mvn(ARM64Code *c, int rd, int rm) {
    arm64_orn_reg(c, rd, A64_XZR, rm);
}

/* ----------------------------------------------------------------
 * Shifts
 * ---------------------------------------------------------------- */

/* LSL immediate (encoded as UBFM) */
void arm64_lsl_imm(ARM64Code *c, int rd, int rn, int shift) {
    int immr = (64 - shift) & 63;
    int imms = 63 - shift;
    uint32_t enc = (1u << 31) | (0x53u << 23) | (1u << 22)
                 | ((uint32_t)(immr & 0x3f) << 16)
                 | ((uint32_t)(imms & 0x3f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* LSR immediate (encoded as UBFM) */
void arm64_lsr_imm(ARM64Code *c, int rd, int rn, int shift) {
    uint32_t enc = (1u << 31) | (0x53u << 23) | (1u << 22)
                 | ((uint32_t)(shift & 0x3f) << 16)
                 | (0x3fu << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ASR immediate (encoded as SBFM) */
void arm64_asr_imm(ARM64Code *c, int rd, int rn, int shift) {
    uint32_t enc = (1u << 31) | (0x13u << 23) | (1u << 22)
                 | ((uint32_t)(shift & 0x3f) << 16)
                 | (0x3fu << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* LSL register (encoded as LSLV) */
void arm64_lsl_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD6u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x8u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* LSR register (encoded as LSRV) */
void arm64_lsr_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD6u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x9u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ASR register (encoded as ASRV) */
void arm64_asr_reg(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0xD6u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0xAu << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Comparisons
 * ---------------------------------------------------------------- */

/* CMP immediate (encoded as SUBS xzr, rn, #imm) */
void arm64_cmp_imm(ARM64Code *c, int rn, int imm) {
    int sh = 0;
    int uimm = imm;
    if (uimm > 0xfff && (uimm & 0xfff) == 0 && (uimm >> 12) <= 0xfff) {
        sh = 1;
        uimm >>= 12;
    }
    uint32_t enc = (1u << 31) | (0x71u << 24) | ((uint32_t)sh << 22)
                 | ((uint32_t)(uimm & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(A64_XZR & 0x1f);
    arm64_emit32(c, enc);
}

/* CMP register (encoded as SUBS xzr, rn, rm) */
void arm64_cmp_reg(ARM64Code *c, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x6Bu << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(A64_XZR & 0x1f);
    arm64_emit32(c, enc);
}

/* CMN immediate (encoded as ADDS xzr, rn, #imm) */
void arm64_cmn_imm(ARM64Code *c, int rn, int imm) {
    uint32_t enc = (1u << 31) | (0x31u << 24)
                 | ((uint32_t)(imm & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(A64_XZR & 0x1f);
    arm64_emit32(c, enc);
}

/* TST register (encoded as ANDS xzr, rn, rm) */
void arm64_tst_reg(ARM64Code *c, int rn, int rm) {
    uint32_t enc = (1u << 31) | (0x6Au << 24)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(A64_XZR & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Conditional select
 * ---------------------------------------------------------------- */

/* CSEL rd, rn, rm, cond */
void arm64_csel(ARM64Code *c, int rd, int rn, int rm, int cond) {
    uint32_t enc = (1u << 31) | (0xD4u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | ((uint32_t)(cond & 0xf) << 12)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* CSET rd, cond (encoded as CSINC rd, xzr, xzr, invert(cond)) */
void arm64_cset(ARM64Code *c, int rd, int cond) {
    int inv = cond ^ 1;
    uint32_t enc = (1u << 31) | (0xD4u << 21)
                 | ((uint32_t)(A64_XZR & 0x1f) << 16)
                 | ((uint32_t)(inv & 0xf) << 12)
                 | (1u << 10)
                 | ((uint32_t)(A64_XZR & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Branch instructions
 * ---------------------------------------------------------------- */

/* B (unconditional, PC-relative) offset is in bytes, must be 4-aligned */
void arm64_b(ARM64Code *c, int32_t offset) {
    int32_t imm26 = offset >> 2;
    uint32_t enc = (0x5u << 26) | ((uint32_t)imm26 & 0x03ffffff);
    arm64_emit32(c, enc);
}

/* B.cond (conditional branch) */
void arm64_b_cond(ARM64Code *c, int cond, int32_t offset) {
    int32_t imm19 = offset >> 2;
    uint32_t enc = (0x54u << 24) | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(cond & 0xf);
    arm64_emit32(c, enc);
}

/* BL (branch with link) */
void arm64_bl(ARM64Code *c, int32_t offset) {
    int32_t imm26 = offset >> 2;
    uint32_t enc = (0x25u << 26) | ((uint32_t)imm26 & 0x03ffffff);
    arm64_emit32(c, enc);
}

/* RET (return, default x30) */
void arm64_ret(ARM64Code *c) {
    uint32_t enc = (0xD65F0000u) | ((uint32_t)(A64_LR & 0x1f) << 5);
    arm64_emit32(c, enc);
}

/* BR rn (branch to register) */
void arm64_br(ARM64Code *c, int rn) {
    uint32_t enc = 0xD61F0000u | ((uint32_t)(rn & 0x1f) << 5);
    arm64_emit32(c, enc);
}

/* BLR rn (branch with link to register) */
void arm64_blr(ARM64Code *c, int rn) {
    uint32_t enc = 0xD63F0000u | ((uint32_t)(rn & 0x1f) << 5);
    arm64_emit32(c, enc);
}

/* CBZ: compare and branch on zero */
void arm64_cbz(ARM64Code *c, int rt, int32_t offset) {
    int32_t imm19 = offset >> 2;
    uint32_t enc = (1u << 31) | (0x34u << 24)
                 | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* CBNZ: compare and branch on non-zero */
void arm64_cbnz(ARM64Code *c, int rt, int32_t offset) {
    int32_t imm19 = offset >> 2;
    uint32_t enc = (1u << 31) | (0x35u << 24)
                 | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* TBZ: test bit and branch on zero */
void arm64_tbz(ARM64Code *c, int rt, int bit, int32_t offset) {
    int b5 = (bit >> 5) & 1;
    int b40 = bit & 0x1f;
    int32_t imm14 = offset >> 2;
    uint32_t enc = ((uint32_t)b5 << 31) | (0x36u << 24)
                 | ((uint32_t)b40 << 19)
                 | (((uint32_t)imm14 & 0x3fff) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* TBNZ: test bit and branch on non-zero */
void arm64_tbnz(ARM64Code *c, int rt, int bit, int32_t offset) {
    int b5 = (bit >> 5) & 1;
    int b40 = bit & 0x1f;
    int32_t imm14 = offset >> 2;
    uint32_t enc = ((uint32_t)b5 << 31) | (0x37u << 24)
                 | ((uint32_t)b40 << 19)
                 | (((uint32_t)imm14 & 0x3fff) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* ADR: form PC-relative address */
void arm64_adr(ARM64Code *c, int rd, int32_t offset) {
    uint32_t immlo = (uint32_t)(offset & 0x3);
    uint32_t immhi = (uint32_t)((offset >> 2) & 0x7ffff);
    uint32_t enc = (immlo << 29) | (0x10u << 24) | (immhi << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Memory: load/store with unsigned offset
 * ---------------------------------------------------------------- */

/* LDR x (64-bit, unsigned offset) */
void arm64_ldr(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 8;
    uint32_t enc = (0xF9u << 24) | (0x1u << 22)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STR x (64-bit, unsigned offset) */
void arm64_str(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 8;
    uint32_t enc = (0xF9u << 24)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* LDR w (32-bit, unsigned offset) */
void arm64_ldr_w(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 4;
    uint32_t enc = (0xB9u << 24) | (0x1u << 22)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STR w (32-bit, unsigned offset) */
void arm64_str_w(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 4;
    uint32_t enc = (0xB9u << 24)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* LDRB (byte load, unsigned offset) */
void arm64_ldrb(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0x39u << 24) | (0x1u << 22)
                 | ((uint32_t)(offset & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STRB (byte store, unsigned offset) */
void arm64_strb(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0x39u << 24)
                 | ((uint32_t)(offset & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* LDRH (halfword load, unsigned offset) */
void arm64_ldrh(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 2;
    uint32_t enc = (0x79u << 24) | (0x1u << 22)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STRH (halfword store, unsigned offset) */
void arm64_strh(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 2;
    uint32_t enc = (0x79u << 24)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Memory: pre/post indexed
 * ---------------------------------------------------------------- */

/* LDR pre-indexed: ldr rt, [rn, #offset]! */
void arm64_ldr_pre(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0xF8u << 24) | (0x1u << 22)
                 | (((uint32_t)offset & 0x1ff) << 12)
                 | (0x3u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STR pre-indexed: str rt, [rn, #offset]! */
void arm64_str_pre(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0xF8u << 24)
                 | (((uint32_t)offset & 0x1ff) << 12)
                 | (0x3u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* LDR post-indexed: ldr rt, [rn], #offset */
void arm64_ldr_post(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0xF8u << 24) | (0x1u << 22)
                 | (((uint32_t)offset & 0x1ff) << 12)
                 | (0x1u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STR post-indexed: str rt, [rn], #offset */
void arm64_str_post(ARM64Code *c, int rt, int rn, int offset) {
    uint32_t enc = (0xF8u << 24)
                 | (((uint32_t)offset & 0x1ff) << 12)
                 | (0x1u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Memory: load/store pair
 * ---------------------------------------------------------------- */

/* LDP (signed offset) */
void arm64_ldp(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA9u << 24) | (0x1u << 22)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* STP (signed offset) */
void arm64_stp(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA9u << 24)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* LDP pre-indexed */
void arm64_ldp_pre(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA9u << 24) | (0x3u << 22)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* STP pre-indexed */
void arm64_stp_pre(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA9u << 24) | (0x2u << 22)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* LDP post-indexed */
void arm64_ldp_post(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA8u << 24) | (0x3u << 22)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* STP post-indexed */
void arm64_stp_post(ARM64Code *c, int rt1, int rt2, int rn, int offset) {
    int simm7 = offset / 8;
    uint32_t enc = (0xA8u << 24) | (0x2u << 22)
                 | (((uint32_t)simm7 & 0x7f) << 15)
                 | ((uint32_t)(rt2 & 0x1f) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt1 & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Stack helpers
 * ---------------------------------------------------------------- */

void arm64_push(ARM64Code *c, int rt1, int rt2) {
    arm64_stp_pre(c, rt1, rt2, A64_SP, -16);
}

void arm64_pop(ARM64Code *c, int rt1, int rt2) {
    arm64_ldp_post(c, rt1, rt2, A64_SP, 16);
}

void arm64_push1(ARM64Code *c, int rt) {
    arm64_str_pre(c, rt, A64_SP, -16);
}

void arm64_pop1(ARM64Code *c, int rt) {
    arm64_ldr_post(c, rt, A64_SP, 16);
}

/* ----------------------------------------------------------------
 * PC-relative literal load
 * ---------------------------------------------------------------- */

void arm64_ldr_literal(ARM64Code *c, int rt, int32_t offset) {
    int32_t imm19 = offset >> 2;
    uint32_t enc = (0x58u << 24) | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Floating point - double precision
 * ---------------------------------------------------------------- */

/* FMOV d (register to register) */
void arm64_fmov_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x10u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FADD d */
void arm64_fadd_d(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x0Au << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FSUB d */
void arm64_fsub_d(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x0Eu << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FMUL d */
void arm64_fmul_d(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x02u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FDIV d */
void arm64_fdiv_d(ARM64Code *c, int rd, int rn, int rm) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x06u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FNEG d */
void arm64_fneg_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x21u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FABS d */
void arm64_fabs_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x20u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FSQRT d */
void arm64_fsqrt_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x23u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FCMP d (sets NZCV flags) */
void arm64_fcmp_d(ARM64Code *c, int rn, int rm) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | ((uint32_t)(rm & 0x1f) << 16)
                 | (0x08u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5);
    arm64_emit32(c, enc);
}

/* FCMP d, #0.0 */
void arm64_fcmp_zero_d(ARM64Code *c, int rn) {
    uint32_t enc = (0x1Eu << 24) | (0x1u << 22) | (0x1u << 21)
                 | (0x08u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | 0x8u;
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Conversions
 * ---------------------------------------------------------------- */

/* SCVTF d, x (int64 -> f64) */
void arm64_scvtf_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (1u << 31) | (0x1Eu << 24) | (0x1u << 22)
                 | (0x1u << 21) | (0x02u << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* UCVTF d, x (uint64 -> f64) */
void arm64_ucvtf_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (1u << 31) | (0x1Eu << 24) | (0x1u << 22)
                 | (0x1u << 21) | (0x03u << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FCVTZS x, d (f64 -> int64, truncate toward zero) */
void arm64_fcvtzs_d(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (1u << 31) | (0x1Eu << 24) | (0x1u << 22)
                 | (0x3u << 20) | (0x0u << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FMOV x, d (fp reg bits -> gp reg) */
void arm64_fmov_to_gp(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (1u << 31) | (0x1Eu << 24) | (0x1u << 22)
                 | (0x1u << 21) | (0x06u << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* FMOV d, x (gp reg -> fp reg) */
void arm64_fmov_from_gp(ARM64Code *c, int rd, int rn) {
    uint32_t enc = (1u << 31) | (0x1Eu << 24) | (0x1u << 22)
                 | (0x1u << 21) | (0x07u << 16)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * FP load/store
 * ---------------------------------------------------------------- */

/* LDR d (FP 64-bit, unsigned offset) */
void arm64_ldr_d(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 8;
    uint32_t enc = (0xFDu << 24) | (0x1u << 22)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* STR d (FP 64-bit, unsigned offset) */
void arm64_str_d(ARM64Code *c, int rt, int rn, int offset) {
    int uoff = offset / 8;
    uint32_t enc = (0xFDu << 24)
                 | ((uint32_t)(uoff & 0xfff) << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(rt & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * NOP and system
 * ---------------------------------------------------------------- */

void arm64_nop(ARM64Code *c) {
    arm64_emit32(c, 0xD503201F);
}

void arm64_brk(ARM64Code *c, uint16_t imm) {
    uint32_t enc = (0xD4u << 24) | (0x2u << 21)
                 | ((uint32_t)imm << 5) | 0x0u;
    arm64_emit32(c, enc);
}

void arm64_svc(ARM64Code *c, uint16_t imm) {
    uint32_t enc = (0xD4u << 24) | ((uint32_t)imm << 5) | 0x1u;
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Patch helpers
 * ---------------------------------------------------------------- */

void arm64_patch_b(ARM64Code *c, int patch_offset, int target_offset) {
    int32_t disp = target_offset - patch_offset;
    int32_t imm26 = disp >> 2;
    uint32_t enc = (0x5u << 26) | ((uint32_t)imm26 & 0x03ffffff);
    c->code[patch_offset + 0] = (uint8_t)(enc & 0xff);
    c->code[patch_offset + 1] = (uint8_t)((enc >> 8) & 0xff);
    c->code[patch_offset + 2] = (uint8_t)((enc >> 16) & 0xff);
    c->code[patch_offset + 3] = (uint8_t)((enc >> 24) & 0xff);
}

void arm64_patch_bcond(ARM64Code *c, int patch_offset, int target_offset) {
    int32_t disp = target_offset - patch_offset;
    int32_t imm19 = disp >> 2;
    /* read original cond from the instruction */
    uint32_t orig = (uint32_t)c->code[patch_offset]
                  | ((uint32_t)c->code[patch_offset + 1] << 8)
                  | ((uint32_t)c->code[patch_offset + 2] << 16)
                  | ((uint32_t)c->code[patch_offset + 3] << 24);
    int cond = orig & 0xf;
    uint32_t enc = (0x54u << 24) | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(cond & 0xf);
    c->code[patch_offset + 0] = (uint8_t)(enc & 0xff);
    c->code[patch_offset + 1] = (uint8_t)((enc >> 8) & 0xff);
    c->code[patch_offset + 2] = (uint8_t)((enc >> 16) & 0xff);
    c->code[patch_offset + 3] = (uint8_t)((enc >> 24) & 0xff);
}

void arm64_patch_cbz(ARM64Code *c, int patch_offset, int target_offset) {
    int32_t disp = target_offset - patch_offset;
    int32_t imm19 = disp >> 2;
    uint32_t orig = (uint32_t)c->code[patch_offset]
                  | ((uint32_t)c->code[patch_offset + 1] << 8)
                  | ((uint32_t)c->code[patch_offset + 2] << 16)
                  | ((uint32_t)c->code[patch_offset + 3] << 24);
    int rt = orig & 0x1f;
    int is_nz = (orig >> 24) & 1;
    uint32_t enc = (1u << 31) | ((0x34u + is_nz) << 24)
                 | (((uint32_t)imm19 & 0x7ffff) << 5)
                 | (uint32_t)(rt & 0x1f);
    c->code[patch_offset + 0] = (uint8_t)(enc & 0xff);
    c->code[patch_offset + 1] = (uint8_t)((enc >> 8) & 0xff);
    c->code[patch_offset + 2] = (uint8_t)((enc >> 16) & 0xff);
    c->code[patch_offset + 3] = (uint8_t)((enc >> 24) & 0xff);
}

/* ----------------------------------------------------------------
 * Register allocator (linear scan)
 *
 * Available scratch registers on ARM64:
 *   x0-x7   - argument/result regs (caller-saved)
 *   x8      - indirect result (caller-saved)
 *   x9-x15  - temporary (caller-saved)
 *   x16-x17 - intra-procedure-call scratch
 *   x19-x28 - callee-saved
 *
 * We use x9-x15 as the scratch pool (7 regs), and x19-x22 as
 * callee-saved slots for variables that span calls.
 * ---------------------------------------------------------------- */

#define RA_NSCRATCH     7
#define RA_NCALLEE_SAVED 4
#define RA_MAX_VARS     256
#define RA_STACK_SLOT   8

typedef struct {
    int live_start;
    int live_end;
    int assigned_reg;    /* -1 = spilled */
    int spill_offset;    /* stack offset if spilled */
    int is_float;
} LiveInterval;

typedef struct {
    LiveInterval intervals[RA_MAX_VARS];
    int nintervals;
    int scratch_regs[RA_NSCRATCH];
    int scratch_busy[RA_NSCRATCH];
    int callee_regs[RA_NCALLEE_SAVED];
    int callee_busy[RA_NCALLEE_SAVED];
    int next_spill_offset;
    int max_spill;
} RegAlloc;

static void ra_init(RegAlloc *ra) {
    memset(ra, 0, sizeof(RegAlloc));
    ra->scratch_regs[0] = A64_X9;
    ra->scratch_regs[1] = A64_X10;
    ra->scratch_regs[2] = A64_X11;
    ra->scratch_regs[3] = A64_X12;
    ra->scratch_regs[4] = A64_X13;
    ra->scratch_regs[5] = A64_X14;
    ra->scratch_regs[6] = A64_X15;
    ra->callee_regs[0] = A64_X19;
    ra->callee_regs[1] = A64_X20;
    ra->callee_regs[2] = A64_X21;
    ra->callee_regs[3] = A64_X22;
    ra->next_spill_offset = 0;
    ra->max_spill = 0;
}

static int ra_add_interval(RegAlloc *ra, int start, int end, int is_float) {
    if (ra->nintervals >= RA_MAX_VARS) return -1;
    int idx = ra->nintervals++;
    ra->intervals[idx].live_start = start;
    ra->intervals[idx].live_end = end;
    ra->intervals[idx].assigned_reg = -1;
    ra->intervals[idx].spill_offset = -1;
    ra->intervals[idx].is_float = is_float;
    return idx;
}

static void ra_expire_old(RegAlloc *ra, int current_pos) {
    for (int i = 0; i < RA_NSCRATCH; i++) {
        if (!ra->scratch_busy[i]) continue;
        /* check if the interval using this reg has ended */
        for (int j = 0; j < ra->nintervals; j++) {
            if (ra->intervals[j].assigned_reg == ra->scratch_regs[i] &&
                ra->intervals[j].live_end <= current_pos) {
                ra->scratch_busy[i] = 0;
                ra->intervals[j].assigned_reg = -1;
                break;
            }
        }
    }
    for (int i = 0; i < RA_NCALLEE_SAVED; i++) {
        if (!ra->callee_busy[i]) continue;
        for (int j = 0; j < ra->nintervals; j++) {
            if (ra->intervals[j].assigned_reg == ra->callee_regs[i] &&
                ra->intervals[j].live_end <= current_pos) {
                ra->callee_busy[i] = 0;
                ra->intervals[j].assigned_reg = -1;
                break;
            }
        }
    }
}

static int ra_alloc_reg(RegAlloc *ra, int interval_idx) {
    LiveInterval *li = &ra->intervals[interval_idx];

    ra_expire_old(ra, li->live_start);

    /* try scratch registers first */
    for (int i = 0; i < RA_NSCRATCH; i++) {
        if (!ra->scratch_busy[i]) {
            ra->scratch_busy[i] = 1;
            li->assigned_reg = ra->scratch_regs[i];
            return li->assigned_reg;
        }
    }

    /* try callee-saved */
    for (int i = 0; i < RA_NCALLEE_SAVED; i++) {
        if (!ra->callee_busy[i]) {
            ra->callee_busy[i] = 1;
            li->assigned_reg = ra->callee_regs[i];
            return li->assigned_reg;
        }
    }

    /* spill: assign a stack slot */
    li->spill_offset = ra->next_spill_offset;
    ra->next_spill_offset += RA_STACK_SLOT;
    if (ra->next_spill_offset > ra->max_spill)
        ra->max_spill = ra->next_spill_offset;
    return -1;
}

static void ra_allocate_all(RegAlloc *ra) {
    /* sort intervals by start position (insertion sort, small N) */
    for (int i = 1; i < ra->nintervals; i++) {
        LiveInterval tmp = ra->intervals[i];
        int j = i - 1;
        while (j >= 0 && ra->intervals[j].live_start > tmp.live_start) {
            ra->intervals[j + 1] = ra->intervals[j];
            j--;
        }
        ra->intervals[j + 1] = tmp;
    }

    for (int i = 0; i < ra->nintervals; i++)
        ra_alloc_reg(ra, i);
}

/* ----------------------------------------------------------------
 * Function prologue/epilogue
 * ---------------------------------------------------------------- */

static void emit_prologue(ARM64Code *c, int frame_size, int save_callee) {
    /* save FP and LR */
    arm64_stp_pre(c, A64_FP, A64_LR, A64_SP, -frame_size);
    arm64_mov_reg(c, A64_FP, A64_SP);

    /* save callee-saved registers if needed */
    if (save_callee >= 2)
        arm64_stp(c, A64_X19, A64_X20, A64_SP, 16);
    if (save_callee >= 4)
        arm64_stp(c, A64_X21, A64_X22, A64_SP, 32);
    if (save_callee >= 6)
        arm64_stp(c, A64_X23, A64_X24, A64_SP, 48);
    if (save_callee >= 8)
        arm64_stp(c, A64_X25, A64_X26, A64_SP, 64);
}

static void emit_epilogue(ARM64Code *c, int frame_size, int save_callee) {
    /* restore callee-saved registers */
    if (save_callee >= 8)
        arm64_ldp(c, A64_X25, A64_X26, A64_SP, 64);
    if (save_callee >= 6)
        arm64_ldp(c, A64_X23, A64_X24, A64_SP, 48);
    if (save_callee >= 4)
        arm64_ldp(c, A64_X21, A64_X22, A64_SP, 32);
    if (save_callee >= 2)
        arm64_ldp(c, A64_X19, A64_X20, A64_SP, 16);

    /* restore FP and LR, deallocate frame */
    arm64_ldp_post(c, A64_FP, A64_LR, A64_SP, frame_size);
    arm64_ret(c);
}

/* ----------------------------------------------------------------
 * Compilation from bytecode
 *
 * On ARM64, we use these register assignments:
 *   x19 = XS stack pointer (base of value stack)
 *   x20 = constants array
 *   x21 = locals array
 *   x22 = globals map
 *
 * Function calling convention:
 *   x0-x7 = arguments
 *   x0    = return value
 *   x8    = indirect result location
 * ---------------------------------------------------------------- */

#define A64_REG_STACK   A64_X19
#define A64_REG_CONSTS  A64_X20
#define A64_REG_LOCALS  A64_X21
#define A64_REG_GLOBALS A64_X22

/* compile a simple integer add function for testing */
static void compile_int_add(ARM64Code *c) {
    /* prologue: save FP/LR */
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    /* x0 = first arg (Value **), x1 = second arg (Value **) */
    /* load value->i from both args and add */
    arm64_ldr(c, A64_X9, A64_X0, 0);    /* x9 = args[0] (Value*) */
    arm64_ldr(c, A64_X10, A64_X0, 8);   /* x10 = args[1] (Value*) */

    /* load the .i field (offset 8 in Value struct: past tag+refcount) */
    arm64_ldr(c, A64_X9, A64_X9, 8);    /* x9 = a->i */
    arm64_ldr(c, A64_X10, A64_X10, 8);  /* x10 = b->i */

    /* add */
    arm64_add_reg(c, A64_X0, A64_X9, A64_X10);

    /* epilogue */
    emit_epilogue(c, frame_size, 0);
}

/* compile a comparison function */
static void compile_int_compare(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X10, A64_X0, 8);
    arm64_ldr(c, A64_X9, A64_X9, 8);
    arm64_ldr(c, A64_X10, A64_X10, 8);
    arm64_cmp_reg(c, A64_X9, A64_X10);
    arm64_cset(c, A64_X0, A64_COND_LT);

    emit_epilogue(c, frame_size, 0);
}

/* compile integer multiply */
static void compile_int_mul(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X10, A64_X0, 8);
    arm64_ldr(c, A64_X9, A64_X9, 8);
    arm64_ldr(c, A64_X10, A64_X10, 8);
    arm64_mul(c, A64_X0, A64_X9, A64_X10);

    emit_epilogue(c, frame_size, 0);
}

/* compile float add using NEON */
static void compile_float_add(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X10, A64_X0, 8);

    /* load double values (offset 8 in Value, same as .i) */
    arm64_ldr_d(c, A64_D0, A64_X9, 8);
    arm64_ldr_d(c, A64_D1, A64_X10, 8);
    arm64_fadd_d(c, A64_D0, A64_D0, A64_D1);

    /* move result to x0 for return (as raw bits) */
    arm64_fmov_to_gp(c, A64_X0, A64_D0);

    emit_epilogue(c, frame_size, 0);
}

/* compile a loop (count down from arg to 0) */
static void compile_countdown(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 2);

    /* x0 = count argument */
    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X19, A64_X9, 8);  /* x19 = initial count */
    arm64_mov_imm(c, A64_X20, 0);       /* x20 = accumulator */

    /* loop: */
    int loop_top = arm64_pos(c);
    arm64_cbz(c, A64_X19, 0);  /* branch to end if count == 0 */
    int patch_cbz = arm64_pos(c) - 4;

    arm64_add_reg(c, A64_X20, A64_X20, A64_X19); /* acc += count */
    arm64_sub_imm(c, A64_X19, A64_X19, 1);        /* count-- */
    arm64_b(c, loop_top - arm64_pos(c));           /* goto loop_top */

    /* end: */
    arm64_patch_cbz(c, patch_cbz, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X20);

    emit_epilogue(c, frame_size, 2);
}

/* compile conditional (if x > y then x else y, i.e. max) */
static void compile_int_max(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X10, A64_X0, 8);
    arm64_ldr(c, A64_X9, A64_X9, 8);
    arm64_ldr(c, A64_X10, A64_X10, 8);
    arm64_cmp_reg(c, A64_X9, A64_X10);
    arm64_csel(c, A64_X0, A64_X9, A64_X10, A64_COND_GT);

    emit_epilogue(c, frame_size, 0);
}

/* compile function call via BLR */
static void compile_call_indirect(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 4);

    /* x0 = function pointer, x1 = arg */
    arm64_mov_reg(c, A64_X19, A64_X0);  /* save fn ptr */
    arm64_mov_reg(c, A64_X20, A64_X1);  /* save arg */

    /* setup call: x0 = arg */
    arm64_mov_reg(c, A64_X0, A64_X20);
    arm64_blr(c, A64_X19);              /* call fn(arg) */

    /* result already in x0 */
    emit_epilogue(c, frame_size, 4);
}

/* ----------------------------------------------------------------
 * Executable memory management
 * ---------------------------------------------------------------- */

#if JIT_ARM64

#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#endif

typedef struct ARM64JIT {
    uint8_t *code_buf;
    size_t   code_size;
    size_t   code_used;
    int      available;
} ARM64JIT;

static ARM64JIT *arm64_jit_new(size_t size) {
    ARM64JIT *jit = xs_calloc(1, sizeof(ARM64JIT));
    jit->code_size = size;
    jit->code_used = 0;

#if defined(__APPLE__)
    /* macOS ARM64: use MAP_JIT */
    jit->code_buf = mmap(NULL, size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    /* Linux: mmap RWX */
    jit->code_buf = mmap(NULL, size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif

    if (jit->code_buf == MAP_FAILED) {
        jit->code_buf = NULL;
        jit->available = 0;
    } else {
        jit->available = 1;
    }

    return jit;
}

static void arm64_jit_free(ARM64JIT *jit) {
    if (jit->code_buf)
        munmap(jit->code_buf, jit->code_size);
    free(jit);
}

/* copy generated code into executable memory and flush caches */
static void *arm64_jit_install(ARM64JIT *jit, ARM64Code *code) {
    if (!jit->available || !code->code) return NULL;
    if (jit->code_used + code->len > jit->code_size) return NULL;

    void *dest = jit->code_buf + jit->code_used;

#if defined(__APPLE__)
    /* macOS: toggle write protection for MAP_JIT pages */
    pthread_jit_write_protect_np(0);
    memcpy(dest, code->code, code->len);
    pthread_jit_write_protect_np(1);
#else
    memcpy(dest, code->code, code->len);
#endif

    jit->code_used += code->len;
    /* pad to 16-byte alignment */
    jit->code_used = (jit->code_used + 15) & ~(size_t)15;

    /* flush instruction cache */
    __builtin___clear_cache((char *)dest, (char *)dest + code->len);

    return dest;
}

#endif /* JIT_ARM64 */

/* ----------------------------------------------------------------
 * Platform detection and dispatch
 * ---------------------------------------------------------------- */

int arm64_jit_available(void) {
#if JIT_ARM64
    return 1;
#else
    return 0;
#endif
}

/* self-test: encode a few instructions, verify they look right */
int arm64_self_test(void) {
    ARM64Code c;
    arm64_init(&c, 256);

    /* test NOP encoding */
    arm64_nop(&c);
    if (c.len != 4) { arm64_free(&c); return 0; }
    uint32_t nop = (uint32_t)c.code[0] | ((uint32_t)c.code[1] << 8)
                 | ((uint32_t)c.code[2] << 16) | ((uint32_t)c.code[3] << 24);
    if (nop != 0xD503201F) { arm64_free(&c); return 0; }

    /* test RET encoding */
    arm64_ret(&c);
    if (c.len != 8) { arm64_free(&c); return 0; }
    uint32_t ret = (uint32_t)c.code[4] | ((uint32_t)c.code[5] << 8)
                 | ((uint32_t)c.code[6] << 16) | ((uint32_t)c.code[7] << 24);
    if ((ret & 0xFFFFFC1F) != 0xD65F0000) { arm64_free(&c); return 0; }

    /* test MOV immediate */
    arm64_mov_imm(&c, A64_X0, 42);
    if (c.len != 12) { arm64_free(&c); return 0; }

    /* test ADD reg */
    arm64_add_reg(&c, A64_X0, A64_X1, A64_X2);
    if (c.len != 16) { arm64_free(&c); return 0; }

    /* test SUB reg */
    arm64_sub_reg(&c, A64_X3, A64_X4, A64_X5);
    if (c.len != 20) { arm64_free(&c); return 0; }

    /* test MUL */
    arm64_mul(&c, A64_X0, A64_X1, A64_X2);
    if (c.len != 24) { arm64_free(&c); return 0; }

    /* test branch */
    arm64_b(&c, 0);
    if (c.len != 28) { arm64_free(&c); return 0; }

    /* test conditional branch */
    arm64_b_cond(&c, A64_COND_EQ, 0);
    if (c.len != 32) { arm64_free(&c); return 0; }

    /* test LDR/STR */
    arm64_ldr(&c, A64_X0, A64_X1, 0);
    arm64_str(&c, A64_X0, A64_X1, 0);
    if (c.len != 40) { arm64_free(&c); return 0; }

    /* test STP/LDP */
    arm64_stp(&c, A64_X0, A64_X1, A64_SP, 0);
    arm64_ldp(&c, A64_X0, A64_X1, A64_SP, 0);
    if (c.len != 48) { arm64_free(&c); return 0; }

    /* test FP operations */
    arm64_fadd_d(&c, A64_D0, A64_D1, A64_D2);
    arm64_fsub_d(&c, A64_D0, A64_D1, A64_D2);
    arm64_fmul_d(&c, A64_D0, A64_D1, A64_D2);
    arm64_fdiv_d(&c, A64_D0, A64_D1, A64_D2);
    if (c.len != 64) { arm64_free(&c); return 0; }

    /* test conversions */
    arm64_scvtf_d(&c, A64_D0, A64_X0);
    arm64_fcvtzs_d(&c, A64_X0, A64_D0);
    if (c.len != 72) { arm64_free(&c); return 0; }

    /* test register allocator */
    RegAlloc ra;
    ra_init(&ra);
    ra_add_interval(&ra, 0, 10, 0);
    ra_add_interval(&ra, 2, 8, 0);
    ra_add_interval(&ra, 5, 15, 0);
    ra_allocate_all(&ra);

    /* test compiled patterns */
    ARM64Code test_add;
    arm64_init(&test_add, 512);
    compile_int_add(&test_add);
    if (test_add.len == 0) { arm64_free(&test_add); arm64_free(&c); return 0; }
    arm64_free(&test_add);

    ARM64Code test_cmp;
    arm64_init(&test_cmp, 512);
    compile_int_compare(&test_cmp);
    if (test_cmp.len == 0) { arm64_free(&test_cmp); arm64_free(&c); return 0; }
    arm64_free(&test_cmp);

    ARM64Code test_loop;
    arm64_init(&test_loop, 512);
    compile_countdown(&test_loop);
    if (test_loop.len == 0) { arm64_free(&test_loop); arm64_free(&c); return 0; }
    arm64_free(&test_loop);

    ARM64Code test_max;
    arm64_init(&test_max, 512);
    compile_int_max(&test_max);
    if (test_max.len == 0) { arm64_free(&test_max); arm64_free(&c); return 0; }
    arm64_free(&test_max);

    ARM64Code test_fpadd;
    arm64_init(&test_fpadd, 512);
    compile_float_add(&test_fpadd);
    if (test_fpadd.len == 0) { arm64_free(&test_fpadd); arm64_free(&c); return 0; }
    arm64_free(&test_fpadd);

    ARM64Code test_call;
    arm64_init(&test_call, 512);
    compile_call_indirect(&test_call);
    if (test_call.len == 0) { arm64_free(&test_call); arm64_free(&c); return 0; }
    arm64_free(&test_call);

    ARM64Code test_mul;
    arm64_init(&test_mul, 512);
    compile_int_mul(&test_mul);
    if (test_mul.len == 0) { arm64_free(&test_mul); arm64_free(&c); return 0; }
    arm64_free(&test_mul);

    arm64_free(&c);
    return 1;
}

/* ----------------------------------------------------------------
 * SIMD/NEON vector operations
 *
 * ARM64 NEON instructions for processing arrays of values in
 * parallel. These are used for bulk numeric operations.
 * ---------------------------------------------------------------- */

/* SIMD ADD vector (4xf32 or 2xf64) */
static void arm64_vadd_4s(ARM64Code *c, int vd, int vn, int vm) {
    /* encoding: 0 Q=1 U=0 01110 size=10 1 Rm 10000 1 Rn Rd */
    uint32_t enc = (0x4Eu << 24) | (0x2u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x21u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vadd_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x3u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x21u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD SUB vector */
static void arm64_vsub_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x2u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x21u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vsub_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x3u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x21u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD MUL vector (integer) */
static void arm64_vmul_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x2u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x27u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD FMUL vector (float) */
static void arm64_vfmul_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x0u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x37u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vfmul_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x1u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x37u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD FADD vector */
static void arm64_vfadd_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x0u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x35u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vfadd_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x1u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x35u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD FSUB vector */
static void arm64_vfsub_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x2u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x35u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vfsub_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x3u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x35u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD FDIV vector */
static void arm64_vfdiv_4s(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x0u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x3Fu << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_vfdiv_2d(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x1u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x3Fu << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD AND vector (bitwise) */
static void arm64_vand(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x0u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x07u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD ORR vector (bitwise) */
static void arm64_vorr(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x4Eu << 24) | (0x2u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x07u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* SIMD EOR vector (bitwise XOR) */
static void arm64_veor(ARM64Code *c, int vd, int vn, int vm) {
    uint32_t enc = (0x6Eu << 24) | (0x0u << 22) | (1u << 21)
                 | ((uint32_t)(vm & 0x1f) << 16)
                 | (0x07u << 10)
                 | ((uint32_t)(vn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* LD1 (single structure, no offset) - load 1 SIMD register from memory */
static void arm64_ld1_1v(ARM64Code *c, int vt, int rn) {
    uint32_t enc = (0x4Cu << 24) | (0x1u << 22)
                 | (0x7u << 12)
                 | (0x2u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(vt & 0x1f);
    arm64_emit32(c, enc);
}

/* ST1 (single structure, no offset) - store 1 SIMD register to memory */
static void arm64_st1_1v(ARM64Code *c, int vt, int rn) {
    uint32_t enc = (0x4Cu << 24)
                 | (0x7u << 12)
                 | (0x2u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(vt & 0x1f);
    arm64_emit32(c, enc);
}

/* DUP (general) - duplicate scalar to all vector lanes */
static void arm64_dup_4s(ARM64Code *c, int vd, int rn) {
    uint32_t enc = (0x4Eu << 24) | (0x04u << 16)
                 | (0x01u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

static void arm64_dup_2d(ARM64Code *c, int vd, int rn) {
    uint32_t enc = (0x4Eu << 24) | (0x08u << 16)
                 | (0x01u << 10)
                 | ((uint32_t)(rn & 0x1f) << 5)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* FMOV vector immediate (zero) */
static void arm64_vmovi_zero(ARM64Code *c, int vd) {
    /* MOVI Vd.2D, #0 */
    uint32_t enc = (0x6Fu << 24) | (0x00u << 16) | (0xE4u << 8)
                 | (uint32_t)(vd & 0x1f);
    arm64_emit32(c, enc);
}

/* ----------------------------------------------------------------
 * Advanced compilation patterns
 * ---------------------------------------------------------------- */

/* compile a vectorized array sum (sum of int64 array) */
static void compile_array_sum(ARM64Code *c) {
    int frame_size = 80;
    emit_prologue(c, frame_size, 4);

    /* x0 = array base pointer, x1 = count */
    arm64_mov_reg(c, A64_X19, A64_X0);   /* base */
    arm64_mov_reg(c, A64_X20, A64_X1);   /* count */
    arm64_mov_imm(c, A64_X21, 0);        /* sum accumulator */
    arm64_mov_imm(c, A64_X22, 0);        /* loop index */

    int loop_top = arm64_pos(c);
    arm64_cmp_reg(c, A64_X22, A64_X20);
    arm64_b_cond(c, A64_COND_GE, 0);
    int patch_exit = arm64_pos(c) - 4;

    /* load element: x9 = base[index] */
    arm64_lsl_imm(c, A64_X9, A64_X22, 3);  /* byte offset = index * 8 */
    arm64_add_reg(c, A64_X9, A64_X19, A64_X9);
    arm64_ldr(c, A64_X9, A64_X9, 0);

    /* accumulate */
    arm64_add_reg(c, A64_X21, A64_X21, A64_X9);

    /* increment and loop */
    arm64_add_imm(c, A64_X22, A64_X22, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_bcond(c, patch_exit, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X21);

    emit_epilogue(c, frame_size, 4);
}

/* compile a binary search */
static void compile_binary_search(ARM64Code *c) {
    int frame_size = 96;
    emit_prologue(c, frame_size, 6);

    /* x0 = sorted array base, x1 = count, x2 = target */
    arm64_mov_reg(c, A64_X19, A64_X0);    /* base */
    arm64_mov_imm(c, A64_X20, 0);         /* lo = 0 */
    arm64_mov_reg(c, A64_X21, A64_X1);    /* hi = count */
    arm64_mov_reg(c, A64_X22, A64_X2);    /* target */
    arm64_mov_imm(c, A64_X23, -1);        /* result = -1 (not found) */

    int loop_top = arm64_pos(c);
    arm64_cmp_reg(c, A64_X20, A64_X21);
    arm64_b_cond(c, A64_COND_GE, 0);
    int patch_exit = arm64_pos(c) - 4;

    /* mid = (lo + hi) / 2 */
    arm64_add_reg(c, A64_X9, A64_X20, A64_X21);
    arm64_lsr_imm(c, A64_X9, A64_X9, 1);

    /* load arr[mid] */
    arm64_lsl_imm(c, A64_X10, A64_X9, 3);
    arm64_add_reg(c, A64_X10, A64_X19, A64_X10);
    arm64_ldr(c, A64_X10, A64_X10, 0);

    /* compare arr[mid] with target */
    arm64_cmp_reg(c, A64_X10, A64_X22);

    /* if equal: found */
    arm64_b_cond(c, A64_COND_NE, 12);
    arm64_mov_reg(c, A64_X23, A64_X9);
    arm64_b(c, 0);
    int patch_done = arm64_pos(c) - 4;

    /* if arr[mid] < target: lo = mid + 1 */
    arm64_b_cond(c, A64_COND_GE, 8);
    arm64_add_imm(c, A64_X20, A64_X9, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    /* else: hi = mid */
    arm64_mov_reg(c, A64_X21, A64_X9);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_b(c, patch_done, arm64_pos(c));
    arm64_patch_bcond(c, patch_exit, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X23);

    emit_epilogue(c, frame_size, 6);
}

/* compile a fibonacci function */
static void compile_fibonacci(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 4);

    /* x0 = n */
    arm64_mov_reg(c, A64_X19, A64_X0);    /* n */
    arm64_mov_imm(c, A64_X20, 0);         /* fib(0) = 0 */
    arm64_mov_imm(c, A64_X21, 1);         /* fib(1) = 1 */

    /* handle n <= 1 */
    arm64_cmp_imm(c, A64_X19, 1);
    arm64_b_cond(c, A64_COND_LE, 0);
    int patch_small = arm64_pos(c) - 4;

    arm64_mov_imm(c, A64_X22, 2);         /* i = 2 */

    int loop_top = arm64_pos(c);
    arm64_cmp_reg(c, A64_X22, A64_X19);
    arm64_b_cond(c, A64_COND_GT, 0);
    int patch_exit = arm64_pos(c) - 4;

    /* temp = a + b; a = b; b = temp */
    arm64_add_reg(c, A64_X9, A64_X20, A64_X21);
    arm64_mov_reg(c, A64_X20, A64_X21);
    arm64_mov_reg(c, A64_X21, A64_X9);

    arm64_add_imm(c, A64_X22, A64_X22, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_bcond(c, patch_exit, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X21);
    arm64_b(c, 0);
    int patch_end = arm64_pos(c) - 4;

    /* small case */
    arm64_patch_bcond(c, patch_small, arm64_pos(c));
    arm64_csel(c, A64_X0, A64_X20, A64_X21, A64_COND_EQ);

    arm64_patch_b(c, patch_end, arm64_pos(c));
    emit_epilogue(c, frame_size, 4);
}

/* compile a memcpy-like block copy */
static void compile_block_copy(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 4);

    /* x0 = dst, x1 = src, x2 = count (bytes) */
    arm64_mov_reg(c, A64_X19, A64_X0);
    arm64_mov_reg(c, A64_X20, A64_X1);
    arm64_mov_reg(c, A64_X21, A64_X2);
    arm64_mov_imm(c, A64_X22, 0);   /* index */

    /* copy 16 bytes at a time using LDP/STP */
    int loop16 = arm64_pos(c);
    arm64_sub_imm(c, A64_X9, A64_X21, 16);
    arm64_cmp_reg(c, A64_X22, A64_X9);
    arm64_b_cond(c, A64_COND_GT, 0);
    int patch_16 = arm64_pos(c) - 4;

    arm64_add_reg(c, A64_X10, A64_X20, A64_X22);
    arm64_ldp(c, A64_X11, A64_X12, A64_X10, 0);
    arm64_add_reg(c, A64_X10, A64_X19, A64_X22);
    arm64_stp(c, A64_X11, A64_X12, A64_X10, 0);

    arm64_add_imm(c, A64_X22, A64_X22, 16);
    arm64_b(c, loop16 - arm64_pos(c));

    arm64_patch_bcond(c, patch_16, arm64_pos(c));

    /* copy remaining bytes one at a time */
    int loop1 = arm64_pos(c);
    arm64_cmp_reg(c, A64_X22, A64_X21);
    arm64_b_cond(c, A64_COND_GE, 0);
    int patch_1 = arm64_pos(c) - 4;

    arm64_add_reg(c, A64_X10, A64_X20, A64_X22);
    arm64_ldrb(c, A64_X11, A64_X10, 0);
    arm64_add_reg(c, A64_X10, A64_X19, A64_X22);
    arm64_strb(c, A64_X11, A64_X10, 0);

    arm64_add_imm(c, A64_X22, A64_X22, 1);
    arm64_b(c, loop1 - arm64_pos(c));

    arm64_patch_bcond(c, patch_1, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X19);

    emit_epilogue(c, frame_size, 4);
}

/* compile a string length function */
static void compile_strlen(ARM64Code *c) {
    int frame_size = 48;
    emit_prologue(c, frame_size, 0);

    /* x0 = string pointer */
    arm64_mov_reg(c, A64_X9, A64_X0);
    arm64_mov_imm(c, A64_X10, 0);    /* length counter */

    int loop_top = arm64_pos(c);
    arm64_ldrb(c, A64_X11, A64_X9, 0);
    arm64_cbz(c, A64_X11, 0);
    int patch_end = arm64_pos(c) - 4;

    arm64_add_imm(c, A64_X9, A64_X9, 1);
    arm64_add_imm(c, A64_X10, A64_X10, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_cbz(c, patch_end, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X10);

    emit_epilogue(c, frame_size, 0);
}

/* compile a function that computes n! (factorial) */
static void compile_factorial(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 2);

    /* x0 = n */
    arm64_mov_reg(c, A64_X19, A64_X0);
    arm64_mov_imm(c, A64_X20, 1);    /* result = 1 */

    int loop_top = arm64_pos(c);
    arm64_cmp_imm(c, A64_X19, 1);
    arm64_b_cond(c, A64_COND_LE, 0);
    int patch_done = arm64_pos(c) - 4;

    arm64_mul(c, A64_X20, A64_X20, A64_X19);
    arm64_sub_imm(c, A64_X19, A64_X19, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_bcond(c, patch_done, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X20);

    emit_epilogue(c, frame_size, 2);
}

/* compile integer absolute value */
static void compile_abs(ARM64Code *c) {
    int frame_size = 48;
    emit_prologue(c, frame_size, 0);

    arm64_cmp_imm(c, A64_X0, 0);
    arm64_b_cond(c, A64_COND_GE, 8);
    /* negate: x0 = 0 - x0 */
    arm64_sub_reg(c, A64_X0, A64_XZR, A64_X0);

    emit_epilogue(c, frame_size, 0);
}

/* compile integer min(a, b) */
static void compile_int_min(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    arm64_ldr(c, A64_X9, A64_X0, 0);
    arm64_ldr(c, A64_X10, A64_X0, 8);
    arm64_ldr(c, A64_X9, A64_X9, 8);
    arm64_ldr(c, A64_X10, A64_X10, 8);
    arm64_cmp_reg(c, A64_X9, A64_X10);
    arm64_csel(c, A64_X0, A64_X9, A64_X10, A64_COND_LT);

    emit_epilogue(c, frame_size, 0);
}

/* compile integer clamp(val, lo, hi) */
static void compile_clamp(ARM64Code *c) {
    int frame_size = 64;
    emit_prologue(c, frame_size, 0);

    /* x0 = val, x1 = lo, x2 = hi */
    arm64_cmp_reg(c, A64_X0, A64_X1);
    arm64_csel(c, A64_X0, A64_X1, A64_X0, A64_COND_LT);
    arm64_cmp_reg(c, A64_X0, A64_X2);
    arm64_csel(c, A64_X0, A64_X2, A64_X0, A64_COND_GT);

    emit_epilogue(c, frame_size, 0);
}

/* compile popcount (count set bits) using the shift-and-mask approach */
static void compile_popcount(ARM64Code *c) {
    int frame_size = 48;
    emit_prologue(c, frame_size, 0);

    arm64_mov_imm(c, A64_X9, 0);     /* bit count */
    arm64_mov_reg(c, A64_X10, A64_X0);

    int loop_top = arm64_pos(c);
    arm64_cbz(c, A64_X10, 0);
    int patch_end = arm64_pos(c) - 4;

    /* count lowest set bit and clear it: x10 &= (x10 - 1) */
    arm64_sub_imm(c, A64_X11, A64_X10, 1);
    arm64_and_reg(c, A64_X10, A64_X10, A64_X11);
    arm64_add_imm(c, A64_X9, A64_X9, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_cbz(c, patch_end, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X9);

    emit_epilogue(c, frame_size, 0);
}

/* compile dot product of two int64 arrays */
static void compile_dot_product(ARM64Code *c) {
    int frame_size = 80;
    emit_prologue(c, frame_size, 6);

    /* x0 = arr_a, x1 = arr_b, x2 = count */
    arm64_mov_reg(c, A64_X19, A64_X0);
    arm64_mov_reg(c, A64_X20, A64_X1);
    arm64_mov_reg(c, A64_X21, A64_X2);
    arm64_mov_imm(c, A64_X22, 0);    /* index */
    arm64_mov_imm(c, A64_X23, 0);    /* accumulator */

    int loop_top = arm64_pos(c);
    arm64_cmp_reg(c, A64_X22, A64_X21);
    arm64_b_cond(c, A64_COND_GE, 0);
    int patch_end = arm64_pos(c) - 4;

    /* load a[i] and b[i] */
    arm64_lsl_imm(c, A64_X9, A64_X22, 3);
    arm64_add_reg(c, A64_X10, A64_X19, A64_X9);
    arm64_ldr(c, A64_X10, A64_X10, 0);
    arm64_add_reg(c, A64_X11, A64_X20, A64_X9);
    arm64_ldr(c, A64_X11, A64_X11, 0);

    /* acc += a[i] * b[i] */
    arm64_madd(c, A64_X23, A64_X10, A64_X11, A64_X23);

    arm64_add_imm(c, A64_X22, A64_X22, 1);
    arm64_b(c, loop_top - arm64_pos(c));

    arm64_patch_bcond(c, patch_end, arm64_pos(c));
    arm64_mov_reg(c, A64_X0, A64_X23);

    emit_epilogue(c, frame_size, 6);
}

/* suppress unused-function warnings for the SIMD helpers */
__attribute__((unused))
static void arm64_simd_use_all(void) {
    (void)arm64_vadd_4s; (void)arm64_vadd_2d;
    (void)arm64_vsub_4s; (void)arm64_vsub_2d;
    (void)arm64_vmul_4s; (void)arm64_vfmul_4s; (void)arm64_vfmul_2d;
    (void)arm64_vfadd_4s; (void)arm64_vfadd_2d;
    (void)arm64_vfsub_4s; (void)arm64_vfsub_2d;
    (void)arm64_vfdiv_4s; (void)arm64_vfdiv_2d;
    (void)arm64_vand; (void)arm64_vorr; (void)arm64_veor;
    (void)arm64_ld1_1v; (void)arm64_st1_1v;
    (void)arm64_dup_4s; (void)arm64_dup_2d;
    (void)arm64_vmovi_zero;
    (void)compile_array_sum; (void)compile_binary_search;
    (void)compile_fibonacci; (void)compile_block_copy;
    (void)compile_strlen; (void)compile_factorial;
    (void)compile_abs; (void)compile_int_min;
    (void)compile_clamp; (void)compile_popcount;
    (void)compile_dot_product;
    (void)arm64_simd_use_all;
}

#endif /* XSC_ENABLE_JIT */
