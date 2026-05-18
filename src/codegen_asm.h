// SPDX-License-Identifier: LGPL-2.1-or-later
// Codegen ASM wrappers — emit assembled bytes to SecBuf via asm_* functions.
// Replaces printf-based text emission in codegen.c.
// Uses arm64_enc.h / x86_enc.h encoder functions under the hood.
#ifndef CODEGEN_ASM_H
#define CODEGEN_ASM_H

#include "obj.h"
#ifdef ARCH_ARM64
#include "arm64_enc.h"
#else
#include "x86_enc.h"
#endif
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ============================================================================
// Register index → physical register number mapping
// codegen allocates regs as indices 0..NUM_REGS-1 into reg64[].
// The encoder functions use physical register numbers.
// ============================================================================

#ifdef ARCH_ARM64
static const int cg_arm_reg[12] = {10, 11, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24};
#define CG_ARM_REG(r)  ((r) < 0 ? -(r) : (r) < 12 ? cg_arm_reg[r] : (r))
#define CG_ARM_FP      29
#define CG_ARM_LR      30
#define CG_ARM_SP      31
#define CG_ARM_XZR     31
#else
static const int cg_x86_reg[8] = {X86_R10, X86_R11, X86_RBX, X86_R12,
                                  X86_R13, X86_R14, X86_R15, X86_RSI};
#define CG_X86_REG(r)  ((X86Reg)(cg_x86_reg[r]))
#define CG_X86_FP      X86_RBP
#define CG_X86_SP      X86_RSP
#endif

// Virtual register index (before CG_ARM_REG / CG_X86_REG mapping)
typedef enum {
    R_V0 = 0,
    R_V1,
    R_V2,
    R_V3,
    R_V4,
    R_V5,
    R_V6,
    R_V7,
    R_V8,
    R_V9,
    R_V10,
    R_V11
} VReg;

// Dry-run guard: skip emission when cg_sec is NULL
#define EMIT_GUARD if (!s || !s->data) return 0;

// ============================================================================
// Label + branch fixup hashtables (chaining, FNV-1a hash)
// ============================================================================

#define CG_HT_SIZE 4096 // power of 2

static uint32_t cg_ht_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (; *name; name++) {
        h ^= (uint8_t)*name;
        h *= 16777619;
    }
    return h & (CG_HT_SIZE - 1);
}

// Label hashtable: name -> offset
typedef struct CgLabelNode {
    const char *name;
    size_t offset;
    struct CgLabelNode *next;
} CgLabelNode;

static CgLabelNode *cg_label_htab[CG_HT_SIZE];
static int cg_label_count = 0;

static void cg_label_ht_reset(void) {
    memset(cg_label_htab, 0, sizeof(cg_label_htab));
    cg_label_count = 0;
}

static void cg_label_ht_add(const char *name, size_t offset) {
    uint32_t h = cg_ht_hash(name);
    for (CgLabelNode *n = cg_label_htab[h]; n; n = n->next) {
        if (strcmp(n->name, name) == 0) {
            n->offset = offset;
            return;
        }
    }
    CgLabelNode *n = arena_alloc(sizeof(CgLabelNode));
    n->name = name;
    n->offset = offset;
    n->next = cg_label_htab[h];
    cg_label_htab[h] = n;
    cg_label_count++;
}

static size_t cg_label_ht_get(const char *name) {
    if (!name) return (size_t)-1;
    uint32_t h = cg_ht_hash(name);
    for (CgLabelNode *n = cg_label_htab[h]; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return n->offset;
    return (size_t)-1;
}

// Fixup hashtable: bucketed by label hash
typedef struct AsmFixupNode {
    size_t instr_off;
    const char *label;
    int type; // 0=jmp rel32, 1=jcc rel32
    struct AsmFixupNode *next;
} AsmFixupNode;

static AsmFixupNode *asm_fixup_htab[CG_HT_SIZE];
static int asm_fixup_count = 0;

static void asm_fixup_ht_reset(void) {
    memset(asm_fixup_htab, 0, sizeof(asm_fixup_htab));
    asm_fixup_count = 0;
}

static void asm_fixup_ht_add(size_t instr_off, const char *label, int type) {
    uint32_t h = cg_ht_hash(label);
    AsmFixupNode *n = arena_alloc(sizeof(AsmFixupNode));
    n->instr_off = instr_off;
    n->label = label;
    n->type = type;
    n->next = asm_fixup_htab[h];
    asm_fixup_htab[h] = n;
    asm_fixup_count++;
}

// Record a pending branch fixup (forward reference)
static void asm_fixup_add(SecBuf *s, size_t instr_off, const char *label, int type) {
    extern bool cg_dry_run;
    if (cg_dry_run) return;
    size_t target = cg_label_ht_get(label);
    if (target != (size_t)-1) {
#ifdef ARCH_ARM64
        uint32_t insn = *(uint32_t *)(s->data + instr_off);
        int64_t delta = (int64_t)((int64_t)target - (int64_t)instr_off);
        if (type == 0) {
            // B: 26-bit signed word offset in bits [25:0]
            int64_t imm = delta / 4;
            insn = (insn & ~0x03FFFFFFU) | (uint32_t)(imm & 0x03FFFFFFU);
        } else if (type == 2) {
            // ADR: 21-bit byte offset, immhi[23:5] | immlo[30:29]
            int32_t immlo = (int32_t)(delta & 3);
            int32_t immhi = (int32_t)(delta >> 2);
            insn = (insn & ~0x60FFFFE0U) | (uint32_t)((immhi & 0x7FFFF) << 5) | (uint32_t)((immlo & 3) << 29);
        } else {
            // B.cond: 19-bit signed word offset in bits [23:5]
            int64_t imm = delta / 4;
            insn = (insn & ~0x00FFFFE0U) | (uint32_t)((imm & 0x7FFFF) << 5);
        }
        secbuf_patch32le(s, instr_off, insn);
#else
        int32_t disp;
        if (type == 0)
            disp = (int32_t)(target - (instr_off + 5));
        else
            disp = (int32_t)(target - (instr_off + 6));
        secbuf_patch32le(s, type == 0 ? instr_off + 1 : instr_off + 2, (uint32_t)disp);
#endif
        return;
    }
    asm_fixup_ht_add(instr_off, label, type);
}

// Resolve pending fixups when a label is defined
static void asm_fixup_resolve(SecBuf *s, const char *label, size_t target_off) {
    uint32_t h = cg_ht_hash(label);
    AsmFixupNode **pp = &asm_fixup_htab[h];
    while (*pp) {
        AsmFixupNode *n = *pp;
        if (strcmp(n->label, label) == 0) {
#ifdef ARCH_ARM64
            uint32_t insn = *(uint32_t *)(s->data + n->instr_off);
            int64_t delta = (int64_t)((int64_t)target_off - (int64_t)n->instr_off);
            if (n->type == 0) {
                // B: 26-bit signed word offset in bits [25:0]
                int64_t imm = delta / 4;
                insn = (insn & ~0x03FFFFFFU) | (uint32_t)(imm & 0x03FFFFFFU);
            } else if (n->type == 2) {
                // ADR: 21-bit byte offset, immhi[23:5] | immlo[30:29]
                int32_t immlo = (int32_t)(delta & 3);
                int32_t immhi = (int32_t)(delta >> 2);
                insn = (insn & ~0x60FFFFE0U) | (uint32_t)((immhi & 0x7FFFF) << 5) | (uint32_t)((immlo & 3) << 29);
            } else {
                // B.cond: 19-bit signed word offset in bits [23:5]
                int64_t imm = delta / 4;
                insn = (insn & ~0x00FFFFE0U) | (uint32_t)((imm & 0x7FFFF) << 5);
            }
            secbuf_patch32le(s, n->instr_off, insn);
#else
            int32_t disp;
            if (n->type == 0)
                disp = (int32_t)(target_off - (n->instr_off + 5));
            else
                disp = (int32_t)(target_off - (n->instr_off + 6));
            secbuf_patch32le(s, n->type == 0 ? n->instr_off + 1 : n->instr_off + 2, (uint32_t)disp);
#endif
            *pp = n->next;
            asm_fixup_count--;
        } else {
            pp = &n->next;
        }
    }
}

// ============================================================================
// Peephole instruction tracking
// ============================================================================

typedef enum {
    ASM_NONE = 0,
    ASM_MOV_RR,
    ASM_MOV_RI,
    ASM_MOV_RRBP,
    ASM_MOV_RBPR,
    ASM_MOV_LOAD,
    ASM_MOV_STORE,
    ASM_MOVSX,
    ASM_MOVZX,
    ASM_LEA_FP,
    ASM_ADD_RR,
    ASM_ADD_RI,
    ASM_SUB_RR,
    ASM_SUB_RI,
    ASM_MUL_RR,
    ASM_IMUL_RRI,
    ASM_NEG,
    ASM_NOT,
    ASM_AND_RR,
    ASM_AND_RI,
    ASM_OR_RR,
    ASM_OR_RI,
    ASM_XOR_RR,
    ASM_XOR_RI,
    ASM_SHL_RI,
    ASM_SHR_RI,
    ASM_SAR_RI,
    ASM_SHL_CL,
    ASM_SHR_CL,
    ASM_SAR_CL,
    ASM_CMP_RR,
    ASM_CMP_RI,
    ASM_CMP_ZERO,
    ASM_TEST_RR,
    ASM_SETCC,
    ASM_CMOVCC,
    ASM_JMP,
    ASM_JMP_LABEL,
    ASM_JCC,
    ASM_JCC_LABEL,
    ASM_CALL,
    ASM_CALL_INDIR,
    ASM_RET,
    ASM_CVTSI2SD,
    ASM_CVTTSD2SI,
    ASM_CVTSD2SS,
    ASM_CVTSS2SD,
    ASM_FMOV_IM,
    ASM_FMOV_MI,
    ASM_FMOV_D0_R,
    ASM_FMOV_R_D0,
    ASM_FOP_RR,
    ASM_FNEG,
    ASM_FABS,
    ASM_FCMP,
    ASM_FCVT_F2I,
    ASM_FCVT_I2F,
    ASM_SCVTF,
    ASM_UCVTF,
    ASM_FCVTZS,
    ASM_FCVTZU,
    ASM_PUSH,
    ASM_POP,
    ASM_PUSH_IMM,
    ASM_STP_FP_LR,
    ASM_LDP_FP_LR,
    ASM_BSWAP,
    ASM_CLZ,
    ASM_RBIT,
    ASM_REV,
    ASM_REV16,
    ASM_LDX,
    ASM_STX,
    ASM_CAS,
    ASM_FENCE,
    ASM_LOCK,
    ASM_CLD,
    ASM_NOP,
    ASM_LEAVE,
    ASM_LABEL,
    ASM_GLBL,
    ASM_WEAK,
    ASM_DIRECTIVE,
} AsmOp;

typedef struct {
    AsmOp op;
    size_t offset;
    size_t count;
    int rd, rs, rt;
    int size;
    int64_t imm;
    int off;
    const char *label;
    int cond;
    int wreg;
    bool is_store;
} AsmInsn;

#define ASM_HISTORY 4
static AsmInsn asm_last[ASM_HISTORY];
static int asm_last_idx = 0;
static int asm_last_count = 0;
static AsmInsn asm_prev_node_last;
static bool asm_prev_node_valid = false;

static void asm_record(AsmOp op, size_t offset, size_t count,
                       int rd, int rs, int rt, int size,
                       int64_t imm, int off, const char *label,
                       int cond, int wreg, bool is_store) {
    AsmInsn *rec = &asm_last[asm_last_idx];
    rec->op = op;
    rec->offset = offset;
    rec->count = count;
    rec->rd = rd;
    rec->rs = rs;
    rec->rt = rt;
    rec->size = size;
    rec->imm = imm;
    rec->off = off;
    rec->label = label;
    rec->cond = cond;
    rec->wreg = wreg;
    rec->is_store = is_store;
    asm_last_idx = (asm_last_idx + 1) % ASM_HISTORY;
    if (asm_last_count < ASM_HISTORY) asm_last_count++;
}

static void asm_peep_save_prev(void) {
    if (asm_last_count > 0) {
        int prev = (asm_last_idx - 1 + ASM_HISTORY) % ASM_HISTORY;
        asm_prev_node_last = asm_last[prev];
        asm_prev_node_valid = true;
    } else {
        asm_prev_node_valid = false;
    }
}

static void asm_peep_node_start(SecBuf *s) {
    (void)s;
    asm_peep_save_prev();
}
static void asm_peep_node_end(SecBuf *s) { (void)s; }

// ============================================================================
// MOV / data movement
// ============================================================================

static size_t asm_mov_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    if (dst == src) return 0;
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(sf, CG_ARM_REG(dst), ARM64_XZR, CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_MOV_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_mov_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_MOV_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

// Move return register (x0/rax) to virtual register r
static size_t asm_mov_retval(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(1, CG_ARM_REG(r), ARM64_XZR, 0, ARM64_LSL, 0)); // mov x{r}, x0
#else
    x86_mov_rr(s, size, CG_X86_REG(r), X86_RAX); // mov %rax/%eax, rr
#endif
    return s->len - off;
}
// Move virtual register r to return register (x0/rax)
static size_t asm_mov_reg_to_retval(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(1, 0, ARM64_XZR, CG_ARM_REG(r), ARM64_LSL, 0)); // mov x0, x{r}
#else
    x86_mov_rr(s, size, X86_RAX, CG_X86_REG(r)); // mov rr, %rax
#endif
    return s->len - off;
}
static size_t asm_mov_imm(SecBuf *s, int r, int size, int64_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    bool is_w = false; // always use 64-bit (x-reg) to match original printf codegen
    uint64_t uval = (uint64_t)imm;
    int rd = CG_ARM_REG(r);
    int sf = 1;
    secbuf_emit32le(s, arm64_movz(sf, rd, (uint16_t)(uval & 0xffff), 0));
    size_t count = 1;
    uint64_t v = uval >> 16;
    int shift = 16;
    int max_shift = 48;
    while (v && shift <= max_shift) {
        secbuf_emit32le(s, arm64_movk(sf, rd, v & 0xffff, shift));
        v >>= 16;
        shift += 16;
        count++;
    }
    asm_record(ASM_MOV_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count * 4;
#else
    if (size == 8 && imm >= INT32_MIN && imm <= INT32_MAX)
        x86_mov_ri(s, 8, CG_X86_REG(r), (int32_t)imm);
    else if (size == 8)
        x86_movabs(s, CG_X86_REG(r), (uint64_t)imm);
    else
        x86_mov_ri(s, size, CG_X86_REG(r), (int32_t)imm);
    size_t count = s->len - off;
    asm_record(ASM_MOV_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count;
#endif
}

// Zero register via xor
static size_t asm_xor_reg_reg(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_eor_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(dst), ARM64_LSL, 0));
    asm_record(ASM_XOR_RR, off, 1, dst, dst, -1, size, 0, 0, NULL, 0, -1, false);
    (void)src;
    return 4;
#else
    x86_xor_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_XOR_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_movq_zero(SecBuf *s, VReg r) { return asm_xor_reg_reg(s, r, r, 8); }
static size_t asm_movl_zero(SecBuf *s, VReg r) { return asm_xor_reg_reg(s, r, r, 4); }

#ifdef ARCH_ARM64
static size_t asm_movk(SecBuf *s, int r, int sf, uint16_t imm16, int shift) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_movk(sf, CG_ARM_REG(r), imm16, shift));
    return 4;
}
#endif

static size_t asm_movsx(SecBuf *s, VReg dst, VReg src, int dst_sz, int src_sz) {
    if (dst_sz <= src_sz) return 0;
    size_t off = s->len;
#ifdef ARCH_ARM64
    if (dst_sz == 8 && src_sz == 4)
        secbuf_emit32le(s, arm64_sxtw(CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 8 && src_sz == 2)
        secbuf_emit32le(s, arm64_sxth(1, CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 8 && src_sz == 1)
        secbuf_emit32le(s, arm64_sxtb(1, CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 4 && src_sz == 2)
        secbuf_emit32le(s, arm64_sxth(0, CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 4 && src_sz == 1)
        secbuf_emit32le(s, arm64_sxtb(0, CG_ARM_REG(dst), CG_ARM_REG(src)));
    else
        return asm_mov_reg_reg(s, dst, src, dst_sz);
    asm_record(ASM_MOVSX, off, 1, dst, src, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_movsx(s, dst_sz, src_sz, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_MOVSX, off, count, dst, src, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_movzx(SecBuf *s, VReg dst, VReg src, int dst_sz, int src_sz) {
    if (dst_sz <= src_sz) return 0;
    size_t off = s->len;
#ifdef ARCH_ARM64
    if (dst_sz == 8 && src_sz == 2)
        secbuf_emit32le(s, arm64_uxth(CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 8 && src_sz == 1)
        secbuf_emit32le(s, arm64_uxtb(CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 4 && src_sz == 2)
        secbuf_emit32le(s, arm64_uxth(CG_ARM_REG(dst), CG_ARM_REG(src)));
    else if (dst_sz == 4 && src_sz == 1)
        secbuf_emit32le(s, arm64_uxtb(CG_ARM_REG(dst), CG_ARM_REG(src)));
    else
        return asm_mov_reg_reg(s, dst, src, dst_sz);
    asm_record(ASM_MOVZX, off, 1, dst, src, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_movzx(s, dst_sz, src_sz, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_MOVZX, off, count, dst, src, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

// ============================================================================
// Arithmetic
// ============================================================================

static size_t asm_add_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_add_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_ADD_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_add_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_ADD_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_add_imm(SecBuf *s, int r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    if (imm >= 0 && imm < 4096) {
        secbuf_emit32le(s, arm64_add_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm, 0));
    } else if (imm > 0 && imm < 4096 * 4096 && (imm & 0xfff) == 0) {
        secbuf_emit32le(s, arm64_add_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm >> 12, 1)); // shifted
    } else {
        // large imm: load into x16, then add
        int v = imm < 0 ? -imm : imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        if (v >> 16) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 16) & 0xffff), 16));
        if (v >> 32) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 32) & 0xffff), 32));
        if (v >> 48) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 48) & 0xffff), 48));
        if (imm < 0)
            secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_add_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), 16, ARM64_LSL, 0));
    }
    asm_record(ASM_ADD_RI, off, (size_t)(s->len - off) / 4, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return s->len - off;
#else
    x86_add_ri(s, size, CG_X86_REG(r), imm);
    size_t count = s->len - off;
    asm_record(ASM_ADD_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_sub_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_SUB_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_sub_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_SUB_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_sub_reg3(SecBuf *s, int dst, int src1, int src2, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(src1), CG_ARM_REG(src2), ARM64_LSL, 0));
    return 4;
#else
    x86_mov_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src1));
    x86_sub_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src2));
    size_t count = s->len - off;
    return count;
#endif
}

static size_t asm_sub_imm(SecBuf *s, int r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    if (imm >= 0 && imm < 4096) {
        secbuf_emit32le(s, arm64_sub_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm, 0));
    } else if (imm > 0 && imm < 4096 * 4096 && (imm & 0xfff) == 0) {
        secbuf_emit32le(s, arm64_sub_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm >> 12, 1)); // shifted
    } else {
        int v = imm < 0 ? -imm : imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        if (v >> 16) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 16) & 0xffff), 16));
        if (v >> 32) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 32) & 0xffff), 32));
        if (v >> 48) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 48) & 0xffff), 48));
        if (imm < 0)
            secbuf_emit32le(s, arm64_add_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), 16, ARM64_LSL, 0));
    }
    asm_record(ASM_SUB_RI, off, (size_t)(s->len - off) / 4, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return s->len - off;
#else
    x86_sub_ri(s, size, CG_X86_REG(r), imm);
    size_t count = s->len - off;
    asm_record(ASM_SUB_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_mul_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_mul(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src)));
    asm_record(ASM_MUL_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_imul_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_MUL_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}
static size_t asm_sdiv_reg_reg(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sdiv(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src)));
    return 4;
#else
    x86_idiv_r(s, size, CG_X86_REG(src));
    size_t count = s->len - off;
    return count;
#endif
}
static size_t asm_udiv_reg_reg(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_udiv(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src)));
    return 4;
#else
    (void)s;
    (void)dst;
    (void)src;
    (void)size;
    return 0;
#endif
}

static size_t asm_neg(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_neg(sf, CG_ARM_REG(r), CG_ARM_REG(r)));
    asm_record(ASM_NEG, off, 1, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_neg_r(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_NEG, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_not(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_mvn(sf, CG_ARM_REG(r), CG_ARM_REG(r), ARM64_LSL, 0));
    asm_record(ASM_NOT, off, 1, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_not_r(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_NOT, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_dec(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sub_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), 1, 0));
    return 4;
#else
    x86_dec_r(s, size, CG_X86_REG(r));
    return s->len - off;
#endif
}

// ============================================================================
// Logical
// ============================================================================

static size_t asm_and_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_and_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_AND_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_and_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_AND_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_or_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_OR_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_or_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_OR_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_eor_reg_reg(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_eor_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0));
    asm_record(ASM_XOR_RR, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_xor_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_XOR_RR, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

// ============================================================================
// Shifts
// ============================================================================

static size_t asm_shl_imm(SecBuf *s, VReg r, int size, uint8_t shift) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_lsl_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), shift));
    asm_record(ASM_SHL_RI, off, 1, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_shl_ri(s, size, CG_X86_REG(r), shift);
    size_t count = s->len - off;
    asm_record(ASM_SHL_RI, off, count, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_shr_imm(SecBuf *s, VReg r, int size, uint8_t shift) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_lsr_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), shift));
    asm_record(ASM_SHR_RI, off, 1, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_shr_ri(s, size, CG_X86_REG(r), shift);
    size_t count = s->len - off;
    asm_record(ASM_SHR_RI, off, count, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_sar_imm(SecBuf *s, VReg r, int size, uint8_t shift) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_asr_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), shift));
    asm_record(ASM_SAR_RI, off, 1, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_sar_ri(s, size, CG_X86_REG(r), shift);
    size_t count = s->len - off;
    asm_record(ASM_SAR_RI, off, count, r, -1, -1, size, shift, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_shl_cl(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_lsl_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), CG_ARM_REG(r)));
    return 4;
#else
    x86_shl_rcl(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_SHL_CL, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_shr_cl(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_lsr_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), CG_ARM_REG(r)));
    return 4;
#else
    x86_shr_rcl(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_SHR_CL, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_sar_cl(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_asr_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), CG_ARM_REG(r)));
    return 4;
#else
    x86_sar_rcl(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_SAR_CL, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_cqo(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64: sxtw x0, w0 for 32-bit, already sign-extended for 64-bit
#else
    x86_cqo(s);
#endif
    return s->len - off;
}

static size_t asm_cdq(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64: sxtw x0, w0
#else
    x86_cdq(s);
#endif
    return s->len - off;
}

static size_t asm_idiv(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64 division
#else
    x86_idiv_r(s, size, CG_X86_REG(r));
#endif
    return s->len - off;
}

static size_t asm_div(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64 unsigned division
#else
    x86_div_r(s, size, CG_X86_REG(r));
#endif
    return s->len - off;
}

// ============================================================================
// Compare / Test
// ============================================================================

static size_t asm_cmp_reg_reg(SecBuf *s, VReg a, VReg b, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_subs_reg(sf, ARM64_XZR, CG_ARM_REG(a), CG_ARM_REG(b), ARM64_LSL, 0));
    asm_record(ASM_CMP_RR, off, 1, a, b, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_cmp_rr(s, size, CG_X86_REG(a), CG_X86_REG(b));
    size_t count = s->len - off;
    asm_record(ASM_CMP_RR, off, count, a, b, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_cmp_zero(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_subs_imm(sf, ARM64_XZR, CG_ARM_REG(r), 0, 0));
#else
    x86_cmp_ri(s, size, CG_X86_REG(r), 0);
#endif
    asm_record(ASM_CMP_ZERO, off, (size_t)(s->len - off), r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return (size_t)(s->len - off);
}

// ============================================================================
// Conditional set / move
// ============================================================================

#ifdef ARCH_ARM64
static size_t asm_cset(SecBuf *s, int r, Arm64Cond cond) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_cset(0, CG_ARM_REG(r), cond));
    asm_record(ASM_SETCC, off, 1, r, -1, -1, 4, 0, 0, NULL, cond, -1, false);
    return 4;
}
#else
static size_t asm_setcc(SecBuf *s, VReg r, X86Cond cond) {
    size_t off = s->len;
    x86_setcc(s, cond, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_SETCC, off, count, r, -1, -1, 1, 0, 0, NULL, cond, -1, false);
    return count;
}
#endif

// ============================================================================
// Branches (placeholder filled via relocation)
// ============================================================================

static size_t asm_call_label(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_bl(0));
#else
    x86_call_rel32(s, 0);
#endif
    asm_record(ASM_CALL, off, (size_t)(s->len - off), -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return off; // offset of instruction start for relocation
}

static size_t asm_call_reg(SecBuf *s, int r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_blr(CG_ARM_REG(r)));
    return 4;
#else
    x86_call_r(s, CG_X86_REG(r));
    asm_record(ASM_CALL_INDIR, off, s->len - off, r, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return s->len - off;
#endif
}

static size_t asm_jcc_label(SecBuf *s, int cond) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_bcond((Arm64Cond)cond, 0));
    asm_record(ASM_JCC, off, 1, -1, -1, -1, 0, 0, 0, NULL, cond, -1, false);
    return off;
#else
    x86_jcc_rel32(s, (X86Cond)cond, 0);
    size_t count = s->len - off;
    asm_record(ASM_JCC, off, count, -1, -1, -1, 0, 0, 0, NULL, cond, -1, false);
    return off;
#endif
}

static size_t asm_jmp_label(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_b(0));
    asm_record(ASM_JMP, off, 1, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return off;
#else
    x86_jmp_rel32(s, 0);
    size_t count = s->len - off;
    asm_record(ASM_JMP, off, count, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return off;
#endif
}

static size_t asm_jmp_reg(SecBuf *s, int r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_br(CG_ARM_REG(r)));
#else
    x86_jmp_r(s, CG_X86_REG(r));
#endif
    asm_record(ASM_JMP, off, (size_t)(s->len - off), r, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return (size_t)(s->len - off);
}

// adr x16, label — emit placeholder, fixup type=2 patches 21-bit byte offset at label def
static size_t asm_adr_x16_label(SecBuf *s, const char *label) {
#ifdef ARCH_ARM64
    size_t off = s->len;
    secbuf_emit32le(s, arm64_adr(16, 0)); // adr x16, 0 (placeholder)
    asm_fixup_add(s, off, label, 2); // type=2 = ADR fixup
    return 4;
#else
    (void)label;
    return 0;
#endif
}
static size_t asm_ret(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ret(CG_ARM_LR));
#else
    x86_ret(s);
#endif
    size_t count = (size_t)(s->len - off);
    asm_record(ASM_RET, off, count, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return count;
}

static size_t asm_leave(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_reg(1, CG_ARM_SP, CG_ARM_FP, ARM64_XZR, ARM64_LSL, 0));
    secbuf_emit32le(s, arm64_ldp(1, CG_ARM_FP, CG_ARM_LR, CG_ARM_SP, 0, false, true));
    return 8;
#else
    x86_leave(s);
    size_t count = s->len - off;
    asm_record(ASM_LEAVE, off, count, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

// ============================================================================
// Stack
// ============================================================================

#ifdef ARCH_ARM64
static size_t asm_stp_fp_lr(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stp(1, CG_ARM_FP, CG_ARM_LR, CG_ARM_SP, -2, true, false)); // stp x29, x30, [sp, #-16]!
    asm_record(ASM_STP_FP_LR, off, 1, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return 4;
}
static size_t asm_ldp_fp_lr(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldp(1, CG_ARM_FP, CG_ARM_LR, CG_ARM_SP, 2, false, true)); // ldp x29, x30, [sp], #16
    asm_record(ASM_LDP_FP_LR, off, 1, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return 4;
}

static size_t asm_stp_sp(SecBuf *s, int rt1, int rt2, int32_t imm7, bool pre, bool post) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stp(1, CG_ARM_REG(rt1), CG_ARM_REG(rt2), CG_ARM_SP, imm7, pre, post));
    return 4;
}

static size_t asm_ldp_sp(SecBuf *s, int rt1, int rt2, int32_t imm7, bool pre, bool post) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldp(1, CG_ARM_REG(rt1), CG_ARM_REG(rt2), CG_ARM_SP, imm7, pre, post));
    return 4;
}

static size_t asm_mov_fp_sp(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_FP, CG_ARM_SP, 0, 0)); // add x29, sp, #0
    return 4;
}

static size_t asm_mov_x0_reg(SecBuf *s, int src) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 0, ARM64_XZR, CG_ARM_REG(src), ARM64_LSL, 0)); // mov x0, x{src}
    return 4;
}
#endif

#ifndef ARCH_ARM64
static size_t asm_push_phy(SecBuf *s, X86Reg r) {
    size_t off = s->len;
    x86_push(s, r);
    asm_record(ASM_PUSH, off, s->len - off, (int)r, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_pop_phy(SecBuf *s, X86Reg r) {
    size_t off = s->len;
    x86_pop(s, r);
    asm_record(ASM_POP, off, s->len - off, (int)r, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_lea_rbp_phy(SecBuf *s, X86Reg reg, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_lea(s, size, reg, m);
    asm_record(ASM_LEA_FP, off, s->len - off, (int)reg, -1, -1, size, 0, -offset, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_mov_rbp_phyreg(SecBuf *s, X86Reg reg, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_rm(s, size, reg, m);
    asm_record(ASM_MOV_RRBP, off, s->len - off, (int)reg, -1, -1, size, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_mov_phyreg_rbp(SecBuf *s, X86Reg reg, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_mr(s, size, m, reg);
    asm_record(ASM_MOV_RBPR, off, s->len - off, (int)reg, -1, -1, size, 0, offset, NULL, 0, -1, true);
    return s->len - off;
}
static size_t asm_movabs_phy(SecBuf *s, X86Reg reg, uint64_t val) {
    size_t off = s->len;
    x86_movabs(s, reg, val);
    asm_record(ASM_MOV_RI, off, s->len - off, (int)reg, -1, -1, 8, (int64_t)val, 0, NULL, 0, -1, false);
    return s->len - off;
}
// movq %rsp, %rbp  (function prologue)
static size_t asm_mov_rsp_rbp(SecBuf *s) {
    size_t off = s->len;
    x86_mov_rr(s, 8, X86_RBP, X86_RSP); // movq %rsp, %rbp
    asm_record(ASM_MOV_RR, off, s->len - off, (int)X86_RBP, (int)X86_RSP, -1, 8, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
// subq $imm, %rsp  (function prologue stack allocation)
static size_t asm_sub_rsp_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
    x86_sub_ri(s, 8, X86_RSP, imm); // subq $imm, %rsp
    asm_record(ASM_SUB_RI, off, s->len - off, (int)X86_RSP, -1, -1, 8, imm, 0, NULL, 0, -1, false);
    return s->len - off;
}
// addq $imm, %rsp  (function epilogue stack deallocation)
static size_t asm_add_rsp_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
    x86_add_ri(s, 8, X86_RSP, imm); // addq $imm, %rsp
    asm_record(ASM_ADD_RI, off, s->len - off, (int)X86_RSP, -1, -1, 8, imm, 0, NULL, 0, -1, false);
    return s->len - off;
}
// store immediate to rbp-relative: movb/movl/movq $imm, -offset(%rbp)
static size_t asm_mov_rbp_imm(SecBuf *s, int size, int offset, int32_t imm) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_mi(s, size, m, imm); // movb/movl/movq $imm, -offset(%rbp)
    asm_record(ASM_MOV_RI, off, s->len - off, -1, -1, -1, size, imm, offset, NULL, 0, -1, true);
    return s->len - off;
}
// movzbl/movzwl -offset(%rbp), dst: zero-extending load from rbp-relative
static size_t asm_movzx_rbp_reg(SecBuf *s, VReg dst, int dst_sz, int src_sz, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movzx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), m); // movzbl/movzwl -off(%rbp), dst
    asm_record(ASM_MOVZX, off, s->len - off, dst, -1, -1, dst_sz, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
// movsbl/movswl -offset(%rbp), dst: sign-extending load from rbp-relative
static size_t asm_movsx_rbp_reg(SecBuf *s, VReg dst, int dst_sz, int src_sz, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movsx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), m); // movsbl/movswl -off(%rbp), dst
    asm_record(ASM_MOVSX, off, s->len - off, dst, -1, -1, dst_sz, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
// movzbl/movzwl off(base), dst: zero-extending load from base+offset
static size_t asm_movzx_base_off_reg(SecBuf *s, VReg dst, VReg base, int64_t disp, int dst_sz, int src_sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(base), X86_NOREG, 1, disp};
    x86_movzx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), m); // movzbl/movzwl disp(base), dst
    asm_record(ASM_MOVZX, off, s->len - off, dst, base, -1, dst_sz, 0, disp, NULL, 0, -1, false);
    return s->len - off;
}
// movsbl/movswl off(base), dst: sign-extending load from base+offset
static size_t asm_movsx_base_off_reg(SecBuf *s, VReg dst, VReg base, int64_t disp, int dst_sz, int src_sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(base), X86_NOREG, 1, disp};
    x86_movsx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), m); // movsbl/movswl disp(base), dst
    asm_record(ASM_MOVSX, off, s->len - off, dst, base, -1, dst_sz, 0, disp, NULL, 0, -1, false);
    return s->len - off;
}
// movl/movq disp(base), dst: regular load from base+offset
static size_t asm_mov_base_off_reg(SecBuf *s, VReg dst, VReg base, int64_t disp, int sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(base), X86_NOREG, 1, disp};
    x86_mov_rm(s, sz, CG_X86_REG(dst), m); // movl/movq disp(base), dst
    asm_record(ASM_MOV_RR, off, s->len - off, dst, base, -1, sz, 0, disp, NULL, 0, -1, false);
    return s->len - off;
}
// movq phy, disp(base_vreg): store physical reg to base+offset
static size_t asm_mov_phy_base_off(SecBuf *s, X86Reg phy, VReg base, int64_t disp, int sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(base), X86_NOREG, 1, disp};
    x86_mov_mr(s, sz, m, phy); // movq phy, disp(%base)
    asm_record(ASM_MOV_RR, off, s->len - off, (int)phy, base, -1, sz, 0, disp, NULL, 0, -1, true);
    return s->len - off;
}
// setcc to physical reg (e.g. sete %al for compare+setne into RAX)
static size_t asm_setcc_phy(SecBuf *s, X86Cond cond, X86Reg reg) {
    size_t off = s->len;
    x86_setcc(s, cond, reg); // setne %al etc.
    asm_record(ASM_SETCC, off, s->len - off, (int)reg, -1, -1, 1, 0, 0, NULL, (int)cond, -1, false);
    return s->len - off;
}
#endif
#ifdef ARCH_ARM64
// x0 = x29 + imm (handles negative and large |imm| via scratch x16)
static size_t asm_add_x0_fp_imm(SecBuf *s, int imm) {
    size_t off = s->len;
    int abs_imm = imm < 0 ? -imm : imm;
    if (abs_imm < 4096) {
        if (imm >= 0)
            secbuf_emit32le(s, arm64_add_imm(1, 0, 29, imm, 0));
        else
            secbuf_emit32le(s, arm64_sub_imm(1, 0, 29, -imm, 0));
    } else {
        int v = abs_imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        if (v >> 16) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 16) & 0xffff), 16));
        if (v >> 32) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 32) & 0xffff), 32));
        if (v >> 48) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 48) & 0xffff), 48));
        if (imm >= 0)
            secbuf_emit32le(s, arm64_add_reg(1, 0, 29, 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_sub_reg(1, 0, 29, 16, ARM64_LSL, 0));
    }
    return s->len - off;
}
static size_t asm_sub_x0_fp_x16(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 0, 29, 16, ARM64_LSL, 0));
    return s->len - off;
}
static size_t asm_add_x0_x0_imm(SecBuf *s, int imm) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, 0, 0, imm, 0));
    return s->len - off;
}
static size_t asm_and_x0_x0_imm(SecBuf *s, uint64_t imm) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_and_imm(1, 0, 0, imm));
    return s->len - off;
}
static size_t asm_sub_sp_sp_x0(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 31, 31, 0, ARM64_LSL, 0));
    return s->len - off;
}
static size_t asm_mov_x0_sp(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, 0, 31, 0, 0));
    return s->len - off;
}
static size_t asm_sub_sp_sp_reg(SecBuf *s, int r) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 31, 31, CG_ARM_REG(r), ARM64_LSL, 0));
    return s->len - o;
}
static size_t asm_mov_reg_sp(SecBuf *s, int r) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_REG(r), 31, 0, 0));
    return s->len - o;
}
static size_t asm_add_sp_sp_x16(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_reg(1, 31, 31, 16, ARM64_LSL, 0));
    return s->len - off;
}
// mov x16, sp
static size_t asm_mov_x16_sp(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, 16, 31, 0, 0)); // mov x16, sp
    return s->len - off;
}
// mov sp, x16
static size_t asm_mov_sp_x16(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, 31, 16, 0, 0)); // mov sp, x16
    return s->len - off;
}
// mov x16, x{base}  — move virtual reg to x16 scratch
static size_t asm_mov_x16_reg(SecBuf *s, int base_r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 16, ARM64_XZR, CG_ARM_REG(base_r), ARM64_LSL, 0)); // mov x16, x{base_r}
    return s->len - off;
}
// str x16, [x{base}, #uimm8] — store x16 to virtual base reg + scaled offset
static size_t asm_str_x16_reg_uoff(SecBuf *s, int base_r, int byte_off) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_str_uoff(3, 16, CG_ARM_REG(base_r), (uint32_t)(byte_off / 8))); // str x16, [base, #byte_off]
    return s->len - off;
}
// ldr x16, [x{base}, #uimm8] — load x16 from virtual base reg + scaled offset
static size_t asm_ldr_x16_reg_uoff(SecBuf *s, int base_r, int byte_off) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(3, 16, CG_ARM_REG(base_r), (uint32_t)(byte_off / 8))); // ldr x16, [base, #byte_off]
    return s->len - off;
}
static size_t asm_str_fp_reg(SecBuf *s, VReg reg) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_str_imm(1, 29, CG_ARM_REG(reg), 0, false)); // str x29, [x{reg}]
    return s->len - off;
}
static size_t asm_ldr_fp_reg(SecBuf *s, VReg reg) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_imm(1, 29, CG_ARM_REG(reg), 0, false)); // ldr x29, [x{reg}]
    return s->len - off;
}
static size_t asm_ldur_x16_fp_minus(SecBuf *s, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_ldur(1, 16, 29, -off));
    return s->len - o;
}
static size_t asm_sub_x17_fp_imm(SecBuf *s, int imm) {
    size_t o = s->len;
    if (imm >= 0 && imm < 4096) {
        secbuf_emit32le(s, arm64_sub_imm(1, 17, 29, imm, 0));
    } else {
        int v = imm < 0 ? -imm : imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        if (v >> 16) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 16) & 0xffff), 16));
        if (v >> 32) secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)((v >> 32) & 0xffff), 32));
        if (imm >= 0)
            secbuf_emit32le(s, arm64_sub_reg(1, 17, 29, 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_add_reg(1, 17, 29, 16, ARM64_LSL, 0));
    }
    return s->len - o;
}
static size_t asm_sub_x17_fp_x17(SecBuf *s) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 17, 29, 17, ARM64_LSL, 0));
    return s->len - o;
}
static size_t asm_ldr_x16_x17(SecBuf *s) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(1, 16, 17, 0));
    return s->len - o;
}
static size_t asm_str_x16_x17(SecBuf *s) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_str_uoff(1, 16, 17, 0));
    return s->len - o;
}
static size_t asm_sub_x16_fp_x17(SecBuf *s) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 16, 29, 17, ARM64_LSL, 0));
    return s->len - o;
}
static size_t asm_stur_x16_fp_minus(SecBuf *s, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_stur(1, 16, 29, -off));
    return s->len - o;
}
static size_t asm_asr_x17_src_63(SecBuf *s, VReg src) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_asr_imm(1, 17, CG_ARM_REG(src), 63)); // asr x17, x{src}, #63
    return s->len - o;
}
static size_t asm_cmn_imm(SecBuf *s, VReg r, int sf, int imm) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_subs_imm(sf, 31, CG_ARM_REG(r), -imm, 0));
    return s->len - o;
}
// r = x29 + imm  (handles large |imm| via scratch x16)
static size_t asm_add_reg_fp_imm(SecBuf *s, int r, int32_t imm) {
    size_t o = s->len;
    int abs_imm = imm < 0 ? -imm : imm;
    if (abs_imm < 4096) {
        if (imm >= 0)
            secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_REG(r), 29, imm, 0));
        else
            secbuf_emit32le(s, arm64_sub_imm(1, CG_ARM_REG(r), 29, -imm, 0));
    } else {
        // mov x16, #|imm|; r = x29 +/- x16
        int v = abs_imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        v >>= 16;
        int sh = 16;
        while (v) {
            secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)(v & 0xffff), sh));
            v >>= 16;
            sh += 16;
        }
        if (imm >= 0)
            secbuf_emit32le(s, arm64_add_reg(1, CG_ARM_REG(r), 29, 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_sub_reg(1, CG_ARM_REG(r), 29, 16, ARM64_LSL, 0));
    }
    return s->len - o;
}
// r = x29 - imm  (handles large |imm| via scratch x16)
static size_t asm_sub_reg_fp_imm(SecBuf *s, int r, int32_t imm) {
    size_t o = s->len;
    int abs_imm = imm < 0 ? -imm : imm;
    if (abs_imm < 4096) {
        if (imm >= 0)
            secbuf_emit32le(s, arm64_sub_imm(1, CG_ARM_REG(r), 29, imm, 0));
        else
            secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_REG(r), 29, -imm, 0));
    } else {
        // mov x16, #|imm|; r = x29 -/+ x16
        int v = abs_imm;
        secbuf_emit32le(s, arm64_movz(1, 16, (uint16_t)(v & 0xffff), 0));
        v >>= 16;
        int sh = 16;
        while (v) {
            secbuf_emit32le(s, arm64_movk(1, 16, (uint16_t)(v & 0xffff), sh));
            v >>= 16;
            sh += 16;
        }
        if (imm >= 0)
            secbuf_emit32le(s, arm64_sub_reg(1, CG_ARM_REG(r), 29, 16, ARM64_LSL, 0));
        else
            secbuf_emit32le(s, arm64_add_reg(1, CG_ARM_REG(r), 29, 16, ARM64_LSL, 0));
    }
    return s->len - o;
}
static size_t asm_sub_reg_fp_reg(SecBuf *s, int dst, int src, int size) {
    size_t o = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(dst), 29, CG_ARM_REG(src), ARM64_LSL, 0));
    return s->len - o;
}
static size_t asm_stur_fp(SecBuf *s, VReg r, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_stur(1, CG_ARM_REG(r), 29, -off)); // stur x{r}, [x29, #-off]
    return s->len - o;
}
static size_t asm_ldur_fp(SecBuf *s, VReg r, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_ldur(1, CG_ARM_REG(r), 29, -off)); // ldur x{r}, [x29, #-off]
    return s->len - o;
}

static size_t asm_stur_phy(SecBuf *s, Arm64Reg rt, Arm64Reg rn, int sf, int32_t off) {
    size_t o = s->len;
    if (sf == 0) secbuf_emit32le(s, arm64_sturb((int)rt, (int)rn, off));
    else if (sf == 1)
        secbuf_emit32le(s, arm64_sturh((int)rt, (int)rn, off));
    else if (sf == 2)
        secbuf_emit32le(s, arm64_stur(0, (int)rt, (int)rn, off));
    else
        secbuf_emit32le(s, arm64_stur(1, (int)rt, (int)rn, off));
    return s->len - o;
}

static size_t asm_ldur_phy(SecBuf *s, Arm64Reg rt, Arm64Reg rn, int sf, int32_t off) {
    size_t o = s->len;
    if (sf == 0) secbuf_emit32le(s, arm64_ldurb((int)rt, (int)rn, off));
    else if (sf == 1)
        secbuf_emit32le(s, arm64_ldurh((int)rt, (int)rn, off));
    else if (sf == 2)
        secbuf_emit32le(s, arm64_ldur(0, (int)rt, (int)rn, off));
    else
        secbuf_emit32le(s, arm64_ldur(1, (int)rt, (int)rn, off));
    return s->len - o;
}
#endif

#ifndef ARCH_ARM64
// Use codegen register indices (0..7) for these wrappers
static size_t asm_mov_rbp_reg(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_rm(s, size, CG_X86_REG(r), m);
    asm_record(ASM_MOV_RRBP, off, s->len - off, r, -1, -1, size, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_mov_reg_rbp(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_mr(s, size, m, CG_X86_REG(r));
    asm_record(ASM_MOV_RBPR, off, s->len - off, r, -1, -1, size, 0, offset, NULL, 0, -1, true);
    return s->len - off;
}
static size_t asm_lea_rbp_reg(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_lea(s, size, CG_X86_REG(r), m);
    asm_record(ASM_LEA_FP, off, s->len - off, r, -1, -1, size, 0, -offset, NULL, 0, -1, false);
    return s->len - off;
}
#endif

static size_t asm_cmp_imm(SecBuf *s, VReg r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_subs_imm(sf, 31, CG_ARM_REG(r), imm, 0));
#else
    x86_cmp_ri(s, size, CG_X86_REG(r), imm);
#endif
    asm_record(ASM_CMP_RI, off, s->len - off, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return s->len - off;
}

#ifndef ARCH_ARM64
static size_t asm_test_reg_reg(SecBuf *s, VReg a, VReg b, int size) {
    size_t off = s->len;
    x86_test_rr(s, size, CG_X86_REG(a), CG_X86_REG(b));
    asm_record(ASM_TEST_RR, off, s->len - off, a, b, -1, size, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_inc(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
    x86_inc_r(s, size, CG_X86_REG(r));
    return s->len - off;
}
#endif

// ============================================================================
// Bit operations
// ============================================================================

static size_t asm_bswap(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    if (size == 4)
        secbuf_emit32le(s, arm64_rev(0, CG_ARM_REG(r), CG_ARM_REG(r)));
    else
        secbuf_emit32le(s, arm64_rev(1, CG_ARM_REG(r), CG_ARM_REG(r)));
    asm_record(ASM_BSWAP, off, 1, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_bswap(s, size, CG_X86_REG(r));
    size_t count = s->len - off;
    asm_record(ASM_BSWAP, off, count, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_clz(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_clz(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    asm_record(ASM_CLZ, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_lzcnt(s, size, CG_X86_REG(dst), CG_X86_REG(src));
    size_t count = s->len - off;
    asm_record(ASM_CLZ, off, count, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_rbit(SecBuf *s, int dst, int src, int size) {
#ifdef ARCH_ARM64
    size_t off = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rbit(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    asm_record(ASM_RBIT, off, 1, dst, src, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    (void)s;
    (void)dst;
    (void)src;
    (void)size;
    return 0;
#endif
}

static size_t asm_rev(SecBuf *s, int r, int size) {
#ifdef ARCH_ARM64
    size_t off = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rev(sf, CG_ARM_REG(r), CG_ARM_REG(r)));
    asm_record(ASM_REV, off, 1, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    return asm_bswap(s, r, size);
#endif
}

static size_t asm_rev16(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rev16(sf, CG_ARM_REG(r), CG_ARM_REG(r))); // rev16 wr, wr
    asm_record(ASM_REV16, off, 1, r, -1, -1, size, 0, 0, NULL, 0, -1, false);
    return 4;
#else
    (void)s;
    (void)r;
    (void)size;
    return 0;
#endif
}

// ============================================================================
// Floating point
// ============================================================================

static size_t asm_cvtsi2ss(SecBuf *s, VReg src_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_scvtf(size == 8, 0, 0, CG_ARM_REG(src_r)));
    // ARM64: single-precision uses same scvtf path as double for now
#else
    x86_cvtsi2ss(s, size, X86_XMM0, CG_X86_REG(src_r)); // cvtsi2ss rr, %xmm0
#endif
    return s->len - off;
}
// cvtsi2ss from physical register (e.g. %rcx for unsigned 64-bit path)
#ifndef ARCH_ARM64
static size_t asm_cvtsi2ss_phy(SecBuf *s, X86Reg src, int size) {
    size_t off = s->len;
    x86_cvtsi2ss(s, size, X86_XMM0, src); // cvtsi2ss src, %xmm0
    return s->len - off;
}
#endif
static size_t asm_cvtsi2sd(SecBuf *s, VReg src_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_scvtf(size == 8, 0, 0, CG_ARM_REG(src_r)));
#else
    x86_cvtsi2sd(s, size, X86_XMM0, CG_X86_REG(src_r));
#endif
    return s->len - off;
}
static size_t asm_cvttsd2si(SecBuf *s, VReg dst_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fcvtzs(size == 8, 0, CG_ARM_REG(dst_r), 0));
#else
    x86_cvttsd2si(s, size, CG_X86_REG(dst_r), X86_XMM0);
#endif
    return s->len - off;
}
static size_t asm_cvtss2sd(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fcvt(1, 0, 0, 0)); // fcvt d0, s0
#else
    x86_cvtss2sd(s, X86_XMM0, X86_XMM0);
#endif
    return s->len - off;
}
static size_t asm_cvtsd2ss(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fcvt(3, 0, 0, 0)); // fcvt s0, d0
#else
    x86_cvtsd2ss(s, X86_XMM0, X86_XMM0);
#endif
    return s->len - off;
}

// ARM64 ldr from register offset (unsigned offset)
static size_t asm_ldr_reg_off(SecBuf *s, VReg dst_r, VReg base_r, int size, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    switch (size) {
    case 1: secbuf_emit32le(s, arm64_ldrb_uoff(CG_ARM_REG(dst_r), CG_ARM_REG(base_r), uimm)); break;
    case 2: secbuf_emit32le(s, arm64_ldrh_uoff(CG_ARM_REG(dst_r), CG_ARM_REG(base_r), uimm)); break;
    case 4: secbuf_emit32le(s, arm64_ldr_uoff(2, CG_ARM_REG(dst_r), CG_ARM_REG(base_r), uimm / 4)); break;
    default: secbuf_emit32le(s, arm64_ldr_uoff(3, CG_ARM_REG(dst_r), CG_ARM_REG(base_r), uimm / 8)); break;
    }
#else
    X86Mem m = {CG_X86_REG(base_r), X86_NOREG, 1, (int64_t)uimm};
    x86_mov_rm(s, size, CG_X86_REG(dst_r), m);
#endif
    return s->len - off;
}
// ARM64 str to register offset (unsigned offset)
static size_t asm_str_reg_off(SecBuf *s, int src_r, int base_r, int size, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    switch (size) {
    case 1: secbuf_emit32le(s, arm64_strb_uoff(CG_ARM_REG(src_r), CG_ARM_REG(base_r), uimm)); break;
    case 2: secbuf_emit32le(s, arm64_strh_uoff(CG_ARM_REG(src_r), CG_ARM_REG(base_r), uimm)); break;
    case 4: secbuf_emit32le(s, arm64_str_uoff(2, CG_ARM_REG(src_r), CG_ARM_REG(base_r), uimm / 4)); break;
    default: secbuf_emit32le(s, arm64_str_uoff(3, CG_ARM_REG(src_r), CG_ARM_REG(base_r), uimm / 8)); break;
    }
#else
    X86Mem m = {CG_X86_REG(base_r), X86_NOREG, 1, (int64_t)uimm};
    x86_mov_mr(s, size, m, CG_X86_REG(src_r));
#endif
    return s->len - off;
}
// ARM64 load fp reg dst_fp_r from [base_r, #byte_off]; size=4→S, 8→D
static size_t asm_ldr_fp_off(SecBuf *s, int dst_fp_r, int base_r, int size, uint32_t byte_off) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sz = (size == 8) ? 3 : 2;
    secbuf_emit32le(s, arm64_ldr_fp(sz, dst_fp_r, CG_ARM_REG(base_r), byte_off));
#else
    (void)dst_fp_r;
    (void)base_r;
    (void)size;
    (void)byte_off;
#endif
    return s->len - off;
}
// ARM64 load float s/d from [base]; size=4→S(32bit), size=8→D(64bit)
static size_t asm_ldr_fp(SecBuf *s, int dst_fp_r, VReg base_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sz = (size == 8) ? 3 : 2; // sz=3→D(64bit), sz=2→S(32bit)
    secbuf_emit32le(s, arm64_ldr_fp(sz, dst_fp_r, CG_ARM_REG(base_r), 0));
#else
    X86Mem m = {CG_X86_REG(base_r), X86_NOREG, 1, 0};
    if (size == 8) x86_movsd_rm(s, (X86XmmReg)dst_fp_r, m);
    else
        x86_movss_rm(s, (X86XmmReg)dst_fp_r, m);
#endif
    return s->len - off;
}
// ARM64 store float s/d to [base]; size=4→S(32bit), size=8→D(64bit)
static size_t asm_str_fp(SecBuf *s, int src_fp_r, VReg base_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sz = (size == 8) ? 3 : 2;
    secbuf_emit32le(s, arm64_str_fp(sz, src_fp_r, CG_ARM_REG(base_r), 0));
#else
    X86Mem m = {CG_X86_REG(base_r), X86_NOREG, 1, 0};
    if (size == 8) x86_movsd_mr(s, m, (X86XmmReg)src_fp_r);
    else
        x86_movss_mr(s, m, (X86XmmReg)src_fp_r);
#endif
    return s->len - off;
}

// ARM64 sxtw
static size_t asm_sxtw(SecBuf *s, int dst_r, int src_r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_sxtw(CG_ARM_REG(dst_r), CG_ARM_REG(src_r)));
#endif
    return s->len - off;
}

// x86_64 movq GP register to XMM register
static size_t asm_movq_r_xmm(SecBuf *s, int xmm_dst, int gp_src) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_movq_r_xmm(s, (X86XmmReg)xmm_dst, CG_X86_REG(gp_src));
#endif
    return s->len - off;
}
// x86_64 movq XMM register to GP register
static size_t asm_movq_xmm_r(SecBuf *s, int gp_dst, int xmm_src) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_movq_xmm_r(s, CG_X86_REG(gp_dst), (X86XmmReg)xmm_src);
#endif
    return s->len - off;
}

// x86_64 SSE/FP binary ops: addsd, subsd, mulsd, divsd
static size_t asm_addsd(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_addsd(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_subsd(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_subsd(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_mulsd(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_mulsd(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_divsd(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_divsd(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
// x86_64 SSE/FP binary ops: addss, subss, mulss, divss
static size_t asm_addss(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_addss(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_subss(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_subss(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_mulss(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_mulss(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_divss(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_divss(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}

// x86_64 ucomisd / ucomiss (float compare, sets EFLAGS)
static size_t asm_ucomisd(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_ucomisd(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}
static size_t asm_ucomiss(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_ucomiss(s, X86_XMM0, X86_XMM1);
#endif
    return s->len - off;
}

// ARM64 fadd, fsub, fmul, fdiv (ftype=0 for single, 1 for double)
static size_t asm_fadd(SecBuf *s, int ftype) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fadd(ftype, 0, 0, 1)); // d0, d0, d1
#endif
    return s->len - off;
}
static size_t asm_fsub(SecBuf *s, int ftype) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fsub(ftype, 0, 0, 1));
#endif
    return s->len - off;
}
static size_t asm_fmul(SecBuf *s, int ftype) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fmul(ftype, 0, 0, 1));
#endif
    return s->len - off;
}
static size_t asm_fdiv(SecBuf *s, int ftype) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fdiv(ftype, 0, 0, 1));
#endif
    return s->len - off;
}

// ARM64 fcmp (ftype=0 for single, 1 for double)
static size_t asm_fcmp(SecBuf *s, int ftype) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fcmp(ftype, 0, 1)); // fcmp d0, d1
#endif
    return s->len - off;
}

// str d{fp_r}, [sp, #uimm] — store float/double to SP-relative slot
#ifdef ARCH_ARM64
static size_t asm_str_fp_sp_off(SecBuf *s, int fp_r, uint32_t uimm) {
    size_t off = s->len;
    int opc = (uimm % 8 == 0) ? 3 : 2; // 3=64-bit (d), 2=32-bit (s) stride
    secbuf_emit32le(s, arm64_str_fp(opc, fp_r, 31, uimm / (opc == 3 ? 8 : 4))); // str d{fp_r}, [sp, #uimm]
    return s->len - off;
}
// asr x17, x{src}, #63 — arithmetic shift right by 63 (sign-extend)
static size_t asm_asr_x17_reg_63(SecBuf *s, VReg src) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_asr_imm(1, 17, CG_ARM_REG(src), 63)); // asr x17, x{src}, #63
    return s->len - off;
}
// ldr w{dst}, [x{src}] / ldr x{dst}, [x{src}] — load GP from reg (sf=0→32bit, 1→64bit)
static size_t asm_ldr_phy_reg(SecBuf *s, int dst_phy, VReg src, int sf) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(sf, dst_phy, CG_ARM_REG(src), 0)); // ldr w/x{dst}, [x{src}]
    return s->len - off;
}
// mov x{dst_phy}, x{src_vreg} — move vreg to physical GP register (via orr xd, xzr, xs)
static size_t asm_mov_phy_reg(SecBuf *s, int dst_phy, VReg src, int sf) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(sf, dst_phy, ARM64_XZR, CG_ARM_REG(src), ARM64_LSL, 0)); // mov x{dst_phy}, x{src}
    return s->len - off;
}
// cmn vreg, #imm — compare negative (subs xzr, reg, imm)
static size_t asm_cmn_vreg_imm(SecBuf *s, VReg r, int sz, int32_t imm) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_subs_imm(sz == 8 ? 1 : 0, 31, CG_ARM_REG(r), imm, 0)); // cmn x{r}, #imm
    return s->len - off;
}
// fcvtzu w/x{r}, d0 — float→unsigned int conversion
static size_t asm_fcvtzu(SecBuf *s, VReg r, int sz) {
    size_t off = s->len;
    int sf = (sz == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_fcvtzu(sf, 1, CG_ARM_REG(r), 0)); // fcvtzu w/x{r}, d0
    return s->len - off;
}
// mov x19, x0 / mov x0, x19 — callee-saved retval save/restore
static size_t asm_mov_x19_x0(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 19, 31, 0, ARM64_LSL, 0)); // mov x19, x0
    return s->len - off;
}
static size_t asm_mov_x0_x19(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 0, 31, 19, ARM64_LSL, 0)); // mov x0, x19
    return s->len - off;
}
// add xrd, xrd, #0 — relocation placeholder for ADD_ABS_LO12
static size_t asm_add_rd_rd_0(SecBuf *s, int rd) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, rd, rd, 0, 0)); // add x{rd}, x{rd}, #0
    return s->len - off;
}
// ldr xrd, [xrd] — load pointer from self (GOT indirection)
static size_t asm_ldr_rd_rd(SecBuf *s, int rd) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(1, rd, rd, 0)); // ldr x{rd}, [x{rd}]
    return s->len - off;
}
#endif

// ============================================================================
// Atomics
// ============================================================================

// ============================================================================
// x86: atomic xchg and lock cmpxchg
// ============================================================================

#ifndef ARCH_ARM64
// xchg sz, (r_addr), r_val  — atomic exchange mem↔reg
static size_t asm_xchg_mem(SecBuf *s, VReg r_addr, VReg r_val, int size) {
    size_t off = s->len;
    // XCHG always has implicit LOCK prefix
    X86Mem m = {CG_X86_REG(r_addr), X86_NOREG, 1, 0};
    // xchg r/m, r: opcode 87 /r (for 2/4/8), 86 /r (for 1)
    X86Reg rv = CG_X86_REG(r_val);
    X86Reg ra = CG_X86_REG(r_addr);
    if (size == 8) secbuf_emit8(s, (uint8_t)(0x48 | ((rv >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0)));
    else if (size == 4 && (rv >= 8 || ra >= 8))
        secbuf_emit8(s, (uint8_t)(0x40 | ((rv >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0)));
    else if (size == 2)
        secbuf_emit8(s, 0x66);
    secbuf_emit8(s, size == 1 ? 0x86 : 0x87);
    uint8_t modrm = (uint8_t)(0x00 | ((rv & 7) << 3) | 4); // rm=4 for SIB
    // Use [r_addr] directly (no SIB if not rsp)
    if ((ra & 7) == 4) { // rsp needs SIB
        secbuf_emit8(s, modrm);
        secbuf_emit8(s, (uint8_t)(0x24)); // SIB: base=rsp, no index
    } else {
        secbuf_emit8(s, (uint8_t)(0x00 | ((rv & 7) << 3) | (ra & 7)));
    }
    return s->len - off;
}

// lock cmpxchg (r_addr), r_desired, size  — rax=expected, result in rax
// Sets ZF=1 if successful
static size_t asm_lock_cmpxchg_mem(SecBuf *s, VReg r_addr, VReg r_desired, int size) {
    size_t off = s->len;
    x86_lock_prefix(s); // lock
    X86Reg rd = CG_X86_REG(r_desired);
    X86Reg ra = CG_X86_REG(r_addr);
    // REX prefix if needed
    uint8_t rex = 0;
    if (size == 8) rex = (uint8_t)(0x48 | ((rd >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0));
    else if (rd >= 8 || ra >= 8)
        rex = (uint8_t)(0x40 | ((rd >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0));
    if (size == 2) secbuf_emit8(s, 0x66);
    if (rex) secbuf_emit8(s, rex);
    secbuf_emit8(s, 0x0F);
    secbuf_emit8(s, size == 1 ? 0xB0 : 0xB1); // cmpxchg m, r
    // ModRM: mod=00, reg=r_desired, rm=r_addr
    if ((ra & 7) == 4) {
        secbuf_emit8(s, (uint8_t)(0x00 | ((rd & 7) << 3) | 4));
        secbuf_emit8(s, (uint8_t)0x24);
    } else {
        secbuf_emit8(s, (uint8_t)(0x00 | ((rd & 7) << 3) | (ra & 7)));
    }
    return s->len - off;
}

// sete r  — set byte if equal (ZF=1)
static size_t asm_sete(SecBuf *s, VReg r) {
    size_t off = s->len;
    x86_setcc(s, X86_E, CG_X86_REG(r)); // sete r8
    return s->len - off;
}

// lock xadd (r_addr), r_old, size  — atomic add-and-fetch old value
// After: r_old = old value at (r_addr), (r_addr) = old + r_old
static size_t asm_lock_xadd_mem(SecBuf *s, VReg r_addr, VReg r_old, int size) {
    size_t off = s->len;
    x86_lock_prefix(s); // lock
    X86Reg rd = CG_X86_REG(r_old);
    X86Reg ra = CG_X86_REG(r_addr);
    uint8_t rex = 0;
    if (size == 8) rex = (uint8_t)(0x48 | ((rd >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0));
    else if (rd >= 8 || ra >= 8)
        rex = (uint8_t)(0x40 | ((rd >= 8 ? 1 : 0) << 2) | (ra >= 8 ? 1 : 0));
    if (size == 2) secbuf_emit8(s, 0x66);
    if (rex) secbuf_emit8(s, rex);
    secbuf_emit8(s, 0x0F);
    secbuf_emit8(s, size == 1 ? 0xC0 : 0xC1); // xadd r/m, r
    // ModRM: mod=00, reg=r_old, rm=r_addr
    if ((ra & 7) == 4) {
        secbuf_emit8(s, (uint8_t)(0x00 | ((rd & 7) << 3) | 4));
        secbuf_emit8(s, (uint8_t)0x24);
    } else {
        secbuf_emit8(s, (uint8_t)(0x00 | ((rd & 7) << 3) | (ra & 7)));
    }
    return s->len - off;
}

// mov sz, -%rbp_off(%rbp), reg  — store reg to spill slot
static size_t asm_mov_rbp_spill(SecBuf *s, VReg r, int size, int rbp_off) {
    size_t off = s->len;
    asm_mov_phyreg_rbp(s, CG_X86_REG(r), size, rbp_off); // mov reg, -rbp_off(%rbp)
    return s->len - off;
}

// mov sz, reg, -%rbp_off(%rbp)  — load reg from spill slot
static size_t asm_mov_spill_rbp(SecBuf *s, VReg r, int size, int rbp_off) {
    size_t off = s->len;
    asm_mov_rbp_phyreg(s, CG_X86_REG(r), size, rbp_off); // mov -rbp_off(%rbp), reg
    return s->len - off;
}

// lock cmpxchg (r_addr), r_new  — compare %rax with (r_addr), swap if equal; result in ZF
// Used for non-add fetch-ops (bitwise)
static size_t asm_lock_cmpxchg_rax(SecBuf *s, VReg r_addr, VReg r_new, int size) {
    return asm_lock_cmpxchg_mem(s, r_addr, r_new, size); // reuse the existing function
}

// add sz, -rbp_off(%rbp), reg  — add spill slot to reg (for add_fetch return)
static size_t asm_add_spill_reg(SecBuf *s, VReg r, int size, int rbp_off) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -rbp_off};
    x86_add_rm(s, size, CG_X86_REG(r), m); // add -rbp_off(%rbp), reg
    return s->len - off;
}
#endif

// ============================================================================
// ARM64: ldxr/stxr exclusive access (for atomic exchange/CAS)
// ============================================================================

#ifdef ARCH_ARM64
// ldxrb/ldxrh/ldxr dst, [base]  — load exclusive
static size_t asm_ldxr(SecBuf *s, VReg dst, VReg base, int size) {
    size_t off = s->len;
    if (size == 1)
        secbuf_emit32le(s, arm64_ldxrb(CG_ARM_REG(dst), CG_ARM_REG(base))); // ldxrb w{dst}, [x{base}]
    else if (size == 2)
        secbuf_emit32le(s, arm64_ldxrh(CG_ARM_REG(dst), CG_ARM_REG(base))); // ldxrh w{dst}, [x{base}]
    else
        secbuf_emit32le(s, arm64_ldxr(size == 8 ? 1 : 0, CG_ARM_REG(dst), CG_ARM_REG(base))); // ldxr w/x{dst}, [x{base}]
    return s->len - off;
}

// stxrb/stxrh/stxr w9, src, [base]  — store exclusive (result in w9)
static size_t asm_stxr(SecBuf *s, VReg src, VReg base, int size) {
    size_t off = s->len;
    if (size == 1)
        secbuf_emit32le(s, arm64_stxrb(9, CG_ARM_REG(src), CG_ARM_REG(base))); // stxrb w9, w{src}, [x{base}]
    else if (size == 2)
        secbuf_emit32le(s, arm64_stxrh(9, CG_ARM_REG(src), CG_ARM_REG(base))); // stxrh w9, w{src}, [x{base}]
    else
        secbuf_emit32le(s, arm64_stxr(size == 8 ? 1 : 0, 9, CG_ARM_REG(src), CG_ARM_REG(base))); // stxr w9, w/x{src}, [x{base}]
    return s->len - off;
}
#endif /* ARCH_ARM64 for ldxr/stxr */

#ifdef ARCH_ARM64
static size_t asm_dmb(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_dmb(0xb));
    asm_record(ASM_FENCE, off, 1, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return 4;
}
static size_t asm_dmb_ishld(SecBuf *s) { return asm_dmb(s); }
static size_t asm_dmb_ishst(SecBuf *s) { return asm_dmb(s); }
#endif /* ARCH_ARM64 for dmb */

// ============================================================================
// CLD / NOP
// ============================================================================

static size_t asm_cld(SecBuf *s) {
    size_t off = s->len;
#ifndef ARCH_ARM64
    x86_cld(s); // cld (clear direction flag)
    asm_record(ASM_CLD, off, (size_t)(s->len - off), -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
#else
    (void)s;
#endif
    return (size_t)(s->len - off);
}

static size_t asm_nop(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_nop());
#else
    x86_nop(s);
#endif
    asm_record(ASM_NOP, off, (size_t)(s->len - off), -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return (size_t)(s->len - off);
}

static size_t asm_and_imm(SecBuf *s, VReg r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_and_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), (uint64_t)(uint32_t)imm));
#else
    x86_and_ri(s, size, CG_X86_REG(r), imm);
#endif
    return s->len - off;
}
static size_t asm_or_imm(SecBuf *s, VReg r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm));
#else
    x86_or_ri(s, size, CG_X86_REG(r), imm);
#endif
    return s->len - off;
}
static size_t asm_xor_imm(SecBuf *s, VReg r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_eor_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm));
#else
    x86_xor_ri(s, size, CG_X86_REG(r), imm);
#endif
    return s->len - off;
}
static size_t asm_movz(SecBuf *s, int r, int sf, uint16_t imm16, int shift) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_movz(sf, CG_ARM_REG(r), imm16, shift));
#endif
    return s->len - off;
}
// fmov x{rd}, d{rn}  — copy fp reg raw bits to integer virtual reg
static size_t asm_fmov_f2i(SecBuf *s, VReg rd, int rn, int sf) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fmov_f2i(sf, CG_ARM_REG(rd), rn));
#endif
    return s->len - off;
}
// fmov d{rd}, x{rs}  — copy integer virtual reg raw bits to fp reg
static size_t asm_fmov_i2f(SecBuf *s, int fp_rd, int int_rs, int sf) {
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fmov_i2f(sf, fp_rd, CG_ARM_REG(int_rs)));
    return 4;
#else
    (void)fp_rd;
    (void)int_rs;
    (void)sf;
    return 0;
#endif
}
// scvtf d0, w/x{rs}  — signed int to double
static size_t asm_scvtf(SecBuf *s, int fp_rd, int int_rs, int sf) {
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_scvtf(sf, 1, fp_rd, CG_ARM_REG(int_rs)));
    return 4;
#else
    (void)fp_rd;
    (void)int_rs;
    (void)sf;
    return 0;
#endif
}
// ucvtf d0, w/x{rs}  — unsigned int to double
static size_t asm_ucvtf(SecBuf *s, int fp_rd, int int_rs, int sf) {
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ucvtf(sf, 1, fp_rd, CG_ARM_REG(int_rs)));
    return 4;
#else
    (void)fp_rd;
    (void)int_rs;
    (void)sf;
    return 0;
#endif
}
static size_t asm_fcvt(SecBuf *s, int opc, int ftype, int rd, int rn) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fcvt(opc, ftype, rd, rn));
#endif
    return s->len - off;
}
static size_t asm_stur(SecBuf *s, int src, int base, int sf, int off) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_stur(sf, CG_ARM_REG(src), CG_ARM_REG(base), off));
#else
    x86_mov_mr(s, sf == 1 ? 8 : 4, x86_mem(CG_X86_REG(base), off), CG_X86_REG(src));
#endif
    return s->len - off2;
}
// unscaled store for any size (byte/half/word/dword), negative offsets ok
static size_t asm_stur_sz(SecBuf *s, int src, int base, int sz, int off) {
#ifdef ARCH_ARM64
    switch (sz) {
    case 1: secbuf_emit32le(s, arm64_sturb(CG_ARM_REG(src), CG_ARM_REG(base), off)); break;
    case 2: secbuf_emit32le(s, arm64_sturh(CG_ARM_REG(src), CG_ARM_REG(base), off)); break;
    case 4: secbuf_emit32le(s, arm64_stur(0, CG_ARM_REG(src), CG_ARM_REG(base), off)); break;
    default: secbuf_emit32le(s, arm64_stur(1, CG_ARM_REG(src), CG_ARM_REG(base), off)); break;
    }
    return 4;
#else
    return asm_stur(s, src, base, sz == 8 ? 1 : 0, off);
#endif
}
static size_t asm_ldur(SecBuf *s, int dst, int base, int sf, int off) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldur(sf, CG_ARM_REG(dst), CG_ARM_REG(base), off));
#else
    x86_mov_rm(s, sf == 1 ? 8 : 4, CG_X86_REG(dst), x86_mem(CG_X86_REG(base), off));
#endif
    return s->len - off2;
}
// unscaled load for any size (byte/half/word/dword), negative offsets ok
static size_t asm_ldur_sz(SecBuf *s, int dst, int base, int sz, int off) {
#ifdef ARCH_ARM64
    switch (sz) {
    case 1: secbuf_emit32le(s, arm64_ldurb(CG_ARM_REG(dst), CG_ARM_REG(base), off)); break;
    case 2: secbuf_emit32le(s, arm64_ldurh(CG_ARM_REG(dst), CG_ARM_REG(base), off)); break;
    case 4: secbuf_emit32le(s, arm64_ldur(0, CG_ARM_REG(dst), CG_ARM_REG(base), off)); break;
    default: secbuf_emit32le(s, arm64_ldur(1, CG_ARM_REG(dst), CG_ARM_REG(base), off)); break;
    }
    return 4;
#else
    return asm_ldur(s, dst, base, sz == 8 ? 1 : 0, off);
#endif
}
static size_t asm_ldr_imm(SecBuf *s, int dst, int base, int sf, int off, bool pre) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_imm(sf, CG_ARM_REG(dst), CG_ARM_REG(base), off, pre));
#else
    x86_mov_rm(s, sf == 1 ? 8 : 4, CG_X86_REG(dst), x86_mem(CG_X86_REG(base), off));
#endif
    return s->len - off2;
}
static size_t asm_str_imm(SecBuf *s, int src, int base, int sf, int off, bool pre) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_imm(sf, CG_ARM_REG(src), CG_ARM_REG(base), off, pre));
#else
    x86_mov_mr(s, sf == 1 ? 8 : 4, x86_mem(CG_X86_REG(base), off), CG_X86_REG(src));
#endif
    return s->len - off2;
}
static size_t asm_stlr(SecBuf *s, int src, int base, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_stlr(size == 8 ? 1 : 0, CG_ARM_REG(src), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_stlrb(SecBuf *s, int src, int base) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_stlrb(CG_ARM_REG(src), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_stlrh(SecBuf *s, int src, int base) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_stlrh(CG_ARM_REG(src), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_ldar(SecBuf *s, int dst, int base, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldar(size == 8 ? 1 : 0, CG_ARM_REG(dst), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_ldarb(SecBuf *s, int dst, int base) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldarb(CG_ARM_REG(dst), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_ldarh(SecBuf *s, int dst, int base) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldarh(CG_ARM_REG(dst), CG_ARM_REG(base)));
#endif
    return s->len - off;
}
static size_t asm_ldrb(SecBuf *s, int dst, int base, int off) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldrb_imm(CG_ARM_REG(dst), CG_ARM_REG(base), off));
#else
    x86_movzx_rm(s, 4, 1, CG_X86_REG(dst), x86_mem(CG_X86_REG(base), off));
#endif
    return s->len - off2;
}
static size_t asm_ldrh(SecBuf *s, int dst, int base, int off) {
    size_t off2 = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldrh_imm(CG_ARM_REG(dst), CG_ARM_REG(base), off));
#else
    x86_movzx_rm(s, 4, 2, CG_X86_REG(dst), x86_mem(CG_X86_REG(base), off));
#endif
    return s->len - off2;
}
static size_t asm_adrp(SecBuf *s, int rd) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_adrp(rd, 0));
#endif
    return s->len - off;
}

#ifndef ARCH_ARM64
// movzx/movsx from memory addressed by a virtual register
static size_t asm_movzx_mem_reg(SecBuf *s, VReg dst, VReg src_addr, int dst_sz, int src_sz) {
    size_t off = s->len;
    x86_movzx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOVZX, off, s->len - off, dst, src_addr, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_movsx_mem_reg(SecBuf *s, VReg dst, VReg src_addr, int dst_sz, int src_sz) {
    size_t off = s->len;
    x86_movsx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOVSX, off, s->len - off, dst, src_addr, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}

// mov from [virtual_reg] to virtual_reg
static size_t asm_mov_mem_reg(SecBuf *s, VReg dst, VReg src_addr, int sz) {
    size_t off = s->len;
    x86_mov_rm(s, sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOV_RR, off, s->len - off, dst, src_addr, -1, sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}

// mov from virtual_reg to [virtual_reg]
static size_t asm_mov_reg_mem(SecBuf *s, VReg src, VReg dst_addr, int sz) {
    size_t off = s->len;
    x86_mov_mr(s, sz, x86_mem(CG_X86_REG(dst_addr), 0), CG_X86_REG(src));
    asm_record(ASM_MOV_RR, off, s->len - off, src, dst_addr, -1, sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}

// movaps from xmm to [rbp - offset]
static size_t asm_movaps_rbp_xmm(SecBuf *s, int xmm_idx, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movaps_mr(s, m, (X86XmmReg)xmm_idx);
    asm_record(ASM_MOV_RBPR, off, s->len - off, xmm_idx, -1, -1, 16, 0, offset, NULL, 0, -1, true);
    return s->len - off;
}

// ALU ops with rbp-relative memory operand
static size_t asm_and_rbp_reg(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_and_rm(s, size, CG_X86_REG(r), m);
    asm_record(ASM_AND_RR, off, s->len - off, r, -1, -1, size, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_or_rbp_reg(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_or_rm(s, size, CG_X86_REG(r), m);
    asm_record(ASM_OR_RR, off, s->len - off, r, -1, -1, size, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_xor_rbp_reg(SecBuf *s, int r, int size, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_xor_rm(s, size, CG_X86_REG(r), m);
    asm_record(ASM_XOR_RR, off, s->len - off, r, -1, -1, size, 0, offset, NULL, 0, -1, false);
    return s->len - off;
}
#endif /* !ARCH_ARM64 */

// ============================================================================
// x86: movss/movsd to/from [rbp ± offset] and physical XMM registers
// ============================================================================

#ifndef ARCH_ARM64
// movss xmm_src, -(offset)(%%rbp)  — store single float to frame
static size_t asm_movss_mr_rbp(SecBuf *s, int xmm_src, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movss_mr(s, m, (X86XmmReg)xmm_src); // movss xmm_src, -(offset)(%%rbp)
    return s->len - off;
}
// movsd xmm_src, -(offset)(%%rbp)  — store double float to frame
static size_t asm_movsd_mr_rbp(SecBuf *s, int xmm_src, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movsd_mr(s, m, (X86XmmReg)xmm_src); // movsd xmm_src, -(offset)(%%rbp)
    return s->len - off;
}
// movss offset(%%rbp), xmm_dst  — load single float from frame
static size_t asm_movss_rm_rbp(SecBuf *s, int xmm_dst, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movss_rm(s, (X86XmmReg)xmm_dst, m); // movss -offset(%%rbp), xmm_dst
    return s->len - off;
}
// movsd -offset(%%rbp), xmm_dst  — load double float from frame
static size_t asm_movsd_rm_rbp(SecBuf *s, int xmm_dst, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movsd_rm(s, (X86XmmReg)xmm_dst, m); // movsd -offset(%%rbp), xmm_dst
    return s->len - off;
}
// cvtsd2ss from XMM param_xmm[i], store to rbp — convert param double to float
static size_t asm_cvtsd2ss_xmm_rbp(SecBuf *s, int xmm_src, int offset) {
    size_t off = s->len;
    x86_cvtsd2ss(s, X86_XMM0, (X86XmmReg)xmm_src); // cvtsd2ss xmm_src, xmm0
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movss_mr(s, m, X86_XMM0); // movss xmm0, -(offset)(%%rbp)
    return s->len - off;
}
// movsd from XMM param to rbp
static size_t asm_movsd_xmm_rbp(SecBuf *s, int xmm_src, int offset) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_movsd_mr(s, m, (X86XmmReg)xmm_src); // movsd xmm_src, -(offset)(%%rbp)
    return s->len - off;
}
// movq from physical int param register to physical r11
static size_t asm_mov_preg_r11(SecBuf *s, X86Reg preg) {
    size_t off = s->len;
    x86_mov_rr(s, 8, X86_R11, preg); // movq preg, %%r11
    return s->len - off;
}
// movb -1(%%r11,%%r10), %%al  — load byte from struct param
static size_t asm_movb_r11_r10_al(SecBuf *s, int64_t disp) {
    size_t off = s->len;
    // movb disp(%%r11, %%r10, 1), %%al
    // REX.B (r11>=8): 0x41 or REX.XB if both? r10=10 (>8), r11=11 (>8)
    // Actually x86: r10=%%r10, r11=%%r11 are physical regs 10,11
    // ModRM: mod=00, reg=0(al), rm=4(SIB); SIB: scale=0, index=r10(2 in SIB with REX), base=r11(3 in SIB with REX)
    // REX: 0100_WRXB = 0100_0011 = 0x43 (W=0, R=0, X=1 for r10 index, B=1 for r11 base)
    secbuf_emit8(s, 0x43); // REX.XB
    secbuf_emit8(s, 0x8A); // MOV r8, r/m8
    if (disp == -1) {
        secbuf_emit8(s, (uint8_t)0x44); // ModRM: mod=01, reg=0(al), rm=4(SIB)
        secbuf_emit8(s, (uint8_t)0x13); // SIB: scale=0, index=010(r10), base=011(r11)
        secbuf_emit8(s, (uint8_t)0xFF); // disp8 = -1
    } else {
        secbuf_emit8(s, (uint8_t)0x04); // ModRM: mod=00, reg=0(al), rm=4(SIB)
        secbuf_emit8(s, (uint8_t)0x13); // SIB
    }
    return s->len - off;
}
// movb %%al, -(offset)-1(%%rbp,%%r10)  — store byte to local struct
static size_t asm_movb_al_rbp_r10(SecBuf *s, int offset) {
    size_t off = s->len;
    // movb %%al, -(offset)-1(%%rbp, %%r10, 1)
    // REX.X for r10 index: 0x42
    secbuf_emit8(s, 0x42); // REX.X
    secbuf_emit8(s, 0x88); // MOV r/m8, r8
    secbuf_emit8(s, (uint8_t)0x84); // ModRM: mod=10, reg=0(al), rm=4(SIB)
    secbuf_emit8(s, (uint8_t)0x15); // SIB: scale=0, index=010(r10), base=101(rbp)
    // disp32 = -(offset) - 1
    int32_t d = -(offset)-1;
    secbuf_emit8(s, (uint8_t)(d & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 8) & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 16) & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 24) & 0xFF));
    return s->len - off;
}
// movb offset-1(%%rbp,%%r10), %%al  — load byte from stack struct
static size_t asm_movb_rbp_r10_al(SecBuf *s, int offset) {
    size_t off = s->len;
    // movb (stack_offset-1)(%%rbp, %%r10, 1), %%al
    secbuf_emit8(s, 0x42); // REX.X
    secbuf_emit8(s, 0x8A); // MOV r8, r/m8
    secbuf_emit8(s, (uint8_t)0x84); // ModRM: mod=10, reg=0(al), rm=4(SIB)
    secbuf_emit8(s, (uint8_t)0x15); // SIB: scale=0, index=010(r10), base=101(rbp)
    int32_t d = offset - 1;
    secbuf_emit8(s, (uint8_t)(d & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 8) & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 16) & 0xFF));
    secbuf_emit8(s, (uint8_t)((d >> 24) & 0xFF));
    return s->len - off;
}
// mov offset(%%rbp), %al/%ax/%eax/%rax  — load from frame using tmpreg-appropriate size
static size_t asm_mov_rbp_tmpreg(SecBuf *s, int offset, int sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, offset};
    x86_mov_rm(s, sz, X86_RAX, m); // mov offset(%%rbp), %%rax/eax/ax/al
    return s->len - off;
}
// mov %al/%ax/%eax/%rax, -(offset)(%%rbp)  — store to frame using tmpreg
static size_t asm_mov_tmpreg_rbp(SecBuf *s, int offset, int sz) {
    size_t off = s->len;
    X86Mem m = {CG_X86_FP, X86_NOREG, 1, -offset};
    x86_mov_mr(s, sz, m, X86_RAX); // mov %%rax/eax/ax/al, -(offset)(%%rbp)
    return s->len - off;
}
#endif /* !ARCH_ARM64 */

// ============================================================================
// Bit scan / count — lzcnt/tzcnt/bsf/bsr/popcnt/cls
// ============================================================================

// tzcnt dst, src  — count trailing zeros (= ctz)
static size_t asm_tzcnt(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64: rbit + clz
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rbit(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    secbuf_emit32le(s, arm64_clz(sf, CG_ARM_REG(dst), CG_ARM_REG(dst)));
    return s->len - off;
#else
    x86_tzcnt(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // tzcnt dst, src
    return s->len - off;
#endif
}

// bsf dst, src  — bit scan forward (= tzcnt, undefined for 0)
static size_t asm_bsf(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rbit(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    secbuf_emit32le(s, arm64_clz(sf, CG_ARM_REG(dst), CG_ARM_REG(dst)));
    return s->len - off;
#else
    x86_bsf(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // bsf dst, src
    return s->len - off;
#endif
}

// bsr dst, src  — bit scan reverse (= 31/63 - clz, undefined for 0)
static size_t asm_bsr(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_clz(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    int bits = (size == 8) ? 63 : 31;
    secbuf_emit32le(s, arm64_sub_imm(sf, CG_ARM_REG(dst), 31, bits, 0)); // rsb: bits - clz
    return s->len - off;
#else
    x86_bsr(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // bsr dst, src
    return s->len - off;
#endif
}

// popcnt dst, src  — population count
static size_t asm_popcnt(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64 software popcount: fmov → cnt → addv → fmov → and
    // Note: ARM64 has no single-instruction scalar popcnt; use NEON
    // Caller is responsible for NEON sequence; this just emits x86 path.
    (void)dst;
    (void)src;
    (void)size;
    return 0;
#else
    x86_popcnt(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // popcnt dst, src
    return s->len - off;
#endif
}

// cls dst, src  — count leading sign bits (ARM64 only)
static size_t asm_cls(SecBuf *s, int dst, int src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_cls(sf, CG_ARM_REG(dst), CG_ARM_REG(src)));
    return s->len - off;
#else
    // x86: no cls; caller uses sar+xor+lzcnt sequence
    (void)dst;
    (void)src;
    (void)size;
    return 0;
#endif
}

// ============================================================================
// Rotate — rol/ror
// ============================================================================

// rol reg, imm  — rotate left by immediate
static size_t asm_rol_imm(SecBuf *s, VReg r, int size, uint8_t shift) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    // ARM64: ror by (bits - shift) == rol
    int sf = (size == 8) ? 1 : 0;
    int bits = size * 8;
    secbuf_emit32le(s, arm64_ror_reg(sf, CG_ARM_REG(r), CG_ARM_REG(r), (uint32_t)(bits - shift) & (bits - 1)));
    return s->len - off;
#else
    x86_rol_ri(s, size, CG_X86_REG(r), shift); // rol $shift, reg
    return s->len - off;
#endif
}

// ============================================================================
// Conditional move / select
// ============================================================================

// cmovcc dst, src  — conditional move (x86) / csel (ARM64)
// cond: X86Cond (x86) or Arm64Cond (ARM64)
static size_t asm_cmov(SecBuf *s, VReg dst, VReg src, int size, int cond) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    // csel dst, src, dst, inverse(cond): if cond true, dst=src, else dst=dst
    // To mimic cmovCC (move if condition true): csel dst, src, dst, cond
    secbuf_emit32le(s, arm64_csel(sf, CG_ARM_REG(dst), CG_ARM_REG(src), CG_ARM_REG(dst), (Arm64Cond)cond));
    return s->len - off;
#else
    x86_cmovcc(s, size, (X86Cond)cond, CG_X86_REG(dst), CG_X86_REG(src)); // cmovCC dst, src
    return s->len - off;
#endif
}

// csel dst, src, zero, eq  — select: if(cond) dst=0 else dst=src (for __builtin_ffs ARM64)
static size_t asm_csel_zero_if_eq(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    // csel dst, xzr, src, eq  (= if eq: 0, else src)
    secbuf_emit32le(s, arm64_csel(sf, CG_ARM_REG(dst), ARM64_XZR, CG_ARM_REG(src), ARM64_EQ));
    return s->len - off;
#else
    (void)dst;
    (void)src;
    (void)size;
    return 0;
#endif
}

// cneg dst, dst, mi  — negate if negative (ARM64 abs)
static size_t asm_cneg_mi(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_cneg(sf, CG_ARM_REG(r), CG_ARM_REG(r), ARM64_MI));
    return s->len - off;
#else
    (void)r;
    (void)size;
    return 0;
#endif
}

// ============================================================================
// LEA with register + small offset (for __builtin_ffs)
// ============================================================================

// leaq 1(src), dst  /  leal 1(src), dst — lea with 1-byte displacement
static size_t asm_lea_disp1(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_add_imm(sf, CG_ARM_REG(dst), CG_ARM_REG(src), 1, 0)); // add dst, src, #1
    return s->len - off;
#else
    X86Mem m = {CG_X86_REG(src), X86_NOREG, 1, 1};
    x86_lea(s, size, CG_X86_REG(dst), m); // lea 1(src), dst
    return s->len - off;
#endif
}

// ============================================================================
// Prefetch
// ============================================================================

#ifdef ARCH_ARM64
// prfm hint, [x{r}]  — prefetch memory
static size_t asm_prfm(SecBuf *s, int r, int prfop) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_prfm_imm(prfop, CG_ARM_REG(r), 0)); // prfm prfop, [x{r}]
    return s->len - off;
}
#endif

#ifndef ARCH_ARM64
// prefetchXX (%reg)  — x86 prefetch (emit as NOP since no single encoder)
// hint: 0=prefetchnta, 1=prefetcht2, 2=prefetcht1, 3=prefetcht0, 4=prefetchw
static size_t asm_prefetch(SecBuf *s, VReg r, int hint) {
    size_t off = s->len;
    // Encode prefetch as 0F 18 /reg mod rm:
    // /0=prefetchnta, /1=prefetcht2, /2=prefetcht1, /3=prefetcht0, prefetchw=0F 0D /1
    uint8_t reg_field = (uint8_t)(hint < 4 ? hint : 1);
    uint8_t modrm = (uint8_t)(0x00 | (reg_field << 3) | (CG_X86_REG(r) & 7));
    if (hint == 4) { // prefetchw: 0F 0D /1
        if ((int)CG_X86_REG(r) >= 8) secbuf_emit8(s, 0x41); // REX.B
        secbuf_emit8(s, 0x0F);
        secbuf_emit8(s, 0x0D);
        secbuf_emit8(s, modrm);
    } else {
        if ((int)CG_X86_REG(r) >= 8) secbuf_emit8(s, 0x41); // REX.B
        secbuf_emit8(s, 0x0F);
        secbuf_emit8(s, 0x18);
        secbuf_emit8(s, modrm);
    }
    return s->len - off;
}
#endif

// ============================================================================
// ARM64 overflow-checked arithmetic: adds/subs/umulh/smulh/umull/smull
// ============================================================================

#ifdef ARCH_ARM64
// adds dst, dst, src  — add and set flags
static size_t asm_adds(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_adds_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0)); // adds dst, dst, src
    return s->len - off;
}

// subs dst, dst, src  — subtract and set flags
static size_t asm_subs(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_subs_reg(sf, CG_ARM_REG(dst), CG_ARM_REG(dst), CG_ARM_REG(src), ARM64_LSL, 0)); // subs dst, dst, src
    return s->len - off;
}

// cset dst, cond  — conditional set (already exists as asm_cset)

// umulh dst, a, b  — unsigned multiply high (128-bit result high word)
static size_t asm_umulh(SecBuf *s, VReg dst, VReg a, VReg b) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_umulh(CG_ARM_REG(dst), CG_ARM_REG(a), CG_ARM_REG(b))); // umulh dst, a, b
    return s->len - off;
}

// smulh dst, a, b  — signed multiply high
static size_t asm_smulh(SecBuf *s, VReg dst, VReg a, VReg b) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_smulh(CG_ARM_REG(dst), CG_ARM_REG(a), CG_ARM_REG(b))); // smulh dst, a, b
    return s->len - off;
}

// umull xdst, wa, wb  — unsigned multiply long (32x32→64)
static size_t asm_umull(SecBuf *s, VReg dst, VReg a, VReg b) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_umull(CG_ARM_REG(dst), CG_ARM_REG(a), CG_ARM_REG(b))); // umull dst, wa, wb
    return s->len - off;
}

// smull xdst, wa, wb  — signed multiply long (32x32→64)
static size_t asm_smull(SecBuf *s, VReg dst, VReg a, VReg b) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_smull(CG_ARM_REG(dst), CG_ARM_REG(a), CG_ARM_REG(b))); // smull dst, wa, wb
    return s->len - off;
}
#endif /* ARCH_ARM64 */

// ============================================================================
// x86 unsigned multiply 1-operand (mul %reg — rdx:rax = rax * reg)
// ============================================================================

#ifndef ARCH_ARM64
// mul src  — unsigned multiply: rdx:rax = rax * src
static size_t asm_mul_1op(SecBuf *s, VReg src, int size) {
    size_t off = s->len;
    x86_imul_r(s, size, CG_X86_REG(src)); // imul src (unsigned 1-op not in x86_enc; use mul)
    // Note: x86_imul_r emits IMUL (signed), for unsigned we need MUL opcode
    // Rewind and emit proper MUL
    s->len = off;
    // MUL r/m: size=8 → REX.W + F7 /4; size=4 → F7 /4
    X86Reg r = CG_X86_REG(src);
    if (size == 8) secbuf_emit8(s, (uint8_t)(0x48 | ((r >> 3) & 1))); // REX.W (+REX.B if r>=8)
    else if ((int)r >= 8)
        secbuf_emit8(s, 0x41); // REX.B
    secbuf_emit8(s, 0xF7);
    secbuf_emit8(s, (uint8_t)(0xC0 | (4 << 3) | (r & 7))); // ModRM: /4 = mul
    return s->len - off;
}
#endif

// ============================================================================
// ARM64 NEON instructions for popcnt / variadic quad float
// ============================================================================

#ifdef ARCH_ARM64
// fmov d30, x{r}  — move GP int64 to NEON d-register (scalar)
static size_t asm_fmov_gp_to_d30(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_fmov_i2f(1, 30, CG_ARM_REG(r))); // fmov d30, x{r}
    return s->len - off;
}

// fmov s30, w{r}  — move GP int32 to NEON s-register (scalar)
static size_t asm_fmov_gp_to_s30(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_fmov_i2f(0, 30, CG_ARM_REG(r))); // fmov s30, w{r}
    return s->len - off;
}

// fmov w{r}, s30  — move NEON s-register scalar to GP int32
static size_t asm_fmov_s30_to_gp(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_fmov_f2i(0, CG_ARM_REG(r), 30)); // fmov w{r}, s30
    return s->len - off;
}

// cnt v30.8b, v30.8b  — popcount bytes in NEON register
// Encoding: 0_101_1110_00_10000_01010_1_nnnnn_ddddd (SIMD CNT)
static size_t asm_neon_cnt_v30(SecBuf *s) {
    size_t off = s->len;
    // cnt vd.8b, vn.8b: 0x0E205800 | (vn<<5) | vd  (Q=0, size=00, opcode=00101, U=0)
    secbuf_emit32le(s, 0x0E205800u | (30u << 5) | 30u); // cnt v30.8b, v30.8b
    return s->len - off;
}

// addv b30, v30.8b  — horizontal add all bytes → byte scalar
// Encoding: 0_0_1_01110_00_11000_11011_10_nnnnn_ddddd
static size_t asm_neon_addv_b30(SecBuf *s) {
    size_t off = s->len;
    // addv bd, vn.8b: 0x0E31B800 | (vn<<5) | bd
    secbuf_emit32le(s, 0x0E31B800u | (30u << 5) | 30u); // addv b30, v30.8b
    return s->len - off;
}

// fmov d{rd}, x{gp}  — for named ld arg: fmov to fp arg register
static size_t asm_fmov_gp_to_d(SecBuf *s, int fp_rd, VReg gp_rs) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_fmov_i2f(1, fp_rd, CG_ARM_REG(gp_rs))); // fmov d{fp_rd}, x{gp_rs}
    return s->len - off;
}

// ubfx x17, x{r}, #52, #11  — extract exponent bits from double
static size_t asm_ubfx_x17_exp(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ubfx(1, 17, CG_ARM_REG(r), 52, 11)); // ubfx x17, x{r}, #52, #11
    return s->len - off;
}

// mov x16, #15360  — constant for quad float exponent bias
static size_t asm_mov_x16_15360(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_movz(1, 16, 15360, 0)); // mov x16, #15360
    return s->len - off;
}

// add x17, x17, x16  — add exponent + bias
static size_t asm_add_x17_x17_x16(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_reg(1, 17, 17, 16, ARM64_LSL, 0)); // add x17, x17, x16
    return s->len - off;
}

// lsl x16, x{r}, #12  — shift mantissa
static size_t asm_lsl_x16_r_12(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsl_imm(1, 16, CG_ARM_REG(r), 12)); // lsl x16, x{r}, #12
    return s->len - off;
}

// lsr x16, x16, #12  — shift back mantissa
static size_t asm_lsr_x16_x16_12(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsr_imm(1, 16, 16, 12)); // lsr x16, x16, #12
    return s->len - off;
}

// and x1, x16, #0xF  — low nibble
static size_t asm_and_x1_x16_0xf(SecBuf *s) {
    size_t off = s->len;
    int N, immr, imms;
    uint64_t enc = arm64_encode_logic_imm(1, 0xF, &N, &immr, &imms);
    secbuf_emit32le(s, arm64_and_imm(1, 1, 16, enc)); // and x1, x16, #0xF
    return s->len - off;
}

// lsl x1, x1, #60  — position low nibble at top
static size_t asm_lsl_x1_60(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsl_imm(1, 1, 1, 60)); // lsl x1, x1, #60
    return s->len - off;
}

// lsr x2, x16, #4  — mantissa high part
static size_t asm_lsr_x2_x16_4(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsr_imm(1, 2, 16, 4)); // lsr x2, x16, #4
    return s->len - off;
}

// lsl x17, x17, #48  — position exponent
static size_t asm_lsl_x17_48(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsl_imm(1, 17, 17, 48)); // lsl x17, x17, #48
    return s->len - off;
}

// orr x2, x2, x17  — combine mantissa + exponent
static size_t asm_orr_x2_x2_x17(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 2, 2, 17, ARM64_LSL, 0)); // orr x2, x2, x17
    return s->len - off;
}

// and x17, x17, #1  — isolate sign bit
static size_t asm_and_x17_1(SecBuf *s) {
    size_t off = s->len;
    int N, immr, imms;
    uint64_t enc = arm64_encode_logic_imm(1, 1, &N, &immr, &imms);
    secbuf_emit32le(s, arm64_and_imm(1, 17, 17, enc)); // and x17, x17, #1
    return s->len - off;
}

// lsl x17, x17, #63  — position sign bit
static size_t asm_lsl_x17_63(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_lsl_imm(1, 17, 17, 63)); // lsl x17, x17, #63
    return s->len - off;
}

// orr x2, x2, x17  — add sign to result (reuse orr_x2_x2_x17)

// mov x1, #0  — zero x1
static size_t asm_mov_x1_0(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_movz(1, 1, 0, 0)); // mov x1, #0
    return s->len - off;
}

// mov x2, #0  — zero x2
static size_t asm_mov_x2_0(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_movz(1, 2, 0, 0)); // mov x2, #0
    return s->len - off;
}

// ins vN.d[0], x1  — insert x1 into NEON vector element 0
static size_t asm_ins_vd0_x1(SecBuf *s, int vn) {
    size_t off = s->len;
    // INS vd.d[0], x1: 0x4E081C00 | (imm5=10000=d[0]) | (vn<<5)|(vd)
    // Encoding: INS (general) = 0_1_001110_00001000_000111_rn_rd for d[0]
    // imm5 for d[idx]: imm5 = (idx<<1)|1 = 0b10000+0 = 0x10 for idx=0... let me use raw encoding
    // 0100_1110_0000_1000_0001_11_xxxxxx_ddddd  (Rt=1, vd=vn)
    // ins vd.d[idx] from xn: 0x4E081C00 | (imm5=0b10000<<idx??)
    // Correct: Q=1, op=1, imm5=10000 (d[0]), imm4=0111, n=x1, d=vn
    // 0x4E081C20 = ins v0.d[0], x1; adjust: set Rd=vn, Rn=1
    secbuf_emit32le(s, 0x4E081C00u | (1u << 5) | (uint32_t)vn); // ins v{vn}.d[0], x1
    return s->len - off;
}

// ins vN.d[1], x2  — insert x2 into NEON vector element 1
static size_t asm_ins_vd1_x2(SecBuf *s, int vn) {
    size_t off = s->len;
    // INS vd.d[1] from x2: imm5=0b10001 (d[1]), Rn=2, Rd=vn
    // 0x4E180C00 | (2<<5) | vn -- let me calculate properly
    // imm5 for d[idx]: bit0=1, bit4..1=idx => d[1]: imm5=0b10001=0x11=17
    // opcode: 0x4E_imm5_xxx_1_1100_Rn_Rd: 0x4E000000 | (imm5<<16) | (7<<12) | (1<<10) | (rn<<5) | rd
    // Actually: ins = 0x4E000000|(Q=1)<<30|(op=0)<<29|(imm5<<16)|(imm4<<12)|(1<<10)|(Rn<<5)|Rd
    // imm5=0b10001, imm4=0b0111
    secbuf_emit32le(s, 0x4E180C00u | (2u << 5) | (uint32_t)vn); // ins v{vn}.d[1], x2 ... approx
    return s->len - off;
}

// mov x8, x{r}  — move hidden ret pointer to x8 for ARM64 ABI
static size_t asm_mov_x8_reg(SecBuf *s, VReg r) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_orr_reg(1, 8, ARM64_XZR, CG_ARM_REG(r), ARM64_LSL, 0)); // mov x8, x{r}
    return s->len - off;
}

// stp q{rt1}, q{rt2}, [sp, #imm7_bytes]  — store NEON quad pair
static size_t asm_stp_q_sp(SecBuf *s, int rt1, int rt2, int32_t byte_off) {
    size_t off = s->len;
    // opc=2 for Q (128-bit), imm7 is in units of 16 bytes
    int32_t imm7 = byte_off / 16;
    secbuf_emit32le(s, arm64_stp_fp(2, rt1, rt2, ARM64_SP, imm7, false, false)); // stp q{rt1}, q{rt2}, [sp, #byte_off]
    return s->len - off;
}

// sub x16, x29, #var->offset  — compute stack frame address
static size_t asm_sub_x16_fp_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sub_imm(1, 16, 29, imm, 0)); // sub x16, x29, #imm
    return s->len - off;
}

// sub x16, x29, x16  — compute stack frame address (large offset)
static size_t asm_sub_x16_fp_x16(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sub_reg(1, 16, 29, 16, ARM64_LSL, 0)); // sub x16, x29, x16
    return s->len - off;
}

// str s{fp_param}, [x16, #off]  — store float parameter to stack
static size_t asm_str_s_x16_off(SecBuf *s, int fp_reg, int32_t off_bytes) {
    size_t off = s->len;
    // str s{fp_reg}, [x16, #off_bytes]: opc=0 (single), imm offset in units of 4
    uint32_t uimm = (uint32_t)(off_bytes / 4);
    secbuf_emit32le(s, arm64_str_fp(0, fp_reg, 16, uimm)); // str s{fp_reg}, [x16, #off_bytes]
    return s->len - off;
}

// str d{fp_param}, [x16, #off]  — store double parameter to stack
static size_t asm_str_d_x16_off(SecBuf *s, int fp_reg, int32_t off_bytes) {
    size_t off = s->len;
    uint32_t uimm = (uint32_t)(off_bytes / 8);
    secbuf_emit32le(s, arm64_str_fp(1, fp_reg, 16, uimm)); // str d{fp_reg}, [x16, #off_bytes]
    return s->len - off;
}

// str s{fp_param}, [x29, #-offset]  — store float parameter to frame
static size_t asm_str_s_fp_neg(SecBuf *s, int fp_reg, int32_t offset) {
    size_t off = s->len;
    // stur s{fp_reg}, [x29, #-offset]  (unscaled for negative)
    // or str s{fp_reg}, [x29, #-offset] using pre-index? Use STUR
    secbuf_emit32le(s, arm64_stur(0, fp_reg, 29, -offset)); // stur s{fp_reg}, [x29, #-offset]
    return s->len - off;
}

// str d{fp_param}, [x29, #-offset]  — store double parameter to frame
static size_t asm_str_d_fp_neg(SecBuf *s, int fp_reg, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stur(1, fp_reg, 29, -offset)); // stur d{fp_reg}, [x29, #-offset]
    return s->len - off;
}

// fcvt s0, d{fp_param}  — double to single conversion (for oldstyle float params)
static size_t asm_fcvt_s0_d(SecBuf *s, int fp_src) {
    size_t off = s->len;
    // fcvt sd, dn: opc=3 (single), ftype=1 (double), rd=0, rn=fp_src
    secbuf_emit32le(s, arm64_fcvt(3, 1, 0, fp_src)); // fcvt s0, d{fp_src}
    return s->len - off;
}

// str s0, [x29, #-offset]  — store s0 (after fcvt)
static size_t asm_str_s0_fp_neg(SecBuf *s, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stur(0, 0, 29, -offset)); // stur s0, [x29, #-offset]
    return s->len - off;
}

// ldr s0, [x29, #spoff]  — load float from stack
static size_t asm_ldr_s0_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_fp(0, 0, 29, (uint32_t)(spoff / 4))); // ldr s0, [x29, #spoff]
    return s->len - off;
}

// ldr d0, [x29, #spoff]  — load double from stack
static size_t asm_ldr_d0_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_fp(1, 0, 29, (uint32_t)(spoff / 8))); // ldr d0, [x29, #spoff]
    return s->len - off;
}

// ldrb w11, [x29, #spoff]  — load byte from stack slot (stack param)
static size_t asm_ldrb_w11_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldrb_uoff(11, 29, (uint32_t)spoff)); // ldrb w11, [x29, #spoff]
    return s->len - off;
}

// ldrh w11, [x29, #spoff]  — load halfword from stack slot
static size_t asm_ldrh_w11_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldrh_uoff(11, 29, (uint32_t)(spoff / 2))); // ldrh w11, [x29, #spoff]
    return s->len - off;
}

// ldr w11, [x29, #spoff]  — load 32-bit from stack slot
static size_t asm_ldr_w11_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(2, 11, 29, (uint32_t)(spoff / 4))); // ldr w11, [x29, #spoff]
    return s->len - off;
}

// ldr x11, [x29, #spoff]  — load 64-bit from stack slot
static size_t asm_ldr_x11_fp_off(SecBuf *s, int32_t spoff) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_uoff(3, 11, 29, (uint32_t)(spoff / 8))); // ldr x11, [x29, #spoff]
    return s->len - off;
}

// strb w11, [x29, #-offset]  — store byte to frame
static size_t asm_strb_w11_fp_neg(SecBuf *s, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sturb(11, 29, -offset)); // sturb w11, [x29, #-offset]
    return s->len - off;
}

// strh w11, [x29, #-offset]  — store halfword to frame
static size_t asm_strh_w11_fp_neg(SecBuf *s, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_sturh(11, 29, -offset)); // sturh w11, [x29, #-offset]
    return s->len - off;
}

// str w11, [x29, #-offset]  — store 32-bit to frame
static size_t asm_str_w11_fp_neg(SecBuf *s, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stur(0, 11, 29, -offset)); // stur w11, [x29, #-offset]  sf=0
    return s->len - off;
}

// str x11, [x29, #-offset]  — store 64-bit to frame
static size_t asm_str_x11_fp_neg(SecBuf *s, int32_t offset) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_stur(1, 11, 29, -offset)); // stur x11, [x29, #-offset] sf=1
    return s->len - off;
}

#endif /* ARCH_ARM64 */

// ============================================================================
// x86: add/sub with flags (for overflow builtins)
// ============================================================================

#ifndef ARCH_ARM64
// add dst, src (with flags for carry/overflow detection)
static size_t asm_add_rr_flags(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
    x86_add_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // add dst, src
    return s->len - off;
}

// sub dst, src (with flags)
static size_t asm_sub_rr_flags(SecBuf *s, VReg dst, VReg src, int size) {
    size_t off = s->len;
    x86_sub_rr(s, size, CG_X86_REG(dst), CG_X86_REG(src)); // sub dst, src
    return s->len - off;
}

// mov [raddr], src  — store register to memory via pointer in raddr
static size_t asm_mov_mem_via_reg(SecBuf *s, VReg src, VReg raddr, int size) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(raddr), X86_NOREG, 1, 0};
    x86_mov_mr(s, size, m, CG_X86_REG(src)); // mov src, (raddr)
    return s->len - off;
}
#endif

// ============================================================================
// x86: mov (%reg), dst  — indirect load via pointer
// ============================================================================

#ifndef ARCH_ARM64
// mov (%rr), %rr  — indirect load (for is_frame_addr / is_ret_addr depth loop)
static size_t asm_mov_indir(SecBuf *s, VReg r, int size) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(r), X86_NOREG, 1, 0};
    x86_mov_rm(s, size, CG_X86_REG(r), m); // mov (%rr), %rr
    return s->len - off;
}

// mov N(%rr), %rr  — indirect load with displacement
static size_t asm_mov_indir_disp(SecBuf *s, VReg r, int64_t disp, int size) {
    size_t off = s->len;
    X86Mem m = {CG_X86_REG(r), X86_NOREG, 1, disp};
    x86_mov_rm(s, size, CG_X86_REG(r), m); // mov disp(%rr), %rr
    return s->len - off;
}
#endif

// ============================================================================
// ARM64 rev16  (already stubbed; provide real implementation)
// ============================================================================

static size_t asm_rev16_real(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_rev16(sf, CG_ARM_REG(r), CG_ARM_REG(r)));
    return s->len - off;
#else
    x86_rol_ri(s, 2, CG_X86_REG(r), 8); // rol $8, %rx16
    return s->len - off;
#endif
}

// ============================================================================
// ARM64: ldr{b,h} [base, #uimm] — unsigned offset load byte/halfword
// ============================================================================
static size_t asm_ldrb_uoff(SecBuf *s, int dst, int base, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldrb_uoff(CG_ARM_REG(dst), CG_ARM_REG(base), uimm)); // ldrb w{dst}, [x{base}, #uimm]
#endif
    return s->len - off;
}
static size_t asm_ldrh_uoff(SecBuf *s, int dst, int base, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldrh_uoff(CG_ARM_REG(dst), CG_ARM_REG(base), uimm)); // ldrh w{dst}, [x{base}, #uimm]
#endif
    return s->len - off;
}
static size_t asm_strb_uoff(SecBuf *s, int src, int base, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_strb_uoff(CG_ARM_REG(src), CG_ARM_REG(base), uimm)); // strb w{src}, [x{base}, #uimm]
#endif
    return s->len - off;
}
static size_t asm_strh_uoff(SecBuf *s, int src, int base, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_strh_uoff(CG_ARM_REG(src), CG_ARM_REG(base), uimm)); // strh w{src}, [x{base}, #uimm]
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: ldrb w16, [x{base}, x9] / strb w16, [x{dst}, x9]
// ============================================================================
static size_t asm_ldrb_w16_x9(SecBuf *s, int base) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_reg(0, 16, CG_ARM_REG(base), 9, false, 0)); // ldrb w16, [x{base}, x9]
#endif
    return s->len - off;
}
static size_t asm_strb_w16_x9(SecBuf *s, int dst) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_reg(0, 16, CG_ARM_REG(dst), 9, false, 0)); // strb w16, [x{dst}, x9]
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: strb/wzr and ldrb/scaled patterns
// ============================================================================
static size_t asm_str_xzr_w11_x9(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_reg(0, ARM64_XZR, 11, 9, false, 0)); // strb wzr, [x11, x9]
#endif
    return s->len - off;
}
static size_t asm_str_xzr_sp_x16(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_reg(3, ARM64_XZR, ARM64_SP, 16, false, 0)); // str xzr, [sp, x16]
#endif
    return s->len - off;
}
static size_t asm_ldrb_w17_sp_x16(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_reg(0, 17, ARM64_SP, 16, false, 0)); // ldrb w17, [sp, x16]
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: stp xzr, xzr, [x{addr}, #zb]
// ============================================================================
static size_t asm_stp_xzr_xzr(SecBuf *s, int addr, int32_t imm7) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_stp(1, ARM64_XZR, ARM64_XZR, CG_ARM_REG(addr), imm7, false, false)); // stp xzr, xzr, [x{addr}, #zb]
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: mov x9, x{reg} via orr,  sub sp, sp, x16,  subs x16, x16, #imm
// ============================================================================
static size_t asm_mov_x9_vreg(SecBuf *s, int r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_orr_reg(1, 9, 31, CG_ARM_REG(r), ARM64_LSL, 0)); // mov x9, x{r}
#endif
    return s->len - off;
}
static size_t asm_mov_x16_vreg(SecBuf *s, int r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_orr_reg(1, 16, 31, CG_ARM_REG(r), ARM64_LSL, 0)); // mov x16, x{r}
#endif
    return s->len - off;
}
static size_t asm_mov_vreg_x12(SecBuf *s, int r) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_orr_reg(1, CG_ARM_REG(r), 31, 12, ARM64_LSL, 0)); // mov x{r}, x12
#endif
    return s->len - off;
}
static size_t asm_mov_x9_2(SecBuf *s, int r_value, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(sf, 9, 31, CG_ARM_REG(r_value), ARM64_LSL, 0)); // mov w9/x9, r_value
#endif
    return s->len - off;
}
static size_t asm_mov_x16_x12(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_orr_reg(1, 16, 31, 12, ARM64_LSL, 0)); // mov x16, x12
#endif
    return s->len - off;
}
static size_t asm_mov_x12_x16(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_orr_reg(1, 12, 31, 16, ARM64_LSL, 0)); // mov x12, x16
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: sub x11, x29, x16  /  sub sp, sp, x16  /  sub x11, x29, #off
// ============================================================================
static size_t asm_sub_x11_fp_x16(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_sub_reg(1, 11, 29, 16, ARM64_LSL, 0)); // sub x11, x29, x16
#endif
    return s->len - off;
}
static size_t asm_sub_sp_sp_x16_v2(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_sub_reg(1, 31, 31, 16, ARM64_LSL, 0)); // sub sp, sp, x16
#endif
    return s->len - off;
}
static size_t asm_sub_x11_fp_imm(SecBuf *s, int32_t offset) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_sub_imm(1, 11, 29, offset, 0)); // sub x11, x29, #offset
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: subs x16, x16, #imm
// ============================================================================
static size_t asm_subs_x16_imm(SecBuf *s, int32_t imm12) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_subs_imm(1, 16, 16, imm12, 0)); // subs x16, x16, #imm12
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: fneg d0, d0  /  fmov d1, xzr  /  csel
// ============================================================================
static size_t asm_fneg_d0(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fneg(1, 0, 0)); // fneg d0, d0
#endif
    return s->len - off;
}
static size_t asm_fmov_d1_xzr(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_fmov_i2f(1, 1, ARM64_XZR)); // fmov d1, xzr
#endif
    return s->len - off;
}
static size_t asm_csel_vs_zero(SecBuf *s, int r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_csel(size == 8 ? 1 : 0, CG_ARM_REG(r), CG_ARM_REG(r), ARM64_XZR, ARM64_VS)); // csel w{r}, w{r}, wzr, vs
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: va_arg helpers — ldr/str w16/x16/x12 to [x{r}, #imm*8]
// ============================================================================
static size_t asm_add_x16_fp_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_imm(1, 16, 29, imm, 0)); // add x16, x29, #imm
#endif
    return s->len - off;
}
static size_t asm_add_x16_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_imm(1, 16, 16, imm, 0)); // add x16, x16, #imm
#endif
    return s->len - off;
}
static size_t asm_add_w16_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_imm(0, 16, 16, imm, 0)); // add w16, w16, #imm
#endif
    return s->len - off;
}
static size_t asm_add_x12_imm(SecBuf *s, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_imm(1, 12, 12, imm, 0)); // add x12, x12, #imm
#endif
    return s->len - off;
}
static size_t asm_add_x12_x12_x17(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_add_reg(1, 12, 12, 17, ARM64_LSL, 0)); // add x12, x12, x17
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: ldr/str with unsigned scaled offset (va_arg patterns)
// ============================================================================
static size_t asm_str_x16_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_uoff(3, 16, CG_ARM_REG(base_r), uimm)); // str x16, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_ldr_x16_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_uoff(3, 16, CG_ARM_REG(base_r), uimm)); // ldr x16, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_str_w16_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_uoff(2, 16, CG_ARM_REG(base_r), uimm)); // str w16, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_ldr_w16_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_uoff(2, 16, CG_ARM_REG(base_r), uimm)); // ldr w16, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_ldr_x12_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_uoff(3, 12, CG_ARM_REG(base_r), uimm)); // ldr x12, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_str_x12_uoff(SecBuf *s, int base_r, uint32_t uimm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_str_uoff(3, 12, CG_ARM_REG(base_r), uimm)); // str x12, [x{base_r}, #uimm*8]
#endif
    return s->len - off;
}
static size_t asm_ldr_x12_0(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_uoff(3, 12, 12, 0)); // ldr x12, [x12]
#endif
    return s->len - off;
}
static size_t asm_ldr_x16_0(SecBuf *s) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_ldr_uoff(3, 16, 12, 0)); // ldr x16, [x12]
#endif
    return s->len - off;
}
static size_t asm_and_x12_imm(SecBuf *s, uint64_t imm_enc) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_and_imm(1, 12, 12, imm_enc)); // and x12, x12, #imm_enc
#endif
    return s->len - off;
}

// ============================================================================
// ARM64: stxr (atomic)
// ============================================================================
static size_t asm_stxr_8(SecBuf *s, int r_tmp, int r_addr, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_stxr(sf, 8, CG_ARM_REG(r_tmp), CG_ARM_REG(r_addr))); // stxr w8, r_tmp, [r_addr]
#endif
    return s->len - off;
}

#endif // CODEGEN_ASM_H
