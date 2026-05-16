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
#define CG_X86_REG(r)  cg_x86_reg[r]
#define CG_X86_FP      X86_RBP
#define CG_X86_SP      X86_RSP
#endif

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

static size_t asm_mov_reg_reg(SecBuf *s, int dst, int src, int size) {
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

// Move ARM64 physical x0 (integer return register) to virtual register r
static size_t asm_mov_retval(SecBuf *s, int r, int size) {
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_reg(sf, CG_ARM_REG(r), ARM64_XZR, 0, ARM64_LSL, 0)); // mov x{r}, x0
    return 4;
#else
    x86_mov_rr(s, size, CG_X86_REG(r), X86_RAX);
    return s->len - s->len; // size computed by caller
#endif
}
static size_t asm_mov_imm(SecBuf *s, int r, int size, int64_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    bool is_w = (size <= 4);
    uint64_t uval = is_w ? (uint64_t)(uint32_t)imm : (uint64_t)imm;
    int rd = CG_ARM_REG(r);
    int sf = is_w ? 0 : 1;
    secbuf_emit32le(s, arm64_movz(sf, rd, (uint16_t)(uval & 0xffff), 0));
    size_t count = 1;
    uint64_t v = uval >> 16;
    int shift = 16;
    int max_shift = is_w ? 16 : 48;
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

static size_t asm_movq_zero(SecBuf *s, int r) { return asm_xor_reg_reg(s, r, r, 8); }
static size_t asm_movl_zero(SecBuf *s, int r) { return asm_xor_reg_reg(s, r, r, 4); }

#ifdef ARCH_ARM64
static size_t asm_movk(SecBuf *s, int r, int sf, uint16_t imm16, int shift) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_movk(sf, CG_ARM_REG(r), imm16, shift));
    return 4;
}
#endif

static size_t asm_movsx(SecBuf *s, int dst, int src, int dst_sz, int src_sz) {
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

static size_t asm_movzx(SecBuf *s, int dst, int src, int dst_sz, int src_sz) {
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

static size_t asm_add_reg_reg(SecBuf *s, int dst, int src, int size) {
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
    secbuf_emit32le(s, arm64_add_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm, 0));
    asm_record(ASM_ADD_RI, off, 1, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_add_ri(s, size, CG_X86_REG(r), imm);
    size_t count = s->len - off;
    asm_record(ASM_ADD_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_sub_reg_reg(SecBuf *s, int dst, int src, int size) {
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
    secbuf_emit32le(s, arm64_sub_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm, 0));
    asm_record(ASM_SUB_RI, off, 1, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return 4;
#else
    x86_sub_ri(s, size, CG_X86_REG(r), imm);
    size_t count = s->len - off;
    asm_record(ASM_SUB_RI, off, count, r, -1, -1, size, imm, 0, NULL, 0, -1, false);
    return count;
#endif
}

static size_t asm_mul_reg_reg(SecBuf *s, int dst, int src, int size) {
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

static size_t asm_neg(SecBuf *s, int r, int size) {
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

static size_t asm_not(SecBuf *s, int r, int size) {
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

static size_t asm_dec(SecBuf *s, int r, int size) {
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

static size_t asm_and_reg_reg(SecBuf *s, int dst, int src, int size) {
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

static size_t asm_or_reg_reg(SecBuf *s, int dst, int src, int size) {
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

static size_t asm_eor_reg_reg(SecBuf *s, int dst, int src, int size) {
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

static size_t asm_shl_imm(SecBuf *s, int r, int size, uint8_t shift) {
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

static size_t asm_shr_imm(SecBuf *s, int r, int size, uint8_t shift) {
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

static size_t asm_sar_imm(SecBuf *s, int r, int size, uint8_t shift) {
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

static size_t asm_shl_cl(SecBuf *s, int r, int size) {
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

static size_t asm_shr_cl(SecBuf *s, int r, int size) {
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

static size_t asm_sar_cl(SecBuf *s, int r, int size) {
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

static size_t asm_cmp_reg_reg(SecBuf *s, int a, int b, int size) {
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

static size_t asm_cmp_zero(SecBuf *s, int r, int size) {
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
static size_t asm_setcc(SecBuf *s, int r, X86Cond cond) {
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
#endif
#ifdef ARCH_ARM64
static size_t asm_add_x0_fp_imm(SecBuf *s, int imm) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_add_imm(1, 0, 29, imm, 0));
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
static size_t asm_str_fp_reg(SecBuf *s, int reg) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_str_imm(1, 29, CG_ARM_REG(reg), 0, false));
    return s->len - off;
}
static size_t asm_ldr_fp_reg(SecBuf *s, int reg) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_ldr_imm(1, 29, CG_ARM_REG(reg), 0, false));
    return s->len - off;
}
static size_t asm_ldur_x16_fp_minus(SecBuf *s, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_ldur(1, 16, 29, -off));
    return s->len - o;
}
static size_t asm_sub_x17_fp_imm(SecBuf *s, int imm) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_sub_imm(1, 17, 29, imm, 0));
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
static size_t asm_asr_x17_63(SecBuf *s, int src) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_asr_imm(1, 17, CG_ARM_REG(src), 63));
    return s->len - o;
}
static size_t asm_cmn_imm(SecBuf *s, int r, int sf, int imm) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_subs_imm(sf, 31, CG_ARM_REG(r), -imm, 0));
    return s->len - o;
}
static size_t asm_add_reg_fp_imm(SecBuf *s, int r, int32_t imm) {
    size_t o = s->len;
    if (imm >= 0)
        secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_REG(r), 29, imm, 0));
    else
        secbuf_emit32le(s, arm64_sub_imm(1, CG_ARM_REG(r), 29, -imm, 0));
    return s->len - o;
}
static size_t asm_sub_reg_fp_imm(SecBuf *s, int r, int32_t imm) {
    size_t o = s->len;
    if (imm >= 0)
        secbuf_emit32le(s, arm64_sub_imm(1, CG_ARM_REG(r), 29, imm, 0));
    else
        secbuf_emit32le(s, arm64_add_imm(1, CG_ARM_REG(r), 29, -imm, 0));
    return s->len - o;
}
static size_t asm_sub_reg_fp_reg(SecBuf *s, int dst, int src, int size) {
    size_t o = s->len;
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_sub_reg(sf, CG_ARM_REG(dst), 29, CG_ARM_REG(src), ARM64_LSL, 0));
    return s->len - o;
}
static size_t asm_stur_fp(SecBuf *s, int r, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_stur(1, CG_ARM_REG(r), 29, -off));
    return s->len - o;
}
static size_t asm_ldur_fp(SecBuf *s, int r, int off) {
    size_t o = s->len;
    secbuf_emit32le(s, arm64_ldur(1, CG_ARM_REG(r), 29, -off));
    return s->len - o;
}

static size_t asm_stur_phy(SecBuf *s, int rt, int rn, int sf, int32_t off) {
    size_t o = s->len;
    if (sf == 0) secbuf_emit32le(s, arm64_sturb(rt, rn, off));
    else if (sf == 1)
        secbuf_emit32le(s, arm64_sturh(rt, rn, off));
    else if (sf == 2)
        secbuf_emit32le(s, arm64_stur(0, rt, rn, off));
    else
        secbuf_emit32le(s, arm64_stur(1, rt, rn, off));
    return s->len - o;
}

static size_t asm_ldur_phy(SecBuf *s, int rt, int rn, int sf, int32_t off) {
    size_t o = s->len;
    if (sf == 0) secbuf_emit32le(s, arm64_ldurb(rt, rn, off));
    else if (sf == 1)
        secbuf_emit32le(s, arm64_ldurh(rt, rn, off));
    else if (sf == 2)
        secbuf_emit32le(s, arm64_ldur(0, rt, rn, off));
    else
        secbuf_emit32le(s, arm64_ldur(1, rt, rn, off));
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

static size_t asm_cmp_imm(SecBuf *s, int r, int size, int32_t imm) {
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
static size_t asm_test_reg_reg(SecBuf *s, int a, int b, int size) {
    size_t off = s->len;
    x86_test_rr(s, size, CG_X86_REG(a), CG_X86_REG(b));
    asm_record(ASM_TEST_RR, off, s->len - off, a, b, -1, size, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_inc(SecBuf *s, int r, int size) {
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
    (void)s;
    (void)r;
    (void)size;
    return 0;
}

// ============================================================================
// Floating point
// ============================================================================

static size_t asm_cvtsi2sd(SecBuf *s, int src_r, int size) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    secbuf_emit32le(s, arm64_scvtf(size == 8, 0, 0, CG_ARM_REG(src_r)));
#else
    x86_cvtsi2sd(s, size, X86_XMM0, CG_X86_REG(src_r));
#endif
    return s->len - off;
}
static size_t asm_cvttsd2si(SecBuf *s, int dst_r, int size) {
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
static size_t asm_ldr_reg_off(SecBuf *s, int dst_r, int base_r, int size, uint32_t uimm) {
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
// ARM64 load float s/d from [base]; size=4→S(32bit), size=8→D(64bit)
static size_t asm_ldr_fp(SecBuf *s, int dst_fp_r, int base_r, int size) {
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
static size_t asm_str_fp(SecBuf *s, int src_fp_r, int base_r, int size) {
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

// ============================================================================
// Atomics
// ============================================================================

#ifdef ARCH_ARM64
static size_t asm_dmb(SecBuf *s) {
    size_t off = s->len;
    secbuf_emit32le(s, arm64_dmb(0xb));
    asm_record(ASM_FENCE, off, 1, -1, -1, -1, 0, 0, 0, NULL, 0, -1, false);
    return 4;
}
static size_t asm_dmb_ishld(SecBuf *s) { return asm_dmb(s); }
static size_t asm_dmb_ishst(SecBuf *s) { return asm_dmb(s); }
#endif

// ============================================================================
// NOP
// ============================================================================

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

static size_t asm_and_imm(SecBuf *s, int r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_and_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), ~imm));
#else
    x86_and_ri(s, size, CG_X86_REG(r), imm);
#endif
    return s->len - off;
}
static size_t asm_or_imm(SecBuf *s, int r, int size, int32_t imm) {
    size_t off = s->len;
#ifdef ARCH_ARM64
    int sf = (size == 8) ? 1 : 0;
    secbuf_emit32le(s, arm64_orr_imm(sf, CG_ARM_REG(r), CG_ARM_REG(r), imm));
#else
    x86_or_ri(s, size, CG_X86_REG(r), imm);
#endif
    return s->len - off;
}
static size_t asm_xor_imm(SecBuf *s, int r, int size, int32_t imm) {
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
#else
    asm_mov_imm(s, r, sf ? 8 : 4, (int64_t)(uint64_t)imm16 << shift);
#endif
    return s->len - off;
}
// fmov x{rd}, d{rn}  — copy fp reg raw bits to integer virtual reg
static size_t asm_fmov_f2i(SecBuf *s, int rd, int rn, int sf) {
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
static size_t asm_movzx_mem_reg(SecBuf *s, int dst, int src_addr, int dst_sz, int src_sz) {
    size_t off = s->len;
    x86_movzx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOVZX, off, s->len - off, dst, src_addr, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}
static size_t asm_movsx_mem_reg(SecBuf *s, int dst, int src_addr, int dst_sz, int src_sz) {
    size_t off = s->len;
    x86_movsx_rm(s, dst_sz, src_sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOVSX, off, s->len - off, dst, src_addr, -1, dst_sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}

// mov from [virtual_reg] to virtual_reg
static size_t asm_mov_mem_reg(SecBuf *s, int dst, int src_addr, int sz) {
    size_t off = s->len;
    x86_mov_rm(s, sz, CG_X86_REG(dst), x86_mem(CG_X86_REG(src_addr), 0));
    asm_record(ASM_MOV_RR, off, s->len - off, dst, src_addr, -1, sz, 0, 0, NULL, 0, -1, false);
    return s->len - off;
}

// mov from virtual_reg to [virtual_reg]
static size_t asm_mov_reg_mem(SecBuf *s, int src, int dst_addr, int sz) {
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

#endif // CODEGEN_ASM_H
