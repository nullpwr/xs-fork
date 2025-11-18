/* arm64.h -- ARM64/AArch64 instruction encoding for the XS JIT backend.
 * Guarded by XSC_ENABLE_JIT; only active on aarch64 targets. */
#ifndef XS_ARM64_H
#define XS_ARM64_H

#ifdef XSC_ENABLE_JIT

#include <stdint.h>
#include <stddef.h>

/* Platform detection */
#if defined(__aarch64__) || defined(_M_ARM64)
#define JIT_ARM64 1
#else
#define JIT_ARM64 0
#endif

/* GP register aliases */
#define A64_X0   0
#define A64_X1   1
#define A64_X2   2
#define A64_X3   3
#define A64_X4   4
#define A64_X5   5
#define A64_X6   6
#define A64_X7   7
#define A64_X8   8
#define A64_X9   9
#define A64_X10  10
#define A64_X11  11
#define A64_X12  12
#define A64_X13  13
#define A64_X14  14
#define A64_X15  15
#define A64_X16  16
#define A64_X17  17
#define A64_X18  18
#define A64_X19  19
#define A64_X20  20
#define A64_X21  21
#define A64_X22  22
#define A64_X23  23
#define A64_X24  24
#define A64_X25  25
#define A64_X26  26
#define A64_X27  27
#define A64_X28  28
#define A64_X29  29   /* frame pointer */
#define A64_X30  30   /* link register */
#define A64_SP   31
#define A64_XZR  31
#define A64_FP   29
#define A64_LR   30

/* FP/SIMD register aliases */
#define A64_D0   0
#define A64_D1   1
#define A64_D2   2
#define A64_D3   3
#define A64_D4   4
#define A64_D5   5
#define A64_D6   6
#define A64_D7   7

/* Condition codes */
#define A64_COND_EQ  0x0    /* equal */
#define A64_COND_NE  0x1    /* not equal */
#define A64_COND_CS  0x2    /* carry set / unsigned >= */
#define A64_COND_CC  0x3    /* carry clear / unsigned < */
#define A64_COND_MI  0x4    /* minus / negative */
#define A64_COND_PL  0x5    /* plus / positive or zero */
#define A64_COND_VS  0x6    /* overflow */
#define A64_COND_VC  0x7    /* no overflow */
#define A64_COND_HI  0x8    /* unsigned > */
#define A64_COND_LS  0x9    /* unsigned <= */
#define A64_COND_GE  0xA    /* signed >= */
#define A64_COND_LT  0xB    /* signed < */
#define A64_COND_GT  0xC    /* signed > */
#define A64_COND_LE  0xD    /* signed <= */
#define A64_COND_AL  0xE    /* always */

/* Code buffer for ARM64 instructions */
typedef struct {
    uint8_t *code;
    int      len;
    int      cap;
    int      overflow;
} ARM64Code;

void arm64_init(ARM64Code *c, int initial_cap);
void arm64_free(ARM64Code *c);
void arm64_emit32(ARM64Code *c, uint32_t instr);

/* Data processing - immediate */
void arm64_mov_imm(ARM64Code *c, int rd, int64_t imm);
void arm64_mov_reg(ARM64Code *c, int rd, int rs);
void arm64_movz(ARM64Code *c, int rd, uint16_t imm, int shift);
void arm64_movk(ARM64Code *c, int rd, uint16_t imm, int shift);
void arm64_movn(ARM64Code *c, int rd, uint16_t imm, int shift);
void arm64_add_imm(ARM64Code *c, int rd, int rn, int imm);
void arm64_add_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_sub_imm(ARM64Code *c, int rd, int rn, int imm);
void arm64_sub_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_mul(ARM64Code *c, int rd, int rn, int rm);
void arm64_sdiv(ARM64Code *c, int rd, int rn, int rm);
void arm64_udiv(ARM64Code *c, int rd, int rn, int rm);
void arm64_msub(ARM64Code *c, int rd, int rn, int rm, int ra);
void arm64_madd(ARM64Code *c, int rd, int rn, int rm, int ra);

/* Logical */
void arm64_and_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_orr_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_eor_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_orn_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_mvn(ARM64Code *c, int rd, int rm);

/* Shifts */
void arm64_lsl_imm(ARM64Code *c, int rd, int rn, int shift);
void arm64_lsr_imm(ARM64Code *c, int rd, int rn, int shift);
void arm64_asr_imm(ARM64Code *c, int rd, int rn, int shift);
void arm64_lsl_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_lsr_reg(ARM64Code *c, int rd, int rn, int rm);
void arm64_asr_reg(ARM64Code *c, int rd, int rn, int rm);

/* Comparisons */
void arm64_cmp_imm(ARM64Code *c, int rn, int imm);
void arm64_cmp_reg(ARM64Code *c, int rn, int rm);
void arm64_cmn_imm(ARM64Code *c, int rn, int imm);
void arm64_tst_reg(ARM64Code *c, int rn, int rm);

/* Conditional select */
void arm64_csel(ARM64Code *c, int rd, int rn, int rm, int cond);
void arm64_cset(ARM64Code *c, int rd, int cond);

/* Branches */
void arm64_b(ARM64Code *c, int32_t offset);
void arm64_b_cond(ARM64Code *c, int cond, int32_t offset);
void arm64_bl(ARM64Code *c, int32_t offset);
void arm64_ret(ARM64Code *c);
void arm64_br(ARM64Code *c, int rn);
void arm64_blr(ARM64Code *c, int rn);
void arm64_cbz(ARM64Code *c, int rt, int32_t offset);
void arm64_cbnz(ARM64Code *c, int rt, int32_t offset);
void arm64_tbz(ARM64Code *c, int rt, int bit, int32_t offset);
void arm64_tbnz(ARM64Code *c, int rt, int bit, int32_t offset);

/* Addressing helpers: ADR, ADRP */
void arm64_adr(ARM64Code *c, int rd, int32_t offset);

/* Memory - load/store with unsigned offset */
void arm64_ldr(ARM64Code *c, int rt, int rn, int offset);
void arm64_str(ARM64Code *c, int rt, int rn, int offset);
void arm64_ldr_w(ARM64Code *c, int rt, int rn, int offset);
void arm64_str_w(ARM64Code *c, int rt, int rn, int offset);
void arm64_ldrb(ARM64Code *c, int rt, int rn, int offset);
void arm64_strb(ARM64Code *c, int rt, int rn, int offset);
void arm64_ldrh(ARM64Code *c, int rt, int rn, int offset);
void arm64_strh(ARM64Code *c, int rt, int rn, int offset);

/* Memory - pre/post indexed */
void arm64_ldr_pre(ARM64Code *c, int rt, int rn, int offset);
void arm64_str_pre(ARM64Code *c, int rt, int rn, int offset);
void arm64_ldr_post(ARM64Code *c, int rt, int rn, int offset);
void arm64_str_post(ARM64Code *c, int rt, int rn, int offset);

/* Memory - load/store pair */
void arm64_ldp(ARM64Code *c, int rt1, int rt2, int rn, int offset);
void arm64_stp(ARM64Code *c, int rt1, int rt2, int rn, int offset);
void arm64_ldp_pre(ARM64Code *c, int rt1, int rt2, int rn, int offset);
void arm64_stp_pre(ARM64Code *c, int rt1, int rt2, int rn, int offset);
void arm64_ldp_post(ARM64Code *c, int rt1, int rt2, int rn, int offset);
void arm64_stp_post(ARM64Code *c, int rt1, int rt2, int rn, int offset);

/* Stack helpers */
void arm64_push(ARM64Code *c, int rt1, int rt2);
void arm64_pop(ARM64Code *c, int rt1, int rt2);
void arm64_push1(ARM64Code *c, int rt);
void arm64_pop1(ARM64Code *c, int rt);

/* Load literal (pc-relative) */
void arm64_ldr_literal(ARM64Code *c, int rt, int32_t offset);

/* Floating point - double precision */
void arm64_fmov_d(ARM64Code *c, int rd, int rn);
void arm64_fadd_d(ARM64Code *c, int rd, int rn, int rm);
void arm64_fsub_d(ARM64Code *c, int rd, int rn, int rm);
void arm64_fmul_d(ARM64Code *c, int rd, int rn, int rm);
void arm64_fdiv_d(ARM64Code *c, int rd, int rn, int rm);
void arm64_fneg_d(ARM64Code *c, int rd, int rn);
void arm64_fabs_d(ARM64Code *c, int rd, int rn);
void arm64_fsqrt_d(ARM64Code *c, int rd, int rn);
void arm64_fcmp_d(ARM64Code *c, int rn, int rm);
void arm64_fcmp_zero_d(ARM64Code *c, int rn);

/* Conversions */
void arm64_scvtf_d(ARM64Code *c, int rd, int rn);   /* int64 -> f64 */
void arm64_ucvtf_d(ARM64Code *c, int rd, int rn);   /* uint64 -> f64 */
void arm64_fcvtzs_d(ARM64Code *c, int rd, int rn);  /* f64 -> int64 */
void arm64_fmov_to_gp(ARM64Code *c, int rd, int rn);  /* fp reg -> gp reg */
void arm64_fmov_from_gp(ARM64Code *c, int rd, int rn);/* gp reg -> fp reg */

/* FP load/store */
void arm64_ldr_d(ARM64Code *c, int rt, int rn, int offset);
void arm64_str_d(ARM64Code *c, int rt, int rn, int offset);

/* NOP */
void arm64_nop(ARM64Code *c);

/* System */
void arm64_brk(ARM64Code *c, uint16_t imm);
void arm64_svc(ARM64Code *c, uint16_t imm);

/* Patch helpers */
void arm64_patch_b(ARM64Code *c, int patch_offset, int target_offset);
void arm64_patch_bcond(ARM64Code *c, int patch_offset, int target_offset);
void arm64_patch_cbz(ARM64Code *c, int patch_offset, int target_offset);

/* Utility: get current code position */
static inline int arm64_pos(ARM64Code *c) { return c->len; }

#endif /* XSC_ENABLE_JIT */
#endif /* XS_ARM64_H */
