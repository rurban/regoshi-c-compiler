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

// ARM64 physical register enum
typedef enum {
    ARM64_X0 = 0,
    ARM64_X1,
    ARM64_X2,
    ARM64_X3,
    ARM64_X4,
    ARM64_X5,
    ARM64_X6,
    ARM64_X7,
    ARM64_X8,
    ARM64_X9,
    ARM64_X10,
    ARM64_X11,
    ARM64_X12,
    ARM64_X13,
    ARM64_X14,
    ARM64_X15,
    ARM64_X16,
    ARM64_X17,
    ARM64_X18,
    ARM64_X19,
    ARM64_X20,
    ARM64_X21,
    ARM64_X22,
    ARM64_X23,
    ARM64_X24,
    ARM64_X25,
    ARM64_X26,
    ARM64_X27,
    ARM64_X28,
    ARM64_X29 = 29, // FP / X29
    ARM64_X30 = 30, // LR
    ARM64_X31 = 31, // SP or XZR depending on context
} Arm64Reg;

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
uint32_t arm64_movz(int sf, Arm64Reg rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_movk(int sf, Arm64Reg rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_movn(int sf, Arm64Reg rd, uint16_t imm16, uint16_t shift16);
uint32_t arm64_add_imm(int sf, Arm64Reg rd, Arm64Reg rn, int32_t imm12, uint16_t shift2);
uint32_t arm64_adds_imm(int sf, Arm64Reg rd, Arm64Reg rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_sub_imm(int sf, Arm64Reg rd, Arm64Reg rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_subs_imm(int sf, Arm64Reg rd, Arm64Reg rn, int32_t imm12, uint32_t shift2);
uint32_t arm64_and_imm(int sf, Arm64Reg rd, Arm64Reg rn, uint64_t imm_enc);
uint32_t arm64_orr_imm(int sf, Arm64Reg rd, Arm64Reg rn, uint64_t imm_enc);
uint32_t arm64_eor_imm(int sf, Arm64Reg rd, Arm64Reg rn, uint64_t imm_enc);
uint32_t arm64_ands_imm(int sf, Arm64Reg rd, Arm64Reg rn, uint64_t imm_enc);

// ---------------------------------------------------------------------------
// Data processing — register
// ---------------------------------------------------------------------------
uint32_t arm64_add_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_adds_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_sub_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_subs_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_and_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_orr_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_eor_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_ands_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_bic_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_mul(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm); // MADD xd,xn,xm,xzr
uint32_t arm64_smull(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm); // SMADDL
uint32_t arm64_umull(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm); // UMADDL
uint32_t arm64_smulh(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_umulh(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sdiv(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_udiv(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsl_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsr_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_asr_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_ror_reg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsl_imm(int sf, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_lsr_imm(int sf, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_asr_imm(int sf, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_neg(int sf, Arm64Reg rd, Arm64Reg rm);
uint32_t arm64_mvn(int sf, Arm64Reg rd, Arm64Reg rm, Arm64Shift sh, int imm6);
uint32_t arm64_clz(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_cls(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_rbit(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_rev(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_rev16(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_rev32(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtb(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxth(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtw(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_uxtb(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_uxth(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_ubfx(int sf, Arm64Reg rd, Arm64Reg rn, int lsb, int width);
uint32_t arm64_sbfx(int sf, Arm64Reg rd, Arm64Reg rn, int lsb, int width);
uint32_t arm64_csel(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Cond cond);
uint32_t arm64_csinc(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Cond cond);
uint32_t arm64_csneg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, Arm64Cond cond);
uint32_t arm64_cset(int sf, Arm64Reg rd, Arm64Cond cond);
uint32_t arm64_cneg(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Cond cond);
uint32_t arm64_adc(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sbc(int sf, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);

// ---------------------------------------------------------------------------
// PC-relative addressing
// ---------------------------------------------------------------------------
// These encode 0 offset; caller adds reloc
uint32_t arm64_adrp(Arm64Reg rd, int32_t page_imm);
uint32_t arm64_adr(Arm64Reg rd, int32_t imm);

// ---------------------------------------------------------------------------
// Load/Store
// ---------------------------------------------------------------------------
uint32_t arm64_ldr_imm(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9, bool pre);
uint32_t arm64_ldr_uoff(int sz, Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_ldrb_uoff(Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_ldrh_uoff(Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_ldrb_imm(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldrh_imm(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldrsb(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldrsh(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldrsw_imm(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldrsw_uoff(Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_str_imm(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9, bool pre);
uint32_t arm64_str_uoff(int sz, Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_strb_uoff(Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_strh_uoff(Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_strb_imm(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_strh_imm(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
// Pair: LDP/STP
uint32_t arm64_ldp(int sf, Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t imm7, bool pre, bool post);
uint32_t arm64_stp(int sf, Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t imm7, bool pre, bool post);
// Register offset
uint32_t arm64_ldr_reg(int sz, Arm64Reg rt, Arm64Reg rn, Arm64Reg rm, bool ext, int s);
uint32_t arm64_str_reg(int sz, Arm64Reg rt, Arm64Reg rn, Arm64Reg rm, bool ext, int s);
// Unscaled immediate (LDUR/STUR)
uint32_t arm64_ldur(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldurb(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_ldurh(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_stur(int sf, Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_sturb(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
uint32_t arm64_sturh(Arm64Reg rt, Arm64Reg rn, int32_t imm9);
// Load-exclusive / store-exclusive
uint32_t arm64_ldxr(int sf, Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_ldxrb(Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_ldxrh(Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stxr(int sf, Arm64Reg rs, Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stxrb(Arm64Reg rs, Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stxrh(Arm64Reg rs, Arm64Reg rt, Arm64Reg rn);
// Load/store acquire-release
uint32_t arm64_ldar(int sf, Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_ldarb(Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_ldarh(Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stlr(int sf, Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stlrb(Arm64Reg rt, Arm64Reg rn);
uint32_t arm64_stlrh(Arm64Reg rt, Arm64Reg rn);

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------
// imm26 / imm19 in instruction units (4 bytes each); caller scales
uint32_t arm64_b(int32_t imm26);
uint32_t arm64_bl(int32_t imm26);
uint32_t arm64_br(Arm64Reg rn);
uint32_t arm64_blr(Arm64Reg rn);
uint32_t arm64_ret(Arm64Reg rn); // rn=30 for normal return
uint32_t arm64_bcond(Arm64Cond cond, int32_t imm19);
uint32_t arm64_cbz(int sf, Arm64Reg rt, int32_t imm19);
uint32_t arm64_cbnz(int sf, Arm64Reg rt, int32_t imm19);
uint32_t arm64_tbz(Arm64Reg rt, int imm6, int32_t imm14);
uint32_t arm64_tbnz(Arm64Reg rt, int imm6, int32_t imm14);

// ---------------------------------------------------------------------------
// System / Misc
// ---------------------------------------------------------------------------
uint32_t arm64_nop(void);
uint32_t arm64_dmb(int opt); // opt=0xb=ish
uint32_t arm64_dsb(int opt);
uint32_t arm64_isb(void);
uint32_t arm64_prfm_imm(int prfop, Arm64Reg rn, uint32_t uimm);

// ---------------------------------------------------------------------------
// FP / SIMD
// ---------------------------------------------------------------------------
uint32_t arm64_fmov_f2i(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fmov_i2f(int sf, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fmov_imm(int ftype, Arm64Reg rd, uint8_t imm8);
uint32_t arm64_fadd(int ftype, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_fsub(int ftype, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_fmul(int ftype, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_fdiv(int ftype, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_fneg(int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fabs(int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fcmp(int ftype, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_fcvt(int opc, int ftype, Arm64Reg rd, Arm64Reg rn); // ftype→opc conversion
uint32_t arm64_scvtf(int sf, int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_ucvtf(int sf, int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fcvtzs(int sf, int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_fcvtzu(int sf, int ftype, Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_ldr_fp(int opc, Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_str_fp(int opc, Arm64Reg rt, Arm64Reg rn, uint32_t uimm);
uint32_t arm64_ldp_fp(int opc, Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t imm7, bool pre, bool post);
uint32_t arm64_stp_fp(int opc, Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t imm7, bool pre, bool post);

// Helpers to build encode-immediate field from N/immr/imms bits
uint64_t arm64_encode_logic_imm(int sf, uint64_t val, int *N, int *immr, int *imms);

#endif // ARM64_ENC_H
