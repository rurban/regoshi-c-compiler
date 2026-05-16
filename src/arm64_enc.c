// SPDX-License-Identifier: LGPL-2.1-or-later
// AArch64 instruction encoder.  All instructions are 32-bit fixed-width.
// Reference: Arm Architecture Reference Manual for A-profile architecture.
#include "rcc.h"
#ifdef ARCH_ARM64
#include "arm64_enc.h"
#include <assert.h>
#include <stdint.h>

// Bit-field helpers
#define BIT(n)         (1u << (n))
#define BITS(hi, lo, v)  (((uint32_t)(v) & ((1u<<((hi)-(lo)+1))-1u)) << (lo))
#define SF(n)          BITS(31,31,(n))
#define OPC(hi, lo, v)   BITS(hi,lo,v)

// ---------------------------------------------------------------------------
// Logical immediate encoding helper
// Based on ARM reference: an immediate is a replicated bitmask.
// Returns 0 if not encodable, else packs N:immr:imms into bits 22:10.
// ---------------------------------------------------------------------------
static bool try_encode_logic_imm(int sf, uint64_t val,
                                 int *N_out, int *immr_out, int *imms_out) {
    if (val == 0 || val == (uint64_t)-1) return false;
    int len;
    uint64_t mask;
    // Try each rotation length (2,4,8,16,32,64)
    for (len = 1; len <= 6; len++) {
        int e = 1 << len; // element size
        if (e > 64) break;
        mask = (e == 64) ? (uint64_t)-1 : ((uint64_t)1 << e) - 1;
        uint64_t elem = val & mask;
        // Check if val is a repetition of elem
        bool ok = true;
        for (int s = e; s < 64; s += e) {
            if (((val >> s) & mask) != elem) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        // elem must be a contiguous run of 1s (possibly rotated)
        if (elem == 0 || (elem & mask) == mask) continue;
        // Find the rotation
        // Count trailing zeros of ~elem to find where 0-run starts
        int ctz_inv = 0;
        uint64_t t = (~elem) & mask;
        while (t & 1) {
            ctz_inv++;
            t >>= 1;
        }
        // Rotation amount = ctz_inv
        int immr = ctz_inv % e;
        // Rotate elem right by immr so the 1-run starts at bit 0
        uint64_t rotated = ((elem >> immr) | (elem << (e - immr))) & mask;
        int cto = 0; // count trailing ones of rotated element
        t = rotated;
        while (t & 1) {
            cto++;
            t >>= 1;
        }
        int imms = cto - 1;
        int N = (e == 64) ? 1 : 0;
        if (!sf && e == 64) return false;
        *N_out = N;
        *immr_out = immr;
        *imms_out = imms | ((e == 64) ? 0 : ((~(e - 1) << 1) & 0x3f));
        return true;
    }
    return false;
}

// Encode a logic immediate; returns 0 if not representable.
uint64_t arm64_encode_logic_imm(int sf, uint64_t val, int *N, int *immr, int *imms) {
    if (!try_encode_logic_imm(sf, val, N, immr, imms)) return 0;
    return 1;
}

static uint32_t logic_imm_field(int sf, uint64_t val) {
    int N = 0, immr = 0, imms = 0;
    (void)try_encode_logic_imm(sf, val, &N, &immr, &imms);
    return BITS(22, 22, N) | BITS(21, 16, immr) | BITS(15, 10, imms);
}

// ---------------------------------------------------------------------------
// Data processing — immediate
// ---------------------------------------------------------------------------

uint32_t arm64_movz(int sf, int rd, uint16_t imm16, uint16_t shift16) {
    assert(shift16 == 0 || shift16 == 16 || shift16 == 32 || shift16 == 48);
    return SF(sf) | 0xd2800000u | BITS(22, 21, shift16 / 16) | BITS(20, 5, imm16) | BITS(4, 0, rd);
}

uint32_t arm64_movk(int sf, int rd, uint16_t imm16, uint16_t shift16) {
    return SF(sf) | 0xf2800000u | BITS(22, 21, shift16 / 16) | BITS(20, 5, imm16) | BITS(4, 0, rd);
}

uint32_t arm64_movn(int sf, int rd, uint16_t imm16, uint16_t shift16) {
    return SF(sf) | 0x92800000u | BITS(22, 21, shift16 / 16) | BITS(20, 5, imm16) | BITS(4, 0, rd);
}

uint32_t arm64_add_imm(int sf, int rd, int rn, int32_t imm12, uint16_t sh) {
    assert(imm12 >= 0 && imm12 < 4096);
    return SF(sf) | 0x11000000u | BITS(23, 22, sh) | BITS(21, 10, imm12) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_adds_imm(int sf, int rd, int rn, int32_t imm12, uint32_t sh) {
    return SF(sf) | 0x31000000u | BITS(23, 22, sh) | BITS(21, 10, imm12) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_sub_imm(int sf, int rd, int rn, int32_t imm12, uint32_t sh) {
    assert(imm12 >= 0 && imm12 < 4096);
    return SF(sf) | 0x51000000u | BITS(23, 22, sh) | BITS(21, 10, imm12) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_subs_imm(int sf, int rd, int rn, int32_t imm12, uint32_t sh) {
    return SF(sf) | 0x71000000u | BITS(23, 22, sh) | BITS(21, 10, imm12) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_and_imm(int sf, int rd, int rn, uint64_t val) {
    return SF(sf) | 0x12000000u | logic_imm_field(sf, val) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_orr_imm(int sf, int rd, int rn, uint64_t val) {
    return SF(sf) | 0x32000000u | logic_imm_field(sf, val) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_eor_imm(int sf, int rd, int rn, uint64_t val) {
    return SF(sf) | 0x52000000u | logic_imm_field(sf, val) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_ands_imm(int sf, int rd, int rn, uint64_t val) {
    return SF(sf) | 0x72000000u | logic_imm_field(sf, val) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// ---------------------------------------------------------------------------
// Data processing — register (shifted register)
// ---------------------------------------------------------------------------
static uint32_t dp_reg(int sf, uint32_t opc, int rd, int rn, int rm,
                       Arm64Shift sh, int imm6) {
    return SF(sf) | opc | BITS(23, 22, sh) | BITS(20, 16, rm) | BITS(15, 10, imm6) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_add_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x0b000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_adds_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x2b000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_sub_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x4b000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_subs_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x6b000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_and_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x0a000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_orr_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x2a000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_eor_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x4a000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_ands_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x6a000000u, rd, rn, rm, sh, i6);
}
uint32_t arm64_bic_reg(int sf, int rd, int rn, int rm, Arm64Shift sh, int i6) {
    return dp_reg(sf, 0x0a200000u, rd, rn, rm, sh, i6);
}

// 3-register
static uint32_t dp3(int sf, uint32_t op, int rd, int rn, int rm, int ra) {
    return SF(sf) | op | BITS(20, 16, rm) | BITS(14, 10, ra) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

uint32_t arm64_mul(int sf, int rd, int rn, int rm) {
    return dp3(sf, 0x1b000000u, rd, rn, rm, ARM64_XZR);
}
uint32_t arm64_smull(int rd, int rn, int rm) {
    return 0x9b200000u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_umull(int rd, int rn, int rm) {
    return 0x9ba00000u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_smulh(int rd, int rn, int rm) {
    return 0x9b407c00u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_umulh(int rd, int rn, int rm) {
    return 0x9bc07c00u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_sdiv(int sf, int rd, int rn, int rm) {
    return SF(sf) | 0x1ac00c00u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_udiv(int sf, int rd, int rn, int rm) {
    return SF(sf) | 0x1ac00800u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// Shifts
static uint32_t dp2(int sf, uint32_t op, int rd, int rn, int rm) {
    return SF(sf) | op | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_lsl_reg(int sf, int rd, int rn, int rm) { return dp2(sf, 0x1ac02000u, rd, rn, rm); }
uint32_t arm64_lsr_reg(int sf, int rd, int rn, int rm) { return dp2(sf, 0x1ac02400u, rd, rn, rm); }
uint32_t arm64_asr_reg(int sf, int rd, int rn, int rm) { return dp2(sf, 0x1ac02800u, rd, rn, rm); }
uint32_t arm64_ror_reg(int sf, int rd, int rn, int rm) { return dp2(sf, 0x1ac02c00u, rd, rn, rm); }

// Immediate shifts use UBFM/SBFM with appropriate fields
// LSL #shift: UBFM rd, rn, #(-shift mod bitwidth), #(bitwidth-1-shift)
uint32_t arm64_lsl_imm(int sf, int rd, int rn, int shift) {
    int w = sf ? 64 : 32;
    int immr = (-shift) & (w - 1);
    int imms = w - 1 - shift;
    return SF(sf) | BITS(29, 23, sf ? 0x66 : 0x26) | BITS(21, 16, immr) | BITS(15, 10, imms) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_lsr_imm(int sf, int rd, int rn, int shift) {
    int w = sf ? 64 : 32;
    int imms = w - 1;
    return SF(sf) | BITS(29, 23, sf ? 0x66 : 0x26) | BITS(21, 16, shift) | BITS(15, 10, imms) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_asr_imm(int sf, int rd, int rn, int shift) {
    int w = sf ? 64 : 32;
    int imms = w - 1;
    return SF(sf) | BITS(29, 23, sf ? 0x7e : 0x1e) | BITS(21, 16, shift) | BITS(15, 10, imms) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// Unary operations
uint32_t arm64_neg(int sf, int rd, int rm) {
    return arm64_sub_reg(sf, rd, ARM64_XZR, rm, ARM64_LSL, 0);
}
uint32_t arm64_mvn(int sf, int rd, int rm, Arm64Shift sh, int imm6) {
    return dp_reg(sf, 0x2a200000u, rd, ARM64_XZR, rm, sh, imm6);
}

// Bit counting
static uint32_t dp1(int sf, uint32_t op, int rd, int rn) {
    return SF(sf) | op | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_clz(int sf, int rd, int rn) { return dp1(sf, 0x5ac01000u, rd, rn); }
uint32_t arm64_cls(int sf, int rd, int rn) { return dp1(sf, 0x5ac01400u, rd, rn); }
uint32_t arm64_rbit(int sf, int rd, int rn) { return dp1(sf, 0x5ac00000u, rd, rn); }
uint32_t arm64_rev(int sf, int rd, int rn) {
    uint32_t opc = sf ? 0x5ac00c00u : 0x5ac00800u;
    return dp1(sf, opc, rd, rn);
}
uint32_t arm64_rev16(int sf, int rd, int rn) { return dp1(sf, 0x5ac00400u, rd, rn); }
uint32_t arm64_rev32(int rd, int rn) { return 0xdac00800u | BITS(9, 5, rn) | BITS(4, 0, rd); }

// Sign/zero extend: use SBFM/UBFM
uint32_t arm64_sxtb(int sf, int rd, int rn) { return SF(sf) | (sf ? 0x93401c00u : 0x13001c00u) | BITS(9, 5, rn) | BITS(4, 0, rd); }
uint32_t arm64_sxth(int sf, int rd, int rn) { return SF(sf) | (sf ? 0x93403c00u : 0x13003c00u) | BITS(9, 5, rn) | BITS(4, 0, rd); }
uint32_t arm64_sxtw(int rd, int rn) { return 0x93407c00u | BITS(9, 5, rn) | BITS(4, 0, rd); }
uint32_t arm64_uxtb(int rd, int rn) { return 0x53001c00u | BITS(9, 5, rn) | BITS(4, 0, rd); }
uint32_t arm64_uxth(int rd, int rn) { return 0x53003c00u | BITS(9, 5, rn) | BITS(4, 0, rd); }

// Bitfield extract
uint32_t arm64_ubfx(int sf, int rd, int rn, int lsb, int width) {
    int imms = lsb + width - 1;
    return SF(sf) | (sf ? 0xd3400000u : 0x53000000u) | BITS(21, 16, lsb) | BITS(15, 10, imms) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_sbfx(int sf, int rd, int rn, int lsb, int width) {
    int imms = lsb + width - 1;
    return SF(sf) | (sf ? 0x93400000u : 0x13000000u) | BITS(21, 16, lsb) | BITS(15, 10, imms) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// Conditional select
uint32_t arm64_csel(int sf, int rd, int rn, int rm, Arm64Cond c) {
    return SF(sf) | 0x1a800000u | BITS(20, 16, rm) | BITS(15, 12, c) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_csinc(int sf, int rd, int rn, int rm, Arm64Cond c) {
    return SF(sf) | 0x1a800400u | BITS(20, 16, rm) | BITS(15, 12, c) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_csneg(int sf, int rd, int rn, int rm, Arm64Cond c) {
    return SF(sf) | 0x5a800400u | BITS(20, 16, rm) | BITS(15, 12, c) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_cset(int sf, int rd, Arm64Cond c) {
    // CSET = CSINC rd, xzr, xzr, invert(c)
    return arm64_csinc(sf, rd, ARM64_XZR, ARM64_XZR, c ^ 1);
}
uint32_t arm64_cneg(int sf, int rd, int rn, Arm64Cond c) {
    return arm64_csneg(sf, rd, rn, rn, c ^ 1);
}
uint32_t arm64_adc(int sf, int rd, int rn, int rm) {
    return SF(sf) | 0x1a000000u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_sbc(int sf, int rd, int rn, int rm) {
    return SF(sf) | 0x5a000000u | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// ---------------------------------------------------------------------------
// PC-relative addressing
// ---------------------------------------------------------------------------
uint32_t arm64_adrp(int rd, int32_t page_imm) {
    uint32_t immlo = (uint32_t)(page_imm & 3);
    uint32_t immhi = (uint32_t)((page_imm >> 2) & 0x7ffff);
    return 0x90000000u | BITS(30, 29, immlo) | BITS(23, 5, immhi) | BITS(4, 0, rd);
}
uint32_t arm64_adr(int rd, int32_t imm) {
    uint32_t immlo = (uint32_t)(imm & 3);
    uint32_t immhi = (uint32_t)((imm >> 2) & 0x7ffff);
    return 0x10000000u | BITS(30, 29, immlo) | BITS(23, 5, immhi) | BITS(4, 0, rd);
}

// ---------------------------------------------------------------------------
// Load/Store
// ---------------------------------------------------------------------------
// Unsigned offset load: size=0→byte,1→half,2→word,3→dword + sf for x-regs
uint32_t arm64_ldr_uoff(int sz, int rt, int rn, uint32_t uimm) {
    // sz: 0=8bit,1=16bit,2=32bit,3=64bit
    uint32_t opc = (sz == 3) ? 0xf9400000u : (sz == 2) ? 0xb9400000u
        : (sz == 1)                                    ? 0x79400000u
                                                       : 0x39400000u;
    return opc | BITS(21, 10, uimm) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldrb_uoff(int rt, int rn, uint32_t uimm) { return arm64_ldr_uoff(0, rt, rn, uimm); }
uint32_t arm64_ldrh_uoff(int rt, int rn, uint32_t uimm) { return arm64_ldr_uoff(1, rt, rn, uimm); }

uint32_t arm64_str_uoff(int sz, int rt, int rn, uint32_t uimm) {
    uint32_t opc = (sz == 3) ? 0xf9000000u : (sz == 2) ? 0xb9000000u
        : (sz == 1)                                    ? 0x79000000u
                                                       : 0x39000000u;
    return opc | BITS(21, 10, uimm) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_strb_uoff(int rt, int rn, uint32_t uimm) { return arm64_str_uoff(0, rt, rn, uimm); }
uint32_t arm64_strh_uoff(int rt, int rn, uint32_t uimm) { return arm64_str_uoff(1, rt, rn, uimm); }

// Pre/post-index (9-bit signed immediate)
static uint32_t ldr_imm9(uint32_t opc, int rt, int rn, int32_t imm9, bool pre) {
    uint32_t idx = pre ? 0xc00u : 0x400u;
    return opc | idx | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldr_imm(int sf, int rt, int rn, int32_t imm9, bool pre) {
    return ldr_imm9(sf ? 0xf8400000u : 0xb8400000u, rt, rn, imm9, pre);
}
uint32_t arm64_ldrb_imm(int rt, int rn, int32_t imm9) {
    return ldr_imm9(0x38400000u, rt, rn, imm9, false);
}
uint32_t arm64_ldrh_imm(int rt, int rn, int32_t imm9) {
    return ldr_imm9(0x78400000u, rt, rn, imm9, false);
}
uint32_t arm64_ldrsb(int sf, int rt, int rn, int32_t imm9) {
    uint32_t opc = sf ? 0x38800000u : 0x38c00000u;
    return ldr_imm9(opc, rt, rn, imm9, false);
}
uint32_t arm64_ldrsh(int sf, int rt, int rn, int32_t imm9) {
    uint32_t opc = sf ? 0x78800000u : 0x78c00000u;
    return ldr_imm9(opc, rt, rn, imm9, false);
}
uint32_t arm64_ldrsw_imm(int rt, int rn, int32_t imm9) {
    return ldr_imm9(0xb8800000u, rt, rn, imm9, false);
}
uint32_t arm64_ldrsw_uoff(int rt, int rn, uint32_t uimm) {
    return 0xb9800000u | BITS(21, 10, uimm) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_str_imm(int sf, int rt, int rn, int32_t imm9, bool pre) {
    return ldr_imm9(sf ? 0xf8000000u : 0xb8000000u, rt, rn, imm9, pre);
}
uint32_t arm64_strb_imm(int rt, int rn, int32_t imm9) {
    return ldr_imm9(0x38000000u, rt, rn, imm9, false);
}
uint32_t arm64_strh_imm(int rt, int rn, int32_t imm9) {
    return ldr_imm9(0x78000000u, rt, rn, imm9, false);
}

// Unscaled (LDUR/STUR)
uint32_t arm64_ldur(int sf, int rt, int rn, int32_t imm9) {
    return (sf ? 0xf8400000u : 0xb8400000u) | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldurb(int rt, int rn, int32_t imm9) {
    return 0x38400000u | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldurh(int rt, int rn, int32_t imm9) {
    return 0x78400000u | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_stur(int sf, int rt, int rn, int32_t imm9) {
    return (sf ? 0xf8000000u : 0xb8000000u) | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_sturb(int rt, int rn, int32_t imm9) {
    return 0x38000000u | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_sturh(int rt, int rn, int32_t imm9) {
    return 0x78000000u | BITS(20, 12, (uint32_t)imm9 & 0x1ff) | BITS(9, 5, rn) | BITS(4, 0, rt);
}

// Register offset
uint32_t arm64_ldr_reg(int sz, int rt, int rn, int rm, bool ext, int s) {
    uint32_t opc = (sz == 3) ? 0xf8600800u : (sz == 2) ? 0xb8600800u
        : (sz == 1)                                    ? 0x78600800u
                                                       : 0x38600800u;
    return opc | BITS(20, 16, rm) | BITS(15, 13, ext ? 6 : 3) | BITS(12, 12, s) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_str_reg(int sz, int rt, int rn, int rm, bool ext, int s) {
    uint32_t opc = (sz == 3) ? 0xf8200800u : (sz == 2) ? 0xb8200800u
        : (sz == 1)                                    ? 0x78200800u
                                                       : 0x38200800u;
    return opc | BITS(20, 16, rm) | BITS(15, 13, ext ? 6 : 3) | BITS(12, 12, s) | BITS(9, 5, rn) | BITS(4, 0, rt);
}

// Load/store pair
static uint32_t ldp_stp(uint32_t base, int rt1, int rt2, int rn, int32_t imm7,
                        bool pre, bool post) {
    uint32_t mode = pre ? 0x01800000u : post ? 0x00800000u
                                             : 0x01000000u;
    return base | mode | BITS(21, 15, (uint32_t)imm7 & 0x7f) | BITS(14, 10, rt2) | BITS(9, 5, rn) | BITS(4, 0, rt1);
}
uint32_t arm64_ldp(int sf, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post) {
    return ldp_stp(sf ? 0xa8400000u : 0x28400000u, rt1, rt2, rn, imm7, pre, post);
}
uint32_t arm64_stp(int sf, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post) {
    return ldp_stp(sf ? 0xa8000000u : 0x28000000u, rt1, rt2, rn, imm7, pre, post);
}

// Exclusive
uint32_t arm64_ldxr(int sf, int rt, int rn) {
    return (sf ? 0xc85f7c00u : 0x885f7c00u) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldxrb(int rt, int rn) { return 0x085f7c00u | BITS(9, 5, rn) | BITS(4, 0, rt); }
uint32_t arm64_ldxrh(int rt, int rn) { return 0x485f7c00u | BITS(9, 5, rn) | BITS(4, 0, rt); }
uint32_t arm64_stxr(int sf, int rs, int rt, int rn) {
    return (sf ? 0xc8007c00u : 0x88007c00u) | BITS(20, 16, rs) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_stxrb(int rs, int rt, int rn) {
    return 0x08007c00u | BITS(20, 16, rs) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_stxrh(int rs, int rt, int rn) {
    return 0x48007c00u | BITS(20, 16, rs) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldar(int sf, int rt, int rn) {
    return (sf ? 0xc8dffc00u : 0x88dffc00u) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_ldarb(int rt, int rn) { return 0x08dffc00u | BITS(9, 5, rn) | BITS(4, 0, rt); }
uint32_t arm64_ldarh(int rt, int rn) { return 0x48dffc00u | BITS(9, 5, rn) | BITS(4, 0, rt); }
uint32_t arm64_stlr(int sf, int rt, int rn) {
    return (sf ? 0xc89ffc00u : 0x889ffc00u) | BITS(9, 5, rn) | BITS(4, 0, rt);
}
uint32_t arm64_stlrb(int rt, int rn) { return 0x089ffc00u | BITS(9, 5, rn) | BITS(4, 0, rt); }
uint32_t arm64_stlrh(int rt, int rn) { return 0x489ffc00u | BITS(9, 5, rn) | BITS(4, 0, rt); }

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------
uint32_t arm64_b(int32_t imm26) {
    return 0x14000000u | ((uint32_t)imm26 & 0x3ffffffu);
}
uint32_t arm64_bl(int32_t imm26) {
    return 0x94000000u | ((uint32_t)imm26 & 0x3ffffffu);
}
uint32_t arm64_br(int rn) { return 0xd61f0000u | BITS(9, 5, rn); }
uint32_t arm64_blr(int rn) { return 0xd63f0000u | BITS(9, 5, rn); }
uint32_t arm64_ret(int rn) { return 0xd65f0000u | BITS(9, 5, rn); }
uint32_t arm64_bcond(Arm64Cond cond, int32_t imm19) {
    return 0x54000000u | BITS(23, 5, (uint32_t)imm19 & 0x7ffff) | BITS(3, 0, cond);
}
uint32_t arm64_cbz(int sf, int rt, int32_t imm19) {
    return SF(sf) | 0x34000000u | BITS(23, 5, (uint32_t)imm19 & 0x7ffff) | BITS(4, 0, rt);
}
uint32_t arm64_cbnz(int sf, int rt, int32_t imm19) {
    return SF(sf) | 0x35000000u | BITS(23, 5, (uint32_t)imm19 & 0x7ffff) | BITS(4, 0, rt);
}
uint32_t arm64_tbz(int rt, int imm6, int32_t imm14) {
    uint32_t b5 = (uint32_t)(imm6 >> 5) & 1;
    uint32_t b40 = (uint32_t)imm6 & 0x1f;
    return 0x36000000u | (b5 << 31) | BITS(23, 19, b40) | BITS(18, 5, (uint32_t)imm14 & 0x3fff) | BITS(4, 0, rt);
}
uint32_t arm64_tbnz(int rt, int imm6, int32_t imm14) {
    uint32_t b5 = (uint32_t)(imm6 >> 5) & 1;
    uint32_t b40 = (uint32_t)imm6 & 0x1f;
    return 0x37000000u | (b5 << 31) | BITS(23, 19, b40) | BITS(18, 5, (uint32_t)imm14 & 0x3fff) | BITS(4, 0, rt);
}

// ---------------------------------------------------------------------------
// System / Misc
// ---------------------------------------------------------------------------
uint32_t arm64_nop(void) { return 0xd503201fu; }
uint32_t arm64_dmb(int opt) { return 0xd50330bfu | BITS(11, 8, opt); }
uint32_t arm64_dsb(int opt) { return 0xd503309fu | BITS(11, 8, opt); }
uint32_t arm64_isb(void) { return 0xd5033fdfu; }
uint32_t arm64_prfm_imm(int prfop, int rn, uint32_t uimm) {
    return 0xf9800000u | BITS(21, 10, uimm) | BITS(9, 5, rn) | BITS(4, 0, prfop);
}

// ---------------------------------------------------------------------------
// FP / SIMD
// ---------------------------------------------------------------------------
// ftype: 0=single, 1=double (most common for rcc)
uint32_t arm64_fmov_f2i(int sf, int rd, int rn) {
    uint32_t ftype = sf ? 1u : 0u;
    return SF(sf) | 0x1e260000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fmov_i2f(int sf, int rd, int rn) {
    uint32_t ftype = sf ? 1u : 0u;
    return SF(sf) | 0x1e270000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fmov_imm(int ftype, int rd, uint8_t imm8) {
    return 0x1e201000u | BITS(23, 22, ftype) | BITS(20, 13, imm8) | BITS(4, 0, rd);
}
static uint32_t fp3(int ftype, uint32_t op, int rd, int rn, int rm) {
    return 0x1e200000u | BITS(23, 22, ftype) | op | BITS(20, 16, rm) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fadd(int ft, int rd, int rn, int rm) { return fp3(ft, 0x002800u, rd, rn, rm); }
uint32_t arm64_fsub(int ft, int rd, int rn, int rm) { return fp3(ft, 0x003800u, rd, rn, rm); }
uint32_t arm64_fmul(int ft, int rd, int rn, int rm) { return fp3(ft, 0x000800u, rd, rn, rm); }
uint32_t arm64_fdiv(int ft, int rd, int rn, int rm) { return fp3(ft, 0x001800u, rd, rn, rm); }
uint32_t arm64_fneg(int ft, int rd, int rn) {
    return 0x1e214000u | BITS(23, 22, ft) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fabs(int ft, int rd, int rn) {
    return 0x1e20c000u | BITS(23, 22, ft) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fcmp(int ft, int rn, int rm) {
    return 0x1e202000u | BITS(23, 22, ft) | BITS(20, 16, rm) | BITS(9, 5, rn);
}
uint32_t arm64_fcvt(int opc, int ftype, int rd, int rn) {
    return 0x1e224000u | BITS(23, 22, ftype) | BITS(16, 15, opc) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_scvtf(int sf, int ftype, int rd, int rn) {
    return SF(sf) | 0x1e220000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_ucvtf(int sf, int ftype, int rd, int rn) {
    return SF(sf) | 0x1e230000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fcvtzs(int sf, int ftype, int rd, int rn) {
    return SF(sf) | 0x1e380000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}
uint32_t arm64_fcvtzu(int sf, int ftype, int rd, int rn) {
    return SF(sf) | 0x1e390000u | BITS(23, 22, ftype) | BITS(9, 5, rn) | BITS(4, 0, rd);
}

// FP load/store unsigned offset: sz=2→S(32bit), sz=3→D(64bit), sz=4→Q(128bit)
// LDR S: 0xBD400000, LDR D: 0xFD400000, LDR Q: 0x3DC00000
// uimm is byte offset; auto-scaled to element size
uint32_t arm64_ldr_fp(int sz, int rt, int rn, uint32_t uimm) {
    switch (sz) {
    case 2: return 0xBD400000u | BITS(21, 10, uimm / 4) | BITS(9, 5, rn) | BITS(4, 0, rt);
    case 3: return 0xFD400000u | BITS(21, 10, uimm / 8) | BITS(9, 5, rn) | BITS(4, 0, rt);
    default: return 0x3DC00000u | BITS(21, 10, uimm / 16) | BITS(9, 5, rn) | BITS(4, 0, rt);
    }
}
uint32_t arm64_str_fp(int sz, int rt, int rn, uint32_t uimm) {
    switch (sz) {
    case 2: return 0xBD000000u | BITS(21, 10, uimm / 4) | BITS(9, 5, rn) | BITS(4, 0, rt);
    case 3: return 0xFD000000u | BITS(21, 10, uimm / 8) | BITS(9, 5, rn) | BITS(4, 0, rt);
    default: return 0x3D800000u | BITS(21, 10, uimm / 16) | BITS(9, 5, rn) | BITS(4, 0, rt);
    }
}
uint32_t arm64_ldp_fp(int opc, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post) {
    uint32_t base = 0x2c400000u | BITS(31, 30, opc);
    return ldp_stp(base, rt1, rt2, rn, imm7, pre, post);
}
uint32_t arm64_stp_fp(int opc, int rt1, int rt2, int rn, int32_t imm7, bool pre, bool post) {
    uint32_t base = 0x2c000000u | BITS(31, 30, opc);
    return ldp_stp(base, rt1, rt2, rn, imm7, pre, post);
}
#endif /* ARCH_ARM64 */
