// SPDX-License-Identifier: LGPL-2.1-or-later
// x86-64 instruction encoder — variable-length encoding (AT&T syntax register names).
// Emits bytes directly into a SecBuf.
#ifndef X86_ENC_H
#define X86_ENC_H

#include "obj.h"
#include <stdint.h>
#include <stdbool.h>

// Register encoding (matches AT&T register order used by rcc)
// 64-bit registers
typedef enum {
    X86_RAX = 0,
    X86_RCX = 1,
    X86_RDX = 2,
    X86_RBX = 3,
    X86_RSP = 4,
    X86_RBP = 5,
    X86_RSI = 6,
    X86_RDI = 7,
    X86_R8 = 8,
    X86_R9 = 9,
    X86_R10 = 10,
    X86_R11 = 11,
    X86_R12 = 12,
    X86_R13 = 13,
    X86_R14 = 14,
    X86_R15 = 15,
    X86_RIP = 16,
    X86_NOREG = -1,
} X86Reg;

// XMM registers
typedef enum {
    X86_XMM0 = 0,
    X86_XMM1 = 1,
    X86_XMM2 = 2,
    X86_XMM3 = 3,
    X86_XMM4 = 4,
    X86_XMM5 = 5,
    X86_XMM6 = 6,
    X86_XMM7 = 7,
    X86_XMM8 = 8,
    X86_XMM9 = 9,
    X86_XMM10 = 10,
    X86_XMM11 = 11,
    X86_XMM12 = 12,
    X86_XMM13 = 13,
    X86_XMM14 = 14,
    X86_XMM15 = 15,
} X86XmmReg;

// Condition codes (for Jcc / SETcc / CMOVcc)
typedef enum {
    X86_O = 0,
    X86_NO = 1,
    X86_B = 2,
    X86_AE = 3,
    X86_E = 4,
    X86_NE = 5,
    X86_BE = 6,
    X86_A = 7,
    X86_S = 8,
    X86_NS = 9,
    X86_P = 10,
    X86_NP = 11,
    X86_L = 12,
    X86_GE = 13,
    X86_LE = 14,
    X86_G = 15,
    // aliases
    X86_C = 2,
    X86_NC = 3,
    X86_Z = 4,
    X86_NZ = 5,
    X86_NAE = 2,
    X86_NBE = 7,
    X86_NGE = 12,
    X86_NLE = 15,
} X86Cond;

// Memory operand: disp(base, index, scale)  index=-1 → no index
typedef struct {
    X86Reg base;
    X86Reg index;
    int scale; // 1,2,4,8
    int64_t disp;
} X86Mem;

static inline X86Mem x86_mem(X86Reg base, int64_t disp) {
    return (X86Mem){base, X86_NOREG, 1, disp};
}
static inline X86Mem x86_mem_idx(X86Reg base, X86Reg idx, int scale, int64_t disp) {
    return (X86Mem){base, idx, scale, disp};
}

// ---------------------------------------------------------------------------
// Integer instructions (size = 1/2/4/8 bytes)
// ---------------------------------------------------------------------------
void x86_mov_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_mov_ri(SecBuf *s, int size, X86Reg dst, int64_t imm);
void x86_mov_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);
void x86_mov_mr(SecBuf *s, int size, X86Mem dst, X86Reg src);
void x86_mov_mi(SecBuf *s, int size, X86Mem dst, int32_t imm);
void x86_or_mi(SecBuf *s, int size, X86Mem dst, int32_t imm);
void x86_cmp_mi(SecBuf *s, int size, X86Mem dst, int32_t imm);
void x86_movabs(SecBuf *s, X86Reg dst, uint64_t imm64); // 64-bit immediate
void x86_movsx(SecBuf *s, int dst_sz, int src_sz, X86Reg dst, X86Reg src);
void x86_movzx(SecBuf *s, int dst_sz, int src_sz, X86Reg dst, X86Reg src);
void x86_movsx_rm(SecBuf *s, int dst_sz, int src_sz, X86Reg dst, X86Mem src);
void x86_movzx_rm(SecBuf *s, int dst_sz, int src_sz, X86Reg dst, X86Mem src);

void x86_lea(SecBuf *s, int size, X86Reg dst, X86Mem src);

// Arithmetic
void x86_add_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_add_ri(SecBuf *s, int size, X86Reg dst, int32_t imm);
void x86_add_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);
void x86_sub_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_sub_ri(SecBuf *s, int size, X86Reg dst, int32_t imm);
void x86_sub_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);
void x86_imul_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_imul_rri(SecBuf *s, int size, X86Reg dst, X86Reg src, int32_t imm);
void x86_imul_r(SecBuf *s, int size, X86Reg src); // RDX:RAX = RAX*src
void x86_idiv_r(SecBuf *s, int size, X86Reg src);
void x86_div_r(SecBuf *s, int size, X86Reg src);
void x86_neg_r(SecBuf *s, int size, X86Reg r);
void x86_not_r(SecBuf *s, int size, X86Reg r);
void x86_inc_r(SecBuf *s, int size, X86Reg r);
void x86_dec_r(SecBuf *s, int size, X86Reg r);
void x86_cdq(SecBuf *s); // sign-extend EAX to EDX:EAX
void x86_cqo(SecBuf *s); // sign-extend RAX to RDX:RAX

// Logical
void x86_and_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_and_ri(SecBuf *s, int size, X86Reg dst, int32_t imm);
void x86_and_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);
void x86_or_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_or_ri(SecBuf *s, int size, X86Reg dst, int32_t imm);
void x86_or_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);
void x86_xor_rr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_xor_ri(SecBuf *s, int size, X86Reg dst, int32_t imm);
void x86_xor_rm(SecBuf *s, int size, X86Reg dst, X86Mem src);

// Shifts
void x86_shl_ri(SecBuf *s, int size, X86Reg r, uint8_t imm);
void x86_shr_ri(SecBuf *s, int size, X86Reg r, uint8_t imm);
void x86_sar_ri(SecBuf *s, int size, X86Reg r, uint8_t imm);
void x86_shl_rcl(SecBuf *s, int size, X86Reg r); // shift by CL
void x86_shr_rcl(SecBuf *s, int size, X86Reg r);
void x86_sar_rcl(SecBuf *s, int size, X86Reg r);
void x86_ror_ri(SecBuf *s, int size, X86Reg r, uint8_t imm);
void x86_rol_ri(SecBuf *s, int size, X86Reg r, uint8_t imm);

// Compare / test
void x86_cmp_rr(SecBuf *s, int size, X86Reg a, X86Reg b);
void x86_cmp_ri(SecBuf *s, int size, X86Reg a, int32_t imm);
void x86_cmp_rm(SecBuf *s, int size, X86Reg a, X86Mem b);
void x86_cmp_mr(SecBuf *s, int size, X86Mem a, X86Reg b);
void x86_test_rr(SecBuf *s, int size, X86Reg a, X86Reg b);
void x86_test_ri(SecBuf *s, int size, X86Reg a, int32_t imm);

// Set/conditional move
void x86_setcc(SecBuf *s, X86Cond cc, X86Reg dst);
void x86_cmovcc(SecBuf *s, int size, X86Cond cc, X86Reg dst, X86Reg src);

// Bit operations
void x86_bsf(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_bsr(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_popcnt(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_lzcnt(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_tzcnt(SecBuf *s, int size, X86Reg dst, X86Reg src);
void x86_bswap(SecBuf *s, int size, X86Reg r); // size=4 or 8

// Stack
void x86_push(SecBuf *s, X86Reg r);
void x86_pop(SecBuf *s, X86Reg r);
void x86_push_imm(SecBuf *s, int32_t imm);

// Control flow
void x86_call_rel32(SecBuf *s, int32_t rel32); // fills 0; caller adds reloc
void x86_call_r(SecBuf *s, X86Reg r);
void x86_jmp_rel32(SecBuf *s, int32_t rel32);
void x86_jmp_rel8(SecBuf *s, int8_t rel8);
void x86_jmp_r(SecBuf *s, X86Reg r);
void x86_jcc_rel32(SecBuf *s, X86Cond cc, int32_t rel32);
void x86_jcc_rel8(SecBuf *s, X86Cond cc, int8_t rel8);
void x86_ret(SecBuf *s);
void x86_leave(SecBuf *s);
void x86_nop(SecBuf *s);

// Misc
void x86_xchg_rr(SecBuf *s, int size, X86Reg a, X86Reg b);
void x86_lock_prefix(SecBuf *s);
void x86_rep_prefix(SecBuf *s);
void x86_repne_prefix(SecBuf *s);
void x86_cld(SecBuf *s);
void x86_mfence(SecBuf *s);
void x86_cpuid(SecBuf *s);

// SSE / FP
void x86_movsd_rr(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_movsd_rm(SecBuf *s, X86XmmReg dst, X86Mem src);
void x86_movsd_mr(SecBuf *s, X86Mem dst, X86XmmReg src);
void x86_movss_rr(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_movss_rm(SecBuf *s, X86XmmReg dst, X86Mem src);
void x86_movss_mr(SecBuf *s, X86Mem dst, X86XmmReg src);
void x86_addsd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_subsd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_mulsd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_divsd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_addss(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_subss(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_mulss(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_divss(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_movq_r_xmm(SecBuf *s, X86XmmReg d, X86Reg src);
void x86_movq_xmm_r(SecBuf *s, X86Reg d, X86XmmReg src);
void x86_ucomisd(SecBuf *s, X86XmmReg a, X86XmmReg b);
void x86_ucomiss(SecBuf *s, X86XmmReg a, X86XmmReg b);
void x86_comisd(SecBuf *s, X86XmmReg a, X86XmmReg b);
void x86_cvtsi2sd(SecBuf *s, int src_size, X86XmmReg dst, X86Reg src);
void x86_cvtsi2ss(SecBuf *s, int src_size, X86XmmReg dst, X86Reg src);
void x86_cvttsd2si(SecBuf *s, int dst_size, X86Reg dst, X86XmmReg src);
void x86_cvttss2si(SecBuf *s, int dst_size, X86Reg dst, X86XmmReg src);
void x86_cvtsd2ss(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_cvtss2sd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_xorpd(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_xorps(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_movaps(SecBuf *s, X86XmmReg dst, X86XmmReg src);
void x86_movaps_mr(SecBuf *s, X86Mem dst, X86XmmReg src);
void x86_pxor(SecBuf *s, X86XmmReg dst, X86XmmReg src);

// x87 (legacy, for long double)
void x86_fldl_m(SecBuf *s, X86Mem src);
void x86_fstpt_m(SecBuf *s, X86Mem dst);

#endif // X86_ENC_H
