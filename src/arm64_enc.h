// SPDX-License-Identifier: LGPL-2.1-or-later
// ARM64 (AArch64) instruction encoder — 32-bit fixed-width encoding.
// Returns the 4-byte instruction word; caller emits it with secbuf_emit32le().
#ifndef ARM64_ENC_H
#define ARM64_ENC_H

#include <stdint.h>
#include <stdbool.h>

// Register numbers (0-30 = x0-x30, 31 = xzr/sp depending on context)
#define ARM64_XZR 31
#define ARM64_SP  31

// Condition codes (cond field in B.cond / CSEL etc.)
typedef enum {
    ARM64_EQ = 0,
    ARM64_NE = 1,
    ARM64_CS = 2,
    ARM64_CC = 3,
    ARM64_MI = 4,
    ARM64_PL = 5,
    ARM64_VS = 6,
    ARM64_VC = 7,
    ARM64_HI = 8,
    ARM64_LS = 9,
    ARM64_GE = 10,
    ARM64_LT = 11,
    ARM64_GT = 12,
    ARM64_LE = 13,
    ARM64_AL = 14,
    ARM64_NV = 15,
    // aliases
    ARM64_HS = 2,
    ARM64_LO = 3,
} Arm64Cond;

// Shift types
typedef enum { ARM64_LSL = 0,
               ARM64_LSR = 1,
               ARM64_ASR = 2,
               ARM64_ROR = 3 } Arm64Shift;

// Extend types (for load/store and ADD extended)
typedef enum {
    ARM64_UXTB = 0,
    ARM64_UXTH = 1,
    ARM64_UXTW = 2,
    ARM64_UXTX = 3,
    ARM64_SXTB = 4,
    ARM64_SXTH = 5,
    ARM64_SXTW = 6,
    ARM64_SXTX = 7,
} Arm64Ext;

// ---------------------------------------------------------------------------
// Data processing — immediate
// ---------------------------------------------------------------------------
uint32_t arm64_movz(int sf, int rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_movk(int sf, int rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_movn(int sf, int rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_add_imm(int sf, int rd, int rn, int32_t imm12, uint16_t shift2);
uint32_t arm64_adds_imm(int sf, int rd, int rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_sub_imm(int sf, int rd, int rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_subs_imm(int sf, int rd, int rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_and_imm(int sf, int rd, int rn, uint64_t imm_enc);
uint32_t arm64_orr_imm(int sf, int rd, int rn, uint64_t imm_enc);
uint32_t arm64_eor_imm(int sf, int rd, int rn, uint64_t imm_enc);
uint32_t arm64_ands_imm(int sf, int rd, int rn, uint64_t imm_enc);

// ---------------------------------------------------------------------------
// Data processing — register
// ---------------------------------------------------------------------------
uint32_t arm64_add_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_adds_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_sub_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_subs_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_and_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_orr_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_eor_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_ands_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_bic_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_mul(int sf, int rd, int rn, int rm); // MADD xd,xn,xm,xzr
uint32_t arm64_smull(int rd, int rn, int rm); // SMADDL
uint32_t arm64_umull(int rd, int rn, int rm); // UMADDL
uint32_t arm64_smulh(int rd, int rn, int rm);
uint32_t arm64_umulh(int rd, int rn, int rm);
uint32_t arm64_sdiv(int sf, int rd, int rn, int rm);
uint32_t arm64_udiv(int sf, int rd, int rn, int rm);
uint32_t arm64_lsl_reg(int sf, int rd, int rn, int rm);
uint32_t arm64_lsr_reg(int sf, int rd, int rn, int rm);
uint32_t arm64_asr_reg(int sf, int rd, int rn, int rm);
uint32_t arm64_ror_reg(int sf, int rd, int rn, int rm);
uint32_t arm64_lsl_imm(int sf, int rd, int rn, int shift);
uint32_t arm64_lsr_imm(int sf, int rd, int rn, int shift);
uint32_t arm64_asr_imm(int sf, int rd, int rn, int shift);
uint32_t arm64_neg(int sf, int rd, int rm);
uint32_t arm64_mvn(int sf, int rd, int rm, Arm64Shift sh, int imm6);
uint32_t arm64_clz(int sf, int rd, int rn);
uint32_t arm64_cls(int sf, int rd, int rn);
uint32_t arm64_rbit(int sf, int rd, int rn);
uint32_t arm64_rev(int sf, int rd, int rn);
uint32_t arm64_rev16(int sf, int rd, int rn);
uint32_t arm64_rev32(int rd, int rn);
uint32_t arm64_sxtb(int sf, int rd, int rn);
uint32_t arm64_sxth(int sf, int rd, int rn);
uint32_t arm64_sxtw(int rd, int rn);
uint32_t arm64_uxtb(int rd, int rn);
uint32_t arm64_uxth(int rd, int rn);
uint32_t arm64_ubfx(int sf, int rd, int rn, int lsb, int width);
uint32_t arm64_sbfx(int sf, int rd, int rn, int lsb, int width);
uint32_t arm64_csel(int sf, int rd, int rn, int rm, Arm64Cond cond);
uint32_t arm64_csinc(int sf, int rd, int rn, int rm, Arm64Cond cond);
uint32_t arm64_csneg(int sf, int rd, int rn, int rm, Arm64Cond cond);
uint32_t arm64_cset(int sf, int rd, Arm64Cond cond);
uint32_t arm64_cneg(int sf, int rd, int rn, Arm64Cond cond);
uint32_t arm64_adc(int sf, int rd, int rn, int rm);
uint32_t arm64_sbc(int sf, int rd, int rn, int rm);

// ---------------------------------------------------------------------------
// PC-relative addressing
// ---------------------------------------------------------------------------
// These encode 0 offset; caller adds reloc
uint32_t arm64_adrp(int rd, int32_t page_imm);
uint32_t arm64_adr(int rd, int32_t imm);

// ---------------------------------------------------------------------------
// Load/Store
// ---------------------------------------------------------------------------
uint32_t arm64_ldr_imm(int sf, int rt, int rn, int32_t imm9, bool pre);
uint32_t arm64_ldr_uoff(int sz, int rt, int rn, uint32_t uimm);
uint32_t arm64_ldrb_uoff(int rt, int rn, uint32_t uimm);
uint32_t arm64_ldrh_uoff(int rt, int rn, uint32_t uimm);
uint32_t arm64_ldrb_imm(int rt, int rn, int32_t imm9);
uint32_t arm64_ldrh_imm(int rt, int rn, int32_t imm9);
uint32_t arm64_ldrsb(int sf, int rt, int rn, int32_t imm9);
uint32_t arm64_ldrsh(int sf, int rt, int rn, int32_t imm9);
uint32_t arm64_ldrsw_imm(int rt, int rn, int32_t imm9);
uint32_t arm64_ldrsw_uoff(int rt, int rn, uint32_t uimm);
uint32_t arm64_str_imm(int sf, int rt, int rn, int32_t imm9, bool pre);
uint32_t arm64_str_uoff(int sz, int rt, int rn, uint32_t uimm);
uint32_t arm64_strb_uoff(int rt, int rn, uint32_t uimm);
uint32_t arm64_strh_uoff(int rt, int rn, uint32_t uimm);
uint32_t arm64_strb_imm(int rt, int rn, int32_t imm9);
uint32_t arm64_strh_imm(int rt, int rn, int32_t imm9);
// Pair: LDP/STP
uint32_t arm64_ldp(int sf, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post);
uint32_t arm64_stp(int sf, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post);
// Register offset
uint32_t arm64_ldr_reg(int sz, int rt, int rn, int rm, bool ext, int s);
uint32_t arm64_str_reg(int sz, int rt, int rn, int rm, bool ext, int s);
// Unscaled immediate (LDUR/STUR)
uint32_t arm64_ldur(int sf, int rt, int rn, int32_t imm9);
uint32_t arm64_stur(int sf, int rt, int rn, int32_t imm9);
// Load-exclusive / store-exclusive
uint32_t arm64_ldxr(int sf, int rt, int rn);
uint32_t arm64_ldxrb(int rt, int rn);
uint32_t arm64_ldxrh(int rt, int rn);
uint32_t arm64_stxr(int sf, int rs, int rt, int rn);
uint32_t arm64_stxrb(int rs, int rt, int rn);
uint32_t arm64_stxrh(int rs, int rt, int rn);
// Load/store acquire-release
uint32_t arm64_ldar(int sf, int rt, int rn);
uint32_t arm64_ldarb(int rt, int rn);
uint32_t arm64_ldarh(int rt, int rn);
uint32_t arm64_stlr(int sf, int rt, int rn);
uint32_t arm64_stlrb(int rt, int rn);
uint32_t arm64_stlrh(int rt, int rn);

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------
// imm26 / imm19 in instruction units (4 bytes each); caller scales
uint32_t arm64_b(int32_t imm26);
uint32_t arm64_bl(int32_t imm26);
uint32_t arm64_br(int rn);
uint32_t arm64_blr(int rn);
uint32_t arm64_ret(int rn); // rn=30 for normal return
uint32_t arm64_bcond(Arm64Cond cond, int32_t imm19);
uint32_t arm64_cbz(int sf, int rt, int32_t imm19);
uint32_t arm64_cbnz(int sf, int rt, int32_t imm19);
uint32_t arm64_tbz(int rt, int imm6, int32_t imm14);
uint32_t arm64_tbnz(int rt, int imm6, int32_t imm14);

// ---------------------------------------------------------------------------
// System / Misc
// ---------------------------------------------------------------------------
uint32_t arm64_nop(void);
uint32_t arm64_dmb(int opt); // opt=0xb=ish
uint32_t arm64_dsb(int opt);
uint32_t arm64_isb(void);
uint32_t arm64_prfm_imm(int prfop, int rn, uint32_t uimm);

// ---------------------------------------------------------------------------
// FP / SIMD
// ---------------------------------------------------------------------------
uint32_t arm64_fmov_f2i(int sf, int rd, int rn);
uint32_t arm64_fmov_i2f(int sf, int rd, int rn);
uint32_t arm64_fmov_imm(int ftype, int rd, uint8_t imm8);
uint32_t arm64_fadd(int ftype, int rd, int rn, int rm);
uint32_t arm64_fsub(int ftype, int rd, int rn, int rm);
uint32_t arm64_fmul(int ftype, int rd, int rn, int rm);
uint32_t arm64_fdiv(int ftype, int rd, int rn, int rm);
uint32_t arm64_fneg(int ftype, int rd, int rn);
uint32_t arm64_fabs(int ftype, int rd, int rn);
uint32_t arm64_fcmp(int ftype, int rn, int rm);
uint32_t arm64_fcvt(int opc, int ftype, int rd, int rn); // ftype→opc conversion
uint32_t arm64_scvtf(int sf, int ftype, int rd, int rn);
uint32_t arm64_ucvtf(int sf, int ftype, int rd, int rn);
uint32_t arm64_fcvtzs(int sf, int ftype, int rd, int rn);
uint32_t arm64_fcvtzu(int sf, int ftype, int rd, int rn);
uint32_t arm64_ldr_fp(int opc, int rt, int rn, uint32_t uimm);
uint32_t arm64_str_fp(int opc, int rt, int rn, uint32_t uimm);
uint32_t arm64_ldp_fp(int opc, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post);
uint32_t arm64_stp_fp(int opc, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post);

// Helpers to build encode-immediate field from N/immr/imms bits
uint64_t arm64_encode_logic_imm(int sf, uint64_t val, int *N, int *immr, int *imms);

#endif // ARM64_ENC_H
